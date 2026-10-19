#include "media/TempoFilter.h"

#include <string>

namespace movix::media {

bool TempoFilter::init(int sampleRate, int channels, double speed)
{
    m_speed = speed;
    // atempo admite 0,5..100: por debajo de 0,5 se encadenan varios.
    std::string chain;
    double remaining = speed;
    while (remaining < 0.5) {
        chain += "atempo=0.5,";
        remaining /= 0.5;
    }
    chain += "atempo=" + std::to_string(remaining);
    return m_filter.init(sampleRate, channels, chain);
}

void TempoFilter::reset()
{
    m_filter.reset();
}

bool TempoFilter::push(const float* samples, int frames)
{
    return m_filter.push(samples, frames);
}

} // namespace movix::media
