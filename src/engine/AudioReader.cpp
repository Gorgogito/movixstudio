#include "engine/AudioReader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace movix::engine {

namespace {
// Un salto hacia delante menor que esto se resuelve descartando muestras en vez de buscar.
constexpr TimeUs kMaxSkipAhead = 500'000;
constexpr int kTempoBlock = 1024;
constexpr TimeUs kDenoisePreroll = 300'000;
} // namespace

bool AudioReader::open(const std::string& path, int sampleRate, int channels, std::string* error)
{
    m_rate = sampleRate;
    m_channels = channels;
    m_fifo.clear();
    m_fifoRead = 0;
    m_basePts = kNoTime;
    m_consumed = 0;
    m_eof = false;
    m_tempoSpeed = 0.0;
    m_denoiseOn = false;
    return m_decoder.open(path, sampleRate, channels, error);
}

void AudioReader::configureDenoise(double amount)
{
    const std::string model = amount > 0.001 ? media::denoiseModel() : std::string();
    const bool want = !model.empty();
    if (want == m_denoiseOn && (!want || (m_denoiser.amount() == std::clamp(amount, 0.0, 1.0) && m_denoiser.model() == model)))
        return;
    m_denoiseOn = want && m_denoiser.init(m_rate, m_channels, model, amount);
    // Lo ya decodificado se procesó con otro ajuste: se vuelve a leer desde donde toque.
    m_fifo.clear();
    m_fifoRead = 0;
    m_basePts = kNoTime;
    m_consumed = 0;
    m_eof = false;
    m_tempoBase = kNoTime;
}

void AudioReader::seekTo(TimeUs t)
{
    ++m_seeks;
    t = std::max<TimeUs>(0, t);
    m_fifo.clear();
    m_fifoRead = 0;
    m_basePts = kNoTime;
    m_consumed = 0;
    m_eof = false;
    if (!m_denoiseOn) {
        m_decoder.seek(t);
        return;
    }
    // La red de RNNoise necesita contexto: se arranca un poco antes y se descarta lo anterior a `t`.
    m_denoiser.reset();
    const TimeUs from = std::max<TimeUs>(0, t - kDenoisePreroll);
    m_decoder.seek(from);
    int64_t discard = (t - from) * m_rate / kUsPerSecond; // frames de salida previos a `t`
    while (discard > 0) {
        if (!m_decoder.read(m_chunk))
            break;
        m_denoised.clear();
        m_denoiser.process(m_chunk.samples.data(), m_chunk.frames, m_denoised);
        const int64_t frames = int64_t(m_denoised.size()) / m_channels;
        const int64_t drop = std::min(frames, discard);
        discard -= drop;
        if (drop < frames) {
            m_basePts = t;
            m_fifo.insert(m_fifo.end(), m_denoised.begin() + std::ptrdiff_t(drop * m_channels), m_denoised.end());
        }
    }
    if (discard > 0) // el medio terminó antes de llegar a `t`
        m_eof = true;
    else if (m_basePts == kNoTime)
        m_basePts = t;
}

bool AudioReader::fill(int frames)
{
    while (available() < frames && !m_eof) {
        if (!m_decoder.read(m_chunk)) {
            m_eof = true;
            if (m_denoiseOn) { // la cola retenida por el filtro
                m_denoised.clear();
                m_denoiser.flush(m_denoised);
                m_fifo.insert(m_fifo.end(), m_denoised.begin(), m_denoised.end());
            }
            break;
        }
        if (m_basePts == kNoTime) {
            m_basePts = m_chunk.pts;
            m_consumed = 0;
        }
        // Compactar el FIFO cuando lo consumido domina, para que no crezca sin límite.
        if (m_fifoRead > 0 && m_fifoRead * 2 >= m_fifo.size() / size_t(m_channels)) {
            m_fifo.erase(m_fifo.begin(), m_fifo.begin() + std::ptrdiff_t(m_fifoRead * size_t(m_channels)));
            m_fifoRead = 0;
        }
        if (m_denoiseOn) {
            m_denoised.clear();
            m_denoiser.process(m_chunk.samples.data(), m_chunk.frames, m_denoised);
            m_fifo.insert(m_fifo.end(), m_denoised.begin(), m_denoised.end());
        } else {
            m_fifo.insert(m_fifo.end(), m_chunk.samples.begin(), m_chunk.samples.end());
        }
    }
    return available() >= frames;
}

void AudioReader::readSource(TimeUs t, int frames, float* dst)
{
    std::memset(dst, 0, size_t(frames) * size_t(m_channels) * sizeof(float));
    // Antes del inicio del medio: silencio hasta llegar a 0.
    int lead = 0;
    if (t < 0) {
        lead = int(std::min<int64_t>(frames, (-t * m_rate + kUsPerSecond - 1) / kUsPerSecond));
        t += TimeUs(lead) * kUsPerSecond / m_rate;
        if (lead >= frames)
            return;
    }

    const TimeUs tolerance = kUsPerSecond / m_rate + 1; // una muestra
    if (m_basePts == kNoTime || std::llabs(t - nextPts()) > tolerance) {
        const TimeUs ahead = m_basePts == kNoTime ? -1 : t - nextPts();
        if (ahead > 0 && ahead < kMaxSkipAhead) {
            const int skip = int(ahead * m_rate / kUsPerSecond);
            fill(skip);
            const int n = std::min(skip, available());
            m_fifoRead += size_t(n);
            m_consumed += n;
        } else {
            seekTo(t);
        }
    }

    const int wanted = frames - lead;
    fill(wanted);
    // Si el medio empieza después de `t`, rellenar con silencio.
    int gap = 0;
    if (m_basePts != kNoTime && nextPts() > t + tolerance)
        gap = int(std::min<TimeUs>(wanted, (nextPts() - t) * m_rate / kUsPerSecond));
    const int n = std::min(wanted - gap, available());
    if (n <= 0)
        return;
    std::memcpy(dst + size_t(lead + gap) * size_t(m_channels), m_fifo.data() + m_fifoRead * size_t(m_channels),
                size_t(n) * size_t(m_channels) * sizeof(float));
    m_fifoRead += size_t(n);
    m_consumed += n;
}

void AudioReader::readTempo(TimeUs t, int frames, double speed, float* dst)
{
    // Instante del medio de la próxima salida si la petición es continua.
    const TimeUs expected = m_tempoBase == kNoTime
        ? kNoTime
        : m_tempoBase + TimeUs(std::llround(double(m_tempoProduced) * speed * kUsPerSecond / m_rate));
    const TimeUs tolerance = TimeUs(std::ceil(speed * kUsPerSecond / m_rate)) + 2;
    if (m_tempoSpeed != speed || expected == kNoTime || std::llabs(t - expected) > tolerance) {
        if (m_tempoSpeed != speed)
            m_tempo.init(m_rate, m_channels, speed);
        else
            m_tempo.reset();
        m_tempoSpeed = speed;
        m_tempoOut.clear();
        m_tempoRead = 0;
        m_tempoBase = t;
        m_tempoProduced = 0;
        m_tempoFeed = t;
    }

    // Alimentar el filtro con el medio en orden hasta tener suficientes muestras de salida.
    m_feed.resize(size_t(kTempoBlock) * size_t(m_channels));
    const size_t needed = size_t(frames) * size_t(m_channels);
    for (int guard = 0; m_tempoOut.size() - m_tempoRead < needed && guard < 512; ++guard) {
        readSource(m_tempoFeed, kTempoBlock, m_feed.data());
        m_tempo.push(m_feed.data(), kTempoBlock);
        m_tempoFeed += TimeUs(kTempoBlock) * kUsPerSecond / m_rate;
        m_tempo.pull(m_tempoOut);
    }
    const size_t n = std::min(needed, m_tempoOut.size() - m_tempoRead);
    std::memcpy(dst, m_tempoOut.data() + m_tempoRead, n * sizeof(float));
    std::memset(dst + n, 0, (needed - n) * sizeof(float));
    m_tempoRead += n;
    m_tempoProduced += frames;
    if (m_tempoRead > 1 << 16) {
        m_tempoOut.erase(m_tempoOut.begin(), m_tempoOut.begin() + std::ptrdiff_t(m_tempoRead));
        m_tempoRead = 0;
    }
}

void AudioReader::ensureWindow(TimeUs lo, TimeUs hi, bool forward)
{
    const auto toFrames = [&](TimeUs d) { return int64_t(std::ceil(double(d) * m_rate / double(kUsPerSecond))); };
    const auto toTime = [&](int64_t f) { return TimeUs(std::llround(double(f) * double(kUsPerSecond) / m_rate)); };
    const TimeUs winEnd = m_winStart == kNoTime ? kNoTime : m_winStart + toTime(m_winFrames);
    if (m_winStart != kNoTime && lo >= m_winStart && hi <= winEnd)
        return;
    if (m_winStart != kNoTime && forward && lo >= m_winStart && lo <= winEnd) {
        // Continuación hacia delante: se descarta lo ya pasado y se añade a partir del final.
        const int64_t drop = std::clamp<int64_t>(toFrames(lo - m_winStart) - 2, 0, m_winFrames);
        m_win.erase(m_win.begin(), m_win.begin() + std::ptrdiff_t(drop * m_channels));
        m_winStart += toTime(drop);
        m_winFrames -= drop;
        const int64_t add = std::max<int64_t>(toFrames(hi - winEnd) + 4, m_rate / 2);
        const size_t at = m_win.size();
        m_win.resize(at + size_t(add * m_channels));
        readSource(winEnd, int(add), m_win.data() + at);
        m_winFrames += add;
        return;
    }
    // Ventana nueva: hacia delante empieza en `lo`; hacia atrás acaba en `hi` (y abarca 2 s).
    const int64_t need = toFrames(hi - lo) + 4;
    const int64_t n = std::max<int64_t>(forward ? m_rate : int64_t(m_rate) * 2, need);
    const TimeUs start = forward ? lo : std::max<TimeUs>(0, hi + toTime(4) - toTime(n));
    const int64_t frames = forward ? n : std::max<int64_t>(need, toFrames(hi - start) + 4);
    m_win.assign(size_t(frames * m_channels), 0.0f);
    readSource(start, int(frames), m_win.data());
    m_winStart = start;
    m_winFrames = frames;
}

void AudioReader::mixVarispeed(TimeUs from, TimeUs to, int frames, float gainStart, float gainEnd, float* out, double denoise)
{
    if (!m_decoder.isOpen() || frames <= 0)
        return;
    configureDenoise(denoise);
    ensureWindow(std::min(from, to), std::max(from, to), from <= to);
    const float step = frames > 1 ? (gainEnd - gainStart) / float(frames) : 0.0f;
    for (int i = 0; i < frames; ++i) {
        // Interpolación lineal entre las dos muestras de la ventana que rodean el instante.
        const double t = double(from) + double(to - from) * (double(i) + 0.5) / double(frames);
        const double pos = (t - double(m_winStart)) * m_rate / double(kUsPerSecond);
        const int64_t i0 = std::clamp<int64_t>(int64_t(std::floor(pos)), 0, std::max<int64_t>(0, m_winFrames - 2));
        const float frac = float(std::clamp(pos - double(i0), 0.0, 1.0));
        const float g = gainStart + step * float(i);
        for (int c = 0; c < m_channels; ++c) {
            const float a = m_win[size_t(i0 * m_channels + c)];
            const float b = i0 + 1 < m_winFrames ? m_win[size_t((i0 + 1) * m_channels + c)] : a;
            out[size_t(i) * size_t(m_channels) + size_t(c)] += (a + (b - a) * frac) * g;
        }
    }
}

void AudioReader::mixInto(TimeUs t, int frames, double speed, float gainStart, float gainEnd, float* out, double denoise)
{
    if (!m_decoder.isOpen() || frames <= 0)
        return;
    configureDenoise(denoise);
    m_scratch.resize(size_t(frames) * size_t(m_channels));
    if (std::abs(speed - 1.0) < 1e-9)
        readSource(t, frames, m_scratch.data());
    else
        readTempo(t, frames, speed, m_scratch.data());

    // Rampa lineal de ganancia a lo largo del bloque (volumen animado, fundidos, transiciones).
    const float step = frames > 1 ? (gainEnd - gainStart) / float(frames) : 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float g = gainStart + step * float(i);
        for (int c = 0; c < m_channels; ++c)
            out[size_t(i) * size_t(m_channels) + size_t(c)] += m_scratch[size_t(i) * size_t(m_channels) + size_t(c)] * g;
    }
}

} // namespace movix::engine
