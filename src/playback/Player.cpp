#include "playback/Player.h"

#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"

#include <QLoggingCategory>

#include <algorithm>

Q_LOGGING_CATEGORY(lcPlayer, "movix.player")
// Estadísticas por segundo; activar con QT_LOGGING_RULES="movix.player.stats.debug=true".
Q_LOGGING_CATEGORY(lcStats, "movix.player.stats", QtWarningMsg)

namespace movix::playback {

namespace {
constexpr TimeUs kAudioTargetBuffer = 500'000; // mantener ~0,5 s de audio decodificado
constexpr int kOutputRate = 48000;
constexpr int kOutputChannels = 2;
} // namespace

Player::Player(QObject* parent) : Player(Options{}, parent) {}

Player::Player(const Options& options, QObject* parent) : QObject(parent), m_options(options)
{
    m_tick.setInterval(33);
    connect(&m_tick, &QTimer::timeout, this, &Player::onTick);
}

Player::~Player()
{
    close();
    m_audio.stop();
}

bool Player::open(const QUrl& url)
{
    return openFile(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

bool Player::openFile(const QString& path)
{
    close();
    const std::string utf8 = path.toStdString();

    std::string err;
    auto info = media::probe(utf8, &err);
    if (!info) {
        fail(QString::fromStdString(err));
        return false;
    }

    if (info->video) {
        auto dec = std::make_unique<media::VideoDecoder>();
        if (dec->open(utf8, media::VideoDecoderOptions{m_options.hardwareDecoding}, &err)) {
            m_frameDuration = dec->frameDuration();
            m_frameRate = dec->frameRate();
            m_videoDecoder = std::move(dec);
        } else {
            qCWarning(lcPlayer) << "Video no disponible:" << err.c_str();
        }
    }
    if (info->audio && ensureAudioOutput()) {
        auto dec = std::make_unique<media::AudioDecoder>();
        if (dec->open(utf8, m_audio.sampleRate(), m_audio.channels(), &err))
            m_audioDecoder = std::move(dec);
        else
            qCWarning(lcPlayer) << "Audio no disponible:" << err.c_str();
    }
    if (!m_videoDecoder && !m_audioDecoder) {
        fail(tr("No se pudo decodificar ningún stream del archivo."));
        return false;
    }

    m_info = std::move(info);
    m_source = QUrl::fromLocalFile(path);
    m_duration = m_info->duration;
    if (m_duration <= 0)
        m_duration = std::max(m_videoDecoder ? m_videoDecoder->duration() : 0, m_audioDecoder ? m_audioDecoder->duration() : 0);
    if (!m_videoDecoder) {
        m_frameRate = 0.0;
        m_frameDuration = 40'000;
    }

    m_dropped = 0;
    m_presented = 0;
    m_statsPresented = 0;
    m_presentedFps = 0.0;
    m_statsTime = SteadyClock::now();
    m_atEnd = false;
    {
        std::lock_guard lock(m_clockMutex);
        m_pausedPos = 0;
    }
    m_audio.setPlaying(false);
    m_audio.reset(0);
    applyVolume();

    startThreads();
    m_tick.start();
    m_error.clear();
    emit mediaChanged();
    emit positionChanged();
    emit statsChanged();
    return true;
}

void Player::close()
{
    const bool wasPlaying = m_playing.exchange(false);
    stopThreads();
    m_tick.stop();
    m_audio.setPlaying(false);
    m_audio.reset(0);
    m_videoDecoder.reset();
    m_audioDecoder.reset();
    {
        std::lock_guard lock(m_videoMutex);
        m_videoQueue.clear();
        m_displayed.reset();
        m_videoDecoderDesc.clear();
    }
    const bool hadMedia = m_info.has_value();
    m_info.reset();
    m_source.clear();
    m_duration = 0;
    {
        std::lock_guard lock(m_clockMutex);
        m_pausedPos = 0;
    }
    if (wasPlaying)
        emit playingChanged();
    if (hadMedia) {
        emit mediaChanged();
        emit positionChanged();
        emit frameReady();
    }
}

bool Player::ensureAudioOutput()
{
    if (m_audioStarted)
        return true;
    std::string err;
    AudioOutput::Config cfg;
    cfg.sampleRate = kOutputRate;
    cfg.channels = kOutputChannels;
    cfg.nullBackend = m_options.nullAudio;
    m_audioStarted = m_audio.start(cfg, &err);
    if (!m_audioStarted) {
        qCWarning(lcPlayer) << "Sin salida de audio:" << err.c_str();
        return false;
    }
    m_audioDevice = QString::fromStdString(m_audio.deviceName());
    return true;
}

void Player::startThreads()
{
    m_quit = false;
    m_audioQuit = false;
    m_videoEof = false;
    m_videoSeekPending = false;
    m_audioSeekPending = false;
    if (m_videoDecoder)
        m_videoThread = std::thread(&Player::videoLoop, this);
    if (m_audioDecoder)
        m_audioThread = std::thread(&Player::audioLoop, this);
}

void Player::stopThreads()
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

void Player::videoLoop()
{
    media::VideoDecoder& dec = *m_videoDecoder;
    for (;;) {
        std::unique_lock lock(m_videoMutex);
        m_videoCv.wait(lock, [this] {
            return m_quit || m_videoSeekPending || (!m_videoEof && m_videoQueue.size() < kMaxQueuedFrames);
        });
        if (m_quit)
            return;
        if (m_videoSeekPending) {
            const TimeUs target = m_videoSeekTarget;
            m_videoSeekPending = false;
            lock.unlock();
            dec.seek(target);
            continue;
        }
        const uint64_t serial = m_videoSerial;
        lock.unlock();

        auto frame = dec.decodeNext();
        std::string desc = dec.decoderDescription();

        lock.lock();
        m_videoDecoderDesc = std::move(desc);
        if (serial != m_videoSerial)
            continue; // hubo una búsqueda mientras se decodificaba: fotograma obsoleto
        if (!frame) {
            m_videoEof = true;
            continue;
        }
        m_videoQueue.push_back(std::move(frame));
        lock.unlock();
        QMetaObject::invokeMethod(this, &Player::frameReady, Qt::QueuedConnection);
    }
}

void Player::audioLoop()
{
    media::AudioDecoder& dec = *m_audioDecoder;
    const int channels = m_audio.channels();
    const int rate = m_audio.sampleRate();
    media::AudioChunk chunk;
    int offset = 0;
    bool eof = false;
    uint64_t generation = m_audio.generation();

    while (!m_audioQuit) {
        bool seekNow = false;
        TimeUs target = 0;
        {
            std::lock_guard lock(m_audioMutex);
            if (m_audioSeekPending) {
                m_audioSeekPending = false;
                seekNow = true;
                target = m_audioSeekTarget;
            }
        }
        if (seekNow) {
            generation = m_audio.generation();
            dec.seek(target);
            chunk.frames = 0;
            offset = 0;
            eof = false;
            continue;
        }
        if (eof) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (offset >= chunk.frames) {
            if (!dec.read(chunk)) {
                eof = true;
                m_audio.setEndOfStream(generation);
                continue;
            }
            offset = 0;
        }
        const int written = m_audio.write(chunk.samples.data() + static_cast<size_t>(offset) * channels, chunk.frames - offset,
                                          chunk.pts + static_cast<TimeUs>(offset) * kUsPerSecond / rate, generation);
        if (written < 0) {
            // La salida se reinició (búsqueda en curso): descartar el bloque y esperar la orden.
            chunk.frames = 0;
            offset = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        offset += written;
        if (written == 0 || m_audio.buffered() > kAudioTargetBuffer)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

TimeUs Player::clockUs() const
{
    if (!m_playing.load()) {
        std::lock_guard lock(m_clockMutex);
        return m_pausedPos;
    }
    const auto now = SteadyClock::now();
    if (m_audioDecoder && !m_audio.drained()) {
        const TimeUs t = m_audio.clock();
        std::lock_guard lock(m_clockMutex);
        m_anchorPos = t;
        m_anchorTime = now;
        return t;
    }
    // Sin audio (o ya terminó): reloj de pared desde el último anclaje.
    std::lock_guard lock(m_clockMutex);
    return m_anchorPos + std::chrono::duration_cast<std::chrono::microseconds>(now - m_anchorTime).count();
}

std::shared_ptr<const media::VideoFrame> Player::frameForPresentation()
{
    const TimeUs t = clockUs();
    const auto now = SteadyClock::now();
    const bool playing = m_playing.load();
    std::lock_guard lock(m_videoMutex);

    // Tolerancia de medio intervalo de refresco: cuando la cadencia del video coincide con la de
    // la pantalla (p. ej. 60 fps a 60 Hz), los fotogramas vencen justo en cada refresco y la
    // fluctuación del reloj haría que unos refrescos no avancen y otros salten dos fotogramas.
    // Elegir el fotograma más cercano (no el último vencido) avanza uno por refresco y centra
    // el error A/V en cero. En pausa no hay tolerancia: se muestra exactamente el de `t`.
    TimeUs tolerance = 0;
    if (playing) {
        if (m_diag.lastCall != SteadyClock::time_point{}) {
            const TimeUs gap = std::chrono::duration_cast<std::chrono::microseconds>(now - m_diag.lastCall).count();
            if (gap > 2'000 && gap < 100'000)
                m_renderInterval = (m_renderInterval * 15 + gap) / 16; // media móvil del intervalo de refresco
            // Diagnóstico: huecos entre renders, saltos del reloj y cola mínima del decodificador.
            m_diag.maxRenderGap = std::max<TimeUs>(m_diag.maxRenderGap, gap);
            m_diag.maxClockStep = std::max<TimeUs>(m_diag.maxClockStep, t - m_diag.lastClock);
        }
        m_diag.minQueue = std::min<int>(m_diag.minQueue, static_cast<int>(m_videoQueue.size()));
        m_diag.lastCall = now;
        tolerance = std::min(m_frameDuration, m_renderInterval) / 2;
    } else {
        m_diag.lastCall = {};
    }
    m_diag.lastClock = t;

    int advanced = 0;
    while (!m_videoQueue.empty() && m_videoQueue.front()->pts <= t + tolerance) {
        m_displayed = std::move(m_videoQueue.front());
        m_videoQueue.pop_front();
        ++advanced;
    }
    if (!m_displayed && !m_videoQueue.empty()) {
        m_displayed = std::move(m_videoQueue.front());
        m_videoQueue.pop_front();
        advanced = 1;
    }
    if (advanced > 0) {
        m_presented += 1;
        if (advanced > 1 && playing)
            m_dropped += advanced - 1;
        m_videoCv.notify_one();
    }
    return m_displayed;
}

void Player::play()
{
    if (!hasMedia() || m_playing.load())
        return;
    if (m_atEnd)
        seekInternal(0, true);
    {
        std::lock_guard lock(m_clockMutex);
        m_anchorPos = m_pausedPos;
        m_anchorTime = SteadyClock::now();
    }
    m_audio.setPlaying(true);
    m_playing = true;
    emit playingChanged();
}

void Player::pause()
{
    if (!m_playing.load())
        return;
    const TimeUs t = std::min(clockUs(), m_duration);
    m_playing = false;
    m_audio.setPlaying(false);
    {
        std::lock_guard lock(m_clockMutex);
        m_pausedPos = t;
    }
    emit playingChanged();
    emit positionChanged();
}

void Player::togglePlayPause()
{
    if (m_playing.load())
        pause();
    else
        play();
}

void Player::stop()
{
    pause();
    seekInternal(0, true);
}

void Player::seek(double seconds)
{
    if (hasMedia())
        seekInternal(fromSeconds(seconds), true);
}

void Player::stepFrames(int delta)
{
    if (!m_videoDecoder || delta == 0)
        return;
    pause();
    TimeUs current = kNoTime;
    TimeUs next = kNoTime;
    {
        std::lock_guard lock(m_videoMutex);
        if (m_displayed)
            current = m_displayed->pts;
        if (delta == 1 && !m_videoQueue.empty())
            next = m_videoQueue.front()->pts;
    }
    if (next != kNoTime) {
        // El siguiente fotograma ya está decodificado: avanzar sin buscar.
        seekInternal(next, false);
        return;
    }
    if (current == kNoTime)
        current = clockUs();
    // Apuntar a la mitad del fotograma deseado para no depender de la precisión de los timestamps.
    seekInternal(current + delta * m_frameDuration + m_frameDuration / 2, true);
}

void Player::seekInternal(TimeUs t, bool flushVideo)
{
    t = std::clamp<TimeUs>(t, 0, std::max<TimeUs>(m_duration, 0));
    m_atEnd = false;
    {
        std::lock_guard lock(m_clockMutex);
        m_pausedPos = t;
        m_anchorPos = t;
        m_anchorTime = SteadyClock::now();
    }
    if (m_videoDecoder && flushVideo) {
        {
            std::lock_guard lock(m_videoMutex);
            m_videoQueue.clear();
            ++m_videoSerial;
            m_videoSeekPending = true;
            m_videoSeekTarget = t;
            m_videoEof = false;
        }
        m_videoCv.notify_one();
    }
    if (m_audioDecoder) {
        m_audio.reset(t);
        std::lock_guard lock(m_audioMutex);
        m_audioSeekPending = true;
        m_audioSeekTarget = t;
    }
    emit positionChanged();
    emit frameReady();
}

void Player::onTick()
{
    if (!hasMedia())
        return;

    if (m_playing.load()) {
        bool videoDone = true;
        if (m_videoDecoder) {
            std::lock_guard lock(m_videoMutex);
            videoDone = m_videoEof && m_videoQueue.empty();
        }
        const bool audioDone = !m_audioDecoder || m_audio.drained();
        const TimeUs t = clockUs();
        if ((videoDone && audioDone) || t >= m_duration + 250'000) {
            pause();
            {
                std::lock_guard lock(m_clockMutex);
                m_pausedPos = std::min(t, m_duration);
            }
            m_atEnd = true;
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
        Diagnostics diag;
        {
            std::lock_guard lock(m_videoMutex);
            diag = m_diag;
            m_diag.maxRenderGap = 0;
            m_diag.maxClockStep = 0;
            m_diag.minQueue = INT_MAX;
        }
        qCDebug(lcStats,
                "pos=%.3f s mostrados/s=%.1f descartados=%d audio_en_buffer=%.0f ms | hueco_render_max=%.1f ms "
                "salto_reloj_max=%.1f ms cola_min=%d",
                toSeconds(clockUs()), m_presentedFps, m_dropped.load(), m_audioDecoder ? toSeconds(m_audio.buffered()) * 1000.0 : 0.0,
                diag.maxRenderGap / 1000.0, diag.maxClockStep / 1000.0, diag.minQueue == INT_MAX ? -1 : diag.minQueue);
        emit statsChanged();
    }
}

QVariantMap Player::mediaInfo() const
{
    QVariantMap map;
    if (!m_info)
        return map;
    map[QStringLiteral("path")] = QString::fromStdString(m_info->path);
    map[QStringLiteral("container")] = QString::fromStdString(m_info->container);
    map[QStringLiteral("duration")] = toSeconds(m_info->duration);
    map[QStringLiteral("bitRate")] = static_cast<qint64>(m_info->bitRate);
    if (m_info->video) {
        const auto& v = *m_info->video;
        map[QStringLiteral("videoCodec")] = QString::fromStdString(v.codec);
        map[QStringLiteral("pixelFormat")] = QString::fromStdString(v.pixelFormat);
        map[QStringLiteral("width")] = v.width;
        map[QStringLiteral("height")] = v.height;
        map[QStringLiteral("frameRate")] = v.frameRate;
        map[QStringLiteral("videoBitRate")] = static_cast<qint64>(v.bitRate);
    }
    if (m_info->audio) {
        const auto& a = *m_info->audio;
        map[QStringLiteral("audioCodec")] = QString::fromStdString(a.codec);
        map[QStringLiteral("sampleFormat")] = QString::fromStdString(a.sampleFormat);
        map[QStringLiteral("sampleRate")] = a.sampleRate;
        map[QStringLiteral("channels")] = a.channels;
        map[QStringLiteral("audioBitRate")] = static_cast<qint64>(a.bitRate);
    }
    return map;
}

QString Player::videoDecoder() const
{
    std::lock_guard lock(m_videoMutex);
    return QString::fromStdString(m_videoDecoderDesc);
}

void Player::setVolume(double volume)
{
    volume = std::clamp(volume, 0.0, 1.0);
    if (qFuzzyCompare(volume, m_volume))
        return;
    m_volume = volume;
    applyVolume();
    emit volumeChanged();
}

void Player::setMuted(bool muted)
{
    if (muted == m_muted)
        return;
    m_muted = muted;
    applyVolume();
    emit volumeChanged();
}

void Player::applyVolume()
{
    // Curva cuadrática: el control lineal del deslizador se percibe más natural.
    m_audio.setVolume(m_muted ? 0.0f : static_cast<float>(m_volume * m_volume));
}

void Player::fail(const QString& message)
{
    m_error = message;
    qCWarning(lcPlayer) << message;
    emit errorOccurred(message);
}

} // namespace movix::playback
