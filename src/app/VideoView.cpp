#include "VideoView.h"

#include "media/GpuFrames.h"
#include "render/Compositor.h"

#include <QQuickWindow>
#include <rhi/qrhi.h>

#include <atomic>
#include <chrono>
#include <memory>

namespace {
const QColor kViewerBackground(8, 9, 12);
constexpr int kScopeWidth = 480;                         // ancho de la copia para los scopes
constexpr std::chrono::milliseconds kScopeInterval{60}; // ~15 lecturas por segundo como máximo

// Dispositivo Direct3D 11 de la ventana para la copia cero: el reproductor decodifica en él y los
// fotogramas no bajan a memoria. nullptr con otro backend o con MOVIX_NO_ZERO_COPY.
void* zeroCopyDevice(QRhi* rhi)
{
#ifdef Q_OS_WIN
    if (rhi && rhi->backend() == QRhi::D3D11 && qEnvironmentVariableIsEmpty("MOVIX_NO_ZERO_COPY")) {
        const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        if (handles && handles->dev && movix::media::enableMultithreadProtection(handles->dev))
            return handles->dev;
    }
#endif
    return nullptr;
}
}

class VideoRenderer : public QQuickRhiItemRenderer {
public:
    ~VideoRenderer() override;
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void updateScopes(QRhiCommandBuffer* cb);

    QRhi* m_rhi = nullptr;
    void* m_device = nullptr; // copia cero (ver zeroCopyDevice)
    std::unique_ptr<movix::render::Compositor> m_compositor;
    std::shared_ptr<const movix::engine::FrameBundle> m_bundle;

    // Scopes: copia reducida del lienzo y su lectura asíncrona.
    std::shared_ptr<ScopeFeed> m_feed;
    std::unique_ptr<QRhiTexture> m_scopeTexture;
    std::unique_ptr<QRhiTextureRenderTarget> m_scopeTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_scopePass;
    QRhiReadbackResult m_readback;
    std::atomic<bool> m_readbackPending{false};
    std::chrono::steady_clock::time_point m_lastScope;
    std::shared_ptr<const movix::engine::FrameBundle> m_scopeBundle; // último fotograma copiado
    QSize m_scopeCanvas;
};

VideoRenderer::~VideoRenderer()
{
    m_scopeTarget.reset();
    m_scopePass.reset();
    m_scopeTexture.reset();
}

void VideoRenderer::initialize(QRhiCommandBuffer*)
{
    if (m_rhi != rhi()) {
        m_compositor.reset();
        m_scopeTarget.reset();
        m_scopePass.reset();
        m_scopeTexture.reset();
        m_rhi = rhi();
        m_device = zeroCopyDevice(m_rhi);
    }
    if (!m_compositor)
        m_compositor = std::make_unique<movix::render::Compositor>(m_rhi);
    m_compositor->setRenderTarget(renderTarget()->renderPassDescriptor(), renderTarget()->sampleCount());
}

void VideoRenderer::synchronize(QQuickRhiItem* item)
{
    auto* view = static_cast<VideoView*>(item);
    // El monitor HDR dibuja con su propio dispositivo: mientras está abierto, en memoria.
    if (view->m_player)
        view->m_player->setGpuDevice(view->m_mirror ? nullptr : m_device);
    m_bundle = view->m_player ? view->m_player->frameForPresentation() : nullptr;
    m_feed = view->m_feed;
    if (view->m_mirror) // el hilo de la interfaz está parado durante la sincronización
        view->m_mirror->present(m_bundle);
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
    m_compositor->setRetainCanvas(m_feed && m_feed->active()); // los scopes leen el lienzo
    m_compositor->setSdrDisplay(true); // proyectos HDR: se ven convertidos a SDR
    m_compositor->prepare(bundle, updates, target, canvas);
    m_compositor->render(cb, renderTarget(), kViewerBackground, updates);
    if (m_feed && m_feed->active())
        updateScopes(cb);
    else
        m_scopeBundle.reset();
}

// La lectura de la GPU se completa en un fotograma posterior y, en pausa, no llegaría ninguno:
// mientras haya una lectura en curso o una imagen nueva sin copiar se pide otro refresco.
void VideoRenderer::updateScopes(QRhiCommandBuffer* cb)
{
    if (m_readbackPending) {
        update();
        return;
    }
    const QRect canvas = m_compositor->canvasRect();
    if (canvas.isEmpty() || !m_compositor->canvasTexture())
        return;
    if (m_bundle == m_scopeBundle && canvas.size() == m_scopeCanvas)
        return; // el scope ya analiza esta imagen
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastScope < kScopeInterval) {
        update();
        return;
    }
    const int w = std::min(kScopeWidth, canvas.width());
    const QSize size(w, std::max(1, int(std::lround(double(w) * canvas.height() / canvas.width()))));
    if (!m_scopeTexture || m_scopeTexture->pixelSize() != size) {
        m_scopeTarget.reset();
        m_scopeTexture.reset(m_rhi->newTexture(QRhiTexture::RGBA8, size, 1,
                                               QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_scopeTexture->create())
            return;
        m_scopeTarget.reset(m_rhi->newTextureRenderTarget({m_scopeTexture.get()}));
        if (!m_scopePass)
            m_scopePass.reset(m_scopeTarget->newCompatibleRenderPassDescriptor());
        m_scopeTarget->setRenderPassDescriptor(m_scopePass.get());
        m_scopeTarget->create();
    }
    m_lastScope = now;
    m_scopeBundle = m_bundle;
    m_scopeCanvas = canvas.size();
    m_readbackPending = true;
    m_readback = {};
    std::weak_ptr<ScopeFeed> feed = m_feed;
    m_readback.completed = [this, feed] {
        if (auto f = feed.lock())
            f->publish(m_readback.data, m_readback.pixelSize);
        m_readbackPending = false;
    };
    QRhiResourceUpdateBatch* readback = m_rhi->nextResourceUpdateBatch();
    readback->readBackTexture(QRhiReadbackDescription(m_scopeTexture.get()), &m_readback);
    m_compositor->copyCanvas(cb, m_scopeTarget.get(), readback);
    update();
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

void VideoView::setMirror(HdrMonitor* mirror)
{
    if (m_mirror == mirror)
        return;
    m_mirror = mirror;
    update(); // el monitor recibe enseguida la imagen actual
    emit mirrorChanged();
}

void VideoView::setScopesActive(bool active)
{
    if (active == m_feed->active())
        return;
    m_feed->setActive(active);
    update(); // en pausa, un fotograma nuevo para tener la primera lectura
    emit scopesActiveChanged();
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
