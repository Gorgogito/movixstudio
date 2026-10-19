#include "core/Project.h"

#include <algorithm>
#include <cmath>
#include <functional>
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
    p.mainSequence = p.activeSequence = p.newId();
    p.activeName = "Principal";
    return p;
}

const Sequence* Project::findSequence(Id id) const
{
    for (const Sequence& seq : sequences)
        if (seq.id == id)
            return &seq;
    return nullptr;
}

const std::vector<Track>* Project::videoTracksOf(Id sequence) const
{
    if (sequence == activeSequence)
        return &videoTracks;
    const Sequence* seq = findSequence(sequence);
    return seq ? &seq->videoTracks : nullptr;
}

const std::vector<Track>* Project::audioTracksOf(Id sequence) const
{
    if (sequence == activeSequence)
        return &audioTracks;
    const Sequence* seq = findSequence(sequence);
    return seq ? &seq->audioTracks : nullptr;
}

FramePos Project::sequenceDuration(Id sequence) const
{
    FramePos end = 0;
    for (const auto* list : {videoTracksOf(sequence), audioTracksOf(sequence)})
        if (list)
            for (const Track& t : *list)
                if (!t.clips.empty())
                    end = std::max(end, t.clips.back().end());
    return end;
}

std::string Project::sequenceName(Id sequence) const
{
    if (sequence == activeSequence)
        return activeName;
    const Sequence* seq = findSequence(sequence);
    return seq ? seq->name : std::string();
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

    auto checkTracks = [&](const std::vector<Track>& vtracks, const std::vector<Track>& atracks) -> bool {
    for (const auto* list : {&vtracks, &atracks}) {
        for (const auto& t : *list) {
            if (!useId(t.id))
                return fail("Id de pista repetido o inválido: " + std::to_string(t.id));
            if ((list == &vtracks) != (t.kind == TrackKind::Video))
                return fail("Pista en la lista equivocada: " + t.name);
            FramePos prevEnd = 0;
            const Clip* prev = nullptr;
            for (const auto& c : t.clips) {
                const std::string cid = std::to_string(c.id);
                if (!useId(c.id))
                    return fail("Id de clip repetido o inválido: " + cid);
                if (c.duration <= 0 || c.start < 0 || c.sourceIn < 0 || c.speed <= 0)
                    return fail("Clip con posición, duración o velocidad inválida: " + cid);
                if (c.start < prevEnd)
                    return fail("Clips solapados o desordenados en " + t.name);
                if (c.fadeIn < 0 || c.fadeOut < 0 || c.fadeIn + c.fadeOut > c.duration || c.audioFadeIn < 0 || c.audioFadeOut < 0
                    || c.audioFadeIn + c.audioFadeOut > c.duration)
                    return fail("Fundidos más largos que el clip: " + cid);
                if (c.transitionIn.active()) {
                    if (!prev || prev->end() != c.start)
                        return fail("Transición sin clip anterior contiguo: " + cid);
                    if (c.transitionIn.duration > std::min(prev->duration, c.duration))
                        return fail("Transición más larga que sus clips: " + cid);
                }
                prevEnd = c.end();
                prev = &c;
                if (c.title) {
                    if (t.kind != TrackKind::Video || c.mediaId != kNoId || c.isCompound())
                        return fail("Título fuera de una pista de video: " + cid);
                    continue;
                }
                if (c.isCompound()) {
                    if (c.mediaId != kNoId || (c.sequenceId != activeSequence && !findSequence(c.sequenceId)))
                        return fail("Clip compuesto sin secuencia: " + cid);
                    continue;
                }
                const MediaItem* m = findMedia(c.mediaId);
                if (!m)
                    return fail("Clip sin medio: " + cid);
                if ((t.kind == TrackKind::Video && !m->hasVideo) || (t.kind == TrackKind::Audio && !m->hasAudio))
                    return fail("Medio incompatible con la pista " + t.name);
            }
        }
    }
    return true;
    };
    if (!checkTracks(videoTracks, audioTracks))
        return false;
    if (activeSequence != kNoId && !useId(activeSequence))
        return fail("Id de secuencia activa inválido");
    for (const Sequence& seq : sequences) {
        if (!useId(seq.id) || seq.id == activeSequence)
            return fail("Id de secuencia repetido o inválido: " + std::to_string(seq.id));
        if (!checkTracks(seq.videoTracks, seq.audioTracks))
            return false;
    }
    if (mainSequence != activeSequence && !findSequence(mainSequence))
        return fail("Falta la secuencia principal");
    for (Id id : sequencePath)
        if (id != mainSequence && !findSequence(id))
            return fail("Camino de secuencias inválido");
    // Sin ciclos: ninguna secuencia puede contenerse a sí misma, directa o indirectamente.
    std::set<Id> visiting;
    std::function<bool(Id)> acyclic = [&](Id seq) {
        if (!visiting.insert(seq).second)
            return false;
        for (const auto* list : {videoTracksOf(seq), audioTracksOf(seq)})
            if (list)
                for (const Track& t : *list)
                    for (const Clip& c : t.clips)
                        if (c.isCompound() && !acyclic(c.sequenceId))
                            return false;
        visiting.erase(seq);
        return true;
    };
    if (!acyclic(activeSequence) || (mainSequence != kNoId && !acyclic(mainSequence)))
        return fail("Un clip compuesto se contiene a sí mismo");
    return true;
}

TimeUs sourceTimeAt(const ProjectSettings& s, const Clip& c, FramePos frame)
{
    return sourceTimeAtTime(s, c, s.frameToTime(frame));
}

TimeUs sourceTimeAtTime(const ProjectSettings& s, const Clip& c, TimeUs t)
{
    const TimeUs offset = t - s.frameToTime(c.start);
    if (c.speed == 1.0)
        return c.sourceIn + offset;
    return c.sourceIn + static_cast<TimeUs>(std::llround(double(offset) * c.speed));
}

std::string trackKindName(TrackKind kind)
{
    return kind == TrackKind::Video ? "video" : "audio";
}

} // namespace movix::core
