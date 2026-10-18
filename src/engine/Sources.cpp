#include "engine/Sources.h"

#include <algorithm>
#include <cstring>

namespace movix::engine {

namespace {

template <typename Map>
void evictLeastRecentlyUsed(Map& readers, size_t maxOpen)
{
    while (readers.size() > maxOpen) {
        auto oldest = std::min_element(readers.begin(), readers.end(),
                                       [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        readers.erase(oldest);
    }
}

} // namespace

std::shared_ptr<const media::VideoFrame> VideoSources::frame(const core::VideoLayer& layer)
{
    Entry& e = m_readers[layer.clipId];
    e.lastUse = ++m_tick;
    if (!e.reader || e.path != layer.media->path) {
        e.reader = std::make_unique<VideoReader>();
        e.path = layer.media->path;
        e.failed = !e.reader->open(e.path, m_hardware);
    }
    std::shared_ptr<const media::VideoFrame> result = e.failed ? nullptr : e.reader->frameAt(layer.sourceTime);
    evictLeastRecentlyUsed(m_readers, m_maxOpen);
    return result;
}

FrameBundle VideoSources::build(const core::Project& project, core::FramePos frame)
{
    FrameBundle bundle;
    bundle.frame = frame;
    bundle.time = project.settings.frameToTime(frame);
    bundle.canvasWidth = project.settings.width;
    bundle.canvasHeight = project.settings.height;
    for (const core::VideoLayer& layer : core::videoLayersAt(project, frame)) {
        if (auto f = this->frame(layer))
            bundle.layers.push_back({layer.clipId, layer.trackIndex, std::move(f)});
    }
    return bundle;
}

AudioReader* AudioMixer::readerFor(core::Id clipId)
{
    auto it = m_readers.find(clipId);
    return it == m_readers.end() ? nullptr : it->second.reader.get();
}

void AudioMixer::mix(const core::Project& project, int64_t samplePos, int frames, float* out)
{
    std::memset(out, 0, size_t(frames) * size_t(m_channels) * sizeof(float));
    const TimeUs t0 = sampleToTime(samplePos);
    const TimeUs t1 = sampleToTime(samplePos + frames);

    for (const core::AudioSegment& seg : core::audioSegmentsIn(project, t0, t1)) {
        // Rango de muestras del bloque cubierto por el tramo.
        const int64_t first = std::clamp<int64_t>(timeToSample(seg.timelineStart) - samplePos, 0, frames);
        const int64_t last = std::clamp<int64_t>(timeToSample(seg.timelineEnd) - samplePos, 0, frames);
        if (last <= first)
            continue;

        Entry& e = m_readers[seg.clipId];
        e.lastUse = ++m_tick;
        if (!e.reader || e.path != seg.media->path) {
            e.reader = std::make_unique<AudioReader>();
            e.path = seg.media->path;
            e.failed = !e.reader->open(e.path, m_rate, m_channels);
        }
        if (e.failed)
            continue;
        // Instante del medio que corresponde a la primera muestra cubierta.
        const TimeUs sourceTime = seg.sourceStart + (sampleToTime(samplePos + first) - seg.timelineStart);
        e.reader->mixInto(sourceTime, int(last - first), seg.gain, out + size_t(first) * size_t(m_channels));
    }
    evictLeastRecentlyUsed(m_readers, m_maxOpen);
}

} // namespace movix::engine
