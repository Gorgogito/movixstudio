#include "core/Edits.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace movix::core::edit {

namespace {

constexpr TimeUs kDefaultStillDuration = 5 * kUsPerSecond;

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

void forEachAnimated(Clip& c, const auto& fn)
{
    for (Animated* a : {&c.transform.x, &c.transform.y, &c.transform.scale, &c.transform.rotation, &c.transform.opacity, &c.volume})
        fn(*a);
}

// Desplaza los keyframes cuando cambia el inicio del clip, para que sigan en el mismo instante.
void shiftKeys(Clip& c, FramePos delta)
{
    if (delta == 0)
        return;
    forEachAnimated(c, [&](Animated& a) {
        for (Keyframe& k : a.keys)
            k.frame += delta;
    });
}

// Instante del medio tras avanzar `frames` fotogramas desde el inicio del clip.
TimeUs advanceSource(const Project& p, const Clip& c, FramePos frames)
{
    return std::max<TimeUs>(0, sourceTimeAt(p.settings, c, c.start + frames));
}

void clampFades(Clip& c)
{
    c.fadeIn = std::clamp<FramePos>(c.fadeIn, 0, c.duration);
    c.fadeOut = std::clamp<FramePos>(c.fadeOut, 0, c.duration - c.fadeIn);
    c.audioFadeIn = std::clamp<FramePos>(c.audioFadeIn, 0, c.duration);
    c.audioFadeOut = std::clamp<FramePos>(c.audioFadeOut, 0, c.duration - c.audioFadeIn);
}

// Parte izquierda (hasta `at`) y derecha (desde `at`) de un clip, con su contenido intacto.
Clip leftPart(const Clip& c, FramePos at)
{
    Clip left = c;
    left.duration = at - c.start;
    left.fadeOut = 0;
    left.audioFadeOut = 0;
    clampFades(left);
    return left;
}

Clip rightPart(const Project& p, const Clip& c, FramePos at, Id newId)
{
    Clip right = c;
    right.id = newId;
    right.start = at;
    right.duration = c.end() - at;
    right.sourceIn = advanceSource(p, c, at - c.start);
    right.fadeIn = 0;
    right.audioFadeIn = 0;
    right.transitionIn = {};
    shiftKeys(right, -(at - c.start));
    clampFades(right);
    return right;
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
        if (c.start < a)
            result.push_back(leftPart(c, a));
        if (c.end() > b) // la izquierda conserva el id original; la derecha recibe uno nuevo
            result.push_back(rightPart(p, c, b, c.start < a ? p.newId() : c.id));
    }
    t.clips = std::move(result);
}

// Abre un hueco de `length` fotogramas en `at` desplazando lo posterior (modo inserción).
void openGap(Project& p, Track& t, FramePos at, FramePos length)
{
    for (size_t i = 0; i < t.clips.size(); ++i) {
        Clip& c = t.clips[i];
        if (c.start < at && c.end() > at) { // el clip que cruza el punto se divide
            Clip right = rightPart(p, c, at, p.newId());
            c = leftPart(c, at);
            t.clips.insert(t.clips.begin() + std::ptrdiff_t(i) + 1, right);
            break;
        }
    }
    for (Clip& c : t.clips)
        if (c.start >= at)
            c.start += length;
}

Id placePrepared(Project& p, Track& t, Clip c, PlaceMode mode)
{
    if (mode == PlaceMode::Insert)
        openGap(p, t, c.start, c.duration);
    else
        clearRange(p, t, c.start, c.end());
    const Id id = c.id;
    insertSorted(t, std::move(c));
    normalize(p);
    return id;
}

void clampValue(Property prop, double& v)
{
    switch (prop) {
    case Property::Scale: v = std::clamp(v, 0.01, 20.0); break;
    case Property::Opacity: v = std::clamp(v, 0.0, 1.0); break;
    case Property::Volume: v = std::clamp(v, 0.0, 4.0); break;
    case Property::PositionX:
    case Property::PositionY: v = std::clamp(v, -5.0, 5.0); break;
    case Property::Rotation: v = std::clamp(v, -3600.0, 3600.0); break;
    }
}

double defaultValue(Property prop)
{
    return (prop == Property::Scale || prop == Property::Opacity || prop == Property::Volume) ? 1.0 : 0.0;
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

void normalize(Project& p)
{
    for (auto* list : {&p.videoTracks, &p.audioTracks}) {
        for (auto& t : *list) {
            for (size_t i = 0; i < t.clips.size(); ++i) {
                Clip& c = t.clips[i];
                clampFades(c);
                if (!c.transitionIn.active()) {
                    c.transitionIn = {};
                    continue;
                }
                const Clip* prev = i > 0 ? &t.clips[i - 1] : nullptr;
                if (!prev || prev->end() != c.start) {
                    c.transitionIn = {}; // ya no hay corte con el clip anterior
                    continue;
                }
                c.transitionIn.duration = std::min({c.transitionIn.duration, prev->duration, c.duration});
            }
        }
    }
}

// --- Medios ---

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
            std::erase_if(t.clips, [&](const Clip& c) { return !c.title && c.mediaId == mediaId; });
    normalize(p);
    return true;
}

// --- Ajustes ---

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
                // Duraciones y keyframes (relativos al clip) se convierten igual que las posiciones.
                auto rel = [&](FramePos r) { return convert(c.start + r) - start; };
                forEachAnimated(c, [&](Animated& a) {
                    for (Keyframe& k : a.keys)
                        k.frame = rel(k.frame);
                });
                c.fadeIn = rel(c.fadeIn);
                c.fadeOut = c.fadeOut ? end - convert(c.end() - c.fadeOut) : 0;
                c.audioFadeIn = rel(c.audioFadeIn);
                c.audioFadeOut = c.audioFadeOut ? end - convert(c.end() - c.audioFadeOut) : 0;
                c.transitionIn.duration = rel(c.transitionIn.duration);
                c.start = start;
                c.duration = std::max<FramePos>(1, end - start);
            }
            // El redondeo es monótono: solo un clip de un fotograma que se alarga a 1 puede solapar.
            for (size_t i = 1; i < t.clips.size(); ++i)
                if (t.clips[i].start < t.clips[i - 1].end())
                    t.clips[i].start = t.clips[i - 1].end();
        }
    }
    normalize(p);
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
    if (media.frameRate > 0 && !media.isStill)
        changed = setFrameRate(p, guessFrameRate(media.frameRate)) || changed;
    return changed;
}

// --- Pistas ---

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
template <typename T, typename F>
bool setTrackField(Project& p, Id trackId, T value, F member)
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
    return setTrackField(p, trackId, muted, &Track::muted);
}

bool setTrackHidden(Project& p, Id trackId, bool hidden)
{
    return setTrackField(p, trackId, hidden, &Track::hidden);
}

bool setTrackLocked(Project& p, Id trackId, bool locked)
{
    return setTrackField(p, trackId, locked, &Track::locked);
}

bool setTrackVolume(Project& p, Id trackId, double volume)
{
    return setTrackField(p, trackId, std::clamp(volume, 0.0, 4.0), &Track::volume);
}

// --- Clips ---

FramePos availableFrames(const Project& p, const MediaItem& media, TimeUs sourceIn, double speed)
{
    if (media.isStill)
        return p.settings.framesIn(kUnlimitedDuration);
    const TimeUs remaining = media.duration - sourceIn;
    return p.settings.framesIn(speed == 1.0 ? remaining : TimeUs(double(remaining) / speed));
}

FramePos maxClipFrames(const Project& p, const Clip& c)
{
    if (c.title)
        return p.settings.framesIn(kUnlimitedDuration);
    const MediaItem* m = p.findMedia(c.mediaId);
    return m ? availableFrames(p, *m, c.sourceIn, c.speed) : c.duration;
}

Id placeClip(Project& p, Id trackId, Id mediaId, FramePos start, TimeUs sourceIn, FramePos duration, PlaceMode mode)
{
    Track* t = p.findTrack(trackId);
    const MediaItem* m = p.findMedia(mediaId);
    if (!t || !m || t->locked || !compatible(*t, *m) || start < 0 || sourceIn < 0)
        return kNoId;
    const FramePos available = availableFrames(p, *m, sourceIn);
    if (duration <= 0)
        duration = m->isStill ? p.settings.framesIn(kDefaultStillDuration) : available;
    duration = std::min(duration, available);
    if (duration <= 0)
        return kNoId;

    Clip c;
    c.id = p.newId();
    c.mediaId = mediaId;
    c.start = start;
    c.duration = duration;
    c.sourceIn = m->isStill ? 0 : sourceIn;
    return placePrepared(p, *t, std::move(c), mode);
}

Id placeTitle(Project& p, Id trackId, FramePos start, FramePos duration, const TitleStyle& style, PlaceMode mode)
{
    Track* t = p.findTrack(trackId);
    if (!t || t->locked || t->kind != TrackKind::Video || start < 0 || duration <= 0)
        return kNoId;
    Clip c;
    c.id = p.newId();
    c.start = start;
    c.duration = duration;
    c.title = style;
    return placePrepared(p, *t, std::move(c), mode);
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

    // Validar pistas destino (un título solo puede ir a pistas de video, y así es por tipo).
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
    normalize(p);
    return true;
}

std::optional<FramePos> trimClip(Project& p, Id clipId, Edge edge, FramePos position)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked)
        return std::nullopt;
    Clip& c = *loc.clip();
    const MediaItem* m = c.title ? nullptr : p.findMedia(c.mediaId);
    if (!c.title && !m)
        return std::nullopt;
    const bool unlimited = c.title || (m && m->isStill);
    const auto& clips = loc.track->clips;

    if (edge == Edge::Start) {
        const FramePos prevEnd = loc.index > 0 ? clips[loc.index - 1].end() : 0;
        // Fotogramas del medio anteriores a sourceIn que se pueden recuperar.
        const FramePos before = unlimited ? c.start : p.settings.timeToFrame(TimeUs(double(c.sourceIn) / c.speed));
        const FramePos lo = std::max(prevEnd, c.start - before);
        const FramePos hi = c.end() - 1;
        const FramePos newStart = std::clamp(position, lo, hi);
        if (newStart == c.start)
            return newStart;
        const FramePos shift = newStart - c.start;
        if (!unlimited)
            c.sourceIn = advanceSource(p, c, shift);
        shiftKeys(c, -shift);
        c.start = newStart;
        c.duration -= shift;
        normalize(p);
        return newStart;
    }

    const FramePos nextStart = loc.index + 1 < clips.size() ? clips[loc.index + 1].start : INT64_MAX;
    const FramePos lo = c.start + 1;
    const FramePos hi = std::min(nextStart, c.start + maxClipFrames(p, c));
    if (hi < lo)
        return std::nullopt;
    const FramePos newEnd = std::clamp(position, lo, hi);
    c.duration = newEnd - c.start;
    normalize(p);
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
    Clip right = rightPart(p, c, at, p.newId());
    c = leftPart(c, at);
    loc.track->clips.insert(loc.track->clips.begin() + std::ptrdiff_t(loc.index) + 1, right);
    normalize(p);
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
    normalize(p);
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

// --- Propiedades animables ---

Animated* property(Clip& c, Property prop)
{
    switch (prop) {
    case Property::PositionX: return &c.transform.x;
    case Property::PositionY: return &c.transform.y;
    case Property::Scale: return &c.transform.scale;
    case Property::Rotation: return &c.transform.rotation;
    case Property::Opacity: return &c.transform.opacity;
    case Property::Volume: return &c.volume;
    }
    return nullptr;
}

const Animated* property(const Clip& c, Property prop)
{
    return property(const_cast<Clip&>(c), prop);
}

bool setProperty(Project& p, Id clipId, Property prop, FramePos rel, double value)
{
    Clip* c = p.findClip(clipId);
    if (!c || p.trackOfClip(clipId)->locked)
        return false;
    clampValue(prop, value);
    Animated* a = property(*c, prop);
    const Animated before = *a;
    a->set(rel, value);
    return !(*a == before);
}

bool toggleKeyframe(Project& p, Id clipId, Property prop, FramePos rel)
{
    Clip* c = p.findClip(clipId);
    if (!c || p.trackOfClip(clipId)->locked)
        return false;
    Animated* a = property(*c, prop);
    if (a->keyAt(rel))
        return a->removeKey(rel);
    a->setKey(rel, a->at(rel));
    return true;
}

bool setKeyframeInterpolation(Project& p, Id clipId, Property prop, FramePos rel, Interpolation interp)
{
    Clip* c = p.findClip(clipId);
    if (!c)
        return false;
    Animated* a = property(*c, prop);
    const Keyframe* k = a->keyAt(rel);
    if (!k || k->interp == interp)
        return false;
    a->setKey(rel, k->value, interp);
    return true;
}

bool resetProperty(Project& p, Id clipId, Property prop)
{
    Clip* c = p.findClip(clipId);
    if (!c)
        return false;
    Animated* a = property(*c, prop);
    const Animated reset(defaultValue(prop));
    if (*a == reset)
        return false;
    *a = reset;
    return true;
}

// --- Velocidad, fundidos, transiciones, títulos ---

bool setClipSpeed(Project& p, Id clipId, double speed)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked)
        return false;
    Clip& c = *loc.clip();
    speed = std::clamp(speed, 0.1, 16.0);
    if (c.title || std::abs(speed - c.speed) < 1e-9)
        return false;
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!m || m->isStill)
        return false;
    const double ratio = c.speed / speed; // > 1 al ralentizar: el clip se alarga
    const auto& clips = loc.track->clips;
    const FramePos nextStart = loc.index + 1 < clips.size() ? clips[loc.index + 1].start : INT64_MAX;
    FramePos duration = std::max<FramePos>(1, std::llround(double(c.duration) * ratio));
    duration = std::min({duration, availableFrames(p, *m, c.sourceIn, speed), nextStart - c.start});
    forEachAnimated(c, [&](Animated& a) {
        for (Keyframe& k : a.keys)
            k.frame = std::llround(double(k.frame) * ratio);
    });
    c.speed = speed;
    c.duration = std::max<FramePos>(1, duration);
    normalize(p);
    return true;
}

bool setFades(Project& p, Id clipId, FramePos fadeIn, FramePos fadeOut)
{
    Clip* c = p.findClip(clipId);
    if (!c)
        return false;
    const Clip before = *c;
    c->fadeIn = std::max<FramePos>(0, fadeIn);
    c->fadeOut = std::max<FramePos>(0, fadeOut);
    clampFades(*c);
    return !(*c == before);
}

bool setAudioFades(Project& p, Id clipId, FramePos fadeIn, FramePos fadeOut)
{
    Clip* c = p.findClip(clipId);
    if (!c)
        return false;
    const Clip before = *c;
    c->audioFadeIn = std::max<FramePos>(0, fadeIn);
    c->audioFadeOut = std::max<FramePos>(0, fadeOut);
    clampFades(*c);
    return !(*c == before);
}

bool setTransition(Project& p, Id clipId, TransitionType type, FramePos duration)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked)
        return false;
    Clip& c = *loc.clip();
    TransitionIn next;
    if (type != TransitionType::None) {
        const Clip* prev = loc.index > 0 ? &loc.track->clips[loc.index - 1] : nullptr;
        if (!prev || prev->end() != c.start)
            return false; // solo en un corte entre dos clips contiguos
        const FramePos longest = std::min(prev->duration, c.duration);
        if (longest < 2)
            return false; // hace falta al menos un fotograma a cada lado del corte
        next.type = type;
        next.duration = std::clamp<FramePos>(duration, 2, longest);
    }
    if (c.transitionIn == next)
        return false;
    c.transitionIn = next;
    return true;
}

bool setTitleStyle(Project& p, Id clipId, const TitleStyle& style)
{
    Clip* c = p.findClip(clipId);
    if (!c || !c->title || *c->title == style)
        return false;
    c->title = style;
    return true;
}

Id detachAudio(Project& p, Id clipId)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->kind != TrackKind::Video || loc.track->locked)
        return kNoId;
    Clip& c = *loc.clip();
    const MediaItem* m = c.title ? nullptr : p.findMedia(c.mediaId);
    if (!m || !m->hasAudio || c.audioDetached)
        return kNoId;
    const Clip source = c;

    // Primera pista de audio libre en el intervalo del clip; si no hay, una nueva.
    Track* target = nullptr;
    for (Track& t : p.audioTracks) {
        if (t.locked)
            continue;
        const bool free = std::none_of(t.clips.begin(), t.clips.end(),
                                       [&](const Clip& x) { return x.start < source.end() && x.end() > source.start; });
        if (free) {
            target = &t;
            break;
        }
    }
    if (!target)
        target = p.findTrack(addTrack(p, TrackKind::Audio));

    Clip audio;
    audio.id = p.newId();
    audio.mediaId = source.mediaId;
    audio.start = source.start;
    audio.duration = source.duration;
    audio.sourceIn = source.sourceIn;
    audio.speed = source.speed;
    audio.volume = source.volume;
    audio.audioFadeIn = source.audioFadeIn;
    audio.audioFadeOut = source.audioFadeOut;
    insertSorted(*target, audio);

    Clip* original = p.findClip(clipId); // la inserción pudo reubicar vectores de otras pistas
    original->audioDetached = true;
    normalize(p);
    return audio.id;
}

// --- Portapapeles ---

Clipboard copyClips(const Project& p, const std::vector<Id>& clipIds)
{
    Clipboard cb;
    bool first = true;
    for (Id id : std::set<Id>(clipIds.begin(), clipIds.end())) {
        const Track* t = p.trackOfClip(id);
        const Clip* c = p.findClip(id);
        if (!t || !c)
            continue;
        const auto& list = p.tracksOf(t->kind);
        cb.items.push_back({*c, t->kind, int(t - list.data())});
        cb.origin = first ? c->start : std::min(cb.origin, c->start);
        first = false;
    }
    return cb;
}

std::vector<Id> pasteClips(Project& p, const Clipboard& cb, FramePos at)
{
    std::vector<Id> created;
    for (const Clipboard::Item& item : cb.items) {
        if (!item.clip.title && !p.findMedia(item.clip.mediaId))
            continue; // el medio ya no está en el proyecto
        auto& list = p.tracksOf(item.kind);
        while (int(list.size()) <= item.trackIndex)
            addTrack(p, item.kind);
        Track& t = p.tracksOf(item.kind)[size_t(item.trackIndex)];
        if (t.locked)
            continue;
        Clip c = item.clip;
        c.id = p.newId();
        c.start = std::max<FramePos>(0, at + (item.clip.start - cb.origin));
        clearRange(p, t, c.start, c.end());
        insertSorted(t, c);
        created.push_back(c.id);
    }
    normalize(p);
    return created;
}

} // namespace movix::core::edit
