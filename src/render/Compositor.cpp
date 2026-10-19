#include "render/Compositor.h"

#include "media/ColorMatrix.h"

#include <QFile>
#include <QMatrix4x4>
#include <rhi/qrhi.h>

#include <cmath>

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
constexpr quint32 kUniformSize = 64 + 64 + 16 + 16; // mvp, matriz de color, recorte, parámetros

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

} // namespace

struct Compositor::Slot {
    std::unique_ptr<QRhiTexture> tex0; // Y o RGBA
    std::unique_ptr<QRhiTexture> tex1; // UV (solo NV12)
    media::PixelFormat format = media::PixelFormat::NV12;
    std::unique_ptr<QRhiBuffer> ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> srb;
    std::shared_ptr<const media::VideoFrame> uploaded; // mantiene vivos los datos hasta la subida
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

void Compositor::ensureCommon()
{
    if (m_vbuf)
        return;
    m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
    m_vbuf->create();
    m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
    m_sampler->create();
    m_solidUbuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize));
    m_solidUbuf->create();
    m_solidSrb.reset(m_rhi->newShaderResourceBindings());
    m_solidSrb->setBindings({QRhiShaderResourceBinding::uniformBuffer(
        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_solidUbuf.get())});
    m_solidSrb->create();
    m_dummy.reset(m_rhi->newTexture(QRhiTexture::R8, QSize(2, 2)));
    m_dummy->create();
}

void Compositor::setRenderTarget(QRhiRenderPassDescriptor* renderPass, int sampleCount)
{
    ensureCommon();
    if (m_nv12Pipeline && m_renderPass && m_renderPass->isCompatible(renderPass) && m_sampleCount == sampleCount)
        return;
    m_renderPass = renderPass;
    m_sampleCount = sampleCount;

    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });

    // Plantilla de bindings de una capa (los SRB reales de cada capa son compatibles con este).
    std::unique_ptr<QRhiShaderResourceBindings> layerLayout(m_rhi->newShaderResourceBindings());
    layerLayout->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                 m_solidUbuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_dummy.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, m_dummy.get(), m_sampler.get()),
    });
    layerLayout->create();

    // Mezcla "sobre" con alfa no premultiplicado.
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

    auto makePipeline = [&](const char* frag, QRhiShaderResourceBindings* srb) {
        std::unique_ptr<QRhiGraphicsPipeline> p(m_rhi->newGraphicsPipeline());
        p->setFlags(QRhiGraphicsPipeline::UsesScissor);
        p->setShaderStages({
            {QRhiShaderStage::Vertex, loadShader(QStringLiteral("layer.vert.qsb"))},
            {QRhiShaderStage::Fragment, loadShader(QString::fromLatin1(frag))},
        });
        p->setVertexInputLayout(layout);
        p->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        p->setTargetBlends({blend});
        p->setSampleCount(sampleCount);
        p->setShaderResourceBindings(srb);
        p->setRenderPassDescriptor(renderPass);
        p->create();
        return p;
    };
    m_nv12Pipeline = makePipeline("layer_nv12.frag.qsb", layerLayout.get());
    m_rgbaPipeline = makePipeline("layer_rgba.frag.qsb", layerLayout.get());
    m_solidPipeline = makePipeline("solid.frag.qsb", m_solidSrb.get());
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
    } else {
        slot.tex0.reset(m_rhi->newTexture(QRhiTexture::R8, size));
        slot.tex0->create();
        slot.tex1.reset(m_rhi->newTexture(QRhiTexture::RG8, QSize((f.width + 1) / 2, (f.height + 1) / 2)));
        slot.tex1->create();
    }
    if (!slot.srb)
        slot.srb.reset(m_rhi->newShaderResourceBindings());
    slot.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                 slot.ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, slot.tex0.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                  slot.tex1 ? slot.tex1.get() : m_dummy.get(), m_sampler.get()),
    });
    slot.srb->create();
    slot.uploaded.reset();
}

void Compositor::prepare(const engine::FrameBundle& bundle, QRhiResourceUpdateBatch* updates, QSize targetSize, QRectF canvasRect)
{
    ensureCommon();
    if (!m_vbufUploaded) {
        updates->uploadStaticBuffer(m_vbuf.get(), kQuad);
        m_vbufUploaded = true;
    }
    m_canvas = canvasRect;

    // Fondo negro del lienzo.
    if (!canvasRect.isEmpty()) {
        const QMatrix4x4 mvp = placement(m_rhi, canvasRect.center(), canvasRect.size() / 2, 0.0, targetSize);
        const float color[16] = {0, 0, 0, 1}; // primera columna = RGBA
        updates->updateDynamicBuffer(m_solidUbuf.get(), 0, 64, mvp.constData());
        updates->updateDynamicBuffer(m_solidUbuf.get(), 64, 64, color);
    }

    m_activeLayers = 0;
    for (const engine::LayerFrame& layer : bundle.layers) {
        if (!layer.frame || canvasRect.isEmpty() || layer.opacity <= 0.0)
            continue;
        if (m_slots.size() <= m_activeLayers) {
            auto slot = std::make_unique<Slot>();
            slot->ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize));
            slot->ubuf->create();
            m_slots.push_back(std::move(slot));
        }
        Slot& slot = *m_slots[m_activeLayers++];
        const media::VideoFrame& f = *layer.frame;
        ensureSlotTextures(slot, f);
        if (slot.uploaded != layer.frame) {
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

        const LayerGeometry g = layerGeometry(canvasRect, f.displayAspect(), layer.transform);
        const QMatrix4x4 mvp = placement(m_rhi, g.center, g.halfSize, g.rotation, targetSize);
        const auto rowMajor = media::yuvToRgbMatrix(f.colorSpace, f.fullRange);
        const QMatrix4x4 color(rowMajor.data());
        const float crop[4] = {float(layer.crop.left), float(layer.crop.top), float(layer.crop.right), float(layer.crop.bottom)};
        const float params[4] = {float(std::clamp(layer.opacity, 0.0, 1.0)), 0, 0, 0};
        updates->updateDynamicBuffer(slot.ubuf.get(), 0, 64, mvp.constData());
        updates->updateDynamicBuffer(slot.ubuf.get(), 64, 64, color.constData());
        updates->updateDynamicBuffer(slot.ubuf.get(), 128, 16, crop);
        updates->updateDynamicBuffer(slot.ubuf.get(), 144, 16, params);
    }
}

void Compositor::draw(QRhiCommandBuffer* cb, QSize targetSize)
{
    if (!m_nv12Pipeline || m_canvas.isEmpty())
        return;
    const QRhiCommandBuffer::VertexInput input(m_vbuf.get(), 0);
    // Tijera al lienzo (QRhi la expresa con origen abajo a la izquierda).
    const QRect c = m_canvas.toAlignedRect().intersected(QRect(QPoint(0, 0), targetSize));
    const QRhiScissor scissor(c.x(), targetSize.height() - (c.y() + c.height()), c.width(), c.height());

    cb->setGraphicsPipeline(m_solidPipeline.get());
    cb->setViewport(QRhiViewport(0, 0, float(targetSize.width()), float(targetSize.height())));
    cb->setScissor(scissor);
    cb->setShaderResources(m_solidSrb.get());
    cb->setVertexInput(0, 1, &input);
    cb->draw(4);
    for (size_t i = 0; i < m_activeLayers; ++i) {
        const Slot& slot = *m_slots[i];
        cb->setGraphicsPipeline(slot.format == media::PixelFormat::RGBA ? m_rgbaPipeline.get() : m_nv12Pipeline.get());
        cb->setViewport(QRhiViewport(0, 0, float(targetSize.width()), float(targetSize.height())));
        cb->setScissor(scissor);
        cb->setShaderResources(slot.srb.get());
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
    }
}

} // namespace movix::render
