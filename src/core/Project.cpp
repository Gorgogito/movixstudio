#include "core/Project.h"

#include <algorithm>
#include <set>

namespace movix::core {

namespace {

// floor(a / b) para b > 0, también con a negativo.
std::int64_t floorDiv(std::int64_t a, std::int64_t b)
{
    std::int64_t q = a / b;
    if ((a % b != 0) && (a < 0))
        --q;
    return q;
}

} // namespace

TimeUs ProjectSettings::frameToTime(FramePos f) const
{
    // f · 10⁶ · den / num, redondeado al microsegundo más cercano.
    const std::int64_t n = f * kUsPerSecond * frameRate.den;
    return floorDiv(2 * n + frameRate.num, 2 * frameRate.num);
}

FramePos ProjectSettings::timeToFrame(TimeUs t) const
{
    // Se suma 1 µs para que el instante redondeado de un fotograma caiga en ese fotograma.
    return floorDiv((t + 1) * frameRate.num, kUsPerSecond * frameRate.den);
}

FramePos ProjectSettings::framesIn(TimeUs duration) const
{
    // Tolerancia de 1 ms: las duraciones de contenedor suelen venir redondeadas.
    return std::max<FramePos>(0, floorDiv((duration + 1000) * frameRate.num, kUsPerSecond * frameRate.den));
}

Project Project::createDefault()
{
    Project p;
    for (int i = 1; i <= 2; ++i) {
        Track t;
        t.id = p.newId();
        t.kind = TrackKind::Video;
        t.name = "V" + std::to_string(i);
        p.videoTracks.push_back(t);
    }
    for (int i = 1; i <= 2; ++i) {
        Track t;
        t.id = p.newId();
        t.kind = TrackKind::Audio;
        t.name = "A" + std::to_string(i);
        p.audioTracks.push_back(t);
    }
    return p;
}

const MediaItem* Project::findMedia(Id id) const
{
    for (const auto& m : media)
        if (m.id == id)
            return &m;
    return nullptr;
}

MediaItem* Project::findMedia(Id id)
{
    return const_cast<MediaItem*>(std::as_const(*this).findMedia(id));
}

const Track* Project::findTrack(Id id) const
{
    for (const auto* list : {&videoTracks, &audioTracks})
        for (const auto& t : *list)
            if (t.id == id)
                return &t;
    return nullptr;
}

Track* Project::findTrack(Id id)
{
    return const_cast<Track*>(std::as_const(*this).findTrack(id));
}

Project::ClipLocation Project::locateClip(Id clipId)
{
    for (auto* list : {&videoTracks, &audioTracks})
        for (auto& t : *list)
            for (size_t i = 0; i < t.clips.size(); ++i)
                if (t.clips[i].id == clipId)
                    return {&t, i};
    return {};
}

const Clip* Project::findClip(Id clipId) const
{
    return const_cast<Project*>(this)->locateClip(clipId).clip();
}

const Track* Project::trackOfClip(Id clipId) const
{
    return const_cast<Project*>(this)->locateClip(clipId).track;
}

FramePos Project::duration() const
{
    FramePos end = 0;
    for (const auto* list : {&videoTracks, &audioTracks})
        for (const auto& t : *list)
            if (!t.clips.empty())
                end = std::max(end, t.clips.back().end());
    return end;
}

bool Project::validate(std::string* why) const
{
    auto fail = [&](const std::string& msg) {
        if (why)
            *why = msg;
        return false;
    };
    if (settings.width <= 0 || settings.height <= 0 || settings.frameRate.num <= 0 || settings.frameRate.den <= 0)
        return fail("Ajustes de proyecto inválidos");

    std::set<Id> ids;
    auto useId = [&](Id id) { return id != kNoId && id < nextId && ids.insert(id).second; };

    for (const auto& m : media)
        if (!useId(m.id))
            return fail("Id de medio repetido o inválido: " + std::to_string(m.id));

    for (const auto* list : {&videoTracks, &audioTracks}) {
        for (const auto& t : *list) {
            if (!useId(t.id))
                return fail("Id de pista repetido o inválido: " + std::to_string(t.id));
            if ((list == &videoTracks) != (t.kind == TrackKind::Video))
                return fail("Pista en la lista equivocada: " + t.name);
            FramePos prevEnd = 0;
            for (const auto& c : t.clips) {
                if (!useId(c.id))
                    return fail("Id de clip repetido o inválido: " + std::to_string(c.id));
                if (c.duration <= 0 || c.start < 0 || c.sourceIn < 0)
                    return fail("Clip con posición o duración inválida: " + std::to_string(c.id));
                if (c.start < prevEnd)
                    return fail("Clips solapados o desordenados en " + t.name);
                prevEnd = c.end();
                const MediaItem* m = findMedia(c.mediaId);
                if (!m)
                    return fail("Clip sin medio: " + std::to_string(c.id));
                if ((t.kind == TrackKind::Video && !m->hasVideo) || (t.kind == TrackKind::Audio && !m->hasAudio))
                    return fail("Medio incompatible con la pista " + t.name);
            }
        }
    }
    return true;
}

std::string trackKindName(TrackKind kind)
{
    return kind == TrackKind::Video ? "video" : "audio";
}

} // namespace movix::core
