#pragma once

#include "core/Project.h"

#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

// Exportación desde la interfaz: ofrece los códecs que funcionan en este equipo, lanza la
// exportación en un hilo con una instantánea del proyecto e informa del progreso.
class ExportController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Lo crea ProjectController")
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(double speed READ speed NOTIFY progressChanged)          // fotogramas por segundo
    Q_PROPERTY(double remaining READ remaining NOTIFY progressChanged)  // segundos estimados
    Q_PROPERTY(QString bottleneck READ bottleneck NOTIFY progressChanged)

public:
    using SnapshotSource = std::function<std::shared_ptr<const movix::core::Project>()>;
    explicit ExportController(SnapshotSource source, QObject* parent = nullptr);
    ~ExportController() override;

    bool isRunning() const { return m_running; }
    double progress() const { return m_progress; }
    double speed() const { return m_speed; }
    double remaining() const { return m_remaining; }
    QString bottleneck() const { return m_bottleneck; }

    // Lista de {name, label, hardware, extension, audioCodec} de los códecs utilizables.
    Q_INVOKABLE QVariantList videoCodecs() const;
    Q_INVOKABLE QUrl suggestedFile(const QString& projectName, const QString& extension) const;
    // settings: path (url), width, height, fpsNum, fpsDen, codec, quality ("alta"/"media"/"baja"),
    // bitRateKbps (0 = calidad constante), audioCodec, audioKbps, includeAudio.
    Q_INVOKABLE void start(const QVariantMap& settings);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void revealInExplorer(const QUrl& file) const;

signals:
    void runningChanged();
    void progressChanged();
    void finished(bool ok, const QString& message, const QUrl& file);

private:
    SnapshotSource m_source;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    bool m_running = false;
    double m_progress = 0;
    double m_speed = 0;
    double m_remaining = 0;
    QString m_bottleneck;
};
