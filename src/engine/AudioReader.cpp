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
