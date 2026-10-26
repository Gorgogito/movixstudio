#include "HdrMonitor.h"

#include "render/Compositor.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QWindow>
#include <rhi/qrhi.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
const QColor kBackground(8, 9, 12); // el mismo fondo que el visor
constexpr auto kDisplayPoll = std::chrono::seconds(1); // la ventana puede pasar a otra pantalla
}

// Ventana nativa del monitor: solo transmite al HdrMonitor lo que le pasa (visible, tamaño, cierre)
// y atiende el teclado y el doble clic para la pantalla completa.
class HdrMonitorWindow : public QWindow {
public:
    explicit HdrMonitorWindow(HdrMonitor* monitor) : m_monitor(monitor)
    {
#ifdef Q_OS_WIN
        setSurfaceType(QSurface::Direct3DSurface);
#endif
        setTitle(QCoreApplication::translate("HdrMonitor", "MovixStudio — Monitor HDR"));
        resize(960, 540);
        setMinimumSize(QSize(160, 90));
    }

protected:
    void exposeEvent(QExposeEvent*) override
    {
        if (isExposed())
            m_monitor->startRenderer();
        m_monitor->setExposed(isExposed());
    }
    void resizeEvent(QResizeEvent*) override { m_monitor->requestFrame(); }
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::PlatformSurface
            && static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            m_monitor->stopRenderer(); // la cadena de intercambio debe irse antes que la ventana nativa
        const bool handled = QWindow::event(e);
        if (e->type() == QEvent::Close || e->type() == QEvent::Hide || e->type() == QEvent::Show
            || e->type() == QEvent::WindowStateChange)
            emit m_monitor->openChanged();
        return handled;
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape) {
            if (windowStates() & Qt::WindowFullScreen)
                showNormal();
            else
                close();
        } else if (e->key() == Qt::Key_F11) {
            toggleFullScreen();
        }
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { toggleFullScreen(); }

private:
    void toggleFullScreen()
    {
        if (windowStates() & Qt::WindowFullScreen)
            showNormal();
        else
            QWindow::showFullScreen();
    }

    HdrMonitor* m_monitor;
};

HdrMonitor::HdrMonitor(QObject* parent) : QObject(parent)
{
    connect(qApp, &QGuiApplication::screenAdded, this, &HdrMonitor::screensChanged);
    connect(qApp, &QGuiApplication::screenRemoved, this, &HdrMonitor::screensChanged);
}

HdrMonitor::~HdrMonitor()
{
    stopRenderer();
    m_window.reset();
}

bool HdrMonitor::available() const
{
#ifdef Q_OS_WIN
    return true;
#else
    return false; // por ahora solo con Direct3D 11
#endif
}

bool HdrMonitor::isOpen() const
{
    return m_window && m_window->isVisible();
}

bool HdrMonitor::isFullScreen() const
{
    return isOpen() && (m_window->windowStates() & Qt::WindowFullScreen);
}

QStringList HdrMonitor::screens() const
{
    QStringList names;
    const auto list = QGuiApplication::screens();
    for (int i = 0; i < list.size(); ++i) {
        const QScreen* s = list[i];
        names << QStringLiteral("%1. %2 (%3×%4)").arg(i + 1).arg(s->name()).arg(s->size().width()).arg(s->size().height());
    }
    return names;
}

void HdrMonitor::ensureWindow()
{
    if (!m_window)
        m_window = std::make_unique<HdrMonitorWindow>(this);
}

void HdrMonitor::showWindowed()
{
    if (!available())
        return;
    ensureWindow();
    m_window->showNormal();
    m_window->requestActivate();
    emit openChanged();
}

void HdrMonitor::showFullScreen(int screenIndex)
{
    if (!available())
        return;
    const auto list = QGuiApplication::screens();
    if (screenIndex < 0 || screenIndex >= list.size())
        return;
    ensureWindow();
    QScreen* screen = list[screenIndex];
    if (m_window->screen() != screen) {
        if (m_window->windowStates() & Qt::WindowFullScreen)
            m_window->showNormal();
        m_window->setScreen(screen);
    }
    m_window->setGeometry(screen->geometry());
    m_window->showFullScreen();
    emit openChanged();
}

void HdrMonitor::close()
{
    if (m_window)
        m_window->close();
    emit openChanged();
}

void HdrMonitor::present(std::shared_ptr<const movix::engine::FrameBundle> bundle)
{
    std::lock_guard lock(m_mutex);
    if (bundle == m_bundle)
        return;
    m_bundle = std::move(bundle);
    m_dirty = true;
    m_cv.notify_one();
}

void HdrMonitor::requestFrame()
{
    std::lock_guard lock(m_mutex);
    m_dirty = true;
    m_cv.notify_one();
}

void HdrMonitor::setExposed(bool exposed)
{
    std::lock_guard lock(m_mutex);
    m_exposed = exposed;
    m_dirty = true;
    m_cv.notify_one();
}

void HdrMonitor::startRenderer()
{
    if (m_thread.joinable())
        return;
    {
        std::lock_guard lock(m_mutex);
        m_stop = false;
        m_dirty = true;
    }
    m_thread = std::thread([this] { renderLoop(); });
}

void HdrMonitor::stopRenderer()
{
    if (!m_thread.joinable())
        return;
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_exposed = false;
        m_cv.notify_one();
    }
    m_thread.join();
}

void HdrMonitor::publishDisplay(bool hdr, double peak, double sdrWhite)
{
    QMetaObject::invokeMethod(this, [this, hdr, peak, sdrWhite] {
        if (hdr == m_hdrActive && peak == m_peakNits && sdrWhite == m_sdrWhiteNits)
            return;
        m_hdrActive = hdr;
        m_peakNits = peak;
        m_sdrWhiteNits = sdrWhite;
        emit displayChanged();
    }, Qt::QueuedConnection);
}

// Hilo de dibujo: crea su dispositivo y la cadena de intercambio de la ventana, y dibuja cada
// fotograma nuevo (o tras un cambio de tamaño). La presentación espera a la sincronía vertical.
void HdrMonitor::renderLoop()
{
    std::unique_ptr<QRhi> rhi;
#ifdef Q_OS_WIN
    QRhiD3D11InitParams params;
    rhi.reset(QRhi::create(QRhi::D3D11, &params));
#endif
    if (!rhi) {
        qWarning("Monitor HDR: no se pudo inicializar la GPU");
        return;
    }
    std::unique_ptr<QRhiSwapChain> swapChain(rhi->newSwapChain());
    std::unique_ptr<QRhiRenderPassDescriptor> renderPass;
    auto compositor = std::make_unique<movix::render::Compositor>(rhi.get());
    swapChain->setWindow(m_window.get());
    bool hdr = false;
    float peak = 1000.0f;
    float sdrWhite = 200.0f;

    // (Re)crea la cadena de intercambio en el formato que admite la pantalla en la que está ahora.
    auto setupSwapChain = [&] {
        const bool wantHdr = swapChain->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear);
        if (renderPass && wantHdr == hdr)
            return;
        hdr = wantHdr;
        swapChain->destroy();
        swapChain->setFormat(hdr ? QRhiSwapChain::HDRExtendedSrgbLinear : QRhiSwapChain::SDR);
        std::unique_ptr<QRhiRenderPassDescriptor> pass(swapChain->newCompatibleRenderPassDescriptor());
        swapChain->setRenderPassDescriptor(pass.get());
        swapChain->createOrResize();
        compositor->setRenderTarget(pass.get(), 1);
        renderPass = std::move(pass);
    };
    auto readDisplay = [&] {
        const QRhiSwapChainHdrInfo info = swapChain->hdrInfo();
        peak = 1000.0f;
        sdrWhite = 200.0f;
        if (info.limitsType == QRhiSwapChainHdrInfo::LuminanceInNits && info.limits.luminanceInNits.maxLuminance > 0.0f)
            peak = info.limits.luminanceInNits.maxLuminance;
        else if (info.limitsType == QRhiSwapChainHdrInfo::ColorComponentValue
                 && info.limits.colorComponentValue.maxColorComponentValue > 0.0f)
            peak = info.limits.colorComponentValue.maxColorComponentValue * 80.0f;
        if (info.sdrWhiteLevel > 0.0f)
            sdrWhite = info.sdrWhiteLevel;
        peak = std::max(peak, sdrWhite);
        publishDisplay(hdr, hdr ? peak : 0.0, hdr ? sdrWhite : 0.0);
    };
    setupSwapChain();
    readDisplay();
    auto lastPoll = std::chrono::steady_clock::now();

    while (true) {
        std::shared_ptr<const movix::engine::FrameBundle> bundle;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, kDisplayPoll, [this] { return m_stop || (m_exposed && m_dirty); });
            if (m_stop)
                break;
            const bool draw = m_exposed && m_dirty;
            const auto now = std::chrono::steady_clock::now();
            if (!draw && now - lastPoll < kDisplayPoll)
                continue;
            m_dirty = false;
            bundle = m_bundle;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPoll >= kDisplayPoll) {
            lastPoll = now;
            const bool wasHdr = hdr;
            setupSwapChain();
            readDisplay();
            if (hdr != wasHdr)
                requestFrame();
        }
        if (swapChain->currentPixelSize() != swapChain->surfacePixelSize() || swapChain->currentPixelSize().isEmpty()) {
            if (!swapChain->createOrResize())
                continue;
        }
        if (swapChain->currentPixelSize().isEmpty())
            continue;
        const QRhi::FrameOpResult begin = rhi->beginFrame(swapChain.get());
        if (begin == QRhi::FrameOpSwapChainOutOfDate) {
            swapChain->createOrResize();
            requestFrame();
            continue;
        }
        if (begin != QRhi::FrameOpSuccess)
            continue;

        static const movix::engine::FrameBundle kEmpty;
        const movix::engine::FrameBundle& b = bundle ? *bundle : kEmpty;
        const QSize target = swapChain->currentPixelSize();
        QRectF canvas;
        if (bundle && b.canvasHeight > 0)
            canvas = movix::render::Compositor::fitRect(target, double(b.canvasWidth) / b.canvasHeight);
        compositor->setSdrDisplay(true);
        compositor->setHdrDisplay(hdr, sdrWhite, peak);
        QColor clear = kBackground;
        if (hdr) { // el fondo en luz lineal, con el brillo del blanco SDR
            auto lin = [&](float c) { return std::pow(c, 2.2f) * sdrWhite / 80.0f; };
            clear = QColor::fromRgbF(lin(kBackground.redF()), lin(kBackground.greenF()), lin(kBackground.blueF()));
        }
        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        compositor->prepare(b, updates, target, canvas);
        compositor->render(swapChain->currentFrameCommandBuffer(), swapChain->currentFrameRenderTarget(), clear, updates);
        rhi->endFrame(swapChain.get());
    }

    compositor.reset();
    swapChain->destroy();
    swapChain.reset();
    renderPass.reset();
    rhi.reset();
}
