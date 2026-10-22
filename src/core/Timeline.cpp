#include "core/Timeline.h"

#include "core/Edits.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

namespace movix::core {

namespace {

// Factor de un fundido de entrada/salida en el fotograma `rel` de un clip de `duration`.
double fadeFactor(FramePos fadeIn, FramePos fadeOut, FramePos rel, FramePos duration)
{
    double f = 1.0;
    if (fadeIn > 0)
        f *= std::clamp(double(rel) / double(fadeIn), 0.0, 1.0);
    if (fadeOut > 0)
        f *= std::clamp(double(duration - 1 - rel) / double(fadeOut), 0.0, 1.0);
    return f;
}

// Clave de la aparición de un clip dentro de la de su compuesto (0 = línea de tiempo activa).
std::uint64_t instanceKey(std::uint64_t outer, Id clip)
{
    if (outer == 0)
        return clip;
    std::uint64_t h = outer * 0x9E3779B97F4A7C15ull ^ (clip + 0x632BE59BD9B4E019ull);
    h ^= h >> 31;
    return h | (1ull << 63); // nunca coincide con un id de la línea de tiempo activa
}

std::vector<VideoLayer> layersOf(const Project& p, std::span<const Track> tracks, FramePos frame, std::uint64_t outer, int depth);

bool contributesVideo(const Project& p, const Clip& c)
{
    if (!c.enabled)
        return false;
    if (c.title || c.isCompound())
        return true;
    const MediaItem* m = p.findMedia(c.mediaId);
    return m && m->hasVideo;
}

// Capa de un clip en el fotograma `f`. `extended`: el clip se usa fuera de sus bordes (pierna
// de una transición), sin fundidos.
VideoLayer makeLayer(const Project& p, const Clip& c, int trackIndex, FramePos f, bool extended, std::uint64_t outer, int depth)
{
    VideoLayer l;
    l.clipId = c.id;
    l.instance = instanceKey(outer, c.id);
    l.trackIndex = trackIndex;
    if (c.title) {
        l.title = &*c.title;
    } else if (c.isCompound()) {
        l.compound = true;
        const FramePos nestedFrame = p.settings.timeToFrame(std::max<TimeUs>(0, sourceTimeAt(p.settings, c, f)));
        const auto* tracks = p.videoTracksOf(c.sequenceId);
        if (!tracks || depth >= kMaxNesting) {
        } else if (!c.isMulticam()) {
            l.nested = layersOf(p, *tracks, nestedFrame, l.instance, depth + 1);
        } else if (p.multicamGrid) {
            // Todos los ángulos, cada uno reducido a su celda (de izquierda a derecha y de arriba abajo).
            const int angles = int(tracks->size());
            const MulticamGrid grid = multicamGridFor(angles);
            for (int a = 0; a < angles; ++a) {
                VideoLayer cell;
                cell.clipId = c.id;
                cell.instance = instanceKey(l.instance, Id(a) + 1);
                cell.trackIndex = a;
                cell.compound = true;
                const auto rect = grid.cell(a);
                cell.transform = {rect.x + rect.width / 2 - 0.5, rect.y + rect.height / 2 - 0.5, grid.scale(), 0.0};
                cell.nested = layersOf(p, std::span(&(*tracks)[size_t(a)], 1), nestedFrame, cell.instance, depth + 1);
                l.nested.push_back(std::move(cell));
            }
        } else if (c.angle < int(tracks->size())) {
            l.nested = layersOf(p, std::span(&(*tracks)[size_t(c.angle)], 1), nestedFrame, l.instance, depth + 1);
        }
    } else {
        l.media = p.findMedia(c.mediaId);
        l.mediaId = c.mediaId;
        l.sourceTime = l.media && l.media->isStill ? 0 : std::max<TimeUs>(0, sourceTimeAt(p.settings, c, f));
    }
    const FramePos rel = f - c.start;
    l.transform = {c.transform.x.at(rel), c.transform.y.at(rel), c.transform.scale.at(rel), c.transform.rotation.at(rel)};
    l.opacity = std::clamp(c.transform.opacity.at(rel), 0.0, 1.0);
    const Transform& tr = c.transform;
    l.crop = {std::clamp(tr.cropLeft.at(rel), 0.0, 1.0), std::clamp(tr.cropTop.at(rel), 0.0, 1.0),
              1.0 - std::clamp(tr.cropRight.at(rel), 0.0, 1.0), 1.0 - std::clamp(tr.cropBottom.at(rel), 0.0, 1.0)};
    if (!c.color.isIdentity()) {
        const AnimatedGrade::Sample g = c.color.at(rel);
        l.grade = g.from;
        l.gradeTo = g.to;
        l.gradeMix = g.t;
    }
    l.chromaKey = c.chromaKey;
    for (const Mask& m : c.masks) {
        if (!m.active())
            continue;
        LayerMask lm{m.shape, m.invert, m.x.at(rel), m.y.at(rel), m.width.at(rel), m.height.at(rel), m.rotation.at(rel),
                     std::max(0.0, m.feather.at(rel)), m.mode, {}};
        if (m.shape == MaskShape::Path)
            lm.points = m.points;
        l.masks.push_back(std::move(lm));
    }
    if (c.matte.active() && !c.isCompound() && !c.title)
        l.matte = &c.matte;
    if (!c.ofx.empty() && !c.isCompound())
        l.ofx = &c.ofx;
    l.clipFrame = rel;
    l.blend = c.blend;
    if (!extended)
        l.opacity *= fadeFactor(c.fadeIn, c.fadeOut, rel, c.duration);
    return l;
}

// Aplica el efecto de transición con progreso t ∈ (0, 1) a la capa saliente (a) y la entrante (b).
// Devuelve qué capas deben dibujarse.
void applyTransition(TransitionType type, double t, VideoLayer& a, VideoLayer& b, bool& drawA, bool& drawB)
{
    drawA = drawB = true;
    switch (type) {
    case TransitionType::Dissolve:
        b.opacity *= t;
        break;
    case TransitionType::DipToBlack:
        if (t < 0.5) {
            a.opacity *= 1.0 - 2.0 * t;
            drawB = false;
        } else {
            b.opacity *= 2.0 * t - 1.0;
            drawA = false;
        }
        break;
    case TransitionType::WipeLeft: // la entrante aparece desde la derecha
        b.crop.left = std::max(b.crop.left, 1.0 - t);
        break;
    case TransitionType::WipeRight:
        b.crop.right = std::min(b.crop.right, t);
        break;
    case TransitionType::PushLeft:
        a.transform.x -= t;
        b.transform.x += 1.0 - t;
        break;
    case TransitionType::PushRight:
        a.transform.x += t;
        b.transform.x -= 1.0 - t;
        break;
    case TransitionType::SlideLeft:
        b.transform.x += 1.0 - t;
        break;
    case TransitionType::None:
        drawA = false;
        break;
    }
}

} // namespace

MulticamGrid multicamGridFor(int angles)
{
    MulticamGrid g;
    g.cols = std::max(1, int(std::ceil(std::sqrt(double(std::max(1, angles))))));
    g.rows = std::max(1, (std::max(1, angles) + g.cols - 1) / g.cols);
    return g;
}

MulticamGrid::Rect MulticamGrid::cell(int angle) const
{
    const double w = 1.0 / cols, h = 1.0 / rows;
    return {double(angle % cols) * w, double(angle / cols) * h, w, h};
}

MulticamGrid::Rect MulticamGrid::image(int angle) const
{
    const Rect c = cell(angle);
    const double s = scale();
    return {c.x + (c.width - s) / 2, c.y + (c.height - s) / 2, s, s};
}

int MulticamGrid::angleAt(double x, double y, int angles) const
{
    for (int a = 0; a < angles; ++a) {
        const Rect r = image(a);
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
            return a;
    }
    return -1;
}

TransitionSpan transitionSpan(const TransitionIn& t)
{
    if (!t.active())
        return {};
    return {t.duration / 2, t.duration - t.duration / 2};
}

namespace {

std::vector<VideoLayer> layersOf(const Project& p, std::span<const Track> tracks, FramePos frame, std::uint64_t outer, int depth)
{
    std::vector<VideoLayer> layers;
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const Track& t = tracks[ti];
        if (t.hidden)
            continue;
        const Clip* c = edit::clipAt(t, frame);
        if (!c)
            continue;
        const size_t idx = size_t(c - t.clips.data());
        const int track = int(ti);

        // ¿Estamos dentro de una transición? Entrante (segunda mitad) o saliente (primera mitad).
        const Clip* outgoing = nullptr;
        const Clip* incoming = nullptr;
        if (c->transitionIn.active() && idx > 0 && frame < c->start + transitionSpan(c->transitionIn).after) {
            outgoing = &t.clips[idx - 1];
            incoming = c;
        } else if (idx + 1 < t.clips.size()) {
            const Clip& next = t.clips[idx + 1];
            if (next.transitionIn.active() && next.start == c->end() && frame >= next.start - transitionSpan(next.transitionIn).before) {
                outgoing = c;
                incoming = &next;
            }
        }

        if (outgoing && incoming && contributesVideo(p, *outgoing) && contributesVideo(p, *incoming)) {
            const TransitionSpan span = transitionSpan(incoming->transitionIn);
            const FramePos k = frame - (incoming->start - span.before); // 0..duration-1
            const double progress = (double(k) + 0.5) / double(incoming->transitionIn.duration);
            VideoLayer a = makeLayer(p, *outgoing, track, frame, outgoing != c, outer, depth);
            VideoLayer b = makeLayer(p, *incoming, track, frame, incoming != c, outer, depth);
            bool drawA = true, drawB = true;
            applyTransition(incoming->transitionIn.type, progress, a, b, drawA, drawB);
            if (drawA)
                layers.push_back(a);
            if (drawB)
                layers.push_back(b);
            continue;
        }
        if (contributesVideo(p, *c))
            layers.push_back(makeLayer(p, *c, track, frame, false, outer, depth));
    }
    return layers;
}

} // namespace

std::vector<VideoLayer> videoLayersAt(const Project& p, FramePos frame)
{
    return layersOf(p, p.videoTracks, frame, 0, 0);
}

namespace {

void collectAudio(const Project& p, std::span<const Track> vtracks, std::span<const Track> atracks, TimeUs t0, TimeUs t1,
                  std::uint64_t outer, int depth, std::vector<AudioSegment>& segments)
{
    const auto& s = p.settings;

    auto collect = [&](const Track& t, bool videoTrack) {
        if (t.muted)
            return;
        for (size_t i = 0; i < t.clips.size(); ++i) {
            const Clip& c = t.clips[i];
            if (!c.enabled || c.title || (videoTrack && c.audioDetached))
                continue;
            const MediaItem* m = c.isCompound() ? nullptr : p.findMedia(c.mediaId);
            if (!c.isCompound() && (!m || !m->hasAudio))
                continue;

            const TimeUs cs = s.frameToTime(c.start);
            const TimeUs ce = s.frameToTime(c.end());
            // Las transiciones prolongan el audio a ambos lados del corte con un fundido cruzado.
            TimeUs xs = cs, xe = ce;          // intervalo sonoro
            TimeUs inFrom = cs, inTo = cs;    // fundido cruzado de entrada
            TimeUs outFrom = ce, outTo = ce;  // fundido cruzado de salida
            if (c.transitionIn.active() && i > 0 && t.clips[i - 1].end() == c.start) {
                const TransitionSpan span = transitionSpan(c.transitionIn);
                inFrom = xs = s.frameToTime(c.start - span.before);
                inTo = s.frameToTime(c.start + span.after);
            }
            if (i + 1 < t.clips.size()) {
                const Clip& next = t.clips[i + 1];
                if (next.transitionIn.active() && next.start == c.end()) {
                    const TransitionSpan span = transitionSpan(next.transitionIn);
                    outFrom = s.frameToTime(next.start - span.before);
                    outTo = xe = s.frameToTime(next.start + span.after);
                }
            }
            if (xe <= t0 || xs >= t1)
                continue;

            const TimeUs fadeInEnd = cs + s.frameToTime(c.audioFadeIn);
            const TimeUs fadeOutStart = ce - s.frameToTime(c.audioFadeOut);
            auto gainAt = [&](TimeUs time) {
                double g = t.volume * c.volume.at(s.timeToFrame(std::clamp(time, cs, ce - 1)) - c.start);
                if (c.audioFadeIn > 0 && time < fadeInEnd)
                    g *= std::clamp(double(time - cs) / double(fadeInEnd - cs), 0.0, 1.0);
                if (c.audioFadeOut > 0 && time > fadeOutStart)
                    g *= std::clamp(double(ce - time) / double(ce - fadeOutStart), 0.0, 1.0);
                // Potencia constante: sin(πx/2) al entrar y cos(πx/2) al salir.
                if (inTo > inFrom && time < inTo)
                    g *= std::sin(std::clamp(double(time - inFrom) / double(inTo - inFrom), 0.0, 1.0) * std::numbers::pi / 2);
                if (outTo > outFrom && time > outFrom)
                    g *= std::cos(std::clamp(double(time - outFrom) / double(outTo - outFrom), 0.0, 1.0) * std::numbers::pi / 2);
                return float(g);
            };

            const TimeUs from = std::max(t0, xs), to = std::min(t1, xe);
            if (c.isCompound()) {
                // El audio de la secuencia en el tramo, llevado al tiempo de esta línea de tiempo.
                const auto* nv = p.videoTracksOf(c.sequenceId);
                const auto* na = p.audioTracksOf(c.sequenceId);
                if (!nv || !na || depth >= kMaxNesting)
                    continue;
                std::span<const Track> sv = *nv, sa = *na;
                if (c.isMulticam()) { // solo la pista del ángulo que suena
                    const int source = c.audioAngle >= 0 ? c.audioAngle : c.angle;
                    const int videoCount = int(nv->size());
                    sv = source < videoCount ? sv.subspan(size_t(source), 1) : std::span<const Track>();
                    sa = source >= videoCount && source - videoCount < int(na->size()) ? sa.subspan(size_t(source - videoCount), 1)
                                                                                      : std::span<const Track>();
                }
                std::vector<AudioSegment> nested;
                collectAudio(p, sv, sa, sourceTimeAtTime(s, c, from), sourceTimeAtTime(s, c, to), instanceKey(outer, c.id),
                             depth + 1, nested);
                auto toOuter = [&](TimeUs nt) { return cs + TimeUs(std::llround(double(nt - c.sourceIn) / c.speed)); };
                for (AudioSegment n : nested) {
                    n.timelineStart = std::clamp(toOuter(n.timelineStart), from, to);
                    n.timelineEnd = std::clamp(toOuter(n.timelineEnd), from, to);
                    if (n.timelineEnd <= n.timelineStart)
                        continue;
                    n.speed *= c.speed;
                    n.gainStart *= gainAt(n.timelineStart);
                    n.gainEnd *= gainAt(n.timelineEnd);
                    segments.push_back(n);
                }
                continue;
            }

            auto emit = [&](TimeUs a, TimeUs b) {
                AudioSegment seg;
                seg.clipId = c.id;
                seg.instance = instanceKey(outer, c.id);
                seg.media = m;
                seg.timelineStart = a;
                seg.timelineEnd = b;
                seg.sourceStart = sourceTimeAtTime(s, c, a);
                seg.speed = c.speed;
                if (c.hasRamp() || c.reverse) {
                    seg.sourceEnd = sourceTimeAtTime(s, c, b);
                    seg.speed = b > a ? double(std::llabs(seg.sourceEnd - seg.sourceStart)) / double(b - a) : c.speed;
                }
                seg.denoise = c.denoise;
                seg.gainStart = gainAt(a);
                seg.gainEnd = gainAt(b);
                segments.push_back(seg);
            };
            if (c.hasRamp()) { // un tramo por fotograma: la velocidad cambia de uno a otro
                for (TimeUs a = from; a < to;) {
                    const TimeUs b = std::min(to, s.frameToTime(s.timeToFrame(a) + 1));
                    emit(a, std::max(b, a + 1));
                    a = std::max(b, a + 1);
                }
            } else {
                emit(from, to);
            }
        }
    };
    for (const Track& t : vtracks)
        collect(t, true);
    for (const Track& t : atracks)
        collect(t, false);
}

} // namespace

TimeUs AudioSegment::timelineFor(TimeUs source) const
{
    if (varispeed()) {
        const TimeUs span = sourceEnd - sourceStart;
        if (span == 0)
            return timelineStart;
        return timelineStart + TimeUs(std::llround(double(source - sourceStart) * double(timelineEnd - timelineStart) / double(span)));
    }
    return timelineStart + TimeUs(std::llround(double(source - sourceStart) / (speed > 0 ? speed : 1.0)));
}

std::vector<AudioSegment> audioSegmentsIn(const Project& p, TimeUs t0, TimeUs t1)
{
    std::vector<AudioSegment> segments;
    collectAudio(p, p.videoTracks, p.audioTracks, t0, t1, 0, 0, segments);
    return segments;
}

} // namespace movix::core
