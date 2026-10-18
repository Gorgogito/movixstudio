#include "core/Timeline.h"

#include "core/Edits.h"

#include <algorithm>

namespace movix::core {

std::vector<VideoLayer> videoLayersAt(const Project& p, FramePos frame)
{
    std::vector<VideoLayer> layers;
    for (size_t i = 0; i < p.videoTracks.size(); ++i) {
        const Track& t = p.videoTracks[i];
        if (t.hidden)
            continue;
        const Clip* c = edit::clipAt(t, frame);
        if (!c || !c->enabled)
            continue;
        const MediaItem* m = p.findMedia(c->mediaId);
        if (!m || !m->hasVideo)
            continue;
        VideoLayer layer;
        layer.clipId = c->id;
        layer.mediaId = m->id;
        layer.media = m;
        layer.trackIndex = int(i);
        layer.sourceTime = c->sourceIn + p.settings.frameToTime(frame - c->start);
        layers.push_back(layer);
    }
    return layers;
}

std::vector<AudioSegment> audioSegmentsIn(const Project& p, TimeUs t0, TimeUs t1)
{
    std::vector<AudioSegment> segments;
    auto collect = [&](const Track& t) {
        if (t.muted)
            return;
        for (const Clip& c : t.clips) {
            if (!c.enabled)
                continue;
            const TimeUs cs = p.settings.frameToTime(c.start);
            const TimeUs ce = p.settings.frameToTime(c.end());
            if (ce <= t0)
                continue;
            if (cs >= t1)
                break; // los clips están ordenados
            const MediaItem* m = p.findMedia(c.mediaId);
            if (!m || !m->hasAudio)
                continue;
            AudioSegment s;
            s.clipId = c.id;
            s.media = m;
            s.timelineStart = std::max(t0, cs);
            s.timelineEnd = std::min(t1, ce);
            s.sourceStart = c.sourceIn + (s.timelineStart - cs);
            s.gain = c.gain;
            segments.push_back(s);
        }
    };
    for (const Track& t : p.videoTracks)
        collect(t);
    for (const Track& t : p.audioTracks)
        collect(t);
    return segments;
}

} // namespace movix::core
