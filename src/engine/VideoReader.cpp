#include "engine/VideoReader.h"

#include <algorithm>

namespace movix::engine {

namespace {
// Los instantes de fotograma de proyecto y de medio se redondean al µs por separado.
constexpr TimeUs kTolerance = 1000;
// Más allá de este salto hacia delante sale más barato buscar que decodificar en orden.
constexpr TimeUs kMaxForwardDecode = 2 * kUsPerSecond;
// Un paso atrás de hasta esto se considera reproducción al revés (se decodifica por bloques).
constexpr TimeUs kMaxBackwardStep = 2 * kUsPerSecond;
// Cada bloque hacia atrás decodifica este tramo y lo guarda.
constexpr TimeUs kReverseChunk = kUsPerSecond;
constexpr size_t kMaxCacheBytes = size_t(256) << 20;

size_t frameBytes(const media::VideoFrame& f)
{
    if (f.onGpu()) // memoria de vídeo, pero cuenta igual
        return size_t(f.width) * size_t(f.height) * 3 / 2 * (f.format == media::PixelFormat::P016 ? 2 : 1);
    return f.y.size() + f.uv.size() + f.rgba.size();
}
} // namespace

bool VideoReader::open(const std::string& path, bool hardware, std::string* error, void* gpuDevice)
{
    m_current.reset();
    m_next.reset();
    m_nextValid = false;
    m_cache.clear();
    m_cacheBytes = 0;
    // 10 bits o más se conservan (P016) para la composición y el HDR.
    return m_decoder.open(path, media::VideoDecoderOptions{hardware, true, hardware ? gpuDevice : nullptr}, error);
}

std::shared_ptr<media::VideoFrame> VideoReader::decodeNext()
{
    return m_decoder.decodeNext();
}

void VideoReader::seekTo(TimeUs t)
{
    ++m_seeks;
    m_decoder.seek(t);
    m_current = decodeNext();
    m_next.reset();
    m_nextValid = false;
}

std::shared_ptr<const media::VideoFrame> VideoReader::fromCache(TimeUs t) const
{
    if (m_cache.empty() || t + kTolerance < m_cache.front()->pts || t > m_cache.back()->pts + kMaxBackwardStep)
        return nullptr;
    // El último fotograma con pts ≤ t (la caché está ordenada y es continua).
    auto it = std::upper_bound(m_cache.begin(), m_cache.end(), t + kTolerance,
                               [](TimeUs v, const std::shared_ptr<media::VideoFrame>& f) { return v < f->pts; });
    if (it == m_cache.begin())
        return nullptr;
    const auto& f = *std::prev(it);
    // Del último de la caché no se sabe cuál le sigue: solo vale dentro de su duración.
    if (std::next(std::prev(it)) == m_cache.end() && (f->duration <= 0 || t >= f->pts + f->duration))
        return nullptr;
    return f;
}

void VideoReader::fillBackward(TimeUs t)
{
    // Se decodifica el segundo anterior hasta donde empieza lo ya guardado (o el fotograma actual)
    // y se pone delante en la caché: los siguientes pasos atrás no necesitan decodificar.
    const TimeUs until = !m_cache.empty() ? m_cache.front()->pts : m_current ? m_current->pts : t + kTolerance;
    if (m_cache.empty() && m_current) {
        m_cache.push_back(m_current);
        m_cacheBytes += frameBytes(*m_current);
    }
    seekTo(std::max<TimeUs>(0, std::min(t, until) - kReverseChunk));
    std::deque<std::shared_ptr<media::VideoFrame>> chunk;
    std::shared_ptr<media::VideoFrame> f = m_current;
    while (f && f->pts + kTolerance < until) {
        chunk.push_back(f);
        f = decodeNext();
    }
    if (!chunk.empty())
        m_current = chunk.back();
    m_next = f; // el siguiente en orden (el primero de la caché anterior, ya decodificado)
    m_nextValid = true;
    for (auto it = chunk.rbegin(); it != chunk.rend(); ++it) {
        m_cache.push_front(*it);
        m_cacheBytes += frameBytes(**it);
    }
    // Al ir hacia atrás sobra lo más reciente.
    while (m_cache.size() > 8 && m_cacheBytes > kMaxCacheBytes) {
        m_cacheBytes -= frameBytes(*m_cache.back());
        m_cache.pop_back();
    }
}

std::shared_ptr<const media::VideoFrame> VideoReader::frameAt(TimeUs t)
{
    if (!m_decoder.isOpen())
        return nullptr;

    if (auto f = fromCache(t))
        return f;
    // Un paso corto hacia atrás: reproducción al revés (o pasos de fotograma hacia atrás).
    const TimeUs reference = !m_cache.empty() ? m_cache.front()->pts : m_current ? m_current->pts : kNoTime;
    if (reference != kNoTime && t + kTolerance < reference && reference - t <= kMaxBackwardStep) {
        fillBackward(t);
        if (auto f = fromCache(t))
            return f;
    }
    if (!m_cache.empty()) { // de vuelta hacia delante: la caché ya no sirve
        m_cache.clear();
        m_cacheBytes = 0;
    }

    const bool forward = m_current && t + kTolerance >= m_current->pts && t - m_current->pts <= kMaxForwardDecode;
    if (!forward)
        seekTo(t);

    for (;;) {
        if (!m_nextValid) {
            m_next = decodeNext();
            m_nextValid = true;
        }
        if (!m_next || m_next->pts > t + kTolerance)
            break;
        m_current = std::move(m_next);
        m_nextValid = false;
    }
    return m_current;
}

} // namespace movix::engine
