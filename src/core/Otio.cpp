#include "core/Otio.h"

#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace movix::core::otio {

namespace {

// Metadatos propios dentro de los objetos OTIO (los demás programas los conservan sin usarlos).
const QString kMeta = QStringLiteral("movixstudio");
const QString kDefaultMedia = QStringLiteral("DEFAULT_MEDIA");

const char* const kTransitionNames[] = {"none", "dissolve", "dipToBlack", "wipeLeft", "wipeRight", "pushLeft", "pushRight", "slideLeft", "ofx"};

// Colores de marcador de OTIO y el ARGB con el que se muestran aquí.
struct MarkerColor {
    const char* name;
    std::uint32_t argb;
};
const MarkerColor kMarkerColors[] = {
    {"YELLOW", 0xFFFFC266}, {"CYAN", 0xFF4FC3F7},    {"RED", 0xFFE5484D},   {"ORANGE", 0xFFFF9A3D},
    {"GREEN", 0xFF5BC26B},  {"BLUE", 0xFF4A86E8},    {"PURPLE", 0xFF9B6BE0}, {"MAGENTA", 0xFFE055D0},
    {"PINK", 0xFFFF8FC8},   {"WHITE", 0xFFF0F0F0},   {"BLACK", 0xFF202020},
};

QString argb(std::uint32_t c)
{
    return QStringLiteral("#%1").arg(c, 8, 16, QLatin1Char('0'));
}

const char* nearestMarkerColor(std::uint32_t c)
{
    auto channel = [](std::uint32_t v, int shift) { return int((v >> shift) & 0xFF); };
    const MarkerColor* best = &kMarkerColors[0];
    int bestDistance = INT32_MAX;
    for (const MarkerColor& m : kMarkerColors) {
        int d = 0;
        for (int shift : {16, 8, 0}) {
            const int diff = channel(c, shift) - channel(m.argb, shift);
            d += diff * diff;
        }
        if (d < bestDistance) {
            bestDistance = d;
            best = &m;
        }
    }
    return best->name;
}

QJsonObject rationalTime(double value, double rate)
{
    return QJsonObject{{"OTIO_SCHEMA", "RationalTime.1"}, {"rate", rate}, {"value", value}};
}

QJsonObject timeRange(double start, double duration, double rate)
{
    return QJsonObject{{"OTIO_SCHEMA", "TimeRange.1"}, {"start_time", rationalTime(start, rate)},
                       {"duration", rationalTime(duration, rate)}};
}

// --- Exportación ---

class Exporter {
public:
    explicit Exporter(const Project& p) : m_p(p), m_rate(p.settings.frameRate.value()) {}

    QJsonObject timeline(const QString& name) const
    {
        const ProjectSettings& s = m_p.settings;
        const QJsonObject meta{
            {"width", s.width},
            {"height", s.height},
            {"frameRate", QJsonArray{qint64(s.frameRate.num), qint64(s.frameRate.den)}},
            {"sampleRate", s.sampleRate},
            {"channels", s.channels},
        };
        QJsonObject stack = sequenceStack(m_p.mainSequence);
        stack["name"] = QStringLiteral("tracks");
        return QJsonObject{
            {"OTIO_SCHEMA", "Timeline.1"},
            {"name", name},
            {"metadata", QJsonObject{{kMeta, meta}}},
            {"global_start_time", rationalTime(0, m_rate)},
            {"tracks", stack},
        };
    }

private:
    QJsonObject frames(FramePos start, FramePos duration) const { return timeRange(double(start), double(duration), m_rate); }
    double toValue(TimeUs t) const { return double(t) * m_rate / double(kUsPerSecond); }

    const std::vector<Marker>& markersOf(Id sequence) const
    {
        static const std::vector<Marker> none;
        if (sequence == m_p.activeSequence)
            return m_p.markers;
        const Sequence* seq = m_p.findSequence(sequence);
        return seq ? seq->markers : none;
    }

    // Pila con las pistas de una secuencia: primero las de video (de abajo arriba), luego las de
    // audio y, al final, el audio de los clips de video.
    QJsonObject sequenceStack(Id sequence) const
    {
        static const std::vector<Track> none;
        const auto* video = m_p.videoTracksOf(sequence);
        const auto* audio = m_p.audioTracksOf(sequence);
        return stackOf(sequence, video ? *video : none, audio ? *audio : none);
    }

    // OTIO no tiene multicámara: el clip se exporta como una pila con todos los ángulos en la que
    // solo están activas la pista del ángulo que se ve y la del audio que suena (así lo reproducen
    // los demás editores); los metadatos del clip (ángulo y audio) la recuperan al importar.
    QJsonObject multicamStack(const Clip& mc) const
    {
        static const std::vector<Track> none;
        const auto* nv = m_p.videoTracksOf(mc.sequenceId);
        const auto* na = m_p.audioTracksOf(mc.sequenceId);
        std::vector<Track> video = nv ? *nv : none, audio = na ? *na : none;
        const int source = mc.audioAngle >= 0 ? mc.audioAngle : mc.angle;
        for (size_t i = 0; i < video.size(); ++i) {
            video[i].hidden = int(i) != mc.angle;
            video[i].muted = int(i) != source; // desactiva la pista de su audio
        }
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i].muted = int(video.size() + i) != source;
        return stackOf(mc.sequenceId, video, audio);
    }

    QJsonObject stackOf(Id sequence, const std::vector<Track>& video, const std::vector<Track>& audio) const
    {
        QJsonArray children;
        for (const Track& t : video)
            children.append(track(t, TrackKind::Video, t.clips, false));
        for (const Track& t : audio)
            children.append(track(t, TrackKind::Audio, t.clips, false));
        {
            for (const Track& t : video) {
                std::vector<Clip> linked;
                for (const Clip& c : t.clips)
                    if (hasLinkedAudio(c))
                        linked.push_back(c);
                if (!linked.empty())
                    children.append(track(t, TrackKind::Audio, linked, true));
            }
        }
        QJsonArray markers;
        for (const Marker& m : markersOf(sequence)) {
            markers.append(QJsonObject{
                {"OTIO_SCHEMA", "Marker.2"},
                {"name", QString::fromStdString(m.label)},
                {"metadata", QJsonObject{{kMeta, QJsonObject{{"color", argb(m.color)}}}}},
                {"color", QLatin1String(nearestMarkerColor(m.color))},
                {"marked_range", frames(m.frame, 0)},
                {"comment", QString::fromStdString(m.label)}, // Kdenlive muestra el comentario, no el nombre
            });
        }
        return QJsonObject{
            {"OTIO_SCHEMA", "Stack.1"},
            {"name", QString::fromStdString(m_p.sequenceName(sequence))},
            {"metadata", QJsonObject{}},
            {"source_range", QJsonValue()},
            {"effects", QJsonArray{}},
            {"markers", markers},
            {"enabled", true},
            {"children", children},
        };
    }

    bool hasLinkedAudio(const Clip& c) const
    {
        if (c.isGenerated() || c.audioDetached)
            return false;
        const MediaItem* m = m_p.findMedia(c.mediaId);
        return m && m->hasAudio;
    }

    QJsonObject track(const Track& t, TrackKind kind, const std::vector<Clip>& clips, bool linkedAudio) const
    {
        QJsonArray children;
        FramePos cursor = 0;
        const Clip* prev = nullptr;
        for (const Clip& c : clips) {
            if (c.start > cursor) {
                children.append(gap(c.start - cursor));
            } else if (prev && c.transitionIn.active()) {
                const TransitionSpan span = transitionSpan(c.transitionIn);
                const QString type = QLatin1String(kTransitionNames[int(c.transitionIn.type)]);
                QJsonObject meta{{"type", type}};
                if (c.transitionIn.type == TransitionType::Ofx)
                    meta["ofx"] = ofxEffectToJson(c.transitionIn.ofx);
                children.append(QJsonObject{
                    {"OTIO_SCHEMA", "Transition.1"},
                    {"name", type},
                    {"metadata", QJsonObject{{kMeta, meta}}},
                    {"in_offset", rationalTime(double(span.before), m_rate)},
                    {"out_offset", rationalTime(double(span.after), m_rate)},
                    {"transition_type", c.transitionIn.type == TransitionType::Dissolve || linkedAudio
                                            ? QStringLiteral("SMPTE_Dissolve")
                                            : QStringLiteral("Custom_Transition")},
                });
            }
            children.append(item(c, kind, linkedAudio));
            cursor = c.end();
            prev = &c;
        }
        QJsonObject meta{{"locked", t.locked}, {"volume", t.volume}};
        if (linkedAudio)
            meta["linkedTo"] = QString::fromStdString(t.name);
        else if (kind == TrackKind::Video)
            meta["muted"] = t.muted; // silencia el audio de sus clips
        // `t` es la pista de video también para su audio: su "silenciar" desactiva esa pista.
        const bool enabled = kind == TrackKind::Video && !linkedAudio ? !t.hidden : !t.muted;
        return QJsonObject{
            {"OTIO_SCHEMA", "Track.1"},
            {"name", linkedAudio ? QStringLiteral("%1 audio").arg(QString::fromStdString(t.name)) : QString::fromStdString(t.name)},
            {"metadata", QJsonObject{{kMeta, meta}}},
            {"source_range", QJsonValue()},
            {"effects", QJsonArray{}},
            {"markers", QJsonArray{}},
            {"enabled", enabled},
            {"children", children},
            {"kind", kind == TrackKind::Video ? QStringLiteral("Video") : QStringLiteral("Audio")},
        };
    }

    QJsonObject gap(FramePos duration) const
    {
        return QJsonObject{
            {"OTIO_SCHEMA", "Gap.1"},     {"name", QString()},      {"metadata", QJsonObject{}},
            {"source_range", frames(0, duration)}, {"effects", QJsonArray{}}, {"markers", QJsonArray{}},
            {"enabled", true},
        };
    }

    QJsonObject item(const Clip& c, TrackKind kind, bool linkedAudio) const
    {
        QJsonObject meta;
        if (!c.volume.isAnimated() && c.volume.value != 1.0)
            meta["volume"] = c.volume.value;
        if (kind == TrackKind::Video && !linkedAudio) {
            if (c.fadeIn)
                meta["fadeIn"] = qint64(c.fadeIn);
            if (c.fadeOut)
                meta["fadeOut"] = qint64(c.fadeOut);
        }
        if (c.audioFadeIn)
            meta["audioFadeIn"] = qint64(c.audioFadeIn);
        if (c.audioFadeOut)
            meta["audioFadeOut"] = qint64(c.audioFadeOut);
        if (c.denoise > 0)
            meta["denoise"] = c.denoise;
        if (linkedAudio)
            meta["linked"] = true;

        QJsonArray effects;
        // OTIO no tiene velocidad variable: se exporta la media; al revés, negativa.
        const double speed = c.hasRamp() && c.duration > 0
                                 ? double(clipSourceSpan(m_p.settings, c)) / double(m_p.settings.frameToTime(c.duration))
                                 : c.speed;
        if (speed != 1.0 || c.reverse) {
            effects.append(QJsonObject{{"OTIO_SCHEMA", "LinearTimeWarp.1"}, {"name", QString()}, {"metadata", QJsonObject{}},
                                       {"effect_name", "LinearTimeWarp"}, {"time_scalar", c.reverse ? -speed : speed}});
        }
        const QJsonObject sourceRange = timeRange(toValue(c.sourceIn), double(c.duration), m_rate);

        if (c.isCompound()) {
            QJsonObject stack = c.isMulticam() ? multicamStack(c) : sequenceStack(c.sequenceId);
            if (c.isMulticam()) {
                meta["angle"] = c.angle;
                meta["audioAngle"] = c.audioAngle;
            }
            stack["source_range"] = sourceRange;
            stack["effects"] = effects;
            stack["enabled"] = c.enabled;
            stack["metadata"] = QJsonObject{{kMeta, meta}};
            return stack;
        }

        QString name;
        QJsonObject reference;
        if (c.title) {
            name = QString::fromStdString(c.title->text);
            reference = QJsonObject{
                {"OTIO_SCHEMA", "GeneratorReference.1"},
                {"name", name},
                {"metadata", QJsonObject{}},
                {"available_range", QJsonValue()},
                {"available_image_bounds", QJsonValue()},
                {"generator_kind", QLatin1String(kTitleGeneratorKind)},
                {"parameters", titleToJson(*c.title)},
            };
        } else if (c.generator) {
            name = QString::fromStdString(c.generator->pluginId);
            reference = QJsonObject{
                {"OTIO_SCHEMA", "GeneratorReference.1"},
                {"name", name},
                {"metadata", QJsonObject{}},
                {"available_range", QJsonValue()},
                {"available_image_bounds", QJsonValue()},
                {"generator_kind", QLatin1String(kOfxGeneratorKind)},
                {"parameters", ofxEffectToJson(*c.generator)},
            };
        } else if (const MediaItem* m = m_p.findMedia(c.mediaId)) {
            name = QString::fromStdString(m->name);
            reference = QJsonObject{
                {"OTIO_SCHEMA", "ExternalReference.1"},
                {"name", name},
                {"metadata", QJsonObject{}},
                {"available_range", m->isStill ? QJsonValue() : QJsonValue(timeRange(0, toValue(m->duration), m_rate))},
                {"available_image_bounds", QJsonValue()},
                // Ruta absoluta tal cual (con barras normales): Kdenlive no entiende las URL file://.
                {"target_url", QDir::fromNativeSeparators(QString::fromStdString(m->path))},
            };
        }
        return QJsonObject{
            {"OTIO_SCHEMA", "Clip.2"},
            {"name", name},
            {"metadata", QJsonObject{{kMeta, meta}}},
            {"source_range", sourceRange},
            {"effects", effects},
            {"markers", QJsonArray{}},
            {"enabled", c.enabled},
            {"media_references", QJsonObject{{kDefaultMedia, reference}}},
            {"active_media_reference_key", kDefaultMedia},
        };
    }

    const Project& m_p;
    double m_rate;
};

// --- Importación ---

QString schemaName(const QJsonObject& o)
{
    return o["OTIO_SCHEMA"].toString().section(QLatin1Char('.'), 0, 0);
}

double seconds(const QJsonValue& v)
{
    const QJsonObject t = v.toObject();
    const double rate = t["rate"].toDouble();
    return rate > 0 ? t["value"].toDouble() / rate : 0.0;
}

struct Range {
    double start = 0;
    double duration = 0;
};

std::optional<Range> rangeOf(const QJsonValue& v)
{
    if (!v.isObject())
        return std::nullopt;
    const QJsonObject r = v.toObject();
    return Range{seconds(r["start_time"]), seconds(r["duration"])};
}

QJsonObject movixMeta(const QJsonObject& o)
{
    return o["metadata"].toObject()[kMeta].toObject();
}

// Referencia de medio activa de un clip (Clip.2 tiene varias; Clip.1, una).
QJsonObject activeReference(const QJsonObject& clip)
{
    if (clip.contains("media_references")) {
        const QJsonObject refs = clip["media_references"].toObject();
        const QString key = clip["active_media_reference_key"].toString(kDefaultMedia);
        return refs[key].toObject();
    }
    return clip["media_reference"].toObject();
}

// Duración propia de un elemento cuando no tiene `source_range`.
double naturalDuration(const QJsonObject& o)
{
    if (const auto r = rangeOf(o["source_range"]))
        return r->duration;
    const QString schema = schemaName(o);
    if (schema == QLatin1String("Clip")) {
        const auto r = rangeOf(activeReference(o)["available_range"]);
        return r ? r->duration : 0.0;
    }
    if (schema == QLatin1String("Track")) {
        double sum = 0;
        for (const QJsonValue& child : o["children"].toArray())
            if (schemaName(child.toObject()) != QLatin1String("Transition"))
                sum += naturalDuration(child.toObject());
        return sum;
    }
    if (schema == QLatin1String("Stack")) {
        double longest = 0;
        for (const QJsonValue& child : o["children"].toArray())
            longest = std::max(longest, naturalDuration(child.toObject()));
        return longest;
    }
    return 0.0;
}

// Primera velocidad de fotogramas que aparezca en los tiempos del documento.
double firstRate(const QJsonValue& v)
{
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (schemaName(o) == QLatin1String("RationalTime"))
            return o["rate"].toDouble();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (const double r = firstRate(*it); r > 0)
                return r;
    } else if (v.isArray()) {
        for (const QJsonValue& x : v.toArray())
            if (const double r = firstRate(x); r > 0)
                return r;
    }
    return 0.0;
}

class Importer {
public:
    Importer(const QString& otioPath, const MediaProbe& probe) : m_dir(QFileInfo(otioPath).absoluteDir()), m_probe(probe) {}

    ImportResult run(const QJsonObject& timeline)
    {
        ImportResult result;
        m_p = Project{};
        ProjectSettings& s = m_p.settings;
        const QJsonObject meta = movixMeta(timeline);
        const QJsonArray rate = meta["frameRate"].toArray();
        if (rate.size() == 2 && rate.at(0).toInteger() > 0 && rate.at(1).toInteger() > 0) {
            s.frameRate = {rate.at(0).toInteger(), rate.at(1).toInteger()};
        } else {
            double fps = timeline["global_start_time"].toObject()["rate"].toDouble();
            if (fps <= 0)
                fps = firstRate(timeline["tracks"]);
            if (fps > 0)
                s.frameRate = edit::guessFrameRate(fps);
        }
        s.width = meta["width"].toInt(0);
        s.height = meta["height"].toInt(0);
        s.sampleRate = meta["sampleRate"].toInt(s.sampleRate);
        s.channels = meta["channels"].toInt(s.channels);
        m_fps = s.frameRate.value();

        m_p.mainSequence = m_p.activeSequence = m_p.newId();
        m_p.activeName = timeline["name"].toString().toStdString();
        if (m_p.activeName.empty())
            m_p.activeName = "Principal";
        importStack(timeline["tracks"].toObject(), m_p.videoTracks, m_p.audioTracks, m_p.markers);

        // Sin resolución guardada, la del primer medio de video.
        if (s.width <= 0 || s.height <= 0) {
            s.width = 1920;
            s.height = 1080;
            for (const MediaItem& m : m_p.media) {
                if (m.hasVideo && m.width > 0 && m.height > 0) {
                    s.width = m.width;
                    s.height = m.height;
                    break;
                }
            }
        }
        if (m_p.videoTracks.empty())
            addEmptyTrack(m_p.videoTracks, TrackKind::Video);
        if (m_p.audioTracks.empty())
            addEmptyTrack(m_p.audioTracks, TrackKind::Audio);

        std::string why;
        if (!m_p.validate(&why)) {
            result.error = QCoreApplication::translate("Otio", "La línea de tiempo no se pudo convertir: %1").arg(QString::fromStdString(why));
            return result;
        }
        result.project = std::move(m_p);
        result.warnings = m_warnings;
        result.missingMedia = m_missing;
        return result;
    }

private:
    FramePos toFrame(double secs) const { return FramePos(std::llround(secs * m_fps)); }
    static TimeUs toUs(double secs) { return TimeUs(std::llround(secs * double(kUsPerSecond))); }

    void warn(const QString& text)
    {
        if (!m_warnings.contains(text))
            m_warnings.append(text);
    }

    void addEmptyTrack(std::vector<Track>& tracks, TrackKind kind)
    {
        Track t;
        t.id = m_p.newId();
        t.kind = kind;
        t.name = (kind == TrackKind::Video ? "V" : "A") + std::to_string(tracks.size() + 1);
        tracks.push_back(t);
    }

    // Contenido de la pila de una multicámara sin lo que cambia entre sus tramos (qué pistas están
    // activas): los tramos de la misma multicámara dan la misma clave.
    static QByteArray multicamKey(const QJsonObject& stack)
    {
        QJsonArray tracks;
        for (const QJsonValue& v : stack["children"].toArray()) {
            QJsonObject t = v.toObject();
            t.remove(QStringLiteral("enabled"));
            QJsonObject metadata = t["metadata"].toObject();
            QJsonObject meta = metadata[kMeta].toObject();
            meta.remove(QStringLiteral("muted"));
            metadata[kMeta] = meta;
            t["metadata"] = metadata;
            tracks.append(t);
        }
        return QJsonDocument(tracks).toJson(QJsonDocument::Compact);
    }

    void importStack(const QJsonObject& stack, std::vector<Track>& video, std::vector<Track>& audio, std::vector<Marker>& markers)
    {
        for (const QJsonValue& v : stack["children"].toArray()) {
            const QJsonObject child = v.toObject();
            if (schemaName(child) != QLatin1String("Track")) {
                warn(QCoreApplication::translate("Otio", "Se omitió un elemento fuera de las pistas (%1).").arg(child["OTIO_SCHEMA"].toString()));
                continue;
            }
            const QString kind = child["kind"].toString();
            if (kind != QLatin1String("Video") && kind != QLatin1String("Audio")) {
                warn(QCoreApplication::translate("Otio", "Se omitió una pista de tipo «%1».").arg(kind));
                continue;
            }
            const TrackKind k = kind == QLatin1String("Video") ? TrackKind::Video : TrackKind::Audio;
            auto& list = k == TrackKind::Video ? video : audio;
            Track t;
            t.id = m_p.newId();
            t.kind = k;
            t.name = child["name"].toString().toStdString();
            if (t.name.empty())
                t.name = (k == TrackKind::Video ? "V" : "A") + std::to_string(list.size() + 1);
            const QJsonObject meta = movixMeta(child);
            t.locked = meta["locked"].toBool();
            t.muted = meta["muted"].toBool();
            t.volume = std::clamp(meta["volume"].toDouble(1.0), 0.0, 4.0);
            if (!child["enabled"].toBool(true))
                (k == TrackKind::Video ? t.hidden : t.muted) = true;
            importTrack(child, t, markers);
            list.push_back(std::move(t));
        }
        addMarkers(stack, 0, 0, markers);
        linkAudio(video, audio);
        std::sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.frame < b.frame; });
        markers.erase(std::unique(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.frame == b.frame; }),
                      markers.end());
    }

    // Marcadores del elemento; `origin` es su posición en la secuencia y `sourceStart`, el instante
    // de su contenido que se ve en ella.
    void addMarkers(const QJsonObject& o, double origin, double sourceStart, std::vector<Marker>& markers)
    {
        for (const QJsonValue& v : o["markers"].toArray()) {
            const QJsonObject m = v.toObject();
            const auto range = rangeOf(m["marked_range"]);
            if (!range)
                continue;
            const FramePos frame = toFrame(origin + range->start - sourceStart);
            if (frame < 0)
                continue;
            Marker marker;
            marker.frame = frame;
            marker.label = m["name"].toString().toStdString();
            if (marker.label.empty())
                marker.label = m["comment"].toString().toStdString();
            bool ok = false;
            const std::uint32_t exact = movixMeta(m)["color"].toString().mid(1).toUInt(&ok, 16);
            if (ok) {
                marker.color = exact;
            } else {
                const QString color = m["color"].toString().toUpper();
                for (const MarkerColor& c : kMarkerColors)
                    if (color == QLatin1String(c.name))
                        marker.color = c.argb;
            }
            markers.push_back(marker);
        }
    }

    void importTrack(const QJsonObject& track, Track& t, std::vector<Marker>& markers)
    {
        addMarkers(track, 0, 0, markers);
        double cursor = 0; // segundos desde el inicio de la pista
        std::optional<QJsonObject> transition;
        for (const QJsonValue& v : track["children"].toArray()) {
            const QJsonObject child = v.toObject();
            const QString schema = schemaName(child);
            if (schema == QLatin1String("Transition")) {
                transition = child; // ocupa el corte entre el elemento anterior y el siguiente
                continue;
            }
            const double duration = naturalDuration(child);
            const FramePos start = toFrame(cursor);
            const FramePos end = toFrame(cursor + duration);
            const double origin = cursor;
            cursor += duration;
            const auto pendingTransition = std::exchange(transition, std::nullopt);
            if (schema == QLatin1String("Gap") || end <= start)
                continue;

            std::optional<Clip> clip = makeClip(child, t.kind);
            if (!clip)
                continue;
            clip->id = m_p.newId();
            clip->start = start;
            clip->duration = end - start;
            clip->fadeIn = std::min(clip->fadeIn, clip->duration);
            clip->fadeOut = std::min(clip->fadeOut, clip->duration - clip->fadeIn);
            clip->audioFadeIn = std::min(clip->audioFadeIn, clip->duration);
            clip->audioFadeOut = std::min(clip->audioFadeOut, clip->duration - clip->audioFadeIn);
            if (clip->mediaId != kNoId)
                growPlaceholder(*clip);
            // Un clip de audio de MovixStudio solo se une a uno de video si se exportó como su audio.
            if (t.kind == TrackKind::Audio && child["metadata"].toObject().contains(kMeta) && !movixMeta(child)["linked"].toBool())
                m_unlinkable.insert(clip->id);
            if (pendingTransition && !t.clips.empty() && t.clips.back().end() == start)
                clip->transitionIn = transitionFrom(*pendingTransition, t.clips.back(), *clip);
            if (schema == QLatin1String("Clip")) { // los de una pila anidada van a su secuencia
                const auto source = rangeOf(child["source_range"]);
                addMarkers(child, origin, source ? source->start : 0, markers);
            }
            t.clips.push_back(std::move(*clip));
        }
    }

    TransitionIn transitionFrom(const QJsonObject& o, const Clip& prev, const Clip& next) const
    {
        TransitionIn result;
        const FramePos longest = std::min(prev.duration, next.duration);
        if (longest < 2)
            return result;
        QString name = movixMeta(o)["type"].toString();
        if (name.isEmpty())
            name = o["name"].toString();
        result.type = TransitionType::Dissolve;
        for (size_t i = 1; i < std::size(kTransitionNames); ++i)
            if (name == QLatin1String(kTransitionNames[i]))
                result.type = TransitionType(i);
        if (result.type == TransitionType::Ofx) { // sin el plugin, una disolvencia
            result.ofx = ofxEffectFromJson(movixMeta(o)["ofx"].toObject());
            if (result.ofx.pluginId.empty())
                result = {TransitionType::Dissolve};
        }
        result.duration = std::clamp<FramePos>(toFrame(seconds(o["in_offset"]) + seconds(o["out_offset"])), 2, longest);
        return result;
    }

    std::optional<Clip> makeClip(const QJsonObject& o, TrackKind kind)
    {
        const QString schema = schemaName(o);
        const QString name = o["name"].toString();
        Clip c;
        c.enabled = o["enabled"].toBool(true);
        const QJsonObject meta = movixMeta(o);
        c.volume = Animated(std::clamp(meta["volume"].toDouble(1.0), 0.0, 4.0));
        c.fadeIn = std::max<qint64>(0, meta["fadeIn"].toInteger());
        c.fadeOut = std::max<qint64>(0, meta["fadeOut"].toInteger());
        c.audioFadeIn = std::max<qint64>(0, meta["audioFadeIn"].toInteger());
        c.audioFadeOut = std::max<qint64>(0, meta["audioFadeOut"].toInteger());
        c.denoise = std::clamp(meta["denoise"].toDouble(), 0.0, 1.0);
        if (!readSpeed(o, c))
            return std::nullopt;
        const auto source = rangeOf(o["source_range"]);

        if (schema == QLatin1String("Stack")) {
            c.reverse = false; // (sin compuestos al revés)
            c.sourceIn = source ? std::max<TimeUs>(0, toUs(source->start)) : 0;
            c.fadeIn = c.fadeOut = 0;
            const bool multicam = meta.contains("angle");
            // Los tramos de una multicámara (uno por cambio de ángulo) comparten su secuencia.
            const QByteArray content = multicam ? multicamKey(o) : QByteArray();
            if (multicam && m_multicams.contains(content)) {
                c.sequenceId = m_multicams[content];
            } else {
                Sequence seq;
                seq.id = m_p.newId();
                seq.name = name.isEmpty() ? "Compuesto" : name.toStdString();
                importStack(o, seq.videoTracks, seq.audioTracks, seq.markers);
                if (multicam) {
                    // El ángulo y el audio los elige el clip, no las pistas desactivadas.
                    for (Track& t : seq.videoTracks)
                        t.hidden = t.muted = false;
                    for (Track& t : seq.audioTracks)
                        t.muted = false;
                    m_multicams[content] = seq.id;
                }
                c.sequenceId = seq.id;
                m_p.sequences.push_back(std::move(seq));
            }
            if (multicam) {
                const Sequence& seq = *m_p.findSequence(c.sequenceId);
                const int videoCount = int(seq.videoTracks.size());
                const int audioCount = int(seq.audioTracks.size());
                if (videoCount > 0) {
                    c.angle = std::clamp(meta["angle"].toInt(), 0, videoCount - 1);
                    const int audio = meta["audioAngle"].toInt(-1);
                    c.audioAngle = audio >= 0 && audio < videoCount + audioCount ? audio : -1;
                }
            }
            return c;
        }
        if (schema != QLatin1String("Clip")) {
            warn(QCoreApplication::translate("Otio", "Se omitieron elementos de tipo %1.").arg(o["OTIO_SCHEMA"].toString()));
            return std::nullopt;
        }

        const QJsonObject ref = activeReference(o);
        const QString refSchema = schemaName(ref);
        if (refSchema == QLatin1String("GeneratorReference")) {
            const QString generatorKind = ref["generator_kind"].toString();
            const bool title = generatorKind == QLatin1String(kTitleGeneratorKind);
            OfxEffect generator;
            if (generatorKind == QLatin1String(kOfxGeneratorKind))
                generator = ofxEffectFromJson(ref["parameters"].toObject());
            if ((!title && generator.pluginId.empty()) || kind != TrackKind::Video) {
                warn(QCoreApplication::translate("Otio", "Se omitieron clips generados de tipo «%1».").arg(generatorKind));
                return std::nullopt;
            }
            if (title)
                c.title = titleFromJson(ref["parameters"].toObject());
            else
                c.generator = std::move(generator);
            c.speed = 1.0;
            c.speedRamp = Animated(1.0);
            c.reverse = false;
            c.audioFadeIn = c.audioFadeOut = 0;
            return c;
        }
        if (refSchema != QLatin1String("ExternalReference")) {
            warn(QCoreApplication::translate("Otio", "Se omitieron clips sin archivo de medio («%1»).").arg(name));
            return std::nullopt;
        }
        const QString path = resolvePath(ref["target_url"].toString());
        if (path.isEmpty()) {
            warn(QCoreApplication::translate("Otio", "Se omitieron clips con medios que no son archivos locales («%1»).").arg(ref["target_url"].toString()));
            return std::nullopt;
        }
        const auto available = rangeOf(ref["available_range"]);
        c.mediaId = mediaFor(path, kind);
        if (c.mediaId == kNoId)
            return std::nullopt;
        // Los tiempos de OTIO cuentan desde el comienzo del medio (su código de tiempo, si lo tiene).
        const double in = (source ? source->start : 0.0) - (available ? available->start : 0.0);
        c.sourceIn = std::max<TimeUs>(0, toUs(in));
        if (kind == TrackKind::Audio)
            c.fadeIn = c.fadeOut = 0;
        return c;
    }

    bool readSpeed(const QJsonObject& o, Clip& c)
    {
        for (const QJsonValue& v : o["effects"].toArray()) {
            const QJsonObject e = v.toObject();
            const QString schema = schemaName(e);
            if (schema == QLatin1String("LinearTimeWarp")) {
                const double scalar = e["time_scalar"].toDouble(1.0);
                if (scalar == 0) {
                    warn(QCoreApplication::translate("Otio", "Los clips congelados se importan a velocidad normal."));
                    continue;
                }
                c.speed *= std::abs(scalar);
                if (scalar < 0)
                    c.reverse = !c.reverse;
            } else if (schema == QLatin1String("FreezeFrame")) {
                warn(QCoreApplication::translate("Otio", "Los clips congelados se importan a velocidad normal."));
            } else {
                // Resolve pone en cada clip sus efectos (zoom dinámico...), casi siempre desactivados.
                const QJsonObject resolve = e["metadata"].toObject()["Resolve_OTIO"].toObject();
                if (resolve["Enabled"] == QJsonValue(false))
                    continue;
                const QString effect = resolve["Effect Name"].toString(e["effect_name"].toString(e["name"].toString(schema)));
                warn(QCoreApplication::translate("Otio", "No se importó el efecto «%1».").arg(effect));
            }
        }
        c.speed = std::clamp(c.speed, 0.01, 100.0);
        c.speedRamp = Animated(c.speed);
        return true;
    }

    QString resolvePath(const QString& target) const
    {
        if (target.isEmpty())
            return {};
        QString path;
        const QUrl url(target);
        if (url.scheme() == QLatin1String("file")) {
            // "file://D:/x" deja la unidad como servidor; "file:///D:/x" es la forma correcta.
            if (url.host().size() == 1)
                path = url.host() + QLatin1Char(':') + url.path();
            else
                path = url.toLocalFile();
        } else if (url.scheme().isEmpty() || url.scheme().size() == 1) {
            path = target; // ruta sin esquema (o con letra de unidad)
        } else {
            return {};
        }
        if (QDir::isRelativePath(path))
            path = m_dir.absoluteFilePath(path);
        return QDir::toNativeSeparators(QDir::cleanPath(path));
    }

    Id mediaFor(const QString& path, TrackKind kind)
    {
        const bool video = kind == TrackKind::Video;
        if (auto it = m_mediaByPath.find(path); it != m_mediaByPath.end()) {
            MediaItem* m = m_p.findMedia(it->second);
            if (m_placeholders.count(m->id)) {
                (video ? m->hasVideo : m->hasAudio) = true;
                return m->id;
            }
            if (video ? m->hasVideo : m->hasAudio)
                return m->id;
            warn(incompatible(path, kind));
            return kNoId;
        }
        std::optional<MediaItem> probed = m_probe ? m_probe(path.toStdString()) : std::nullopt;
        if (probed && !(video ? probed->hasVideo : probed->hasAudio)) {
            warn(incompatible(path, kind));
            return kNoId;
        }
        MediaItem item;
        if (probed) {
            item = *probed;
        } else {
            // Se conserva con los datos del OTIO para que el proyecto se pueda reconectar después.
            item.path = path.toStdString();
            item.name = QFileInfo(path).fileName().toStdString();
            (video ? item.hasVideo : item.hasAudio) = true;
            m_missing.append(path);
        }
        const Id id = edit::addMedia(m_p, item);
        if (!probed)
            m_placeholders.insert(id);
        m_mediaByPath[path] = id;
        return id;
    }

    static QString incompatible(const QString& path, TrackKind kind)
    {
        return kind == TrackKind::Video ? QCoreApplication::translate("Otio", "«%1» no tiene video: se omitieron sus clips de video.").arg(QFileInfo(path).fileName())
                                        : QCoreApplication::translate("Otio", "«%1» no tiene audio: se omitieron sus clips de audio.").arg(QFileInfo(path).fileName());
    }

    // Un medio que falta debe durar al menos lo que usan sus clips.
    void growPlaceholder(const Clip& c)
    {
        if (!m_placeholders.count(c.mediaId))
            return;
        MediaItem* m = m_p.findMedia(c.mediaId);
        const TimeUs used = c.sourceIn + TimeUs(std::llround(double(m_p.settings.frameToTime(c.duration)) * c.speed));
        m->duration = std::max(m->duration, used);
    }

    // Los demás editores separan el audio de los clips de video en pistas de audio: el clip de
    // audio que coincide exactamente con uno de video vuelve a ser su audio; si no hay ninguno,
    // el clip de video queda sin audio propio (como si se hubiera separado).
    void linkAudio(std::vector<Track>& video, std::vector<Track>& audio)
    {
        const TimeUs tolerance = m_p.settings.frameDuration() / 2;
        std::set<Id> touched;
        for (Track& v : video) {
            for (Clip& c : v.clips) {
                if (c.isGenerated())
                    continue;
                const MediaItem* m = m_p.findMedia(c.mediaId);
                if (!m || !m->hasAudio)
                    continue;
                bool linked = false;
                for (Track& a : audio) {
                    auto it = std::find_if(a.clips.begin(), a.clips.end(), [&](const Clip& x) {
                        return !m_unlinkable.count(x.id) && x.mediaId == c.mediaId && x.start == c.start && x.duration == c.duration
                            && std::abs(x.sourceIn - c.sourceIn) <= tolerance && std::abs(x.speed - c.speed) < 1e-6;
                    });
                    if (it == a.clips.end())
                        continue;
                    c.volume = it->volume;
                    c.audioFadeIn = it->audioFadeIn;
                    c.audioFadeOut = it->audioFadeOut;
                    c.denoise = it->denoise;
                    a.clips.erase(it);
                    touched.insert(a.id);
                    linked = true;
                    break;
                }
                c.audioDetached = !linked;
            }
        }
        for (Track& a : audio) {
            if (!touched.count(a.id))
                continue;
            // Al quitar clips, una transición puede quedarse sin su clip anterior.
            for (size_t i = 0; i < a.clips.size(); ++i) {
                Clip& c = a.clips[i];
                if (c.transitionIn.active() && (i == 0 || a.clips[i - 1].end() != c.start))
                    c.transitionIn = {};
            }
        }
        std::erase_if(audio, [&](const Track& a) { return touched.count(a.id) && a.clips.empty(); });
    }

    QDir m_dir;
    MediaProbe m_probe;
    Project m_p;
    double m_fps = 30.0;
    std::map<QString, Id> m_mediaByPath;
    std::map<QByteArray, Id> m_multicams; // contenido de la pila → secuencia multicámara
    std::set<Id> m_placeholders;
    std::set<Id> m_unlinkable;
    QStringList m_warnings;
    QStringList m_missing;
};

// La primera línea de tiempo del documento (puede venir dentro de una colección).
std::optional<QJsonObject> findTimeline(const QJsonObject& o)
{
    const QString schema = schemaName(o);
    if (schema == QLatin1String("Timeline"))
        return o;
    if (schema == QLatin1String("SerializableCollection"))
        for (const QJsonValue& child : o["children"].toArray())
            if (auto t = findTimeline(child.toObject()))
                return t;
    return std::nullopt;
}

} // namespace

QByteArray exportTimeline(const Project& project, const QString& name)
{
    return QJsonDocument(Exporter(project).timeline(name)).toJson(QJsonDocument::Indented);
}

ImportResult importTimeline(const QByteArray& json, const QString& otioPath, const MediaProbe& probe)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull()) {
        ImportResult r;
        r.error = QCoreApplication::translate("Otio", "El archivo no es un OTIO válido (%1).").arg(parseError.errorString());
        return r;
    }
    const auto timeline = findTimeline(doc.object());
    if (!timeline) {
        ImportResult r;
        r.error = QCoreApplication::translate("Otio", "El archivo no contiene ninguna línea de tiempo de OpenTimelineIO.");
        return r;
    }
    return Importer(otioPath, probe).run(*timeline);
}

ImportResult importFile(const QString& path, const MediaProbe& probe)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        ImportResult r;
        r.error = QCoreApplication::translate("Otio", "No se pudo abrir el archivo: %1").arg(file.errorString());
        return r;
    }
    return importTimeline(file.readAll(), path, probe);
}

bool exportFile(const Project& project, const QString& path, QString* error)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(exportTimeline(project, QFileInfo(path).completeBaseName()));
        if (file.commit())
            return true;
    }
    if (error)
        *error = file.errorString();
    return false;
}

} // namespace movix::core::otio
