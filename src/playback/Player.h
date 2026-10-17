#pragma once

#include "media/MediaInfo.h"
#include "media/VideoFrame.h"
#include "playback/AudioOutput.h"

#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace movix::media {
class AudioDecoder;
class VideoDecoder;
} // namespace movix::media

namespace movix::playback {

// Reproductor de un medio: decodifica video y audio en hilos propios, usa el audio como
// reloj maestro (o un reloj de pared si no hay audio) y entrega al renderizador el fotograma
// que corresponde a cada instante, descartando los que llegan tarde.
class Player : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source NOTIFY mediaChanged)
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY mediaChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY mediaChanged)
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY mediaChanged)
    Q_PROPERTY(double duration READ duration NOTIFY mediaChanged)
    Q_PROPERTY(double frameRate READ frameRate NOTIFY mediaChanged)
    Q_PROPERTY(QVariantMap mediaInfo READ mediaInfo NOTIFY mediaChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ isMuted WRITE setMuted NOTIFY volumeChanged)
    Q_PROPERTY(QString videoDecoder READ videoDecoder NOTIFY statsChanged)
    Q_PROPERTY(QString audioDevice READ audioDevice NOTIFY statsChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY statsChanged)
    Q_PROPERTY(double presentedFps READ presentedFps NOTIFY statsChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorOccurred)

public:
    struct Options {
        bool hardwareDecoding = true;
        bool nullAudio = false; // sin dispositivo real (pruebas)
    };

    explicit Player(QObject* parent = nullptr);
    explicit Player(const Options& options, QObject* parent = nullptr);
    ~Player() override;

    Q_INVOKABLE bool open(const QUrl& url);
    bool openFile(const QString& path);
    Q_INVOKABLE void close();

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void stepFrames(int delta);

    QUrl source() const { return m_source; }
    bool hasMedia() const { return m_info.has_value(); }
    bool hasVideo() const { return m_videoDecoder != nullptr; }
    bool hasAudio() const { return m_audioDecoder != nullptr; }
    double duration() const { return toSeconds(m_duration); }
    double frameRate() const { return m_frameRate; }
    QVariantMap mediaInfo() const;
    double position() const { return toSeconds(std::min(clockUs(), m_duration)); }
    bool isPlaying() const { return m_playing.load(); }
    double volume() const { return m_volume; }
    void setVolume(double volume);
    bool isMuted() const { return m_muted; }
    void setMuted(bool muted);
    QString videoDecoder() const;
    QString audioDevice() const { return m_audioDevice; }
    int droppedFrames() const { return m_dropped.load(); }
    double presentedFps() const { return m_presentedFps; }
    QString errorString() const { return m_error; }

    // --- Seguro desde cualquier hilo (lo llama el hilo de render) ---
    // Fotograma que debe mostrarse ahora mismo según el reloj; puede ser nullptr.
    std::shared_ptr<const media::VideoFrame> frameForPresentation();
    TimeUs clockUs() const;
    TimeUs frameDurationUs() const { return m_frameDuration; }

signals:
    void mediaChanged();
    void positionChanged();
    void playingChanged();
    void volumeChanged();
    void statsChanged();
    void errorOccurred(const QString& message);
    void frameReady();   // hay un fotograma nuevo para mostrar (útil en pausa)
    void endReached();

private:
    void startThreads();
    void stopThreads();
    void videoLoop();
    void audioLoop();
    void seekInternal(TimeUs t, bool flushVideo);
    void onTick();
    void fail(const QString& message);
    void applyVolume();
    bool ensureAudioOutput();

    Options m_options;
    QUrl m_source;
    std::optional<media::MediaInfo> m_info;
    std::unique_ptr<media::VideoDecoder> m_videoDecoder;
    std::unique_ptr<media::AudioDecoder> m_audioDecoder;
    AudioOutput m_audio;
    bool m_audioStarted = false;
    QString m_audioDevice;
    QString m_error;
    TimeUs m_duration = 0;
    TimeUs m_frameDuration = 40'000;
    double m_frameRate = 0.0;
    double m_volume = 1.0;
    bool m_muted = false;

    // Reloj
    using SteadyClock = std::chrono::steady_clock;
    std::atomic<bool> m_playing{false};
    bool m_atEnd = false;                      // se llegó al final; play() vuelve a empezar
    mutable std::mutex m_clockMutex;
    TimeUs m_pausedPos = 0;                    // posición mientras está en pausa
    mutable TimeUs m_anchorPos = 0;            // reloj de pared: posición en m_anchorTime
    mutable SteadyClock::time_point m_anchorTime{};

    // Video
    static constexpr size_t kMaxQueuedFrames = 8;
    std::thread m_videoThread;
    mutable std::mutex m_videoMutex;
    std::condition_variable m_videoCv;
    std::deque<std::shared_ptr<media::VideoFrame>> m_videoQueue;
    std::shared_ptr<media::VideoFrame> m_displayed;
    uint64_t m_videoSerial = 0;
    bool m_videoSeekPending = false;
    TimeUs m_videoSeekTarget = 0;
    bool m_videoEof = false;
    bool m_quit = false;
    std::string m_videoDecoderDesc;

    // Audio
    std::thread m_audioThread;
    std::mutex m_audioMutex;
    bool m_audioSeekPending = false;
    TimeUs m_audioSeekTarget = 0;
    std::atomic<bool> m_audioQuit{false};

    // Estadísticas
    struct Diagnostics {
        SteadyClock::time_point lastCall{};
        TimeUs lastClock = 0;
        TimeUs maxRenderGap = 0;
        TimeUs maxClockStep = 0;
        int minQueue = INT_MAX;
    };
    Diagnostics m_diag; // protegido por m_videoMutex
    TimeUs m_renderInterval = 16'667; // intervalo de refresco estimado (protegido por m_videoMutex)
    QTimer m_tick;
    std::atomic<int> m_dropped{0};
    std::atomic<int> m_presented{0};
    double m_presentedFps = 0.0;
    SteadyClock::time_point m_statsTime{};
    int m_statsPresented = 0;
};

} // namespace movix::playback
