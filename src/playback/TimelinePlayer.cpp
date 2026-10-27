#include "playback/TimelinePlayer.h"

#include "render/MaskRenderer.h"
#include "render/TitleRenderer.h"

#include <QLoggingCategory>

#include <algorithm>

Q_LOGGING_CATEGORY(lcTimelinePlayer, "movix.player")
// Estadísticas por segundo; activar con QT_LOGGING_RULES="movix.player.stats.debug=true".
Q_LOGGING_CATEGORY(lcPlayerStats, "movix.player.stats", QtWarningMsg)

namespace movix::playback {

namespace {
constexpr int kChannels = 2;
constexpr int kAudioBlock = 1024;
// Audio mezclado por adelantado. Poco, para que las ediciones se oigan enseguida.
constexpr TimeUs kAudioAhead = 200'000;
} // namespace

TimelinePlayer::TimelinePlayer(QObject* parent) : TimelinePlayer(Options{}, parent) {}

TimelinePlayer::TimelinePlayer(const Options& options, QObject* parent) : QObject(parent), m_options(options)
{
    AudioOutput::Config cfg;
    cfg.sampleRate = m_sampleRate;
    cfg.channels = kChannels;
    cfg.nullBackend = options.nullAudio;
    std::string err;
    if (m_audio.start(cfg, &err))
        m_audioDevice = QString::fromStdString(m_audio.deviceName());
    else
        qCWarning(lcTimelinePlayer) << "Sin salida de audio; se usa el reloj del sistema:" << err.c_str();
    m_audio.reset(0);

    m_tick.setInterval(33);
    connect(&m_tick, &QTimer::timeout, this, &TimelinePlayer::onTick);
    m_tick.start();
    m_statsTime = SteadyClock::now();
    startThreads();
}

TimelinePlayer::~TimelinePlayer()
{
    stopThreads();
    m_audio.stop();
}

void TimelinePlayer::startThreads()
{
    m_videoThread = std::thread(&TimelinePlayer::videoLoop, this);
    m_audioThread = std::thread(&TimelinePlayer::audioLoop, this);
}

void TimelinePlayer::stopThreads()
{
    {
        std::lock_guard lock(m_videoMutex);
        m_quit = true;
    }
    m_videoCv.notify_all();
    m_audioQuit = true;
    if (m_videoThread.joinable())
        m_videoThread.join();
    if (m_audioThread.joinable())
        m_audioThread.join();
}

std::shared_ptr<const core::Project> TimelinePlayer::project() const
{
    std::lock_guard lock(m_projectMutex);
    return m_project;
}

void TimelinePlayer::setProject(std::shared_ptr<const core::Project> project)
{
    const TimeUs oldDuration = m_durationTime.load();
    const core::FramePos oldFrames = m_durationFrames.load();
    {
        std::lock_guard lock(m_projectMutex);
        m_project = std::move(project);
        m_durationFrames = m_project ? m_project->duration() : 0;
        m_durationTime = m_project ? m_project->durationTime() : 0;
    }
    // Lo ya compuesto y mezclado corresponde al proyecto anterior: se rehace desde la posición actual.
    const TimeUs position = std::min(clockUs(), m_durationTime.load());
    seekInternal(position, true);
    if (m_durationTime.load() != oldDuration || m_durationFrames.load() != oldFrames)
        emit durationChanged();
}

core::FramePos TimelinePlayer::frameAt(TimeUs t) const
{
    auto p = project();
    return p ? p->settings.timeToFrame(t) : 0;
}

qint64 TimelinePlayer::positionFrame() const
{
    return frameAt(std::min(clockUs(), m_durationTime.load()));
}

double TimelinePlayer::frameRate() const
{
    auto p = project();
    return p ? p->settings.frameRate.value() : 30.0;
}

void TimelinePlayer::setPathMapper(std::function<std::string(const core::MediaItem&)> mapper)
{
    {
        std::lock_guard lock(m_videoMutex);
        m_pathMapper = std::move(mapper);
        ++m_pathMapperVersion;
    }
    m_videoCv.notify_all();
}

void TimelinePlayer::setGpuDevice(void* device)
{
    {
        std::lock_guard lock(m_videoMutex);
        if (m_gpuDevice == device)
            return;
        m_gpuDevice = device;
        // Los fotogramas compuestos son del dispositivo anterior: se componen otra vez desde el que
        // se muestra (en pausa, el monitor HDR necesita ese mismo en memoria).
        m_videoSeekFrame = m_displayed ? m_displayed->frame : m_videoQueue.empty() ? m_nextFrame : m_videoQueue.front()->frame;
        m_videoQueue.clear();
        ++m_videoSerial;
        m_videoSeekPending = true;
        m_videoEof = false;
    }
    m_videoCv.notify_all();
}

void TimelinePlayer::videoLoop()
{
    engine::VideoSources sources(m_options.hardwareDecoding);
    uint64_t mapperVersion = 0;
    sources.setTitleRenderer(std::make_shared<render::QtTitleRenderer>());
    sources.setMaskRenderer(std::make_shared<render::QtMaskRenderer>());
    for (;;) {
        std::unique_lock lock(m_videoMutex);
        m_videoCv.wait(lock, [this] {
            return m_quit || m_videoSeekPending || (!m_videoEof && m_videoQueue.size() < kMaxQueued);
        });
        if (m_quit)
            return;
        if (m_videoSeekPending) {
            m_nextFrame = m_videoSeekFrame;
            m_videoSeekPending = false;
            m_videoEof = false;
        }
        const core::FramePos k = m_nextFrame;
        const uint64_t serial = m_videoSerial;
        if (mapperVersion != m_pathMapperVersion) {
            sources.setPathMapper(m_pathMapper);
            mapperVersion = m_pathMapperVersion;
        }
        sources.setGpuDevice(m_gpuDevice);
        lock.unlock();

        const auto proj = project();
        if (!proj || k >= proj->duration()) {
            lock.lock();
            if (serial == m_videoSerial && !m_videoSeekPending)
                m_videoEof = true;
            continue;
        }
        auto bundle = std::make_shared<engine::FrameBundle>(sources.build(*proj, k));

        lock.lock();
        if (serial != m_videoSerial || m_videoSeekPending)
            continue; // hubo una búsqueda o una edición mientras se componía
        m_videoQueue.push_back(std::move(bundle));
        m_nextFrame = k + 1;
        lock.unlock();
        QMetaObject::invokeMethod(this, &TimelinePlayer::frameReady, Qt::QueuedConnection);
    }
}

void TimelinePlayer::audioLoop()
{
    engine::AudioMixer mixer(m_sampleRate, kChannels);
    std::vector<float> block(size_t(kAudioBlock) * kChannels);
    int64_t samplePos = 0;
    uint64_t generation = m_audio.generation();
    bool endSignalled = false;

    while (!m_audioQuit) {
        {
            std::lock_guard lock(m_audioMutex);
            if (m_audioSeekPending) {
                m_audioSeekPending = false;
                samplePos = m_audioSeekTarget * m_sampleRate / kUsPerSecond;
                generation = m_audio.generation();
                endSignalled = false;
            }
        }
        const auto proj = project();
        const int64_t endSample = proj ? proj->durationTime() * m_sampleRate / kUsPerSecond : 0;
        if (samplePos >= endSample) {
            if (!endSignalled) {
                m_audio.setEndOfStream(generation);
                endSignalled = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (m_audio.buffered() > kAudioAhead) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const int n = int(std::min<int64_t>(kAudioBlock, endSample - samplePos));
        mixer.mix(*proj, samplePos, n, block.data());
        for (size_t i = 0; i < size_t(n) * kChannels; ++i)
            block[i] = std::clamp(block[i], -1.0f, 1.0f);

        int offset = 0;
        bool stale = false;
        while (offset < n && !m_audioQuit) {
            const int w = m_audio.write(block.data() + size_t(offset) * kChannels, n - offset,
                                        (samplePos + offset) * kUsPerSecond / m_sampleRate, generation);
            if (w < 0) {
                stale = true; // la salida se reinició (búsqueda/edición): se descarta el bloque
                break;
            }
            offset += w;
            if (w == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!stale)
            samplePos += n;
    }
}

TimeUs TimelinePlayer::clockUs() const
{
    std::lock_guard lock(m_clockMutex);
    return liveClockLocked();
}

TimeUs TimelinePlayer::liveClockLocked() const
{
    if (!m_playing.load())
        return m_pausedPos;
    const auto now = SteadyClock::now();
    // El audio manda mientras suena. Sin dispositivo, o cuando el audio ya terminó (su reloj se
    // detiene justo antes del final por la latencia), sigue un reloj de pared desde el último valor.
    if (m_audio.isRunning() && !m_audio.drained()) {
        const TimeUs t = m_audio.clock();
        m_anchorPos = t;
        m_anchorTime = now;
        return t;
    }
    return m_anchorPos + std::chrono::duration_cast<std::chrono::microseconds>(now - m_anchorTime).count();
}

std::shared_ptr<const engine::FrameBundle> TimelinePlayer::displayedFrame()
{
    std::lock_guard lock(m_videoMutex);
    return m_displayed;
}

std::shared_ptr<const engine::FrameBundle> TimelinePlayer::frameForPresentation()
{
    const TimeUs t = clockUs();
    const bool playing = m_playing.load();
    const auto now = SteadyClock::now();
    std::lock_guard lock(m_videoMutex);

    // Tolerancia de medio refresco (ver la explicación en el diseño del reproductor de F0):
    // evita saltos cuando la cadencia del video coincide con la de la pantalla.
    TimeUs tolerance = 0;
    if (playing) {
        if (m_lastPresent != SteadyClock::time_point{}) {
            const TimeUs gap = std::chrono::duration_cast<std::chrono::microseconds>(now - m_lastPresent).count();
            if (gap > 2'000 && gap < 100'000)
                m_renderInterval = (m_renderInterval * 15 + gap) / 16;
        }
        m_lastPresent = now;
        const auto proj = project();
        const TimeUs frameDur = proj ? proj->settings.frameDuration() : 33'333;
        tolerance = std::min(frameDur, m_renderInterval) / 2;
    } else {
        m_lastPresent = {};
    }

    int advanced = 0;
    while (!m_videoQueue.empty() && m_videoQueue.front()->time <= t + tolerance) {
        m_displayed = std::move(m_videoQueue.front());
        m_videoQueue.pop_front();
        ++advanced;
    }
    if (advanced > 0) {
        m_presented += 1;
        if (advanced > 1 && playing)
            m_dropped += advanced - 1;
        m_videoCv.notify_one();
    }
    if (!m_displayed || (!hasContent() && !m_displayed->layers.empty())) {
        // Sin nada que mostrar: lienzo vacío del tamaño del proyecto.
        auto blank = std::make_shared<engine::FrameBundle>();
        if (const auto proj = project()) {
            blank->canvasWidth = proj->settings.width;
            blank->canvasHeight = proj->settings.height;
        }
        m_displayed = std::move(blank);
    }
    return m_displayed;
}

void TimelinePlayer::play()
{
    if (!hasContent() || m_playing.load())
        return;
    if (m_atEnd || m_pausedPos >= m_durationTime.load())
        seekInternal(0, true);
    m_audio.setPlaying(true);
    {
        std::lock_guard lock(m_clockMutex);
        m_anchorPos = m_pausedPos;
        m_anchorTime = SteadyClock::now();
        m_playing = true;
    }
    emit playingChanged();
}

void TimelinePlayer::pause()
{
    if (!m_playing.load())
        return;
    {
        std::lock_guard lock(m_clockMutex);
        m_pausedPos = std::min(liveClockLocked(), m_durationTime.load());
        m_playing = false;
    }
    m_audio.setPlaying(false);
    emit playingChanged();
    emit positionChanged();
}

void TimelinePlayer::togglePlayPause()
{
    if (m_playing.load())
        pause();
    else
        play();
}

void TimelinePlayer::stop()
{
    pause();
    seekInternal(0, true);
}

void TimelinePlayer::seek(double seconds)
{
    seekInternal(fromSeconds(seconds), true);
}

void TimelinePlayer::seekFrame(qint64 frame)
{
    auto p = project();
    if (p)
        seekInternal(p->settings.frameToTime(frame), true);
}

void TimelinePlayer::stepFrames(int delta)
{
    auto p = project();
    if (!p || !hasContent() || delta == 0)
        return;
    pause();
    core::FramePos current;
    bool nextQueued = false;
    {
        std::lock_guard lock(m_videoMutex);
        current = m_displayed && !m_displayed->layers.empty() ? m_displayed->frame : frameAt(m_pausedPos);
        nextQueued = delta == 1 && !m_videoQueue.empty() && m_videoQueue.front()->frame == current + 1;
    }
    const core::FramePos target = std::clamp<core::FramePos>(current + delta, 0, m_durationFrames.load() - 1);
    // Si el siguiente fotograma ya está compuesto, basta con mover el reloj.
    seekInternal(p->settings.frameToTime(target), !nextQueued);
}

void TimelinePlayer::seekInternal(TimeUs t, bool flushVideo)
{
    const TimeUs duration = m_durationTime.load();
    t = std::clamp<TimeUs>(t, 0, duration);
    m_atEnd = false;
    core::FramePos frame = frameAt(t);
    if (m_durationFrames.load() > 0)
        frame = std::min<core::FramePos>(frame, m_durationFrames.load() - 1); // al final se muestra el último

    {
        std::lock_guard lock(m_clockMutex);
        m_pausedPos = t;
        m_anchorPos = t;
        m_anchorTime = SteadyClock::now();
        // El reloj de audio se reinicia en `t`; mientras llega el audio nuevo, se queda quieto.
        m_audio.reset(t);
    }
    {
        std::lock_guard lock(m_audioMutex);
        m_audioSeekPending = true;
        m_audioSeekTarget = t;
    }
    if (flushVideo) {
        std::lock_guard lock(m_videoMutex);
        m_videoQueue.clear();
        ++m_videoSerial;
        m_videoSeekPending = true;
        m_videoSeekFrame = frame;
        m_videoEof = false;
    }
    m_videoCv.notify_one();
    emit positionChanged();
    emit frameReady();
}

void TimelinePlayer::onTick()
{
    if (m_playing.load()) {
        const TimeUs t = clockUs();
        if (t >= m_durationTime.load()) {
            {
                std::lock_guard lock(m_clockMutex);
                m_playing = false;
                m_pausedPos = m_durationTime.load();
            }
            m_audio.setPlaying(false);
            m_atEnd = true;
            emit playingChanged();
            emit endReached();
        }
        emit positionChanged();
    }

    const auto now = SteadyClock::now();
    const double elapsed = std::chrono::duration<double>(now - m_statsTime).count();
    if (elapsed >= 1.0) {
        const int presented = m_presented.load();
        m_presentedFps = (presented - m_statsPresented) / elapsed;
        m_statsPresented = presented;
        m_statsTime = now;
        qCDebug(lcPlayerStats, "pos=%.3f s mostrados/s=%.1f descartados=%d audio_en_buffer=%.0f ms", toSeconds(clockUs()),
                m_presentedFps, m_dropped.load(), toSeconds(m_audio.buffered()) * 1000.0);
        emit statsChanged();
    }
}

void TimelinePlayer::setVolume(double volume)
{
    volume = std::clamp(volume, 0.0, 1.0);
    if (qFuzzyCompare(volume, m_volume))
        return;
    m_volume = volume;
    applyVolume();
    emit volumeChanged();
}

void TimelinePlayer::setMuted(bool muted)
{
    if (muted == m_muted)
        return;
    m_muted = muted;
    applyVolume();
    emit volumeChanged();
}

void TimelinePlayer::applyVolume()
{
    // Curva cuadrática: el control lineal del deslizador se percibe más natural.
    m_audio.setVolume(m_muted ? 0.0f : static_cast<float>(m_volume * m_volume));
}

} // namespace movix::playback
