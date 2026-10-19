#include "render/Compositor.h"

#include "color/Hdr.h"
#include "media/ColorMatrix.h"

#include <QFile>
#include <QFloat16>
#include <QMatrix4x4>
#include <QVarLengthArray>
#include <rhi/qrhi.h>

#include <cmath>
#include <algorithm>
#include <cstring>
#include <numbers>

namespace movix::render {

namespace {

QShader loadShader(const QString& name)
{
    QFile f(QStringLiteral(":/movix/shaders/") + name);
    if (!f.open(QIODevice::ReadOnly))
        qFatal("No se encontró el shader %s", qPrintable(name));
    return QShader::fromSerialized(f.readAll());
}

// Rectángulo unidad en NDC con coordenadas de textura (v = 0 arriba).
constexpr float kQuad[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

// Bloque de uniformes común a todos los shaders (ver layer.frag).
struct Uniforms {
    float mvp[16];
    float yuvToRgb[16];
    float crop[4];
    float params[4];
    float key[4];
    float keyChroma[4];
    float mask0[4];
    float mask1[4];
    float lutCoord[4];
    float canvas[4];
    float signal[4];
};
static_assert(sizeof(Uniforms) == 272);
constexpr quint32 kUniformSize = sizeof(Uniforms);

// Lleva el rectángulo unidad a un rectángulo rotado de centro `c` y semiejes `half` en píxeles
// del destino (origen arriba a la izquierda), y de ahí a coordenadas de recorte.
QMatrix4x4 placement(QRhi* rhi, QPointF c, QSizeF half, double rotationDeg, QSize target)
{
    QMatrix4x4 m = rhi->clipSpaceCorrMatrix();
    // Píxeles → NDC (y hacia arriba).
    QMatrix4x4 toNdc;
    toNdc.setToIdentity();
    toNdc(0, 0) = float(2.0 / target.width());
    toNdc(0, 3) = -1.0f;
    toNdc(1, 1) = float(-2.0 / target.height());
    toNdc(1, 3) = 1.0f;
    m *= toNdc;
    m.translate(float(c.x()), float(c.y()));
    m.rotate(float(rotationDeg), 0.0f, 0.0f, 1.0f); // con y hacia abajo, positivo = horario
    m.scale(float(half.width()), float(-half.height())); // la parte superior del rectángulo (v = 0) arriba
    return m;
}

void setVec(float* dst, float a, float b = 0, float c = 0, float d = 0)
{
    dst[0] = a;
    dst[1] = b;
    dst[2] = c;
    dst[3] = d;
}

} // namespace

struct Compositor::Slot {
    std::unique_ptr<QRhiTexture> tex0; // Y o RGBA
    std::unique_ptr<QRhiTexture> tex1; // UV (solo NV12)
    media::PixelFormat format = media::PixelFormat::NV12;
    std::unique_ptr<QRhiBuffer> ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> srb;
    std::shared_ptr<const media::VideoFrame> uploaded; // mantiene vivos los datos hasta la subida
    std::unique_ptr<QRhiTexture> lutTex;
    std::shared_ptr<const color::Lut3D> lutUploaded;
    bool useLut = false;
    bool blends = false; // modo de fusión distinto de normal
    std::unique_ptr<QRhiTexture> maskTex;
    std::shared_ptr<const engine::MaskImage> maskUploaded;
    bool useMaskTex = false;
    // Clip compuesto: un compositor propio dibuja su secuencia en `nestedTex` (transparente).
    bool nested = false;
    std::unique_ptr<QRhiTexture> nestedTex;
    std::unique_ptr<QRhiTextureRenderTarget> nestedTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> nestedPass;
    std::unique_ptr<Compositor> child;
    QRhiResourceUpdateBatch* childUpdates = nullptr;
    // Texturas con las que se creó `srb` (para rehacerlo solo si cambian).
    QRhiTexture* bound[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
};

Compositor::Compositor(QRhi* rhi) : m_rhi(rhi) {}

Compositor::~Compositor() = default;

QRectF Compositor::fitRect(QSizeF target, double aspect)
{
    if (target.isEmpty() || aspect <= 0.0)
        return {};
    const double targetAspect = target.width() / target.height();
    QSizeF size = targetAspect > aspect ? QSizeF(target.height() * aspect, target.height())
                                        : QSizeF(target.width(), target.width() / aspect);
    return QRectF(QPointF((target.width() - size.width()) / 2, (target.height() - size.height()) / 2), size);
}

Compositor::LayerGeometry Compositor::layerGeometry(const QRectF& canvas, double frameAspect, const core::LayerTransform& t)
{
    const QRectF fit = fitRect(canvas.size(), frameAspect);
    LayerGeometry g;
    g.center = canvas.center() + QPointF(t.x * canvas.width(), t.y * canvas.height());
    g.halfSize = QSizeF(fit.width() / 2 * t.scale, fit.height() / 2 * t.scale);
    g.rotation = t.rotation;
    return g;
}

QRhiTexture* Compositor::canvasTexture() const
{
    return m_canvasRect.isEmpty() || !m_canvasValid ? nullptr : m_canvas.get();
}

void Compositor::ensureCommon()
{
    if (m_vbuf)
        return;
    m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
    m_vbuf->create();
    m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_sampler->create();
    m_solidUbuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize));
    m_solidUbuf->create();
    m_blitUbuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize));
    m_blitUbuf->create();
    m_dummy2D.reset(m_rhi->newTexture(QRhiTexture::RGBA8, QSize(2, 2)));
    m_dummy2D->create();
    m_dummy3D.reset(m_rhi->newTexture(QRhiTexture::RGBA8, 2, 2, 2, 1, QRhiTexture::ThreeDimensional));
    m_dummy3D->create();
    // Lienzo en coma flotante (sin bandas al fusionar) si la GPU puede renderizar en ese formato.
    m_canvasFormat = m_rhi->isTextureFormatSupported(QRhiTexture::RGBA16F) ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
    m_solidSrb.reset(m_rhi->newShaderResourceBindings());
    m_solidSrb->setBindings({QRhiShaderResourceBinding::uniformBuffer(
        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_solidUbuf.get())});
    m_solidSrb->create();
}

namespace {

QRhiVertexInputLayout quadLayout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    return layout;
}

std::unique_ptr<QRhiGraphicsPipeline> makePipeline(QRhi* rhi, const char* frag, QRhiShaderResourceBindings* srb,
                                                   QRhiRenderPassDescriptor* pass, int sampleCount, bool blend)
{
    std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
    p->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral("layer.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QString::fromLatin1(frag))},
    });
    p->setVertexInputLayout(quadLayout());
    p->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    if (blend) { // "sobre" con alfa no premultiplicado (el lienzo es opaco)
        QRhiGraphicsPipeline::TargetBlend b;
        b.enable = true;
        b.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        b.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        b.srcAlpha = QRhiGraphicsPipeline::One;
        b.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        p->setTargetBlends({b});
    }
    p->setSampleCount(sampleCount);
    p->setShaderResourceBindings(srb);
    p->setRenderPassDescriptor(pass);
    p->create();
    return p;
}

} // namespace

std::unique_ptr<QRhiShaderResourceBindings> Compositor::layerLayout()
{
    // Plantilla de bindings de una capa (los SRB reales de cada capa son compatibles con esta).
    std::unique_ptr<QRhiShaderResourceBindings> layout(m_rhi->newShaderResourceBindings());
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    const auto frag = QRhiShaderResourceBinding::FragmentStage;
    layout->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, stages, m_solidUbuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, frag, m_dummy2D.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, frag, m_dummy2D.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, frag, m_dummy3D.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(4, frag, m_dummy2D.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(5, frag, m_dummy2D.get(), m_sampler.get()),
    });
    layout->create();
    return layout;
}

bool Compositor::ensureCanvas(QSize size)
{
    if (m_canvas && m_canvas->pixelSize() == size)
        return true;
    const auto format = QRhiTexture::Format(m_canvasFormat);
    m_canvasTarget.reset();
    m_canvas.reset(m_rhi->newTexture(format, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    m_backdrop.reset(m_rhi->newTexture(format, size, 1, QRhiTexture::UsedAsTransferSource));
    if (!m_canvas->create() || !m_backdrop->create()) {
        m_canvas.reset();
        m_backdrop.reset();
        return false;
    }
    // Las pasadas sucesivas sobre el lienzo conservan lo ya dibujado.
    m_canvasTarget.reset(m_rhi->newTextureRenderTarget({m_canvas.get()}, QRhiTextureRenderTarget::PreserveColorContents));
    if (!m_canvasPass)
        m_canvasPass.reset(m_canvasTarget->newCompatibleRenderPassDescriptor());
    m_canvasTarget->setRenderPassDescriptor(m_canvasPass.get());
    m_canvasTarget->create();

    if (!m_layerPipeline) {
        const auto layout = layerLayout();
        m_layerPipeline = makePipeline(m_rhi, "layer.frag.qsb", layout.get(), m_canvasPass.get(), 1, true);
        m_solidPipeline = makePipeline(m_rhi, "solid.frag.qsb", m_solidSrb.get(), m_canvasPass.get(), 1, false);
    }
    // La copia final y la de los scopes leen el lienzo nuevo.
    m_copySource = nullptr;
    if (m_blitSrb) {
        m_blitSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                     m_blitUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_canvas.get(), m_sampler.get()),
        });
        m_blitSrb->create();
    }
    for (auto& slot : m_slots)
        updateSlotBindings(*slot); // el fondo de los modos de fusión es una textura nueva
    return true;
}

void Compositor::setRenderTarget(QRhiRenderPassDescriptor* renderPass, int sampleCount)
{
    ensureCommon();
    if (m_blitPipeline && m_targetPass && m_targetPass->isCompatible(renderPass) && m_sampleCount == sampleCount)
        return;
    m_targetPass = renderPass;
    m_sampleCount = sampleCount;
    if (!m_blitSrb)
        m_blitSrb.reset(m_rhi->newShaderResourceBindings());
    m_blitSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                 m_blitUbuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_canvas ? m_canvas.get() : m_dummy2D.get(),
                                                  m_sampler.get()),
    });
    m_blitSrb->create();
    m_blitPipeline = makePipeline(m_rhi, "canvas.frag.qsb", m_blitSrb.get(), renderPass, sampleCount, false);
    // Dibujo directo en el destino (sin lienzo intermedio) cuando ninguna capa lee su fondo.
    const auto layout = layerLayout();
    m_layerPipelineDirect = makePipeline(m_rhi, "layer.frag.qsb", layout.get(), renderPass, sampleCount, true);
    m_solidPipelineDirect = makePipeline(m_rhi, "solid.frag.qsb", m_solidSrb.get(), renderPass, sampleCount, false);
}

void Compositor::ensureSlotTextures(Slot& slot, const media::VideoFrame& f)
{
    const QSize size(f.width, f.height);
    if (slot.tex0 && slot.format == f.format && slot.tex0->pixelSize() == size)
        return;
    slot.format = f.format;
    if (f.format == media::PixelFormat::RGBA) {
        slot.tex0.reset(m_rhi->newTexture(QRhiTexture::RGBA8, size));
        slot.tex0->create();
        slot.tex1.reset();
    } else if (f.format == media::PixelFormat::P016) { // 10 bits o más: planos de 16 bits
        slot.tex0.reset(m_rhi->newTexture(QRhiTexture::R16, size));
        slot.tex0->create();
        slot.tex1.reset(m_rhi->newTexture(QRhiTexture::RG16, QSize((f.width + 1) / 2, (f.height + 1) / 2)));
        slot.tex1->create();
    } else {
        slot.tex0.reset(m_rhi->newTexture(QRhiTexture::R8, size));
        slot.tex0->create();
        slot.tex1.reset(m_rhi->newTexture(QRhiTexture::RG8, QSize((f.width + 1) / 2, (f.height + 1) / 2)));
        slot.tex1->create();
    }
    slot.uploaded.reset();
}

void Compositor::ensureSlotLut(Slot& slot, const std::shared_ptr<const color::Lut3D>& lut, QRhiResourceUpdateBatch* updates)
{
    slot.useLut = lut && lut->size >= 2;
    if (!slot.useLut || slot.lutUploaded == lut)
        return;
    const int n = lut->size;
    if (!slot.lutTex || slot.lutTex->pixelSize() != QSize(n, n) || slot.lutTex->depth() != n) {
        slot.lutTex.reset(m_rhi->newTexture(QRhiTexture::RGBA16F, n, n, n, 1, QRhiTexture::ThreeDimensional));
        slot.lutTex->create();
    }
    // Una subida por corte de profundidad (en texturas 3D, `layer` es el corte).
    const size_t sliceValues = size_t(n) * size_t(n) * 4;
    QVarLengthArray<QRhiTextureUploadEntry, 64> entries;
    for (int z = 0; z < n; ++z) {
        QByteArray bytes(qsizetype(sliceValues * sizeof(qfloat16)), Qt::Uninitialized);
        qFloatToFloat16(reinterpret_cast<qfloat16*>(bytes.data()), lut->rgba.data() + size_t(z) * sliceValues, qsizetype(sliceValues));
        entries.append(QRhiTextureUploadEntry(z, 0, QRhiTextureSubresourceUploadDescription(bytes)));
    }
    QRhiTextureUploadDescription desc;
    desc.setEntries(entries.cbegin(), entries.cend());
    updates->uploadTexture(slot.lutTex.get(), desc);
    slot.lutUploaded = lut;
}

void Compositor::ensureSlotMask(Slot& slot, const std::shared_ptr<const engine::MaskImage>& mask, QRhiResourceUpdateBatch* updates)
{
    slot.useMaskTex = mask && mask->width > 0 && mask->height > 0;
    if (!slot.useMaskTex || slot.maskUploaded == mask)
        return;
    const QSize size(mask->width, mask->height);
    if (!slot.maskTex || slot.maskTex->pixelSize() != size) {
        slot.maskTex.reset(m_rhi->newTexture(QRhiTexture::R8, size));
        slot.maskTex->create();
    }
    updates->uploadTexture(slot.maskTex.get(),
                           QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(
                                                            QByteArray::fromRawData(reinterpret_cast<const char*>(mask->alpha.data()),
                                                                                    qsizetype(mask->alpha.size())))));
    slot.maskUploaded = mask; // mantiene vivos los datos hasta la subida
}

void Compositor::prepareNested(Slot& slot, const engine::FrameBundle& nested, QSize size)
{
    if (!slot.nestedTex || slot.nestedTex->pixelSize() != size) {
        slot.nestedTarget.reset();
        slot.nestedTex.reset(m_rhi->newTexture(QRhiTexture::Format(m_canvasFormat), size, 1, QRhiTexture::RenderTarget));
        slot.nestedTex->create();
        slot.nestedTarget.reset(m_rhi->newTextureRenderTarget({slot.nestedTex.get()}));
        if (!slot.nestedPass)
            slot.nestedPass.reset(slot.nestedTarget->newCompatibleRenderPassDescriptor());
        slot.nestedTarget->setRenderPassDescriptor(slot.nestedPass.get());
        slot.nestedTarget->create();
    }
    if (!slot.child) {
        slot.child = std::make_unique<Compositor>(m_rhi);
        slot.child->m_transparent = true;
        slot.child->setRenderTarget(slot.nestedPass.get(), 1);
    }
    slot.childUpdates = m_rhi->nextResourceUpdateBatch();
    slot.child->prepare(nested, slot.childUpdates, size, QRectF(QPointF(0, 0), QSizeF(size)));
}

void Compositor::updateSlotBindings(Slot& slot)
{
    QRhiTexture* want[5] = {
        slot.nested ? slot.nestedTex.get() : slot.tex0.get(),
        slot.tex1 && !slot.nested ? slot.tex1.get() : m_dummy2D.get(),
        slot.useLut ? slot.lutTex.get() : m_dummy3D.get(),
        slot.blends && m_backdrop ? m_backdrop.get() : m_dummy2D.get(),
        slot.useMaskTex ? slot.maskTex.get() : m_dummy2D.get(),
    };
    if (!want[0] || (slot.srb && std::equal(std::begin(want), std::end(want), std::begin(slot.bound))))
        return;
    if (!slot.srb)
        slot.srb.reset(m_rhi->newShaderResourceBindings());
    const auto frag = QRhiShaderResourceBinding::FragmentStage;
    slot.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | frag, slot.ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, frag, want[0], m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, frag, want[1], m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, frag, want[2], m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(4, frag, want[3], m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(5, frag, want[4], m_sampler.get()),
    });
    slot.srb->create();
    std::copy(std::begin(want), std::end(want), std::begin(slot.bound));
}

void Compositor::prepare(const engine::FrameBundle& bundle, QRhiResourceUpdateBatch* updates, QSize targetSize, QRectF canvasRect)
{
    ensureCommon();
    if (!m_vbufUploaded) {
        updates->uploadStaticBuffer(m_vbuf.get(), kQuad);
        m_vbufUploaded = true;
    }
    m_targetSize = targetSize;
    // Lienzo en píxeles enteros del destino: la copia final es exacta, píxel a píxel.
    m_canvasRect = QRect(QPoint(qRound(canvasRect.left()), qRound(canvasRect.top())),
                         QPoint(qRound(canvasRect.right()) - 1, qRound(canvasRect.bottom()) - 1));
    m_canvasRect = m_canvasRect.intersected(QRect(QPoint(0, 0), targetSize));
    m_activeLayers = 0;
    if (m_canvasRect.isEmpty() || !ensureCanvas(m_canvasRect.size())) {
        m_canvasRect = {};
        return;
    }
    const QSize canvasSize = m_canvasRect.size();
    const QRectF canvas(QPointF(0, 0), QSizeF(canvasSize));

    Uniforms u{};
    // Fondo negro del lienzo.
    {
        const QMatrix4x4 mvp = placement(m_rhi, canvas.center(), canvas.size() / 2, 0.0, canvasSize);
        std::memcpy(u.mvp, mvp.constData(), sizeof(u.mvp));
        setVec(u.yuvToRgb, 0, 0, 0, m_transparent ? 0.0f : 1.0f); // primera columna = RGBA
        updates->updateDynamicBuffer(m_solidUbuf.get(), 0, kUniformSize, &u);
    }
    // Copia final al rectángulo del lienzo en el destino.
    {
        Uniforms b{};
        const QMatrix4x4 mvp = placement(m_rhi, QRectF(m_canvasRect).center(), QSizeF(canvasSize) / 2, 0.0, targetSize);
        std::memcpy(b.mvp, mvp.constData(), sizeof(b.mvp));
        setVec(b.params, m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f, m_transparent ? 1.0f : 0.0f);
        m_displayConvert = m_sdrDisplay && bundle.colorMode != core::ColorMode::SDR;
        setVec(b.signal, m_displayConvert ? float(int(bundle.colorMode)) : 0.0f);
        updates->updateDynamicBuffer(m_blitUbuf.get(), 0, kUniformSize, &b);
    }

    for (const engine::LayerFrame& layer : bundle.layers) {
        if ((!layer.frame && !layer.nested) || layer.opacity <= 0.0)
            continue;
        if (m_slots.size() <= m_activeLayers) {
            auto slot = std::make_unique<Slot>();
            slot->ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize));
            slot->ubuf->create();
            m_slots.push_back(std::move(slot));
        }
        Slot& slot = *m_slots[m_activeLayers++];
        slot.nested = layer.nested != nullptr;
        static const media::VideoFrame kNestedFormat = [] {
            media::VideoFrame f;
            f.format = media::PixelFormat::RGBA;
            return f;
        }();
        media::VideoFrame nestedInfo = kNestedFormat; // tamaño y aspecto de la secuencia anidada
        if (slot.nested) {
            nestedInfo.width = layer.nested->canvasWidth;
            nestedInfo.height = layer.nested->canvasHeight;
            prepareNested(slot, *layer.nested, canvasSize);
        }
        const media::VideoFrame& f = slot.nested ? nestedInfo : *layer.frame;
        if (!slot.nested)
            ensureSlotTextures(slot, f);
        if (!slot.nested && slot.uploaded != layer.frame) {
            // fromRawData evita copiar; `uploaded` mantiene vivos los datos hasta que se suben.
            auto raw = [](const std::vector<uint8_t>& v) {
                return QRhiTextureSubresourceUploadDescription(
                    QByteArray::fromRawData(reinterpret_cast<const char*>(v.data()), qsizetype(v.size())));
            };
            if (f.format == media::PixelFormat::RGBA) {
                updates->uploadTexture(slot.tex0.get(), QRhiTextureUploadEntry(0, 0, raw(f.rgba)));
            } else {
                updates->uploadTexture(slot.tex0.get(), QRhiTextureUploadEntry(0, 0, raw(f.y)));
                updates->uploadTexture(slot.tex1.get(), QRhiTextureUploadEntry(0, 0, raw(f.uv)));
            }
            slot.uploaded = layer.frame;
        }
        ensureSlotLut(slot, layer.lut, updates);
        ensureSlotMask(slot, layer.maskImage, updates);
        slot.blends = layer.blend != core::BlendMode::Normal;
        updateSlotBindings(slot);

        const LayerGeometry g = layerGeometry(canvas, f.displayAspect(), layer.transform);
        const QMatrix4x4 mvp = placement(m_rhi, g.center, g.halfSize, g.rotation, canvasSize);
        const auto rowMajor = media::yuvToRgbMatrix(f.colorSpace, f.fullRange, f.format == media::PixelFormat::P016);
        const QMatrix4x4 colorMatrix(rowMajor.data());
        std::memcpy(u.mvp, mvp.constData(), sizeof(u.mvp));
        std::memcpy(u.yuvToRgb, colorMatrix.constData(), sizeof(u.yuvToRgb));
        setVec(u.crop, float(layer.crop.left), float(layer.crop.top), float(layer.crop.right), float(layer.crop.bottom));
        setVec(u.params, float(std::clamp(layer.opacity, 0.0, 1.0)), float(int(layer.blend)), slot.useLut ? 1.0f : 0.0f,
               f.format == media::PixelFormat::RGBA ? 1.0f : 0.0f);
        const core::ChromaKey& key = layer.chromaKey;
        const auto keyChroma = color::chroma(color::argbToRgb(key.color));
        setVec(u.key, key.enabled ? 1.0f : 0.0f, float(key.tolerance), float(key.softness), float(key.spill));
        // keyChroma.w: la textura trae el color premultiplicado (secuencia anidada).
        setVec(u.keyChroma, keyChroma[0], keyChroma[1], float(f.displayAspect()), slot.nested ? 1.0f : 0.0f);
        if (slot.useMaskTex) {
            setVec(u.mask0, 5.0f);
            setVec(u.mask1, 0.0f);
        } else if (layer.masks.size() == 1 && layer.masks[0].shape != core::MaskShape::Path) {
            const core::LayerMask& m = layer.masks[0];
            setVec(u.mask0, float(int(m.shape)), m.invert ? 1.0f : 0.0f, float(m.feather), float(m.rotation * std::numbers::pi / 180.0));
            setVec(u.mask1, float(m.x), float(m.y), float(m.width), float(m.height));
        } else {
            setVec(u.mask0, 0.0f);
            setVec(u.mask1, 0.0f);
        }
        const float n = slot.useLut ? float(layer.lut->size) : 2.0f;
        setVec(u.lutCoord, (n - 1.0f) / n, 0.5f / n);
        setVec(u.canvas, 1.0f / float(canvasSize.width()), 1.0f / float(canvasSize.height()));
        // Señal de la fuente → la del proyecto. Las imágenes y títulos son SDR Rec.709; las secuencias
        // anidadas ya están en la del proyecto; con OCIO, la LUT ya hace la conversión.
        const bool video = !slot.nested && f.format != media::PixelFormat::RGBA;
        const auto source = video && f.transfer == media::Transfer::PQ  ? color::Signal::PQ
                            : video && f.transfer == media::Transfer::HLG ? color::Signal::HLG
                                                                          : color::Signal::SDR;
        const bool source2020 = video && f.colorSpace == media::ColorSpace::BT2020;
        const bool convert = !slot.nested && !layer.rawSignal && color::needsConversion(source, source2020, bundle.colorMode);
        setVec(u.signal, float(int(source)), source2020 ? 1.0f : 0.0f, float(int(bundle.colorMode)), convert ? 1.0f : 0.0f);
        updates->updateDynamicBuffer(slot.ubuf.get(), 0, kUniformSize, &u);
    }
}

void Compositor::drawQuad(QRhiCommandBuffer* cb, QRhiGraphicsPipeline* pipeline, QRhiShaderResourceBindings* srb, QSize viewport)
{
    drawQuad(cb, pipeline, srb, QRhiViewport(0, 0, float(viewport.width()), float(viewport.height())));
}

void Compositor::drawQuad(QRhiCommandBuffer* cb, QRhiGraphicsPipeline* pipeline, QRhiShaderResourceBindings* srb,
                          const QRhiViewport& viewport)
{
    const QRhiCommandBuffer::VertexInput input(m_vbuf.get(), 0);
    cb->setGraphicsPipeline(pipeline);
    cb->setViewport(viewport);
    cb->setShaderResources(srb);
    cb->setVertexInput(0, 1, &input);
    cb->draw(4);
}

void Compositor::render(QRhiCommandBuffer* cb, QRhiRenderTarget* target, const QColor& clearColor, QRhiResourceUpdateBatch* updates,
                        QRhiResourceUpdateBatch* afterFinal)
{
    m_lastPassCount = 0;
    m_canvasValid = false;
    // Las secuencias anidadas se componen antes, cada una en su textura.
    for (size_t j = 0; j < m_activeLayers; ++j) {
        Slot& slot = *m_slots[j];
        if (slot.nested && slot.child) {
            slot.child->render(cb, slot.nestedTarget.get(), QColor(0, 0, 0, 0), slot.childUpdates);
            m_lastPassCount += slot.child->lastPassCount();
            slot.childUpdates = nullptr;
        }
    }
    if (m_canvasRect.isEmpty() || !m_canvasTarget || !m_blitPipeline) {
        cb->beginPass(target, clearColor, {1.0f, 0}, updates);
        cb->endPass(afterFinal);
        m_lastPassCount = 1;
        return;
    }
    const QSize canvasSize = m_canvasRect.size();
    const bool anyBlend = std::any_of(m_slots.begin(), m_slots.begin() + std::ptrdiff_t(m_activeLayers),
                                      [](const auto& slot) { return slot->blends; });
    if (!anyBlend && !m_retainCanvas && !m_displayConvert) {
        // Una sola pasada sobre el destino; el lienzo es una región de él (QRhi cuenta y desde abajo).
        const QRhiViewport vp(float(m_canvasRect.x()), float(m_targetSize.height() - m_canvasRect.y() - m_canvasRect.height()),
                              float(canvasSize.width()), float(canvasSize.height()));
        cb->beginPass(target, clearColor, {1.0f, 0}, updates);
        drawQuad(cb, m_solidPipelineDirect.get(), m_solidSrb.get(), vp);
        for (size_t j = 0; j < m_activeLayers; ++j)
            drawQuad(cb, m_layerPipelineDirect.get(), m_slots[j]->srb.get(), vp);
        cb->endPass(afterFinal);
        m_lastPassCount = 1;
        return;
    }
    m_canvasValid = true;
    // Pasadas sobre el lienzo: una nueva antes de cada capa que necesita leer lo que tiene debajo.
    size_t i = 0;
    bool first = true;
    while (first || i < m_activeLayers) {
        QRhiResourceUpdateBatch* batch = updates;
        if (!first) {
            batch = m_rhi->nextResourceUpdateBatch();
            batch->copyTexture(m_backdrop.get(), m_canvas.get());
        }
        cb->beginPass(m_canvasTarget.get(), Qt::black, {1.0f, 0}, batch);
        ++m_lastPassCount;
        if (first)
            drawQuad(cb, m_solidPipeline.get(), m_solidSrb.get(), canvasSize);
        // Solo la primera capa de una pasada (no la de la inicial) tiene su fondo ya copiado.
        const size_t passStart = i;
        for (size_t j = i; j < m_activeLayers; ++j) {
            const Slot& slot = *m_slots[j];
            if (slot.blends && (j > passStart || first)) {
                i = j;
                break;
            }
            drawQuad(cb, m_layerPipeline.get(), slot.srb.get(), canvasSize);
            i = j + 1;
        }
        cb->endPass();
        first = false;
    }
    cb->beginPass(target, clearColor, {1.0f, 0}, nullptr);
    drawQuad(cb, m_blitPipeline.get(), m_blitSrb.get(), m_targetSize);
    cb->endPass(afterFinal);
    ++m_lastPassCount;
}

void Compositor::copyCanvas(QRhiCommandBuffer* cb, QRhiTextureRenderTarget* target, QRhiResourceUpdateBatch* afterPass)
{
    if (!canvasTexture() || !target) {
        if (afterPass)
            afterPass->release();
        return;
    }
    QRhiRenderPassDescriptor* pass = target->renderPassDescriptor();
    if (!m_copyUbuf) {
        m_copyUbuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize));
        m_copyUbuf->create();
        m_copySrb.reset(m_rhi->newShaderResourceBindings());
    }
    if (m_copySource != m_canvas.get()) {
        m_copySrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                     m_copyUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_canvas.get(), m_sampler.get()),
        });
        m_copySrb->create();
        m_copySource = m_canvas.get();
    }
    if (!m_copyPipeline || !m_copyPass || !m_copyPass->isCompatible(pass)) {
        m_copyPipeline = makePipeline(m_rhi, "canvas.frag.qsb", m_copySrb.get(), pass, 1, false);
        m_copyPass = pass;
    }
    const QSize size = target->pixelSize();
    Uniforms u{};
    const QMatrix4x4 mvp = placement(m_rhi, QPointF(size.width() / 2.0, size.height() / 2.0), QSizeF(size) / 2, 0.0, size);
    std::memcpy(u.mvp, mvp.constData(), sizeof(u.mvp));
    setVec(u.params, m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f);
    QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
    updates->updateDynamicBuffer(m_copyUbuf.get(), 0, kUniformSize, &u);
    cb->beginPass(target, Qt::black, {1.0f, 0}, updates);
    drawQuad(cb, m_copyPipeline.get(), m_copySrb.get(), size);
    cb->endPass(afterPass);
}

} // namespace movix::render
