#include "engine/VideoReader.h"

namespace movix::engine {

namespace {
// Los instantes de fotograma de proyecto y de medio se redondean al µs por separado.
constexpr TimeUs kTolerance = 1000;
// Más allá de este salto hacia delante sale más barato buscar que decodificar en orden.
constexpr TimeUs kMaxForwardDecode = 2 * kUsPerSecond;
} // namespace

bool VideoReader::open(const std::string& path, bool hardware, std::string* error)
{
    m_current.reset();
    m_next.reset();
    m_nextValid = false;
    return m_decoder.open(path, media::VideoDecoderOptions{hardware}, error);
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

std::shared_ptr<const media::VideoFrame> VideoReader::frameAt(TimeUs t)
{
    if (!m_decoder.isOpen())
        return nullptr;

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
