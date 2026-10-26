#pragma once

#include "engine/Sources.h"

#include <QObject>
#include <QSize>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

class QWindow;
class HdrMonitorWindow;

// Monitor HDR: una ventana aparte (o a pantalla completa en otra pantalla) con la misma imagen que
// el visor, pero con una cadena de intercambio en coma flotante (scRGB) cuando la pantalla tiene el
// HDR activado, así que los proyectos HDR se ven con sus nits reales. La interfaz de Qt Quick no
// gestiona el color y no puede vivir en una ventana HDR; por eso el monitor es una ventana propia.
// Recibe los fotogramas que presenta el visor (VideoView::mirror) y los dibuja en un hilo propio con
// su propio dispositivo gráfico, para que la espera a la sincronía vertical no frene la interfaz.
// En una pantalla sin HDR muestra la conversión a SDR, como el visor.
class HdrMonitor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)
    Q_PROPERTY(bool fullScreen READ isFullScreen NOTIFY openChanged)
    Q_PROPERTY(bool hdrActive READ hdrActive NOTIFY displayChanged)
    Q_PROPERTY(double peakNits READ peakNits NOTIFY displayChanged)
    Q_PROPERTY(double sdrWhiteNits READ sdrWhiteNits NOTIFY displayChanged)
    Q_PROPERTY(QStringList screens READ screens NOTIFY screensChanged)

public:
    explicit HdrMonitor(QObject* parent = nullptr);
    ~HdrMonitor() override;

    bool available() const;
    bool isOpen() const;
    bool isFullScreen() const;
    bool hdrActive() const { return m_hdrActive; }
    double peakNits() const { return m_peakNits; }
    double sdrWhiteNits() const { return m_sdrWhiteNits; }
    QStringList screens() const;

    // Abre el monitor en una ventana normal, o a pantalla completa en la pantalla `index` de `screens`.
    Q_INVOKABLE void showWindowed();
    Q_INVOKABLE void showFullScreen(int screenIndex);
    Q_INVOKABLE void close();

    // Fotograma que acaba de presentar el visor. Se llama desde el hilo de render de Qt Quick.
    void present(std::shared_ptr<const movix::engine::FrameBundle> bundle);

signals:
    void openChanged();
    void displayChanged();
    void screensChanged();

private:
    friend class HdrMonitorWindow;
    void ensureWindow();
    void startRenderer();
    void stopRenderer();
    void requestFrame();
    void setExposed(bool exposed);
    void renderLoop();
    void publishDisplay(bool hdr, double peak, double sdrWhite);

    std::unique_ptr<HdrMonitorWindow> m_window;
    bool m_hdrActive = false;
    double m_peakNits = 0.0;
    double m_sdrWhiteNits = 0.0;

    // Estado compartido con el hilo de dibujo.
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::shared_ptr<const movix::engine::FrameBundle> m_bundle;
    bool m_dirty = false;
    bool m_exposed = false;
    bool m_stop = false;
    std::thread m_thread;
};
