#include "render/Compositor.h"

#include "media/ColorMatrix.h"

#include <QFile>
#include <QMatrix4x4>
#include <rhi/qrhi.h>

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
constexpr quint32 kUniformSize = 128; // mvp + matriz de color

// Transforma el rectángulo unidad en `rect` (píxeles del destino, origen arriba a la izquierda).
QMatrix4x4 rectTransform(QRhi* rhi, const QRectF& rect, QSize target)
{
    QMatrix4x4 m = rhi->clipSpaceCorrMatrix();
    const double w = target.width(), h = target.height();
    m.translate(float(rect.center().x() / w * 2.0 - 1.0), float(1.0 - rect.center().y() / h * 2.0));
    m.scale(float(rect.width() / w), float(rect.height() / h));
    return m;
}

} // namespace

struct Compositor::Slot {
    std::unique_ptr<QRhiTexture> y;
    std::unique_ptr<QRhiTexture> uv;
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
    if (m_layerPipeline && m_renderPass && m_renderPass->isCompatible(renderPass) && m_sampleCount == sampleCount)
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

    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

    auto makePipeline = [&](const char* frag, QRhiShaderResourceBindings* srb) {
        std::unique_ptr<QRhiGraphicsPipeline> p(m_rhi->newGraphicsPipeline());
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
    m_layerPipeline = makePipeline("layer_nv12.frag.qsb", layerLayout.get());
    m_solidPipeline = makePipeline("solid.frag.qsb", m_solidSrb.get());
}

void Compositor::ensureSlotTextures(Slot& slot, int width, int height)
{
    if (slot.y && slot.y->pixelSize() == QSize(width, height))
        return;
    slot.y.reset(m_rhi->newTexture(QRhiTexture::R8, QSize(width, height)));
    slot.y->create();
    slot.uv.reset(m_rhi->newTexture(QRhiTexture::RG8, QSize((width + 1) / 2, (height + 1) / 2)));
    slot.uv->create();
    if (!slot.srb)
        slot.srb.reset(m_rhi->newShaderResourceBindings());
    slot.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                 slot.ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, slot.y.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, slot.uv.get(), m_sampler.get()),
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

    // Fondo negro del lienzo.
    m_drawCanvas = !canvasRect.isEmpty();
    if (m_drawCanvas) {
        const QMatrix4x4 mvp = rectTransform(m_rhi, canvasRect, targetSize);
        const float color[16] = {0, 0, 0, 1}; // primera columna = RGBA
        updates->updateDynamicBuffer(m_solidUbuf.get(), 0, 64, mvp.constData());
        updates->updateDynamicBuffer(m_solidUbuf.get(), 64, 64, color);
    }

    m_activeLayers = 0;
    for (const engine::LayerFrame& layer : bundle.layers) {
        if (!layer.frame || canvasRect.isEmpty())
            continue;
        if (m_slots.size() <= m_activeLayers) {
            auto slot = std::make_unique<Slot>();
            slot->ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize));
            slot->ubuf->create();
            m_slots.push_back(std::move(slot));
        }
        Slot& slot = *m_slots[m_activeLayers++];
        const media::VideoFrame& f = *layer.frame;
        ensureSlotTextures(slot, f.width, f.height);
        if (slot.uploaded != layer.frame) {
            // fromRawData evita copiar; `uploaded` mantiene vivos los datos hasta que se suben.
            QRhiTextureSubresourceUploadDescription y(QByteArray::fromRawData(reinterpret_cast<const char*>(f.y.data()), qsizetype(f.y.size())));
            QRhiTextureSubresourceUploadDescription uv(QByteArray::fromRawData(reinterpret_cast<const char*>(f.uv.data()), qsizetype(f.uv.size())));
            updates->uploadTexture(slot.y.get(), QRhiTextureUploadEntry(0, 0, y));
            updates->uploadTexture(slot.uv.get(), QRhiTextureUploadEntry(0, 0, uv));
            slot.uploaded = layer.frame;
        }
        // La capa se ajusta al lienzo conservando su relación de aspecto.
        const QRectF inCanvas = fitRect(canvasRect.size(), f.displayAspect()).translated(canvasRect.topLeft());
        const QMatrix4x4 mvp = rectTransform(m_rhi, inCanvas, targetSize);
        const auto rowMajor = media::yuvToRgbMatrix(f.colorSpace, f.fullRange);
        const QMatrix4x4 color(rowMajor.data());
        updates->updateDynamicBuffer(slot.ubuf.get(), 0, 64, mvp.constData());
        updates->updateDynamicBuffer(slot.ubuf.get(), 64, 64, color.constData());
    }
}

void Compositor::draw(QRhiCommandBuffer* cb, QSize targetSize)
{
    if (!m_layerPipeline)
        return;
    const QRhiCommandBuffer::VertexInput input(m_vbuf.get(), 0);
    cb->setViewport(QRhiViewport(0, 0, float(targetSize.width()), float(targetSize.height())));
    if (m_drawCanvas) {
        cb->setGraphicsPipeline(m_solidPipeline.get());
        cb->setShaderResources(m_solidSrb.get());
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
    }
    for (size_t i = 0; i < m_activeLayers; ++i) {
        cb->setGraphicsPipeline(m_layerPipeline.get());
        cb->setShaderResources(m_slots[i]->srb.get());
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
    }
}

} // namespace movix::render
