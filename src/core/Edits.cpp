#include "core/Edits.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace movix::core::edit {

bool sequenceContains(const Project& p, Id sequence, Id target);

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
    Transform& t = c.transform;
    for (Animated* a : {&t.x, &t.y, &t.scale, &t.rotation, &t.opacity, &t.cropLeft, &t.cropTop, &t.cropRight, &t.cropBottom, &c.volume,
                        &c.speedRamp})
        fn(*a);
    for (Mask& m : c.masks)
        for (Animated* a : {&m.x, &m.y, &m.width, &m.height, &m.rotation, &m.feather})
            fn(*a);
    for (OfxEffect& e : c.ofx)
        for (auto& [name, components] : e.params)
            for (Animated& a : components)
                fn(a);
}

// Fotograma (relativo al clip) de cada keyframe del clip: propiedades y corrección de color.
void forEachKeyFrame(Clip& c, const auto& fn)
{
    forEachAnimated(c, [&](Animated& a) {
        for (Keyframe& k : a.keys)
            fn(k.frame);
    });
    for (GradeKey& k : c.color.keys)
        fn(k.frame);
    for (Mask& m : c.masks)
        for (PathKey& k : m.pathKeys)
            fn(k.frame);
}

// Desplaza los keyframes cuando cambia el inicio del clip, para que sigan en el mismo instante.
void shiftKeys(Clip& c, FramePos delta)
{
    if (delta == 0)
        return;
    forEachKeyFrame(c, [&](std::int64_t& f) { f += delta; });
}

// Inicio (en el medio) del tramo del clip tras avanzar `frames` fotogramas su comienzo: hacia delante
// avanza; al revés, el comienzo del clip muestra el final del tramo, así que sourceIn no cambia.
TimeUs advanceSource(const Project& p, const Clip& c, FramePos frames)
{
    if (c.reverse)
        return c.sourceIn;
    return std::max<TimeUs>(0, c.sourceIn + sourceAdvance(p.settings, c, p.settings.frameToTime(frames)));
}

void clampFades(Clip& c)
{
    c.fadeIn = std::clamp<FramePos>(c.fadeIn, 0, c.duration);
    c.fadeOut = std::clamp<FramePos>(c.fadeOut, 0, c.duration - c.fadeIn);
    c.audioFadeIn = std::clamp<FramePos>(c.audioFadeIn, 0, c.duration);
    c.audioFadeOut = std::clamp<FramePos>(c.audioFadeOut, 0, c.duration - c.audioFadeIn);
}

// Parte izquierda (hasta `at`) y derecha (desde `at`) de un clip, con su contenido intacto.
Clip leftPart(const Project& p, const Clip& c, FramePos at)
{
    Clip left = c;
    left.duration = at - c.start;
    if (c.reverse) // la izquierda se queda con el final del tramo del medio
        left.sourceIn = c.sourceIn + clipSourceSpan(p.settings, c) - clipSourceSpan(p.settings, left);
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
            result.push_back(leftPart(p, c, a));
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
            c = leftPart(p, c, at);
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
    case Property::Speed: v = std::clamp(v, 0.1, 16.0); break;
    case Property::PositionX:
    case Property::PositionY: v = std::clamp(v, -5.0, 5.0); break;
    case Property::Rotation:
    case Property::MaskRotation: v = std::clamp(v, -3600.0, 3600.0); break;
    case Property::CropLeft:
    case Property::CropTop:
    case Property::CropRight:
    case Property::CropBottom: v = std::clamp(v, 0.0, 0.99); break;
    case Property::MaskX:
    case Property::MaskY: v = std::clamp(v, -2.0, 3.0); break;
    case Property::MaskWidth:
    case Property::MaskHeight: v = std::clamp(v, 0.0, 4.0); break;
    case Property::MaskFeather: v = std::clamp(v, 0.0, 1.0); break;
    }
}

double defaultValue(Property prop)
{
    switch (prop) {
    case Property::Scale:
    case Property::Opacity:
    case Property::Volume:
    case Property::Speed: return 1.0;
    case Property::MaskX:
    case Property::MaskY: return 0.5;
    case Property::MaskWidth:
    case Property::MaskHeight: return 0.6;
    case Property::MaskFeather: return 0.02;
    default: return 0.0;
    }
}

bool isEditableVideoClip(Project& p, Id clipId, Clip*& clip)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->kind != TrackKind::Video || loc.track->locked)
        return false;
    clip = loc.clip();
    return true;
}

// Curva sin efecto (vacía, o solo la diagonal) → vacía; si no, ordenada y dentro de 0..1.
void sanitizeCurve(std::vector<CurvePoint>& curve)
{
    for (CurvePoint& pt : curve) {
        pt.x = std::clamp(pt.x, 0.0, 1.0);
        pt.y = std::clamp(pt.y, 0.0, 1.0);
    }
    std::stable_sort(curve.begin(), curve.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    curve.erase(std::unique(curve.begin(), curve.end(), [](const CurvePoint& a, const CurvePoint& b) { return std::abs(a.x - b.x) < 1e-4; }),
                curve.end());
    if (std::all_of(curve.begin(), curve.end(), [](const CurvePoint& pt) { return std::abs(pt.x - pt.y) < 1e-6; }))
        curve.clear();
}

void sanitizeWheel(ColorWheel& w)
{
    for (double* v : {&w.r, &w.g, &w.b, &w.master})
        *v = std::clamp(*v, -1.0, 1.0);
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
    // Secuencias en uso: la principal, la activa, las del camino y las que contienen sus compuestos.
    std::set<Id> used;
    std::vector<Id> pending{p.mainSequence, p.activeSequence};
    pending.insert(pending.end(), p.sequencePath.begin(), p.sequencePath.end());
    while (!pending.empty()) {
        const Id seq = pending.back();
        pending.pop_back();
        if (seq == kNoId || !used.insert(seq).second)
            continue;
        for (const auto* list : {p.videoTracksOf(seq), p.audioTracksOf(seq)})
            if (list)
                for (const Track& t : *list)
                    for (const Clip& c : t.clips)
                        if (c.isCompound())
                            pending.push_back(c.sequenceId);
    }
    std::erase_if(p.sequences, [&](const Sequence& seq) { return !used.contains(seq.id); });

    p.forEachTrack([&](Track& t) {
        {
            for (size_t i = 0; i < t.clips.size(); ++i) {
                Clip& c = t.clips[i];
                if (c.isCompound()) // la secuencia pudo acortarse mientras se editaba
                    c.duration = std::clamp<FramePos>(maxClipFrames(p, c), 1, c.duration);
                if (!c.hasRamp())
                    c.speedRamp.value = c.speed; // el primer keyframe parte de la velocidad fija
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
    });
}

bool sequenceContains(const Project& p, Id sequence, Id target)
{
    if (sequence == target)
        return true;
    for (const auto* list : {p.videoTracksOf(sequence), p.audioTracksOf(sequence)})
        if (list)
            for (const Track& t : *list)
                for (const Clip& c : t.clips)
                    if (c.isCompound() && sequenceContains(p, c.sequenceId, target))
                        return true;
    return false;
}

// --- Clips compuestos ---

namespace {

Track newTrack(Project& p, TrackKind kind, int number)
{
    Track t;
    t.id = p.newId();
    t.kind = kind;
    t.name = (kind == TrackKind::Video ? "V" : "A") + std::to_string(number);
    return t;
}

bool rangeFree(const Track& t, FramePos a, FramePos b)
{
    return std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.start < b && c.end() > a; });
}

} // namespace

Id createCompound(Project& p, const std::vector<Id>& clipIds, const std::string& name)
{
    struct Pick {
        TrackKind kind;
        size_t track;
        Clip clip;
    };
    std::vector<Pick> picks;
    for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        auto& tracks = p.tracksOf(kind);
        for (size_t ti = 0; ti < tracks.size(); ++ti)
            for (const Clip& c : tracks[ti].clips)
                if (std::find(clipIds.begin(), clipIds.end(), c.id) != clipIds.end()) {
                    if (tracks[ti].locked)
                        return kNoId;
                    picks.push_back({kind, ti, c});
                }
    }
    if (picks.empty())
        return kNoId;
    FramePos from = INT64_MAX, to = 0;
    size_t minIndex[2] = {SIZE_MAX, SIZE_MAX}, maxIndex[2] = {0, 0};
    for (const Pick& pk : picks) {
        from = std::min(from, pk.clip.start);
        to = std::max(to, pk.clip.end());
        const int k = pk.kind == TrackKind::Video ? 0 : 1;
        minIndex[k] = std::min(minIndex[k], pk.track);
        maxIndex[k] = std::max(maxIndex[k], pk.track);
    }
    const bool hasVideo = minIndex[0] != SIZE_MAX;
    const bool hasAudio = minIndex[1] != SIZE_MAX;

    // La secuencia nueva conserva las distancias y el orden de las pistas (siempre con al menos
    // una pista de cada tipo, para poder seguir editándola).
    Sequence seq;
    seq.id = p.newId();
    seq.name = name;
    const size_t videoCount = hasVideo ? maxIndex[0] - minIndex[0] + 1 : 1;
    const size_t audioCount = hasAudio ? maxIndex[1] - minIndex[1] + 1 : 1;
    for (size_t i = 0; i < videoCount; ++i)
        seq.videoTracks.push_back(newTrack(p, TrackKind::Video, int(i) + 1));
    for (size_t i = 0; i < audioCount; ++i)
        seq.audioTracks.push_back(newTrack(p, TrackKind::Audio, int(i) + 1));
    for (Pick& pk : picks) {
        const int k = pk.kind == TrackKind::Video ? 0 : 1;
        Clip c = pk.clip;
        c.start -= from;
        auto& list = pk.kind == TrackKind::Video ? seq.videoTracks : seq.audioTracks;
        insertSorted(list[pk.track - minIndex[k]], std::move(c));
        auto& source = p.tracksOf(pk.kind)[pk.track].clips;
        std::erase_if(source, [&](const Clip& x) { return x.id == pk.clip.id; });
    }

    Clip compound;
    compound.id = p.newId();
    compound.sequenceId = seq.id;
    compound.start = from;
    compound.duration = to - from;
    const TrackKind kind = hasVideo ? TrackKind::Video : TrackKind::Audio;
    const size_t index = hasVideo ? minIndex[0] : minIndex[1];
    Track* target = &p.tracksOf(kind)[index];
    if (!rangeFree(*target, from, to)) // ocupado por clips que no se eligieron: pista nueva
        target = p.findTrack(addTrack(p, kind));
    const Id id = compound.id;
    p.sequences.push_back(std::move(seq));
    insertSorted(*target, std::move(compound));
    normalize(p);
    return id;
}

namespace {

// Recorrido del tramo de un clip: instante del origen (µs) que se ve en cada instante del clip
// (µs desde su comienzo), con la velocidad (también variable) y la marcha atrás. Precalculado por
// fotogramas, igual que sourceAdvance, para poder evaluarlo e invertirlo muchas veces.
class SourceMap {
public:
    SourceMap(const ProjectSettings& s, const Clip& c) : m_s(s), m_c(c), m_span(double(clipSourceSpan(s, c)))
    {
        m_advance.reserve(size_t(c.duration) + 2);
        double acc = 0.0;
        for (FramePos k = 0; k <= c.duration + 1; ++k) {
            m_advance.push_back(acc);
            acc += double(s.frameToTime(k + 1) - s.frameToTime(k)) * (c.speedAt(k) + c.speedAt(k + 1)) / 2.0;
        }
    }
    double length() const { return double(m_s.frameToTime(m_c.duration)); }
    double source(double t) const
    {
        const double advance = m_c.hasRamp() ? rampAdvance(t) : t * m_c.speed;
        return double(m_c.sourceIn) + (m_c.reverse ? m_span - advance : advance);
    }
    // Instante del clip que muestra `source` (el recorrido es monótono).
    double timeOf(double source) const
    {
        double lo = 0.0, hi = length();
        const bool rising = !m_c.reverse;
        for (int i = 0; i < 64; ++i) {
            const double mid = (lo + hi) / 2;
            ((this->source(mid) < source) == rising ? lo : hi) = mid;
        }
        return (lo + hi) / 2;
    }

private:
    double rampAdvance(double t) const
    {
        if (t <= 0)
            return t * m_c.speedAt(0);
        const FramePos k = std::min<FramePos>(m_s.timeToFrame(TimeUs(t)), FramePos(m_advance.size()) - 2);
        const double a = double(m_s.frameToTime(k)), b = double(m_s.frameToTime(k + 1));
        const double v0 = m_c.speedAt(k), v1 = m_c.speedAt(k + 1);
        const double end = std::min(t, b);
        const double vEnd = v0 + (v1 - v0) * (end - a) / (b - a);
        return m_advance[size_t(k)] + (end - a) * (v0 + vEnd) / 2.0 + (t > b ? (t - b) * v1 : 0.0);
    }

    const ProjectSettings& m_s;
    const Clip& m_c;
    double m_span;
    std::vector<double> m_advance; // avance acumulado al empezar cada fotograma
};

// Cambia los fotogramas de unos keyframes; si el orden se invierte (clip al revés), cada tramo
// pasa a empezar en el otro extremo y se lleva su interpolación.
template <class Key>
void remapKeys(std::vector<Key>& keys, const auto& map, bool flip)
{
    for (Key& k : keys)
        k.frame = map(k.frame);
    if (!flip)
        return;
    std::reverse(keys.begin(), keys.end());
    for (size_t i = 0; i + 1 < keys.size(); ++i)
        keys[i].interp = keys[i + 1].interp;
    if (!keys.empty())
        keys.back().interp = Interpolation::Linear;
}

// Un clip de la secuencia de un compuesto (en sus fotogramas) tal como lo muestra el compuesto en la
// línea de tiempo: su tramo del origen recorrido con la velocidad y la dirección compuestas.
// nullopt si en la línea de tiempo ocupa menos de un fotograma.
std::optional<Clip> retimeForCompound(const Project& p, const Clip& comp, const SourceMap& compMap, const Clip& piece)
{
    const ProjectSettings& s = p.settings;
    const double fps = s.frameRate.value();
    const double compStart = double(s.frameToTime(comp.start));
    const double pieceStart = double(s.frameToTime(piece.start));
    auto frameOf = [&](double t) { return FramePos(std::llround(t / 1e6 * fps)); };
    // Fotograma relativo al compuesto en que se ve el instante `nested` de su secuencia.
    auto compFrame = [&](double nested) { return frameOf(compMap.timeOf(nested)); };
    const FramePos fa = compFrame(pieceStart), fb = compFrame(double(s.frameToTime(piece.end())));
    const FramePos lo = std::min(fa, fb), hi = std::max(fa, fb);
    if (hi <= lo)
        return std::nullopt;
    const bool flip = comp.reverse;

    const SourceMap pieceMap(s, piece);
    // Instante del origen de la pieza en el instante `u` (µs) del clip nuevo.
    const double newStart = double(s.frameToTime(comp.start + lo));
    auto sourceAt = [&](double u) { return pieceMap.source(compMap.source(newStart - compStart + u) - pieceStart); };
    Clip out = piece;
    out.start = comp.start + lo;
    out.duration = hi - lo;
    const double length = double(s.frameToTime(out.start + out.duration)) - newStart;
    const double s0 = sourceAt(0.0), s1 = sourceAt(length);
    out.reverse = s1 < s0;
    if (piece.title)
        out.reverse = false; // los títulos no tienen tramo que recorrer
    else
        out.sourceIn = TimeUs(std::llround(std::min(s0, s1)));
    const double span = std::abs(s1 - s0);
    out.speed = std::clamp(span / length, 0.01, 100.0);
    out.speedRamp = Animated(out.speed);
    if (comp.hasRamp() || piece.hasRamp()) {
        // Velocidad en el comienzo de cada fotograma (como speedAt) y sin los keyframes que sobran.
        const double h = double(s.frameDuration()) / 2;
        std::vector<Keyframe> keys;
        for (FramePos k = 0; k <= out.duration; ++k) {
            const double t = double(s.frameToTime(out.start + k)) - newStart;
            const double a = std::max(0.0, t - h), b = std::min(length, t + h);
            const double v = b > a ? std::abs(sourceAt(b) - sourceAt(a)) / (b - a) : out.speed;
            keys.push_back({k, std::clamp(v, 0.01, 100.0), Interpolation::Linear});
        }
        std::vector<Keyframe> kept;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (!kept.empty() && i + 1 < keys.size()) {
                const Keyframe& a = kept.back();
                const Keyframe& b = keys[i + 1];
                const double expected = a.value + (b.value - a.value) * double(keys[i].frame - a.frame) / double(b.frame - a.frame);
                if (std::abs(expected - keys[i].value) < 1e-4)
                    continue;
            }
            kept.push_back(keys[i]);
        }
        out.speedRamp.keys = std::move(kept);
        if (out.speedRamp.keys.size() > 1) {
            // Ajuste fino: el tramo recorrido es exactamente el de la pieza.
            const double got = double(clipSourceSpan(s, out));
            if (got > 0)
                for (Keyframe& k : out.speedRamp.keys)
                    k.value *= span / got;
        } else {
            out.speedRamp.keys.clear();
        }
    }

    // Keyframes, fundidos y transición siguen al contenido.
    auto map = [&](std::int64_t rel) { return compFrame(double(s.frameToTime(piece.start + rel))) - lo; };
    Transform& tr = out.transform;
    for (Animated* a : {&tr.x, &tr.y, &tr.scale, &tr.rotation, &tr.opacity, &tr.cropLeft, &tr.cropTop, &tr.cropRight, &tr.cropBottom,
                        &out.volume})
        remapKeys(a->keys, map, flip);
    for (Mask& m : out.masks) {
        for (Animated* a : {&m.x, &m.y, &m.width, &m.height, &m.rotation, &m.feather})
            remapKeys(a->keys, map, flip);
        remapKeys(m.pathKeys, map, flip);
    }
    for (OfxEffect& e : out.ofx)
        for (auto& [name, components] : e.params)
            for (Animated& a : components)
                remapKeys(a.keys, map, flip);
    remapKeys(out.color.keys, map, flip);
    const double scale = double(out.duration) / double(piece.duration);
    auto scaled = [&](FramePos f) { return FramePos(std::llround(double(f) * scale)); };
    out.fadeIn = scaled(flip ? piece.fadeOut : piece.fadeIn);
    out.fadeOut = scaled(flip ? piece.fadeIn : piece.fadeOut);
    out.audioFadeIn = scaled(flip ? piece.audioFadeOut : piece.audioFadeIn);
    out.audioFadeOut = scaled(flip ? piece.audioFadeIn : piece.audioFadeOut);
    out.transitionIn.duration = scaled(out.transitionIn.duration);
    clampFades(out);
    return out;
}
} // namespace

std::vector<Id> breakCompound(Project& p, Id clipId)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked)
        return {};
    const Clip comp = *loc.clip();
    if (!comp.isCompound())
        return {};
    const auto* nv = p.videoTracksOf(comp.sequenceId);
    const auto* na = p.audioTracksOf(comp.sequenceId);
    if (!nv || !na)
        return {};
    std::vector<Track> nestedVideo = *nv, nestedAudio = *na; // copias: el proyecto cambia abajo
    if (comp.isMulticam()) {
        // Solo el ángulo que se ve y el audio que suena.
        const int videoCount = int(nestedVideo.size());
        const int source = comp.audioAngle >= 0 ? comp.audioAngle : comp.angle;
        std::vector<Track> video, audio;
        if (comp.angle < videoCount)
            video.push_back(nestedVideo[size_t(comp.angle)]);
        if (source != comp.angle) {
            for (Track& t : video)
                for (Clip& c : t.clips)
                    c.audioDetached = true; // su audio no es el que suena
            if (source < videoCount) {
                Track t = nestedVideo[size_t(source)];
                t.kind = TrackKind::Audio;
                std::erase_if(t.clips, [&](const Clip& c) {
                    const MediaItem* m = c.title || c.isCompound() ? nullptr : p.findMedia(c.mediaId);
                    return !m || !m->hasAudio || c.audioDetached;
                });
                for (Clip& c : t.clips)
                    c.fadeIn = c.fadeOut = 0;
                audio.push_back(std::move(t));
            } else if (source - videoCount < int(nestedAudio.size())) {
                audio.push_back(nestedAudio[size_t(source - videoCount)]);
            }
        }
        nestedVideo = std::move(video);
        nestedAudio = std::move(audio);
    }
    const TrackKind kind = loc.track->kind;
    const size_t base = size_t(std::distance(p.tracksOf(kind).data(), loc.track));
    loc.track->clips.erase(loc.track->clips.begin() + std::ptrdiff_t(loc.index));

    // Tramo de la secuencia que se ve (en sus fotogramas), recortado a los clips.
    const ProjectSettings& s = p.settings;
    const SourceMap compMap(s, comp);
    const TimeUs from = comp.sourceIn, to = comp.sourceIn + clipSourceSpan(s, comp);
    const FramePos a = s.timeToFrame(from);
    const FramePos b = std::max(a + 1, s.timeToFrame(to - 1) + 1);
    auto retimed = [&](const Track& nested) {
        std::vector<Clip> clips;
                for (const Clip& c : nested.clips) {
            if (c.end() <= a || c.start >= b)
                continue;
            Clip piece = c;
            if (piece.start < a)
                piece = rightPart(p, piece, a, piece.id);
            if (piece.end() > b)
                piece = leftPart(p, piece, b);
            if (auto r = retimeForCompound(p, comp, compMap, piece)) {
                r->id = p.newId(); // la secuencia original se conserva (deshacer)
                clips.push_back(std::move(*r));
            }
        }
        if (comp.reverse) {
            // Al revés, la transición de cada clip (desde el anterior) pasa al que ahora le sigue.
            std::sort(clips.begin(), clips.end(), [](const Clip& x, const Clip& y) { return x.start < y.start; });
            for (size_t i = clips.size(); i-- > 1;)
                clips[i].transitionIn = clips[i - 1].end() == clips[i].start ? clips[i - 1].transitionIn : TransitionIn{};
            if (!clips.empty())
                clips[0].transitionIn = {};
        }
        return clips;
    };
    auto freeFor = [](const Track& t, const std::vector<Clip>& clips) {
        return std::none_of(clips.begin(), clips.end(), [&](const Clip& c) {
            return std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& o) { return o.start < c.end() && c.start < o.end(); });
        });
    };
    std::vector<Id> created;
    auto put = [&](Track& t, std::vector<Clip>& clips) {
        for (Clip& c : clips)
            created.push_back(placePrepared(p, t, std::move(c), PlaceMode::Overwrite));
    };
    // Video: juntas y en orden desde la pista del compuesto, para que nada quede entre sus capas ni
    // las tape; si la pista siguiente está ocupada en ese tramo, se intercala una nueva.
    size_t next = kind == TrackKind::Video ? base : 0;
    for (const Track& nested : nestedVideo) {
        auto clips = retimed(nested);
        if (clips.empty())
            continue;
        auto& list = p.tracksOf(TrackKind::Video);
        if (next >= list.size() || !freeFor(list[next], clips))
            addTrack(p, TrackKind::Video, int(next));
        put(p.tracksOf(TrackKind::Video)[next], clips);
        ++next;
    }
    // Audio (el orden no cambia la mezcla): la primera pista libre desde la del compuesto o A1.
    for (const Track& nested : nestedAudio) {
        auto clips = retimed(nested);
        if (clips.empty())
            continue;
        auto& list = p.tracksOf(TrackKind::Audio);
        size_t target = kind == TrackKind::Audio ? base : 0;
        while (target < list.size() && !freeFor(list[target], clips))
            ++target;
        if (target >= list.size())
            addTrack(p, TrackKind::Audio);
        put(p.tracksOf(TrackKind::Audio)[target], clips);
    }
    normalize(p);
    return created;
}

// --- Multicámara ---

Id createMulticam(Project& p, const std::vector<MulticamSource>& sources, const std::string& name, Id trackId, FramePos at)
{
    Track* target = p.findTrack(trackId);
    if (!target || target->locked || target->kind != TrackKind::Video || sources.empty())
        return kNoId;
    TimeUs origin = INT64_MAX;
    for (const MulticamSource& src : sources) {
        const MediaItem* m = p.findMedia(src.mediaId);
        if (!m || (!m->hasVideo && !m->hasAudio) || m->isStill)
            return kNoId;
        origin = std::min(origin, src.offset);
    }

    Sequence seq;
    seq.id = p.newId();
    seq.name = name;
    FramePos length = 0;
    for (const MulticamSource& src : sources) {
        const MediaItem& m = *p.findMedia(src.mediaId);
        auto& list = m.hasVideo ? seq.videoTracks : seq.audioTracks;
        Track t = newTrack(p, m.hasVideo ? TrackKind::Video : TrackKind::Audio, int(list.size()) + 1);
        t.name = m.name; // el nombre del ángulo
        Clip c;
        c.id = p.newId();
        c.mediaId = m.id;
        // Lo que empezó antes que la grabación más temprana se recorta (no cabe antes de 0).
        c.start = p.settings.timeToFrame(src.offset - origin);
        if (std::abs(src.speed - 1.0) > 1e-9 && src.speed > 0.5 && src.speed < 2.0) {
            c.speed = src.speed;
            c.speedRamp = Animated(src.speed);
        }
        c.duration = p.settings.framesIn(TimeUs(double(m.duration) / c.speed));
        if (c.duration <= 0)
            return kNoId;
        length = std::max(length, c.end());
        t.clips.push_back(std::move(c));
        list.push_back(std::move(t));
    }
    if (seq.videoTracks.empty())
        return kNoId; // hace falta al menos un ángulo con imagen
    if (seq.audioTracks.empty())
        seq.audioTracks.push_back(newTrack(p, TrackKind::Audio, 1));
    const int videoCount = int(seq.videoTracks.size());
    const bool externalAudio = !seq.audioTracks.front().clips.empty();

    Clip mc;
    mc.id = p.newId();
    mc.sequenceId = seq.id;
    mc.start = std::max<FramePos>(0, at);
    mc.duration = length;
    mc.angle = 0;
    mc.audioAngle = externalAudio ? videoCount : -1; // el micrófono aparte, si lo hay
    p.sequences.push_back(std::move(seq));
    return placePrepared(p, *p.findTrack(trackId), std::move(mc), PlaceMode::Overwrite);
}

int multicamAngles(const Project& p, const Clip& c)
{
    const auto* tracks = c.isMulticam() ? p.videoTracksOf(c.sequenceId) : nullptr;
    return tracks ? int(tracks->size()) : 0;
}

int multicamAudioSources(const Project& p, const Clip& c)
{
    const auto* audio = c.isMulticam() ? p.audioTracksOf(c.sequenceId) : nullptr;
    return audio ? multicamAngles(p, c) + int(audio->size()) : 0;
}

bool setMulticamAngle(Project& p, Id clipId, int angle)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked || !loc.clip()->isMulticam())
        return false;
    Clip& c = *loc.clip();
    if (angle < 0 || angle >= multicamAngles(p, c) || c.angle == angle)
        return false;
    c.angle = angle;
    return true;
}

bool setMulticamAudio(Project& p, Id clipId, int audioAngle)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked || !loc.clip()->isMulticam())
        return false;
    Clip& c = *loc.clip();
    if (audioAngle < -1 || audioAngle >= multicamAudioSources(p, c) || c.audioAngle == audioAngle)
        return false;
    c.audioAngle = audioAngle;
    return true;
}

Id switchAngle(Project& p, Id clipId, FramePos at, int angle)
{
    const Clip* c = p.findClip(clipId);
    if (!c || !c->isMulticam() || angle < 0 || angle >= multicamAngles(p, *c) || at < c->start || at >= c->end())
        return kNoId;
    if (c->angle == angle)
        return clipId;
    const Id target = at == c->start ? clipId : splitClip(p, clipId, at);
    if (target == kNoId || !setMulticamAngle(p, target, angle))
        return kNoId;
    return target;
}

bool openSequence(Project& p, Id sequence)
{
    if (sequence == p.activeSequence)
        return false;
    auto it = std::find_if(p.sequences.begin(), p.sequences.end(), [&](const Sequence& s) { return s.id == sequence; });
    if (it == p.sequences.end())
        return false;
    Sequence target = std::move(*it);
    p.sequences.erase(it);
    Sequence current{p.activeSequence, p.activeName, std::move(p.videoTracks), std::move(p.audioTracks), std::move(p.markers)};
    // Volver a una secuencia del camino lo recorta; abrir otra nueva lo alarga.
    auto back = std::find(p.sequencePath.begin(), p.sequencePath.end(), sequence);
    if (back != p.sequencePath.end())
        p.sequencePath.erase(back, p.sequencePath.end());
    else
        p.sequencePath.push_back(current.id);
    p.sequences.push_back(std::move(current));
    p.videoTracks = std::move(target.videoTracks);
    p.audioTracks = std::move(target.audioTracks);
    p.markers = std::move(target.markers);
    p.activeSequence = target.id;
    p.activeName = std::move(target.name);
    normalize(p);
    return true;
}

bool closeSequence(Project& p)
{
    return !p.sequencePath.empty() && openSequence(p, p.sequencePath.back());
}

bool renameSequence(Project& p, Id sequence, const std::string& name)
{
    if (sequence == p.activeSequence) {
        if (p.activeName == name)
            return false;
        p.activeName = name;
        return true;
    }
    for (Sequence& seq : p.sequences)
        if (seq.id == sequence && seq.name != name) {
            seq.name = name;
            return true;
        }
    return false;
}

bool setColorMode(Project& p, ColorMode mode)
{
    if (p.settings.colorMode == mode)
        return false;
    p.settings.colorMode = mode;
    return true;
}

bool setLinearLight(Project& p, bool on)
{
    if (p.settings.linearLight == on)
        return false;
    p.settings.linearLight = on;
    return true;
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
    p.forEachTrack([&](Track& t) { std::erase_if(t.clips, [&](const Clip& c) { return !c.isGenerated() && c.mediaId == mediaId; }); });
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
    // Todas las secuencias (también las de los clips compuestos) y sus marcadores.
    auto convertMarkers = [&](std::vector<Marker>& markers) {
        for (Marker& m : markers)
            m.frame = convert(m.frame);
        markers.erase(std::unique(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.frame == b.frame; }),
                      markers.end());
    };
    convertMarkers(p.markers);
    for (Sequence& seq : p.sequences)
        convertMarkers(seq.markers);
    p.forEachTrack([&](Track& t) {
        {
            for (auto& c : t.clips) {
                const FramePos start = convert(c.start);
                const FramePos end = convert(c.end());
                // Duraciones y keyframes (relativos al clip) se convierten igual que las posiciones.
                auto rel = [&](FramePos r) { return convert(c.start + r) - start; };
                forEachKeyFrame(c, [&](std::int64_t& f) { f = rel(f); });
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
    });
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
    TimeUs total = 0; // medio (o secuencia) disponible
    if (c.isCompound()) {
        total = p.settings.frameToTime(p.sequenceDuration(c.sequenceId));
    } else {
        const MediaItem* m = p.findMedia(c.mediaId);
        if (!m)
            return c.duration;
        if (m->isStill)
            return p.settings.framesIn(kUnlimitedDuration);
        total = m->duration;
    }
    // Al revés, el clip va del final de su tramo hacia el principio del medio.
    const TimeUs available = c.reverse ? c.sourceIn + clipSourceSpan(p.settings, c) : total - c.sourceIn;
    return framesForSource(p.settings, c, available);
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

int addMarkers(Project& p, const std::vector<FramePos>& frames, const std::string& label, std::uint32_t color)
{
    int added = 0;
    for (FramePos f : frames) {
        if (f < 0)
            continue;
        auto it = std::lower_bound(p.markers.begin(), p.markers.end(), f, [](const Marker& m, FramePos v) { return m.frame < v; });
        if (it != p.markers.end() && it->frame == f)
            continue;
        p.markers.insert(it, Marker{f, label, color});
        ++added;
    }
    return added;
}

bool removeMarker(Project& p, FramePos frame)
{
    return std::erase_if(p.markers, [&](const Marker& m) { return m.frame == frame; }) > 0;
}

bool clearMarkers(Project& p, const std::string& label)
{
    return std::erase_if(p.markers, [&](const Marker& m) { return label.empty() || m.label == label; }) > 0;
}

bool replaceClipMedia(Project& p, Id clipId, Id mediaId, TimeUs sourceIn)
{
    Clip* c = p.findClip(clipId);
    const MediaItem* m = p.findMedia(mediaId);
    if (!c || !m || c->title || c->isCompound() || (c->mediaId == mediaId && c->sourceIn == sourceIn))
        return false;
    // Un fotograma de tolerancia: el medio procesado puede terminar justo al final del clip.
    if (availableFrames(p, *m, sourceIn, c->speed) + 1 < c->duration)
        return false;
    c->mediaId = mediaId;
    c->sourceIn = sourceIn;
    c->matte = {}; // el mate era del medio anterior
    return true;
}

bool setMatte(Project& p, Id clipId, const AiMatte& matte)
{
    auto loc = p.locateClip(clipId);
    Clip* c = loc.clip();
    if (!c || loc.track->kind != TrackKind::Video || c->title || c->isCompound() || c->matte == matte)
        return false;
    c->matte = matte;
    return true;
}

bool setDenoise(Project& p, Id clipId, double amount)
{
    Clip* c = p.findClip(clipId);
    if (!c || c->title || c->isCompound())
        return false;
    const MediaItem* m = p.findMedia(c->mediaId);
    amount = std::clamp(amount, 0.0, 1.0);
    if (!m || !m->hasAudio || c->denoise == amount)
        return false;
    c->denoise = amount;
    return true;
}

bool setTranscript(Project& p, Id mediaId, TranscriptRef transcript)
{
    MediaItem* m = p.findMedia(mediaId);
    if (!m || m->transcript == transcript)
        return false;
    m->transcript = std::move(transcript);
    return true;
}

Id addSubtitleTrack(Project& p, const std::vector<Cue>& cues, const TitleStyle& style, const std::string& name)
{
    const Id trackId = addTrack(p, TrackKind::Video);
    if (!name.empty())
        p.findTrack(trackId)->name = name;
    const TimeUs half = p.settings.frameDuration() / 2;
    FramePos previousEnd = 0;
    for (const Cue& cue : cues) {
        const FramePos start = std::max(previousEnd, p.settings.timeToFrame(cue.start + half));
        const FramePos end = std::max(start + 1, p.settings.timeToFrame(cue.end + half));
        TitleStyle s = style;
        s.text = cue.text;
        const Id clipId = placeTitle(p, trackId, start, end - start, s);
        if (Clip* c = p.findClip(clipId))
            c->transform.y = Animated(kSubtitleOffsetY);
        previousEnd = end;
    }
    return trackId;
}

FramePos removeRanges(Project& p, std::vector<FrameRange> ranges)
{
    std::erase_if(ranges, [](const FrameRange& r) { return r.end <= std::max<FramePos>(r.start, 0); });
    std::sort(ranges.begin(), ranges.end(), [](const FrameRange& a, const FrameRange& b) { return a.start < b.start; });
    std::vector<FrameRange> merged;
    for (FrameRange r : ranges) {
        r.start = std::max<FramePos>(r.start, 0);
        if (!merged.empty() && r.start <= merged.back().end)
            merged.back().end = std::max(merged.back().end, r.end);
        else
            merged.push_back(r);
    }

    FramePos removed = 0;
    // De derecha a izquierda: así los tramos pendientes no se desplazan.
    for (auto r = merged.rbegin(); r != merged.rend(); ++r) {
        bool changed = false;
        for (auto* list : {&p.videoTracks, &p.audioTracks}) {
            for (Track& t : *list) {
                if (t.locked)
                    continue;
                for (FramePos cut : {r->start, r->end})
                    if (const Clip* c = clipAt(t, cut); c && c->start < cut)
                        splitClip(p, c->id, cut);
                const auto before = t.clips.size();
                std::erase_if(t.clips, [&](const Clip& c) { return c.start >= r->start && c.end() <= r->end; });
                changed |= t.clips.size() != before;
                for (Clip& c : t.clips)
                    if (c.start >= r->end) {
                        c.start -= r->end - r->start;
                        changed = true;
                    }
            }
        }
        // Los marcadores del tramo desaparecen y los posteriores se adelantan con los clips.
        std::erase_if(p.markers, [&](const Marker& m) { return m.frame >= r->start && m.frame < r->end; });
        for (Marker& m : p.markers)
            if (m.frame >= r->end) {
                m.frame -= r->end - r->start;
                changed = true;
            }
        if (changed)
            removed += r->end - r->start;
    }
    if (removed > 0)
        normalize(p);
    return removed;
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
    const MediaItem* m = c.isGenerated() ? nullptr : p.findMedia(c.mediaId);
    if (!c.isGenerated() && !m)
        return std::nullopt;
    const bool unlimited = c.title || (m && m->isStill);
    const auto& clips = loc.track->clips;

    if (edge == Edge::Start) {
        const FramePos prevEnd = loc.index > 0 ? clips[loc.index - 1].end() : 0;
        // Fotogramas del medio anteriores a sourceIn que se pueden recuperar.
        // (Al revés, alargar el comienzo muestra medio posterior al final del tramo.)
        TimeUs recoverable = c.sourceIn;
        if (c.reverse) {
            const TimeUs total = c.isCompound() ? p.settings.frameToTime(p.sequenceDuration(c.sequenceId)) : m ? m->duration : 0;
            recoverable = std::max<TimeUs>(0, total - c.sourceIn - clipSourceSpan(p.settings, c));
        }
        const FramePos before = unlimited ? c.start : p.settings.timeToFrame(TimeUs(double(recoverable) / c.speedAt(0)));
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
    const TimeUs spanBefore = clipSourceSpan(p.settings, c);
    c.duration = newEnd - c.start;
    if (c.reverse) // el final del clip muestra el principio del tramo: se mueve sourceIn
        c.sourceIn = std::max<TimeUs>(0, c.sourceIn + spanBefore - clipSourceSpan(p.settings, c));
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
    c = leftPart(p, c, at);
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

Animated* property(Clip& c, Property prop, int maskIndex)
{
    Mask* m = maskIndex >= 0 && size_t(maskIndex) < c.masks.size() ? &c.masks[size_t(maskIndex)] : nullptr;
    switch (prop) {
    case Property::PositionX: return &c.transform.x;
    case Property::PositionY: return &c.transform.y;
    case Property::Scale: return &c.transform.scale;
    case Property::Rotation: return &c.transform.rotation;
    case Property::Opacity: return &c.transform.opacity;
    case Property::Volume: return &c.volume;
    case Property::Speed: return &c.speedRamp;
    case Property::CropLeft: return &c.transform.cropLeft;
    case Property::CropTop: return &c.transform.cropTop;
    case Property::CropRight: return &c.transform.cropRight;
    case Property::CropBottom: return &c.transform.cropBottom;
    case Property::MaskX: return m ? &m->x : nullptr;
    case Property::MaskY: return m ? &m->y : nullptr;
    case Property::MaskWidth: return m ? &m->width : nullptr;
    case Property::MaskHeight: return m ? &m->height : nullptr;
    case Property::MaskRotation: return m ? &m->rotation : nullptr;
    case Property::MaskFeather: return m ? &m->feather : nullptr;
    }
    return nullptr;
}

const Animated* property(const Clip& c, Property prop, int maskIndex)
{
    return property(const_cast<Clip&>(c), prop, maskIndex);
}

namespace {

// La velocidad variable solo en clips de un medio con duración (no títulos, imágenes ni compuestos).
bool canRamp(const Project& p, const Clip& c)
{
    const MediaItem* m = c.isGenerated() ? nullptr : p.findMedia(c.mediaId);
    return m && !m->isStill;
}

// Con velocidad variable el clip no puede pedir más medio del que hay: se acorta si hace falta.
void fitRamp(Project& p, Clip& c)
{
    c.duration = std::clamp<FramePos>(maxClipFrames(p, c), 1, c.duration);
    clampFades(c);
}

} // namespace

bool setProperty(Project& p, Id clipId, Property prop, FramePos rel, double value, int maskIndex)
{
    Clip* c = p.findClip(clipId);
    if (!c || p.trackOfClip(clipId)->locked)
        return false;
    clampValue(prop, value);
    if (prop == Property::Speed) {
        if (!c->hasRamp()) // sin keyframes: la velocidad fija (que ajusta la duración)
            return setClipSpeed(p, clipId, value);
        const Animated before = c->speedRamp;
        c->speedRamp.set(rel, value);
        fitRamp(p, *c);
        return !(c->speedRamp == before);
    }
    Animated* a = property(*c, prop, maskIndex);
    if (!a)
        return false;
    const Animated before = *a;
    a->set(rel, value);
    return !(*a == before);
}

bool toggleKeyframe(Project& p, Id clipId, Property prop, FramePos rel, int maskIndex)
{
    Clip* c = p.findClip(clipId);
    if (!c || p.trackOfClip(clipId)->locked)
        return false;
    Animated* a = property(*c, prop, maskIndex);
    if (!a)
        return false;
    if (prop == Property::Speed) {
        if (!canRamp(p, *c))
            return false;
        c->speedRamp.value = c->speed;
    }
    if (a->keyAt(rel)) {
        a->removeKey(rel);
    } else {
        a->setKey(rel, a->at(rel));
    }
    if (prop == Property::Speed)
        fitRamp(p, *c);
    return true;
}

bool setKeyframeInterpolation(Project& p, Id clipId, Property prop, FramePos rel, Interpolation interp, int maskIndex)
{
    Clip* c = p.findClip(clipId);
    if (!c)
        return false;
    Animated* a = property(*c, prop, maskIndex);
    if (!a)
        return false;
    const Keyframe* k = a->keyAt(rel);
    if (!k || k->interp == interp)
        return false;
    a->setKey(rel, k->value, interp);
    return true;
}

bool resetProperty(Project& p, Id clipId, Property prop, int maskIndex)
{
    Clip* c = p.findClip(clipId);
    if (!c)
        return false;
    Animated* a = property(*c, prop, maskIndex);
    if (!a)
        return false;
    if (prop == Property::Speed) { // velocidad normal y sin keyframes (ajusta la duración)
        const bool hadRamp = c->hasRamp();
        c->speedRamp = Animated(c->speed);
        return setClipSpeed(p, clipId, 1.0) || hadRamp;
    }
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
    const MediaItem* m = c.isCompound() ? nullptr : p.findMedia(c.mediaId);
    if (!c.isCompound() && (!m || m->isStill))
        return false;
    const double ratio = c.speed / speed; // > 1 al ralentizar: el clip se alarga
    const auto& clips = loc.track->clips;
    const FramePos nextStart = loc.index + 1 < clips.size() ? clips[loc.index + 1].start : INT64_MAX;
    FramePos duration = std::max<FramePos>(1, std::llround(double(c.duration) * ratio));
    Clip probe = c;
    probe.speed = speed;
    const FramePos available = m ? availableFrames(p, *m, c.sourceIn, speed) : maxClipFrames(p, probe);
    duration = std::min({duration, available, nextStart - c.start});
    c.speedRamp = Animated(speed); // una velocidad fija sustituye a la variable
    forEachKeyFrame(c, [&](std::int64_t& f) { f = std::llround(double(f) * ratio); });
    c.speed = speed;
    c.duration = std::max<FramePos>(1, duration);
    normalize(p);
    return true;
}

bool setReverse(Project& p, Id clipId, bool reverse)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked)
        return false;
    Clip& c = *loc.clip();
    if (c.reverse == reverse || !canRamp(p, c))
        return false;
    c.reverse = reverse; // el mismo tramo del medio: sourceIn es su principio en ambos sentidos
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

// --- Efectos de imagen ---

ColorGrade sanitized(ColorGrade g)
{
    g.exposure = std::clamp(g.exposure, -4.0, 4.0);
    for (double* v : {&g.contrast, &g.saturation, &g.vibrance, &g.temperature, &g.tint, &g.highlights, &g.shadows, &g.whites, &g.blacks})
        *v = std::clamp(*v, -1.0, 1.0);
    sanitizeWheel(g.lift);
    sanitizeWheel(g.gamma);
    sanitizeWheel(g.gain);
    for (auto* curve : {&g.curveMaster, &g.curveRed, &g.curveGreen, &g.curveBlue})
        sanitizeCurve(*curve);
    for (HslBand& b : g.hsl) {
        b.hue = std::clamp(b.hue, -30.0, 30.0);
        b.saturation = std::clamp(b.saturation, -1.0, 1.0);
        b.luminance = std::clamp(b.luminance, -1.0, 1.0);
    }
    g.lutIntensity = std::clamp(g.lutIntensity, 0.0, 1.0);
    return g;
}

bool setColorGrade(Project& p, Id clipId, const ColorGrade& grade, FramePos rel)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c))
        return false;
    const AnimatedGrade before = c->color;
    c->color.set(rel, sanitized(grade));
    return !(c->color == before);
}

bool toggleColorKeyframe(Project& p, Id clipId, FramePos rel)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c))
        return false;
    if (c->color.keyAt(rel))
        return c->color.removeKey(rel);
    c->color.setKey(rel, c->color.valueAt(rel));
    return true;
}

bool setColorKeyInterpolation(Project& p, Id clipId, FramePos rel, Interpolation interp)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c))
        return false;
    const GradeKey* k = c->color.keyAt(rel);
    if (!k || k->interp == interp)
        return false;
    c->color.setKey(rel, k->grade, interp);
    return true;
}

bool setAnimatedGrade(Project& p, Id clipId, const AnimatedGrade& grade)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c))
        return false;
    AnimatedGrade g = grade;
    g.value = sanitized(g.value);
    for (GradeKey& k : g.keys)
        k.grade = sanitized(k.grade);
    std::sort(g.keys.begin(), g.keys.end(), [](const GradeKey& a, const GradeKey& b) { return a.frame < b.frame; });
    if (c->color == g)
        return false;
    c->color = std::move(g);
    return true;
}

bool setChromaKey(Project& p, Id clipId, const ChromaKey& key)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c))
        return false;
    ChromaKey k = key;
    k.color |= 0xFF000000u;
    k.tolerance = std::clamp(k.tolerance, 0.0, 1.0);
    k.softness = std::clamp(k.softness, 0.0, 1.0);
    k.spill = std::clamp(k.spill, 0.0, 1.0);
    if (c->chromaKey == k)
        return false;
    c->chromaKey = k;
    return true;
}

int addMask(Project& p, Id clipId, MaskShape shape)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c) || shape == MaskShape::None || c->masks.size() >= kMaxMasks)
        return -1;
    Mask m;
    m.shape = shape;
    if (shape == MaskShape::Path)
        m.points = defaultMaskPath();
    c->masks.push_back(m);
    return int(c->masks.size()) - 1;
}

bool removeMask(Project& p, Id clipId, int index)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c) || index < 0 || size_t(index) >= c->masks.size())
        return false;
    c->masks.erase(c->masks.begin() + index);
    return true;
}

bool setMaskShape(Project& p, Id clipId, int index, MaskShape shape)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c) || index < 0 || size_t(index) >= c->masks.size() || shape == MaskShape::None)
        return false;
    Mask& m = c->masks[size_t(index)];
    if (m.shape == shape)
        return false;
    m.shape = shape;
    if (shape == MaskShape::Path && m.points.size() < 3)
        m.points = defaultMaskPath();
    return true;
}

bool setMaskOptions(Project& p, Id clipId, int index, MaskMode mode, bool invert)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c) || index < 0 || size_t(index) >= c->masks.size())
        return false;
    Mask& m = c->masks[size_t(index)];
    if (m.mode == mode && m.invert == invert)
        return false;
    m.mode = mode;
    m.invert = invert;
    return true;
}

namespace {
void upsertPathKey(Mask& m, FramePos rel, std::vector<PathPoint> points, Interpolation interp)
{
    auto it = std::lower_bound(m.pathKeys.begin(), m.pathKeys.end(), rel, [](const PathKey& k, FramePos f) { return k.frame < f; });
    if (it != m.pathKeys.end() && it->frame == rel) {
        it->points = std::move(points);
        it->interp = interp;
    } else {
        m.pathKeys.insert(it, PathKey{rel, std::move(points), interp});
    }
}

// Punto nuevo entre `a` y `b` (sin tangentes): para añadirlo a los demás keyframes de la forma.
PathPoint midpoint(const PathPoint& a, const PathPoint& b)
{
    return {(a.x + b.x) / 2, (a.y + b.y) / 2};
}
} // namespace

bool setMaskPoints(Project& p, Id clipId, int index, std::vector<PathPoint> points, FramePos rel)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c) || index < 0 || size_t(index) >= c->masks.size() || points.size() < 3
        || points.size() > 256)
        return false;
    for (PathPoint& pt : points)
        for (double* v : {&pt.x, &pt.y, &pt.inX, &pt.inY, &pt.outX, &pt.outY})
            *v = std::clamp(*v, -4.0, 4.0);
    Mask& m = c->masks[size_t(index)];
    if (m.pathKeys.empty()) {
        if (m.points == points)
            return false;
        m.points = std::move(points);
        return true;
    }
    // Con keyframes se cambia (o se crea) el del fotograma; añadir o quitar un punto se hace en todos.
    const std::vector<PathPoint> current = m.pathAt(rel);
    if (current == points)
        return false;
    if (points.size() != current.size()) {
        const size_t n = current.size();
        size_t at = 0;
        if (points.size() == n + 1) {
            while (at < n && current[at] == points[at])
                ++at;
            for (PathKey& k : m.pathKeys)
                k.points.insert(k.points.begin() + std::ptrdiff_t(at), midpoint(k.points[(at + n - 1) % n], k.points[at % n]));
        } else if (points.size() + 1 == n) {
            while (at < points.size() && current[at] == points[at])
                ++at;
            for (PathKey& k : m.pathKeys)
                k.points.erase(k.points.begin() + std::ptrdiff_t(at));
        } else { // otra forma distinta: deja de estar animada
            m.pathKeys.clear();
            m.points = std::move(points);
            return true;
        }
    }
    const PathKey* existing = m.pathKeyAt(rel);
    upsertPathKey(m, rel, std::move(points), existing ? existing->interp : Interpolation::Linear);
    return true;
}

bool toggleMaskPathKey(Project& p, Id clipId, int index, FramePos rel)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c) || index < 0 || size_t(index) >= c->masks.size())
        return false;
    Mask& m = c->masks[size_t(index)];
    if (m.shape != MaskShape::Path)
        return false;
    auto it = std::find_if(m.pathKeys.begin(), m.pathKeys.end(), [&](const PathKey& k) { return k.frame == rel; });
    if (it != m.pathKeys.end()) {
        if (m.pathKeys.size() == 1) // al quitar el último, su forma queda fija
            m.points = it->points;
        m.pathKeys.erase(it);
    } else {
        upsertPathKey(m, rel, m.pathAt(rel), Interpolation::Linear);
    }
    return true;
}

bool setMaskPathKeyInterpolation(Project& p, Id clipId, int index, FramePos rel, Interpolation interp)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c) || index < 0 || size_t(index) >= c->masks.size())
        return false;
    for (PathKey& k : c->masks[size_t(index)].pathKeys)
        if (k.frame == rel) {
            if (k.interp == interp)
                return false;
            k.interp = interp;
            return true;
        }
    return false;
}

bool setBlendMode(Project& p, Id clipId, BlendMode mode)
{
    Clip* c = nullptr;
    if (!isEditableVideoClip(p, clipId, c) || int(mode) < 0 || int(mode) >= kBlendModeCount || c->blend == mode)
        return false;
    c->blend = mode;
    return true;
}

namespace {
Clip* ofxClip(Project& p, Id clipId)
{
    auto loc = p.locateClip(clipId);
    if (!loc.track || loc.track->locked || loc.track->kind != TrackKind::Video)
        return nullptr;
    return loc.clip();
}
} // namespace

int addOfxEffect(Project& p, Id clipId, const std::string& pluginId)
{
    Clip* c = ofxClip(p, clipId);
    if (!c || pluginId.empty() || c->ofx.size() >= kMaxOfxEffects)
        return -1;
    c->ofx.push_back({pluginId, true, {}});
    return int(c->ofx.size()) - 1;
}

bool removeOfxEffect(Project& p, Id clipId, int index)
{
    Clip* c = ofxClip(p, clipId);
    if (!c || index < 0 || index >= int(c->ofx.size()))
        return false;
    c->ofx.erase(c->ofx.begin() + index);
    return true;
}

bool setOfxEffectEnabled(Project& p, Id clipId, int index, bool enabled)
{
    Clip* c = ofxClip(p, clipId);
    if (!c || index < 0 || index >= int(c->ofx.size()) || c->ofx[size_t(index)].enabled == enabled)
        return false;
    c->ofx[size_t(index)].enabled = enabled;
    return true;
}

bool setOfxParam(Project& p, Id clipId, int index, const std::string& name, std::vector<double> value, FramePos rel)
{
    Clip* c = ofxClip(p, clipId);
    if (!c || index < 0 || index >= int(c->ofx.size()) || name.empty() || value.empty())
        return false;
    std::vector<Animated>& components = c->ofx[size_t(index)].params[name];
    const std::vector<Animated> before = components;
    components.resize(value.size());
    for (size_t i = 0; i < value.size(); ++i)
        components[i].set(rel, value[i]);
    return components != before;
}

bool toggleOfxKey(Project& p, Id clipId, int index, const std::string& name, FramePos rel, const std::vector<double>& current,
                  Interpolation interp)
{
    Clip* c = ofxClip(p, clipId);
    if (!c || index < 0 || index >= int(c->ofx.size()) || name.empty() || current.empty())
        return false;
    std::vector<Animated>& components = c->ofx[size_t(index)].params[name];
    if (!components.empty() && components[0].keyAt(rel)) {
        for (Animated& a : components)
            a.removeKey(rel);
        return true;
    }
    components.resize(current.size());
    for (size_t i = 0; i < current.size(); ++i)
        components[i].setKey(rel, current[i], interp);
    return true;
}

bool setOfxKeyInterpolation(Project& p, Id clipId, int index, const std::string& name, FramePos rel, Interpolation interp)
{
    Clip* c = ofxClip(p, clipId);
    if (!c || index < 0 || index >= int(c->ofx.size()))
        return false;
    auto it = c->ofx[size_t(index)].params.find(name);
    if (it == c->ofx[size_t(index)].params.end() || it->second.empty() || !it->second[0].keyAt(rel))
        return false;
    bool changed = false;
    for (Animated& a : it->second)
        if (const Keyframe* k = a.keyAt(rel); k && k->interp != interp) {
            a.setKey(rel, k->value, interp);
            changed = true;
        }
    return changed;
}

bool moveOfxEffect(Project& p, Id clipId, int from, int to)
{
    Clip* c = ofxClip(p, clipId);
    const int n = c ? int(c->ofx.size()) : 0;
    if (!c || from < 0 || from >= n || to < 0 || to >= n || from == to)
        return false;
    OfxEffect e = std::move(c->ofx[size_t(from)]);
    c->ofx.erase(c->ofx.begin() + from);
    c->ofx.insert(c->ofx.begin() + to, std::move(e));
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
    audio.denoise = source.denoise;
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
        if (item.clip.isCompound() ? !p.videoTracksOf(item.clip.sequenceId) || sequenceContains(p, item.clip.sequenceId, p.activeSequence)
                                   : !item.clip.title && !p.findMedia(item.clip.mediaId))
            continue; // el medio ya no está, o el compuesto contendría la secuencia en la que se pega
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
