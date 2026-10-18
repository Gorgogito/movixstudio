#pragma once

#include "core/Project.h"
#include "engine/Sources.h"
#include "playback/AudioOutput.h"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace movix::playback {

// Reproductor de la línea de tiempo.
//
// Trabaja sobre una instantánea inmutable del proyecto (se sustituye en cada edición sin perder
// la posición). Un hilo compone los fotogramas por adelantado (decodificando cada capa) y otro
// mezcla el audio; el audio es el reloj maestro y siempre existe (silencio donde no hay clips).
// El renderizador pide en cada refresco el fotograma que corresponde al reloj.
class TimelinePlayer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 positionFrame READ positionFrame NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qint64 durationFrames READ durationFrames NOTIFY durationChanged)
    Q_PROPERTY(double frameRate READ frameRate NOTIFY durationChanged)
    Q_PROPERTY(bool hasContent READ hasContent NOTIFY durationChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ isMuted WRITE setMuted NOTIFY volumeChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY statsChanged)
    Q_PROPERTY(double presentedFps READ presentedFps NOTIFY statsChanged)
    Q_PROPERTY(QString audioDevice READ audioDevice NOTIFY statsChanged)

public:
    struct Options {
        bool hardwareDecoding = true;
        bool nullAudio = false; // dispositivo virtual (pruebas)
    };

    explicit TimelinePlayer(QObject* parent = nullptr);
    explicit TimelinePlayer(const Options& options, QObject* parent = nullptr);
    ~TimelinePlayer() override;

    // Sustituye el proyecto; conserva la posición (limitada a la nueva duración).
    void setProject(std::shared_ptr<const core::Project> project);
    std::shared_ptr<const core::Project> project() const;

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void seekFrame(qint64 frame);
    Q_INVOKABLE void stepFrames(int delta);

    bool isPlaying() const { return m_playing.load(); }
    double position() const { return toSeconds(std::min(clockUs(), m_durationTime.load())); }
    qint64 positionFrame() const;
    double duration() const { return toSeconds(m_durationTime.load()); }
    qint64 durationFrames() const { return m_durationFrames.load(); }
    double frameRate() const;
    bool hasContent() const { return m_durationFrames.load() > 0; }
    double volume() const { return m_volume; }
    void setVolume(double volume);
    bool isMuted() const { return m_muted; }
    void setMuted(bool muted);
    int droppedFrames() const { return m_dropped.load(); }
    double presentedFps() const { return m_presentedFps; }
    QString audioDevice() const { return m_audioDevice; }

    // --- Seguro desde cualquier hilo (lo llama el hilo de render) ---
    std::shared_ptr<const engine::FrameBundle> frameForPresentation();
    TimeUs clockUs() const;

signals:
    void playingChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void statsChanged();
    void frameReady();
    void endReached();

private:
    void startThreads();
    void stopThreads();
    void videoLoop();
    void audioLoop();
    void seekInternal(TimeUs t, bool flushVideo);
    void onTick();
    void applyVolume();
    core::FramePos frameAt(TimeUs t) const;

    using SteadyClock = std::chrono::steady_clock;
    Options m_options;
    AudioOutput m_audio;
    QString m_audioDevice;
    double m_volume = 1.0;
    bool m_muted = false;
    int m_sampleRate = 48000;

    // Proyecto (protegido por m_projectMutex; los hilos copian el puntero)
    mutable std::mutex m_projectMutex;
    std::shared_ptr<const core::Project> m_project;
    std::atomic<TimeUs> m_durationTime{0};
    std::atomic<core::FramePos> m_durationFrames{0};

    // Reloj
    std::atomic<bool> m_playing{false};
    bool m_atEnd = false;
    mutable std::mutex m_clockMutex;
    TimeUs m_pausedPos = 0;
    mutable TimeUs m_anchorPos = 0;                  // reloj de pared de respaldo
    mutable SteadyClock::time_point m_anchorTime{};
    TimeUs liveClockLocked() const;                  // requiere m_clockMutex

    // Video
    static constexpr size_t kMaxQueued = 6;
    std::thread m_videoThread;
    mutable std::mutex m_videoMutex;
    std::condition_variable m_videoCv;
    std::deque<std::shared_ptr<engine::FrameBundle>> m_videoQueue;
    std::shared_ptr<engine::FrameBundle> m_displayed;
    uint64_t m_videoSerial = 0;
    bool m_videoSeekPending = false;
    core::FramePos m_videoSeekFrame = 0;
    core::FramePos m_nextFrame = 0;
    bool m_videoEof = false;
    bool m_quit = false;
    SteadyClock::time_point m_lastPresent{};
    TimeUs m_renderInterval = 16'667;

    // Audio
    std::thread m_audioThread;
    std::mutex m_audioMutex;
    bool m_audioSeekPending = false;
    TimeUs m_audioSeekTarget = 0;
    std::atomic<bool> m_audioQuit{false};

    // Estadísticas
    QTimer m_tick;
    std::atomic<int> m_dropped{0};
    std::atomic<int> m_presented{0};
    double m_presentedFps = 0.0;
    SteadyClock::time_point m_statsTime{};
    int m_statsPresented = 0;
};

} // namespace movix::playback
