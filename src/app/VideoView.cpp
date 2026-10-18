#include "VideoView.h"

#include "render/Compositor.h"

#include <QQuickWindow>
#include <rhi/qrhi.h>

#include <memory>

namespace {
const QColor kViewerBackground(8, 9, 12);
}

class VideoRenderer : public QQuickRhiItemRenderer {
public:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    QRhi* m_rhi = nullptr;
    std::unique_ptr<movix::render::Compositor> m_compositor;
    std::shared_ptr<const movix::engine::FrameBundle> m_bundle;
};

void VideoRenderer::initialize(QRhiCommandBuffer*)
{
    if (m_rhi != rhi()) {
        m_compositor.reset();
        m_rhi = rhi();
    }
    if (!m_compositor)
        m_compositor = std::make_unique<movix::render::Compositor>(m_rhi);
    m_compositor->setRenderTarget(renderTarget()->renderPassDescriptor(), renderTarget()->sampleCount());
}

void VideoRenderer::synchronize(QQuickRhiItem* item)
{
    auto* view = static_cast<VideoView*>(item);
    m_bundle = view->m_player ? view->m_player->frameForPresentation() : nullptr;
}

void VideoRenderer::render(QRhiCommandBuffer* cb)
{
    const QSize target = renderTarget()->pixelSize();
    QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
    static const movix::engine::FrameBundle kEmpty;
    const movix::engine::FrameBundle& bundle = m_bundle ? *m_bundle : kEmpty;
    QRectF canvas;
    if (m_bundle && bundle.canvasHeight > 0)
        canvas = movix::render::Compositor::fitRect(target, double(bundle.canvasWidth) / bundle.canvasHeight);
    m_compositor->prepare(bundle, updates, target, canvas);
    cb->beginPass(renderTarget(), kViewerBackground, {1.0f, 0}, updates);
    m_compositor->draw(cb, target);
    cb->endPass();
}

VideoView::VideoView(QQuickItem* parent) : QQuickRhiItem(parent) {}

QQuickRhiItemRenderer* VideoView::createRenderer()
{
    return new VideoRenderer;
}

void VideoView::setPlayer(movix::playback::TimelinePlayer* player)
{
    if (m_player == player)
        return;
    if (m_player)
        disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    if (m_player) {
        connect(m_player, &movix::playback::TimelinePlayer::frameReady, this, &QQuickItem::update);
        connect(m_player, &movix::playback::TimelinePlayer::playingChanged, this, &QQuickItem::update);
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
    // Durante la reproducción se pide un fotograma en cada refresco (vsync).
    if (m_player && m_player->isPlaying())
        update();
}
