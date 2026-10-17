#include "VideoView.h"

#include "media/ColorMatrix.h"

#include <QFile>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <rhi/qrhi.h>

#include <memory>

using movix::media::VideoFrame;

namespace {

QShader loadShader(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        qFatal("No se encontró el shader %s", qPrintable(path));
    return QShader::fromSerialized(f.readAll());
}

// Rectángulo que cubre el área (en NDC) con coordenadas de textura; v = 0 es la fila superior.
constexpr float kQuad[] = {
    // x,    y,    u,    v
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

constexpr int kUniformSize = 2 * 64; // mvp + yuvToRgb

} // namespace

class VideoRenderer : public QQuickRhiItemRenderer {
public:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void ensureTextures(int width, int height);
    void rebuildBindings();

    QRhi* m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiTexture> m_texY;
    std::unique_ptr<QRhiTexture> m_texUV;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    bool m_vbufUploaded = false;

    std::shared_ptr<const VideoFrame> m_pending;  // elegido en synchronize()
    std::shared_ptr<const VideoFrame> m_uploaded; // el que está en las texturas (sus datos deben vivir hasta subirse)
};

void VideoRenderer::initialize(QRhiCommandBuffer*)
{
    if (m_rhi != rhi()) {
        m_pipeline.reset();
        m_srb.reset();
        m_texY.reset();
        m_texUV.reset();
        m_sampler.reset();
        m_ubuf.reset();
        m_vbuf.reset();
        m_uploaded.reset();
        m_vbufUploaded = false;
        m_rhi = rhi();
    }

    if (!m_vbuf) {
        m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
        m_vbuf->create();
        m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize));
        m_ubuf->create();
        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                          QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_sampler->create();
        ensureTextures(16, 16); // marcador hasta el primer fotograma
    }

    // El pipeline depende del formato del destino de render; se recrea si cambia.
    QRhiRenderPassDescriptor* rp = renderTarget()->renderPassDescriptor();
    if (m_pipeline && m_pipeline->renderPassDescriptor()->isCompatible(rp) && m_pipeline->sampleCount() == renderTarget()->sampleCount())
        return;

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/video.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/video_nv12.frag.qsb"))},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    m_pipeline->setVertexInputLayout(layout);
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setSampleCount(renderTarget()->sampleCount());
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(rp);
    m_pipeline->create();
}

void VideoRenderer::ensureTextures(int width, int height)
{
    if (m_texY && m_texY->pixelSize() == QSize(width, height))
        return;
    const QSize chroma((width + 1) / 2, (height + 1) / 2);
    m_texY.reset(m_rhi->newTexture(QRhiTexture::R8, QSize(width, height)));
    m_texY->create();
    m_texUV.reset(m_rhi->newTexture(QRhiTexture::RG8, chroma));
    m_texUV->create();
    rebuildBindings();
}

void VideoRenderer::rebuildBindings()
{
    if (!m_srb)
        m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                 m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_texY.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, m_texUV.get(), m_sampler.get()),
    });
    m_srb->create();
}

void VideoRenderer::synchronize(QQuickRhiItem* item)
{
    auto* view = static_cast<VideoView*>(item);
    m_pending = view->m_player ? view->m_player->frameForPresentation() : nullptr;
}

void VideoRenderer::render(QRhiCommandBuffer* cb)
{
    QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
    if (!m_vbufUploaded) {
        updates->uploadStaticBuffer(m_vbuf.get(), kQuad);
        m_vbufUploaded = true;
    }

    if (!m_pending) {
        m_uploaded.reset();
    } else if (m_pending != m_uploaded) {
        const VideoFrame& f = *m_pending;
        ensureTextures(f.width, f.height);
        // fromRawData evita copiar: m_uploaded mantiene vivos los datos hasta el siguiente fotograma.
        QRhiTextureSubresourceUploadDescription y(QByteArray::fromRawData(reinterpret_cast<const char*>(f.y.data()), qsizetype(f.y.size())));
        QRhiTextureSubresourceUploadDescription uv(QByteArray::fromRawData(reinterpret_cast<const char*>(f.uv.data()), qsizetype(f.uv.size())));
        updates->uploadTexture(m_texY.get(), QRhiTextureUploadEntry(0, 0, y));
        updates->uploadTexture(m_texUV.get(), QRhiTextureUploadEntry(0, 0, uv));
        m_uploaded = m_pending;
    }

    const QSize target = renderTarget()->pixelSize();
    if (m_uploaded) {
        // Ajuste con bandas (letterbox / pillarbox) respetando la relación de aspecto de visualización.
        const double frameAspect = m_uploaded->displayAspect();
        const double targetAspect = target.height() > 0 ? double(target.width()) / target.height() : 1.0;
        float sx = 1.0f, sy = 1.0f;
        if (targetAspect > frameAspect)
            sx = float(frameAspect / targetAspect);
        else
            sy = float(targetAspect / frameAspect);
        QMatrix4x4 mvp = m_rhi->clipSpaceCorrMatrix();
        mvp.scale(sx, sy);
        const auto rowMajor = movix::media::yuvToRgbMatrix(m_uploaded->colorSpace, m_uploaded->fullRange);
        const QMatrix4x4 color(rowMajor.data());
        updates->updateDynamicBuffer(m_ubuf.get(), 0, 64, mvp.constData());
        updates->updateDynamicBuffer(m_ubuf.get(), 64, 64, color.constData());
    }

    cb->beginPass(renderTarget(), QColor(8, 9, 12), {1.0f, 0}, updates);
    if (m_uploaded) {
        cb->setGraphicsPipeline(m_pipeline.get());
        cb->setViewport(QRhiViewport(0, 0, float(target.width()), float(target.height())));
        cb->setShaderResources(m_srb.get());
        const QRhiCommandBuffer::VertexInput input(m_vbuf.get(), 0);
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
    }
    cb->endPass();
}

VideoView::VideoView(QQuickItem* parent) : QQuickRhiItem(parent) {}

QQuickRhiItemRenderer* VideoView::createRenderer()
{
    return new VideoRenderer;
}

void VideoView::setPlayer(movix::playback::Player* player)
{
    if (m_player == player)
        return;
    if (m_player)
        disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    if (m_player) {
        connect(m_player, &movix::playback::Player::frameReady, this, &QQuickItem::update);
        connect(m_player, &movix::playback::Player::playingChanged, this, &QQuickItem::update);
    }
    update();
    emit playerChanged();
}

void VideoView::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickRhiItem::itemChange(change, data);
    if (change == ItemSceneChange) {
        disconnect(m_animatingConnection);
        if (data.window)
            m_animatingConnection = connect(data.window, &QQuickWindow::afterAnimating, this, &VideoView::onAfterAnimating);
    }
}

void VideoView::onAfterAnimating()
{
    // Durante la reproducción se pide un fotograma en cada refresco (vsync) para elegir
    // el fotograma de video que toca según el reloj.
    if (m_player && m_player->isPlaying())
        update();
}
