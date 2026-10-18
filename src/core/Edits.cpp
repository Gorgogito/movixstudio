#include "core/Edits.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace movix::core::edit {

namespace {

bool compatible(const Track& t, const MediaItem& m)
{
    return t.kind == TrackKind::Video ? m.hasVideo : m.hasAudio;
}

void insertSorted(Track& t, Clip clip)
{
    auto it = std::lower_bound(t.clips.begin(), t.clips.end(), clip.start,
                               [](const Clip& c, FramePos s) { return c.start < s; });
    t.clips.insert(it, std::move(clip));
}

// Libera el intervalo [a, b) de la pista recortando, dividiendo o quitando lo que lo ocupe.
void clearRange(Project& p, Track& t, FramePos a, FramePos b)
{
    std::vector<Clip> result;
    result.reserve(t.clips.size() + 1);
    for (const Clip& c : t.clips) {
        if (c.end() <= a || c.start >= b) {
            result.push_back(c);
            continue;
        }
        if (c.start < a) { // queda una parte a la izquierda
            Clip left = c;
            left.duration = a - c.start;
            result.push_back(left);
        }
        if (c.end() > b) { // queda una parte a la derecha
            Clip right = c;
            if (c.start < a)
                right.id = p.newId(); // la izquierda conserva el id original
            right.start = b;
            right.duration = c.end() - b;
            right.sourceIn = c.sourceIn + p.settings.frameToTime(b - c.start);
            result.push_back(right);
        }
    }
    t.clips = std::move(result);
}

} // namespace

const Clip* clipAt(const Track& track, FramePos f)
{
    auto it = std::upper_bound(track.clips.begin(), track.clips.end(), f,
                               [](FramePos v, const Clip& c) { return v < c.start; });
    if (it == track.clips.begin())
        return nullptr;
    --it;
    return f < it->end() ? &*it : nullptr;
}

Id addMedia(Project& p, MediaItem item)
{
    item.id = p.newId();
    p.media.push_back(std::move(item));
    return p.media.back().id;
}

bool removeMedia(Project& p, Id mediaId)
{
    auto it = std::find_if(p.media.begin(), p.media.end(), [&](const MediaItem& m) { return m.id == mediaId; });
    if (it == p.media.end())
        return false;
    p.media.erase(it);
    for (auto* list : {&p.videoTracks, &p.audioTracks})
        for (auto& t : *list)
            std::erase_if(t.clips, [&](const Clip& c) { return c.mediaId == mediaId; });
    return true;
}

bool setFrameRate(Project& p, Rational rate)
{
    if (rate.num <= 0 || rate.den <= 0 || rate == p.settings.frameRate)
        return false;
    const ProjectSettings old = p.settings;
    p.settings.frameRate = rate;
    const TimeUs half = p.settings.frameDuration() / 2;
    auto convert = [&](FramePos f) { return p.settings.timeToFrame(old.frameToTime(f) + half); };
    for (auto* list : {&p.videoTracks, &p.audioTracks}) {
        for (auto& t : *list) {
            for (auto& c : t.clips) {
                const FramePos start = convert(c.start);
                const FramePos end = convert(c.end());
                c.start = start;
                c.duration = std::max<FramePos>(1, end - start);
            }
            // El redondeo es monótono: solo un clip de un fotograma que se alarga a 1 puede solapar.
            for (size_t i = 1; i < t.clips.size(); ++i)
                if (t.clips[i].start < t.clips[i - 1].end())
                    t.clips[i].start = t.clips[i - 1].end();
        }
    }
    return true;
}

bool setResolution(Project& p, int width, int height)
{
    width &= ~1; // los formatos 4:2:0 exigen dimensiones pares
    height &= ~1;
    if (width < 16 || height < 16 || (width == p.settings.width && height == p.settings.height))
        return false;
    p.settings.width = width;
    p.settings.height = height;
    return true;
}

Rational guessFrameRate(double fps)
{
    constexpr Rational kStandard[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48, 1},
                                      {50, 1},       {60000, 1001}, {60, 1}, {120, 1}};
    for (const Rational& r : kStandard)
        if (std::abs(r.value() - fps) < 0.01)
            return r;
    return {std::max<std::int64_t>(1, std::llround(fps)), 1};
}

bool matchSettingsToMedia(Project& p, const MediaItem& media)
{
    if (p.duration() > 0 || !media.hasVideo || media.width <= 0 || media.height <= 0)
        return false;
    bool changed = setResolution(p, media.width, media.height);
    if (media.frameRate > 0)
        changed = setFrameRate(p, guessFrameRate(media.frameRate)) || changed;
    return changed;
}

Id addTrack(Project& p, TrackKind kind, int position)
{
    auto& list = p.tracksOf(kind);
    Track t;
    t.id = p.newId();
    t.kind = kind;
    // Nombre libre más bajo: V1, V2... / A1, A2...
    const char prefix = kind == TrackKind::Video ? 'V' : 'A';
    for (int n = 1;; ++n) {
        const std::string name = prefix + std::to_string(n);
        if (std::none_of(list.begin(), list.end(), [&](const Track& x) { return x.name == name; })) {
            t.name = name;
            break;
        }
    }
    const size_t pos = (position < 0 || size_t(position) > list.size()) ? list.size() : size_t(position);
    list.insert(list.begin() + std::ptrdiff_t(pos), std::move(t));
    return list[pos].id;
}

bool removeTrack(Project& p, Id trackId)
{
    for (auto* list : {&p.videoTracks, &p.audioTracks}) {
        auto it = std::find_if(list->begin(), list->end(), [&](const Track& t) { return t.id == trackId; });
        if (it != list->end()) {
            if (it->locked)
                return false;
            list->erase(it);
            return true;
        }
    }
    return false;
}

namespace {
template <typename F>
bool setTrackFlag(Project& p, Id trackId, bool value, F member)
{
    Track* t = p.findTrack(trackId);
    if (!t || t->*member == value)
        return false;
    t->*member = value;
    return true;
}
} // namespace

bool setTrackMuted(Project& p, Id trackId, bool muted)
{
    return setTrackFlag(p, trackId, muted, &Track::muted);
}

bool setTrackHidden(Project& p, Id trackId, bool hidden)
{
    return setTrackFlag(p, trackId, hidden, &Track::hidden);
}

bool setTrackLocked(Project& p, Id trackId, bool locked)
{
    return setTrackFlag(p, trackId, locked, &Track::locked);
}

FramePos availableFrames(const Project& p, const MediaItem& media, TimeUs sourceIn)
{
    return p.settings.framesIn(media.duration - sourceIn);
}

Id placeClip(Project& p, Id trackId, Id mediaId, FramePos start, TimeUs sourceIn, FramePos duration)
{
    Track* t = p.findTrack(trackId);
    const MediaItem* m = p.findMedia(mediaId);
    if (!t || !m || t->locked || !compatible(*t, *m) || start < 0 || sourceIn < 0)
        return kNoId;
    const FramePos available = availableFrames(p, *m, sourceIn);
    if (duration <= 0 || duration > available)
        duration = available;
    if (duration <= 0)
        return kNoId;

    Clip c;
    c.id = p.newId();
    c.mediaId = mediaId;
    c.start = start;
    c.duration = duration;
    c.sourceIn = sourceIn;
    clearRange(p, *t, c.start, c.end());
    insertSorted(*t, c);
    return c.id;
}

bool moveClips(Project& p, const std::vector<Id>& clipIds, FramePos delta, int trackOffset)
{
    struct Moving {
        Clip clip;
        TrackKind kind;
        size_t trackIndex;
    };
    std::vector<Moving> moving;
    const std::set<Id> idSet(clipIds.begin(), clipIds.end());
    for (Id id : idSet) {
        auto loc = p.locateClip(id);
        if (!loc.track || loc.track->locked)
            return false;
        auto& list = p.tracksOf(loc.track->kind);
        const size_t index = size_t(loc.track - list.data());
        moving.push_back({*loc.clip(), loc.track->kind, index});
    }
    if (moving.empty())
        return false;

    // Limitar el desplazamiento para que ningún clip quede antes del inicio.
    FramePos minStart = moving.front().clip.start;
    for (const auto& m : moving)
        minStart = std::min(minStart, m.clip.start);
    delta = std::max(delta, -minStart);
    if (delta == 0 && trackOffset == 0)
        return false;

    // Validar pistas destino.
    for (const auto& m : moving) {
        const auto& list = p.tracksOf(m.kind);
        const std::int64_t target = std::int64_t(m.trackIndex) + trackOffset;
        if (target < 0 || target >= std::int64_t(list.size()) || list[size_t(target)].locked)
            return false;
    }

    // Quitar de su posición actual y volver a colocar (sobrescribiendo el destino).
    for (auto* list : {&p.videoTracks, &p.audioTracks})
        for (auto& t : *list)
            std::erase_if(t.clips, [&](const Clip& c) { return idSet.contains(c.id); });
    std::sort(moving.begin(), moving.end(), [](const Moving& a, const Moving& b) { return a.clip.start < b.clip.start; });
    for (auto& m : moving) {
        Track& target = p.tracksOf(m.kind)[size_t(std::int64_t(m.trackIndex) + trackOffset)];
        m.clip.start += delta;
        clearRange(p, target, m.clip.start, m.clip.end());
        insertSorted(target, m.clip);
    }
    return true;
}

std::optional<FramePos> trimClip(Project& p, Id clipId, Edge edge, FramePos position)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked)
        return std::nullopt;
    Clip& c = *loc.clip();
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!m)
        return std::nullopt;
    const auto& clips = loc.track->clips;

    if (edge == Edge::Start) {
        const FramePos prevEnd = loc.index > 0 ? clips[loc.index - 1].end() : 0;
        // Fotogramas del medio anteriores a sourceIn que se pueden recuperar.
        const FramePos before = p.settings.timeToFrame(c.sourceIn);
        const FramePos lo = std::max(prevEnd, c.start - before);
        const FramePos hi = c.end() - 1;
        const FramePos newStart = std::clamp(position, lo, hi);
        if (newStart == c.start)
            return newStart;
        const FramePos shift = newStart - c.start;
        c.sourceIn = std::max<TimeUs>(0, c.sourceIn + p.settings.frameToTime(shift));
        c.start = newStart;
        c.duration -= shift;
        return newStart;
    }

    const FramePos nextStart = loc.index + 1 < clips.size() ? clips[loc.index + 1].start : INT64_MAX;
    const FramePos lo = c.start + 1;
    const FramePos hi = std::min(nextStart, c.start + availableFrames(p, *m, c.sourceIn));
    if (hi < lo)
        return std::nullopt;
    const FramePos newEnd = std::clamp(position, lo, hi);
    c.duration = newEnd - c.start;
    return newEnd;
}

Id splitClip(Project& p, Id clipId, FramePos at)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked)
        return kNoId;
    Clip& c = *loc.clip();
    if (at <= c.start || at >= c.end())
        return kNoId;
    Clip right = c;
    right.id = p.newId();
    right.start = at;
    right.duration = c.end() - at;
    right.sourceIn = c.sourceIn + p.settings.frameToTime(at - c.start);
    c.duration = at - c.start;
    loc.track->clips.insert(loc.track->clips.begin() + std::ptrdiff_t(loc.index) + 1, right);
    return right.id;
}

std::vector<Id> splitAt(Project& p, FramePos at, const std::vector<Id>& clipIds)
{
    std::vector<Id> targets = clipIds;
    if (targets.empty()) {
        for (auto* list : {&p.videoTracks, &p.audioTracks})
            for (const auto& t : *list)
                if (!t.locked)
                    if (const Clip* c = clipAt(t, at))
                        targets.push_back(c->id);
    }
    std::vector<Id> created;
    for (Id id : targets)
        if (Id right = splitClip(p, id, at); right != kNoId)
            created.push_back(right);
    return created;
}

bool deleteClips(Project& p, const std::vector<Id>& clipIds, bool ripple)
{
    // Agrupar por pista para poder cerrar huecos de derecha a izquierda.
    std::map<Track*, std::vector<Clip>> byTrack;
    for (Id id : std::set<Id>(clipIds.begin(), clipIds.end())) {
        auto loc = p.locateClip(id);
        if (!loc.track)
            continue;
        if (loc.track->locked)
            return false;
        byTrack[loc.track].push_back(*loc.clip());
    }
    if (byTrack.empty())
        return false;

    for (auto& [track, removed] : byTrack) {
        std::sort(removed.begin(), removed.end(), [](const Clip& a, const Clip& b) { return a.start > b.start; });
        for (const Clip& r : removed) {
            std::erase_if(track->clips, [&](const Clip& c) { return c.id == r.id; });
            if (ripple) {
                for (Clip& c : track->clips)
                    if (c.start >= r.end())
                        c.start -= r.duration;
            }
        }
    }
    return true;
}

bool setClipGain(Project& p, Id clipId, float gain)
{
    Clip* c = p.findClip(clipId);
    gain = std::clamp(gain, 0.0f, 4.0f);
    if (!c || c->gain == gain)
        return false;
    c->gain = gain;
    return true;
}

bool setClipEnabled(Project& p, Id clipId, bool enabled)
{
    Clip* c = p.findClip(clipId);
    if (!c || c->enabled == enabled)
        return false;
    c->enabled = enabled;
    return true;
}

} // namespace movix::core::edit
