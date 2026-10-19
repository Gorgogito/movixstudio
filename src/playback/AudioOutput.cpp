#include "playback/AudioOutput.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace movix::playback {

using SteadyClock = std::chrono::steady_clock;

struct AudioOutput::Impl {
    ma_context context{};
    ma_device device{};
    bool contextInit = false;
    bool deviceInit = false;
    int rate = 48000;
    int channels = 2;
    TimeUs latency = 0;

    mutable std::mutex mutex;
    std::vector<float> ring;      // capacity * channels
    int capacity = 0;             // en frames
    int readPos = 0;
    int size = 0;
    uint64_t generation = 0;
    bool havePts = false;
    TimeUs basePts = 0;           // pts de la primera muestra tras el reset
    int64_t consumed = 0;         // frames entregados al dispositivo desde basePts
    TimeUs resetPosition = 0;
    bool endOfStream = false;
    bool playing = false;
    SteadyClock::time_point lastCallback{};
    int lastCallbackFrames = 0;
    mutable TimeUs lastClock = kNoTime;
    mutable TimeUs smoothPos = kNoTime; // reloj suavizado (ver clock())
    mutable SteadyClock::time_point smoothAt{};
    std::atomic<float> volume{1.0f};

    static void callback(ma_device* device, void* output, const void*, ma_uint32 frames)
    {
        static_cast<Impl*>(device->pUserData)->render(static_cast<float*>(output), static_cast<int>(frames));
    }

    void render(float* out, int frames)
    {
        const float vol = volume.load(std::memory_order_relaxed);
        std::lock_guard lock(mutex);
        const int n = playing ? std::min(frames, size) : 0;
        int done = 0;
        while (done < n) {
            const int chunk = std::min(n - done, capacity - readPos);
            const float* src = ring.data() + static_cast<size_t>(readPos) * channels;
            float* dst = out + static_cast<size_t>(done) * channels;
            const size_t count = static_cast<size_t>(chunk) * channels;
            for (size_t i = 0; i < count; ++i)
                dst[i] = src[i] * vol;
            readPos = (readPos + chunk) % capacity;
            done += chunk;
        }
        if (n < frames)
            std::memset(out + static_cast<size_t>(n) * channels, 0, static_cast<size_t>(frames - n) * channels * sizeof(float));
        size -= n;
        consumed += n;
        lastCallback = SteadyClock::now();
        lastCallbackFrames = n;
    }

    TimeUs framesToUs(int64_t f) const { return f * kUsPerSecond / rate; }
};

AudioOutput::AudioOutput() : d(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput()
{
    stop();
}

bool AudioOutput::start(const Config& config, std::string* error)
{
    stop();
    d->rate = config.sampleRate;
    d->channels = config.channels;
    d->capacity = std::max(1024, static_cast<int>(config.bufferSeconds * config.sampleRate));
    d->ring.assign(static_cast<size_t>(d->capacity) * d->channels, 0.0f);

    ma_context* ctx = nullptr;
    if (config.nullBackend) {
        const ma_backend backends[] = {ma_backend_null};
        if (ma_context_init(backends, 1, nullptr, &d->context) != MA_SUCCESS) {
            if (error)
                *error = "No se pudo inicializar el backend de audio nulo.";
            return false;
        }
        d->contextInit = true;
        ctx = &d->context;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = static_cast<ma_uint32>(d->channels);
    cfg.sampleRate = static_cast<ma_uint32>(d->rate);
    cfg.dataCallback = &Impl::callback;
    cfg.pUserData = d.get();
    cfg.performanceProfile = ma_performance_profile_low_latency;

    if (ma_device_init(ctx, &cfg, &d->device) != MA_SUCCESS) {
        if (error)
            *error = "No se pudo abrir el dispositivo de audio.";
        stop();
        return false;
    }
    d->deviceInit = true;

    // Latencia aproximada entre la entrega de muestras y su salida por el altavoz.
    const auto& pb = d->device.playback;
    if (pb.internalSampleRate > 0)
        d->latency = static_cast<TimeUs>(pb.internalPeriodSizeInFrames) * std::max<ma_uint32>(pb.internalPeriods, 1)
            * kUsPerSecond / pb.internalSampleRate;

    if (ma_device_start(&d->device) != MA_SUCCESS) {
        if (error)
            *error = "No se pudo iniciar el dispositivo de audio.";
        stop();
        return false;
    }
    return true;
}

void AudioOutput::stop()
{
    if (d->deviceInit) {
        ma_device_uninit(&d->device);
        d->deviceInit = false;
    }
    if (d->contextInit) {
        ma_context_uninit(&d->context);
        d->contextInit = false;
    }
}

bool AudioOutput::isRunning() const
{
    return d->deviceInit;
}

int AudioOutput::sampleRate() const
{
    return d->rate;
}

int AudioOutput::channels() const
{
    return d->channels;
}

std::string AudioOutput::deviceName() const
{
    return d->deviceInit ? std::string(d->device.playback.name) : std::string();
}

uint64_t AudioOutput::generation() const
{
    std::lock_guard lock(d->mutex);
    return d->generation;
}

int AudioOutput::write(const float* data, int frames, TimeUs pts, uint64_t generation)
{
    std::lock_guard lock(d->mutex);
    if (generation != d->generation)
        return -1;
    if (!d->havePts) {
        d->havePts = true;
        d->basePts = pts;
        d->consumed = 0;
    }
    const int n = std::min(frames, d->capacity - d->size);
    int done = 0;
    int writePos = (d->readPos + d->size) % d->capacity;
    while (done < n) {
        const int chunk = std::min(n - done, d->capacity - writePos);
        std::memcpy(d->ring.data() + static_cast<size_t>(writePos) * d->channels,
                    data + static_cast<size_t>(done) * d->channels, static_cast<size_t>(chunk) * d->channels * sizeof(float));
        writePos = (writePos + chunk) % d->capacity;
        done += chunk;
    }
    d->size += n;
    return n;
}

void AudioOutput::setEndOfStream(uint64_t generation)
{
    std::lock_guard lock(d->mutex);
    if (generation == d->generation)
        d->endOfStream = true;
}

void AudioOutput::reset(TimeUs position)
{
    std::lock_guard lock(d->mutex);
    ++d->generation;
    d->readPos = 0;
    d->size = 0;
    d->havePts = false;
    d->consumed = 0;
    d->resetPosition = position;
    d->endOfStream = false;
    d->lastCallbackFrames = 0;
    d->lastClock = kNoTime;
    d->smoothPos = kNoTime;
}

void AudioOutput::setPlaying(bool playing)
{
    std::lock_guard lock(d->mutex);
    d->playing = playing;
    if (!playing)
        d->lastCallbackFrames = 0;
}

void AudioOutput::setVolume(float volume)
{
    d->volume.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_relaxed);
}

TimeUs AudioOutput::clock() const
{
    std::lock_guard lock(d->mutex);
    if (!d->havePts)
        return d->resetPosition;

    // Posición de la última muestra entregada al dispositivo...
    TimeUs t = d->basePts + d->framesToUs(d->consumed);
    // ...menos el bloque recién entregado y la latencia del dispositivo, más el tiempo
    // transcurrido desde la entrega (interpolación entre callbacks para un avance suave).
    if (d->playing && d->lastCallbackFrames > 0) {
        const TimeUs block = d->framesToUs(d->lastCallbackFrames);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - d->lastCallback).count();
        t += std::clamp<TimeUs>(elapsed, 0, block) - block;
    }
    t -= d->latency;
    // Tras una búsqueda a `resetPosition`, el primer bloque empieza en la muestra entera anterior
    // (hasta una muestra antes): el reloj no debe informar de un instante anterior al pedido.
    t = std::max({t, d->basePts, d->resetPosition});

    // Suavizado: los callbacks llegan a intervalos irregulares (periodos de 10–40 ms según el
    // dispositivo), así que la posición medida avanza a saltos. Se sigue un reloj continuo que
    // avanza con el tiempo real y se corrige poco a poco hacia la medida; si se aleja mucho
    // (búsqueda, falta de datos), se ajusta de golpe.
    const auto now = SteadyClock::now();
    if (d->playing && d->lastCallbackFrames > 0 && d->smoothPos != kNoTime) {
        TimeUs predicted =
            d->smoothPos + std::chrono::duration_cast<std::chrono::microseconds>(now - d->smoothAt).count();
        const TimeUs error = t - predicted;
        if (std::llabs(error) > 80'000)
            predicted = t;
        else
            predicted += error / 10;
        t = predicted;
    }
    d->smoothPos = t;
    d->smoothAt = now;
    if (d->lastClock != kNoTime && t < d->lastClock)
        t = d->lastClock; // nunca retroceder
    d->lastClock = t;
    return t;
}

bool AudioOutput::drained() const
{
    std::lock_guard lock(d->mutex);
    return d->endOfStream && d->size == 0;
}

TimeUs AudioOutput::buffered() const
{
    std::lock_guard lock(d->mutex);
    return d->framesToUs(d->size);
}

} // namespace movix::playback
