#include "engine/AudioReader.h"

#include <algorithm>
#include <cstdlib>

namespace movix::engine {

namespace {
// Un salto hacia delante menor que esto se resuelve descartando muestras en vez de buscar.
constexpr TimeUs kMaxSkipAhead = 500'000;
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
    return m_decoder.open(path, sampleRate, channels, error);
}

void AudioReader::seekTo(TimeUs t)
{
    ++m_seeks;
    m_decoder.seek(t);
    m_fifo.clear();
    m_fifoRead = 0;
    m_basePts = kNoTime;
    m_consumed = 0;
    m_eof = false;
}

bool AudioReader::fill(int frames)
{
    auto available = [&] { return int((m_fifo.size() / size_t(m_channels)) - m_fifoRead); };
    while (available() < frames && !m_eof) {
        if (!m_decoder.read(m_chunk)) {
            m_eof = true;
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
        m_fifo.insert(m_fifo.end(), m_chunk.samples.begin(), m_chunk.samples.end());
    }
    return available() >= frames;
}

void AudioReader::mixInto(TimeUs t, int frames, float gain, float* out)
{
    if (!m_decoder.isOpen() || frames <= 0)
        return;
    const TimeUs tolerance = kUsPerSecond / m_rate + 1; // una muestra
    auto available = [&] { return int((m_fifo.size() / size_t(m_channels)) - m_fifoRead); };

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

    fill(frames);
    // Si el medio empieza después de `t` (o tras una búsqueda al principio), rellenar con silencio.
    int lead = 0;
    if (m_basePts != kNoTime && nextPts() > t + tolerance)
        lead = int(std::min<TimeUs>(frames, (nextPts() - t) * m_rate / kUsPerSecond));

    const int n = std::min(frames - lead, available());
    if (n <= 0)
        return;
    const float* src = m_fifo.data() + m_fifoRead * size_t(m_channels);
    float* dst = out + size_t(lead) * size_t(m_channels);
    const size_t count = size_t(n) * size_t(m_channels);
    for (size_t i = 0; i < count; ++i)
        dst[i] += src[i] * gain;
    m_fifoRead += size_t(n);
    m_consumed += n;
}

} // namespace movix::engine
