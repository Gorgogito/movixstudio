#include "core/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cmath>

namespace movix::core {

namespace {

const QString kFormatName = QStringLiteral("movixstudio-project");

// Nombres estables de los enumerados en el archivo (no dependen del orden en el código).
const char* const kTransitionNames[] = {"none", "dissolve", "dipToBlack", "wipeLeft", "wipeRight", "pushLeft", "pushRight", "slideLeft"};
const char* const kInterpolationNames[] = {"linear", "hold", "smooth"};
const char* const kBlendNames[] = {"normal",    "multiply",  "screen",    "overlay",    "darken",    "lighten", "colorDodge",
                                   "colorBurn", "hardLight", "softLight", "difference", "exclusion", "add",     "subtract"};
const char* const kMaskNames[] = {"none", "rectangle", "ellipse", "linear", "path"};
const char* const kMaskModeNames[] = {"add", "subtract", "intersect"};
const char* const kColorModeNames[] = {"sdr", "pq", "hlg"};
static_assert(std::size(kBlendNames) == kBlendModeCount);

template <typename E, size_t N>
E enumFromName(const QString& name, const char* const (&names)[N], E fallback)
{
    for (size_t i = 0; i < N; ++i)
        if (name == QLatin1String(names[i]))
            return E(i);
    return fallback;
}

// Un valor constante se guarda como número; uno animado, con su lista de keyframes.
QJsonValue animatedToJson(const Animated& a)
{
    if (!a.isAnimated())
        return a.value;
    QJsonArray keys;
    for (const Keyframe& k : a.keys)
        keys.append(QJsonArray{qint64(k.frame), k.value, QLatin1String(kInterpolationNames[int(k.interp)])});
    return QJsonObject{{"value", a.value}, {"keys", keys}};
}

Animated animatedFromJson(const QJsonValue& v, double fallback)
{
    if (v.isDouble())
        return Animated(v.toDouble());
    Animated a(fallback);
    if (!v.isObject())
        return a;
    const QJsonObject o = v.toObject();
    a.value = o["value"].toDouble(fallback);
    for (const QJsonValue& kv : o["keys"].toArray()) {
        const QJsonArray k = kv.toArray();
        a.setKey(k.at(0).toInteger(), k.at(1).toDouble(),
                 enumFromName(k.at(2).toString(), kInterpolationNames, Interpolation::Linear));
    }
    return a;
}

QString argb(std::uint32_t c)
{
    return QStringLiteral("#%1").arg(c, 8, 16, QLatin1Char('0'));
}

std::uint32_t argbFromJson(const QJsonValue& v, std::uint32_t fallback)
{
    bool ok = false;
    const std::uint32_t c = v.toString().mid(1).toUInt(&ok, 16);
    return ok ? c : fallback;
}

QJsonObject titleToJson(const TitleStyle& s)
{
    return QJsonObject{
        {"text", QString::fromStdString(s.text)},
        {"font", QString::fromStdString(s.font)},
        {"size", s.size},
        {"color", argb(s.color)},
        {"bold", s.bold},
        {"italic", s.italic},
        {"align", s.align},
        {"outline", s.outline},
        {"outlineColor", argb(s.outlineColor)},
        {"shadow", s.shadow},
        {"background", argb(s.background)},
    };
}

TitleStyle titleFromJson(const QJsonObject& o)
{
    TitleStyle s;
    s.text = o["text"].toString(QString::fromStdString(s.text)).toStdString();
    s.font = o["font"].toString(QString::fromStdString(s.font)).toStdString();
    s.size = o["size"].toDouble(s.size);
    s.color = argbFromJson(o["color"], s.color);
    s.bold = o["bold"].toBool(s.bold);
    s.italic = o["italic"].toBool(s.italic);
    s.align = o["align"].toInt(s.align);
    s.outline = o["outline"].toDouble(s.outline);
    s.outlineColor = argbFromJson(o["outlineColor"], s.outlineColor);
    s.shadow = o["shadow"].toBool(s.shadow);
    s.background = argbFromJson(o["background"], s.background);
    return s;
}

QJsonArray wheelToJson(const ColorWheel& w)
{
    return QJsonArray{w.r, w.g, w.b, w.master};
}

ColorWheel wheelFromJson(const QJsonValue& v)
{
    const QJsonArray a = v.toArray();
    return {a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble(), a.at(3).toDouble()};
}

QJsonArray curveToJson(const std::vector<CurvePoint>& curve)
{
    QJsonArray a;
    for (const CurvePoint& pt : curve)
        a.append(QJsonArray{pt.x, pt.y});
    return a;
}

std::vector<CurvePoint> curveFromJson(const QJsonValue& v)
{
    std::vector<CurvePoint> curve;
    for (const QJsonValue& pt : v.toArray())
        curve.push_back({pt.toArray().at(0).toDouble(), pt.toArray().at(1).toDouble()});
    return curve;
}

// La LUT se guarda como los medios: ruta absoluta y relativa al proyecto.
QJsonObject gradeToJson(const ColorGrade& g, const QDir& projectDir)
{
    QJsonArray hsl;
    for (const HslBand& b : g.hsl)
        hsl.append(QJsonArray{b.hue, b.saturation, b.luminance});
    QJsonObject o{
        {"exposure", g.exposure},
        {"contrast", g.contrast},
        {"saturation", g.saturation},
        {"vibrance", g.vibrance},
        {"temperature", g.temperature},
        {"tint", g.tint},
        {"highlights", g.highlights},
        {"shadows", g.shadows},
        {"whites", g.whites},
        {"blacks", g.blacks},
        {"lift", wheelToJson(g.lift)},
        {"gamma", wheelToJson(g.gamma)},
        {"gain", wheelToJson(g.gain)},
        {"curves", QJsonObject{{"master", curveToJson(g.curveMaster)},
                               {"red", curveToJson(g.curveRed)},
                               {"green", curveToJson(g.curveGreen)},
                               {"blue", curveToJson(g.curveBlue)}}},
        {"hsl", hsl},
    };
    if (!g.inputSpace.empty())
        o["inputSpace"] = QString::fromStdString(g.inputSpace);
    if (!g.lutPath.empty()) {
        const QString path = QString::fromStdString(g.lutPath);
        o["lut"] = QJsonObject{{"path", QDir::fromNativeSeparators(path)},
                               {"relativePath", projectDir.relativeFilePath(path)},
                               {"intensity", g.lutIntensity}};
    }
    return o;
}

ColorGrade gradeFromJson(const QJsonObject& o, const QDir& projectDir, QStringList& missing)
{
    ColorGrade g;
    g.exposure = o["exposure"].toDouble();
    g.contrast = o["contrast"].toDouble();
    g.saturation = o["saturation"].toDouble();
    g.vibrance = o["vibrance"].toDouble();
    g.temperature = o["temperature"].toDouble();
    g.tint = o["tint"].toDouble();
    g.highlights = o["highlights"].toDouble();
    g.shadows = o["shadows"].toDouble();
    g.whites = o["whites"].toDouble();
    g.blacks = o["blacks"].toDouble();
    g.inputSpace = o["inputSpace"].toString().toStdString();
    g.lift = wheelFromJson(o["lift"]);
    g.gamma = wheelFromJson(o["gamma"]);
    g.gain = wheelFromJson(o["gain"]);
    const QJsonObject curves = o["curves"].toObject();
    g.curveMaster = curveFromJson(curves["master"]);
    g.curveRed = curveFromJson(curves["red"]);
    g.curveGreen = curveFromJson(curves["green"]);
    g.curveBlue = curveFromJson(curves["blue"]);
    const QJsonArray hsl = o["hsl"].toArray();
    for (qsizetype i = 0; i < hsl.size() && i < qsizetype(g.hsl.size()); ++i) {
        const QJsonArray b = hsl.at(i).toArray();
        g.hsl[size_t(i)] = {b.at(0).toDouble(), b.at(1).toDouble(), b.at(2).toDouble()};
    }
    if (o.contains("lut")) {
        const QJsonObject lut = o["lut"].toObject();
        QString path = lut["path"].toString();
        if (!QFileInfo::exists(path)) {
            const QString relative = projectDir.absoluteFilePath(lut["relativePath"].toString());
            if (QFileInfo::exists(relative))
                path = QDir::cleanPath(relative);
            else if (!missing.contains(QDir::toNativeSeparators(path)))
                missing.append(QDir::toNativeSeparators(path));
        }
        g.lutPath = QDir::toNativeSeparators(path).toStdString();
        g.lutIntensity = lut["intensity"].toDouble(1.0);
    }
    return g;
}

// Sin keyframes, la corrección se guarda tal cual; con ellos, {value, keys: [{frame, interp, grade}]}.
QJsonObject animatedGradeToJson(const AnimatedGrade& a, const QDir& projectDir)
{
    if (!a.isAnimated())
        return gradeToJson(a.value, projectDir);
    QJsonArray keys;
    for (const GradeKey& k : a.keys)
        keys.append(QJsonObject{{"frame", qint64(k.frame)},
                                {"interp", QLatin1String(kInterpolationNames[int(k.interp)])},
                                {"grade", gradeToJson(k.grade, projectDir)}});
    return QJsonObject{{"value", gradeToJson(a.value, projectDir)}, {"keys", keys}};
}

AnimatedGrade animatedGradeFromJson(const QJsonObject& o, const QDir& projectDir, QStringList& missing)
{
    AnimatedGrade a;
    if (!o.contains("keys")) {
        a.value = gradeFromJson(o, projectDir, missing);
        return a;
    }
    a.value = gradeFromJson(o["value"].toObject(), projectDir, missing);
    for (const QJsonValue& v : o["keys"].toArray()) {
        const QJsonObject k = v.toObject();
        a.setKey(k["frame"].toInteger(), gradeFromJson(k["grade"].toObject(), projectDir, missing),
                 enumFromName(k["interp"].toString(), kInterpolationNames, Interpolation::Linear));
    }
    return a;
}

QJsonObject maskToJson(const Mask& m)
{
    QJsonObject o{{"shape", QLatin1String(kMaskNames[int(m.shape)])},
                  {"mode", QLatin1String(kMaskModeNames[int(m.mode)])},
                  {"invert", m.invert},
                  {"x", animatedToJson(m.x)},
                  {"y", animatedToJson(m.y)},
                  {"width", animatedToJson(m.width)},
                  {"height", animatedToJson(m.height)},
                  {"rotation", animatedToJson(m.rotation)},
                  {"feather", animatedToJson(m.feather)}};
    if (!m.points.empty()) {
        QJsonArray pts;
        for (const PathPoint& pt : m.points)
            pts.append(QJsonArray{pt.x, pt.y, pt.inX, pt.inY, pt.outX, pt.outY});
        o["points"] = pts;
    }
    return o;
}

Mask maskFromJson(const QJsonObject& m)
{
    Mask mask;
    const Mask def;
    mask.shape = enumFromName(m["shape"].toString(), kMaskNames, MaskShape::None);
    mask.mode = enumFromName(m["mode"].toString(), kMaskModeNames, MaskMode::Add);
    mask.invert = m["invert"].toBool();
    mask.x = animatedFromJson(m["x"], def.x.value);
    mask.y = animatedFromJson(m["y"], def.y.value);
    mask.width = animatedFromJson(m["width"], def.width.value);
    mask.height = animatedFromJson(m["height"], def.height.value);
    mask.rotation = animatedFromJson(m["rotation"], def.rotation.value);
    mask.feather = animatedFromJson(m["feather"], def.feather.value);
    for (const QJsonValue& v : m["points"].toArray()) {
        const QJsonArray a = v.toArray();
        mask.points.push_back({a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble(), a.at(3).toDouble(), a.at(4).toDouble(),
                               a.at(5).toDouble()});
    }
    return mask;
}

QJsonObject trackToJson(const Track& t, const QDir& projectDir)
{
    QJsonArray clips;
    for (const Clip& c : t.clips) {
        QJsonObject o{
            {"id", qint64(c.id)},
            {"mediaId", qint64(c.mediaId)},
            {"sequenceId", qint64(c.sequenceId)},
            {"start", qint64(c.start)},
            {"duration", qint64(c.duration)},
            {"sourceIn", qint64(c.sourceIn)},
            {"speed", c.speed},
            {"enabled", c.enabled},
            {"volume", animatedToJson(c.volume)},
            {"transform", QJsonObject{{"x", animatedToJson(c.transform.x)},
                                      {"y", animatedToJson(c.transform.y)},
                                      {"scale", animatedToJson(c.transform.scale)},
                                      {"rotation", animatedToJson(c.transform.rotation)},
                                      {"opacity", animatedToJson(c.transform.opacity)},
                                      {"crop", QJsonArray{animatedToJson(c.transform.cropLeft), animatedToJson(c.transform.cropTop),
                                                          animatedToJson(c.transform.cropRight),
                                                          animatedToJson(c.transform.cropBottom)}}}},
            {"fades", QJsonArray{qint64(c.fadeIn), qint64(c.fadeOut)}},
            {"audioFades", QJsonArray{qint64(c.audioFadeIn), qint64(c.audioFadeOut)}},
            {"audioDetached", c.audioDetached},
        };
        if (c.denoise > 0)
            o["denoise"] = c.denoise;
        if (c.title)
            o["title"] = titleToJson(*c.title);
        if (c.transitionIn.active())
            o["transitionIn"] = QJsonObject{{"type", QLatin1String(kTransitionNames[int(c.transitionIn.type)])},
                                            {"duration", qint64(c.transitionIn.duration)}};
        if (!c.color.isIdentity())
            o["color"] = animatedGradeToJson(c.color, projectDir);
        if (c.chromaKey != ChromaKey{}) {
            const ChromaKey& k = c.chromaKey;
            o["chromaKey"] = QJsonObject{{"enabled", k.enabled},
                                         {"color", argb(k.color)},
                                         {"tolerance", k.tolerance},
                                         {"softness", k.softness},
                                         {"spill", k.spill}};
        }
        if (!c.masks.empty()) {
            QJsonArray masks;
            for (const Mask& m : c.masks)
                masks.append(maskToJson(m));
            o["masks"] = masks;
        }
        if (c.blend != BlendMode::Normal)
            o["blend"] = QLatin1String(kBlendNames[int(c.blend)]);
        clips.append(o);
    }
    return QJsonObject{
        {"id", qint64(t.id)},
        {"name", QString::fromStdString(t.name)},
        {"muted", t.muted},
        {"hidden", t.hidden},
        {"locked", t.locked},
        {"volume", t.volume},
        {"clips", clips},
    };
}

Track trackFromJson(const QJsonObject& o, TrackKind kind, const QDir& projectDir, QStringList& missing)
{
    Track t;
    t.id = Id(o["id"].toInteger());
    t.kind = kind;
    t.name = o["name"].toString().toStdString();
    t.muted = o["muted"].toBool();
    t.hidden = o["hidden"].toBool();
    t.locked = o["locked"].toBool();
    t.volume = o["volume"].toDouble(1.0);
    for (const QJsonValue& v : o["clips"].toArray()) {
        const QJsonObject c = v.toObject();
        Clip clip;
        clip.id = Id(c["id"].toInteger());
        clip.mediaId = Id(c["mediaId"].toInteger());
        clip.sequenceId = Id(c["sequenceId"].toInteger());
        clip.start = c["start"].toInteger();
        clip.duration = c["duration"].toInteger();
        clip.sourceIn = c["sourceIn"].toInteger();
        clip.speed = c["speed"].toDouble(1.0);
        clip.enabled = c["enabled"].toBool(true);
        // Versión 1: el volumen era un número llamado "gain".
        clip.volume = c.contains("volume") ? animatedFromJson(c["volume"], 1.0) : Animated(c["gain"].toDouble(1.0));
        const QJsonObject tr = c["transform"].toObject();
        clip.transform.x = animatedFromJson(tr["x"], 0.0);
        clip.transform.y = animatedFromJson(tr["y"], 0.0);
        clip.transform.scale = animatedFromJson(tr["scale"], 1.0);
        clip.transform.rotation = animatedFromJson(tr["rotation"], 0.0);
        clip.transform.opacity = animatedFromJson(tr["opacity"], 1.0);
        const QJsonArray crop = tr["crop"].toArray();
        clip.transform.cropLeft = animatedFromJson(crop.at(0), 0.0);
        clip.transform.cropTop = animatedFromJson(crop.at(1), 0.0);
        clip.transform.cropRight = animatedFromJson(crop.at(2), 0.0);
        clip.transform.cropBottom = animatedFromJson(crop.at(3), 0.0);
        clip.fadeIn = c["fades"].toArray().at(0).toInteger();
        clip.fadeOut = c["fades"].toArray().at(1).toInteger();
        clip.audioFadeIn = c["audioFades"].toArray().at(0).toInteger();
        clip.audioFadeOut = c["audioFades"].toArray().at(1).toInteger();
        clip.audioDetached = c["audioDetached"].toBool();
        clip.denoise = std::clamp(c["denoise"].toDouble(0.0), 0.0, 1.0);
        if (c.contains("title"))
            clip.title = titleFromJson(c["title"].toObject());
        if (c.contains("transitionIn")) {
            const QJsonObject ti = c["transitionIn"].toObject();
            clip.transitionIn.type = enumFromName(ti["type"].toString(), kTransitionNames, TransitionType::None);
            clip.transitionIn.duration = ti["duration"].toInteger();
        }
        if (c.contains("color"))
            clip.color = animatedGradeFromJson(c["color"].toObject(), projectDir, missing);
        if (c.contains("chromaKey")) {
            const QJsonObject k = c["chromaKey"].toObject();
            const ChromaKey def;
            clip.chromaKey = {k["enabled"].toBool(), argbFromJson(k["color"], def.color), k["tolerance"].toDouble(def.tolerance),
                              k["softness"].toDouble(def.softness), k["spill"].toDouble(def.spill)};
        }
        // Versión 3: una sola máscara llamada "mask".
        if (c.contains("mask")) {
            Mask m = maskFromJson(c["mask"].toObject());
            if (m.active())
                clip.masks.push_back(m);
        }
        for (const QJsonValue& mv : c["masks"].toArray()) {
            Mask m = maskFromJson(mv.toObject());
            if (m.active() && clip.masks.size() < kMaxMasks)
                clip.masks.push_back(m);
        }
        clip.blend = enumFromName(c["blend"].toString(), kBlendNames, BlendMode::Normal);
        t.clips.push_back(clip);
    }
    return t;
}

} // namespace

// Cada palabra es [inicio µs, fin µs, texto, confianza] para que el archivo no crezca de más.
QJsonObject transcriptToJson(const Transcript& t)
{
    QJsonArray words;
    for (const TranscriptWord& w : t.words)
        words.append(QJsonArray{qint64(w.start), qint64(w.end), QString::fromStdString(w.text),
                                std::round(double(w.probability) * 1000.0) / 1000.0});
    return QJsonObject{{"language", QString::fromStdString(t.language)},
                       {"model", QString::fromStdString(t.model)},
                       {"words", words}};
}

Transcript transcriptFromJson(const QJsonObject& o)
{
    Transcript t;
    t.language = o["language"].toString().toStdString();
    t.model = o["model"].toString().toStdString();
    for (const QJsonValue& v : o["words"].toArray()) {
        const QJsonArray a = v.toArray();
        TranscriptWord w;
        w.start = a.at(0).toInteger();
        w.end = a.at(1).toInteger();
        w.text = a.at(2).toString().toStdString();
        w.probability = float(a.at(3).toDouble(1.0));
        t.words.push_back(std::move(w));
    }
    return t;
}

static QJsonArray markersToJson(const std::vector<Marker>& markers)
{
    QJsonArray list;
    for (const Marker& m : markers)
        list.append(QJsonArray{qint64(m.frame), QString::fromStdString(m.label), argb(m.color)});
    return list;
}

static std::vector<Marker> markersFromJson(const QJsonValue& v)
{
    std::vector<Marker> markers;
    for (const QJsonValue& e : v.toArray()) {
        const QJsonArray a = e.toArray();
        markers.push_back({a.at(0).toInteger(), a.at(1).toString().toStdString(), argbFromJson(a.at(2), 0xFFFFC266)});
    }
    std::sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.frame < b.frame; });
    return markers;
}

QByteArray serializeProject(const Project& p, const QString& projectFilePath)
{
    const QDir projectDir = QFileInfo(projectFilePath).absoluteDir();

    QJsonArray media;
    for (const MediaItem& m : p.media) {
        const QString path = QString::fromStdString(m.path);
        media.append(QJsonObject{
            {"id", qint64(m.id)},
            {"path", QDir::fromNativeSeparators(path)},
            {"relativePath", projectDir.relativeFilePath(path)},
            {"name", QString::fromStdString(m.name)},
            {"duration", qint64(m.duration)},
            {"hasVideo", m.hasVideo},
            {"hasAudio", m.hasAudio},
            {"width", m.width},
            {"height", m.height},
            {"frameRate", m.frameRate},
            {"isStill", m.isStill},
        });
        if (m.transcript) {
            QJsonObject o = media.last().toObject();
            o["transcript"] = transcriptToJson(*m.transcript);
            media.replace(media.size() - 1, o);
        }
    }
    QJsonArray video, audio;
    for (const Track& t : p.videoTracks)
        video.append(trackToJson(t, projectDir));
    for (const Track& t : p.audioTracks)
        audio.append(trackToJson(t, projectDir));
    QJsonArray sequences;
    for (const Sequence& seq : p.sequences) {
        QJsonArray sv, sa;
        for (const Track& t : seq.videoTracks)
            sv.append(trackToJson(t, projectDir));
        for (const Track& t : seq.audioTracks)
            sa.append(trackToJson(t, projectDir));
        sequences.append(QJsonObject{{"id", qint64(seq.id)},
                                     {"name", QString::fromStdString(seq.name)},
                                     {"videoTracks", sv},
                                     {"audioTracks", sa},
                                     {"markers", markersToJson(seq.markers)}});
    }
    QJsonArray path;
    for (Id id : p.sequencePath)
        path.append(qint64(id));

    const QJsonObject root{
        {"format", kFormatName},
        {"version", kProjectFormatVersion},
        {"application", QStringLiteral("MovixStudio %1").arg(QCoreApplication::applicationVersion())},
        {"settings",
         QJsonObject{
             {"width", p.settings.width},
             {"height", p.settings.height},
             {"frameRate", QJsonObject{{"num", qint64(p.settings.frameRate.num)}, {"den", qint64(p.settings.frameRate.den)}}},
             {"sampleRate", p.settings.sampleRate},
             {"channels", p.settings.channels},
             {"colorMode", QLatin1String(kColorModeNames[int(p.settings.colorMode)])},
         }},
        {"nextId", qint64(p.nextId)},
        {"media", media},
        {"videoTracks", video},
        {"audioTracks", audio},
        {"markers", markersToJson(p.markers)},
        // Las pistas de arriba son las de la secuencia activa; el resto, en "sequences".
        {"activeSequence", QJsonObject{{"id", qint64(p.activeSequence)}, {"name", QString::fromStdString(p.activeName)}}},
        {"mainSequence", qint64(p.mainSequence)},
        {"sequencePath", path},
        {"sequences", sequences},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

LoadResult deserializeProject(const QByteArray& json, const QString& projectFilePath)
{
    LoadResult result;
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        result.error = QCoreApplication::translate("ProjectFile", "El archivo no es un proyecto válido (%1).").arg(parseError.errorString());
        return result;
    }
    const QJsonObject root = doc.object();
    if (root["format"].toString() != kFormatName) {
        result.error = QCoreApplication::translate("ProjectFile", "El archivo no es un proyecto de MovixStudio.");
        return result;
    }
    if (root["version"].toInt() > kProjectFormatVersion) {
        result.error = QCoreApplication::translate("ProjectFile", "El proyecto se creó con una versión más reciente de MovixStudio.");
        return result;
    }

    Project p;
    const QJsonObject s = root["settings"].toObject();
    p.settings.width = s["width"].toInt(1920);
    p.settings.height = s["height"].toInt(1080);
    p.settings.frameRate.num = s["frameRate"].toObject()["num"].toInteger(30);
    p.settings.frameRate.den = s["frameRate"].toObject()["den"].toInteger(1);
    p.settings.sampleRate = s["sampleRate"].toInt(48000);
    p.settings.channels = s["channels"].toInt(2);
    p.settings.colorMode = enumFromName(s["colorMode"].toString(), kColorModeNames, ColorMode::SDR);
    p.nextId = Id(root["nextId"].toInteger(1));

    const QDir projectDir = QFileInfo(projectFilePath).absoluteDir();
    for (const QJsonValue& v : root["media"].toArray()) {
        const QJsonObject o = v.toObject();
        MediaItem m;
        m.id = Id(o["id"].toInteger());
        QString path = o["path"].toString();
        if (!QFileInfo::exists(path)) {
            const QString relative = projectDir.absoluteFilePath(o["relativePath"].toString());
            if (QFileInfo::exists(relative))
                path = QDir::cleanPath(relative);
            else
                result.missingMedia.append(QDir::toNativeSeparators(path));
        }
        m.path = QDir::toNativeSeparators(path).toStdString();
        m.name = o["name"].toString().toStdString();
        m.duration = o["duration"].toInteger();
        m.hasVideo = o["hasVideo"].toBool();
        m.hasAudio = o["hasAudio"].toBool();
        m.width = o["width"].toInt();
        m.height = o["height"].toInt();
        m.frameRate = o["frameRate"].toDouble();
        m.isStill = o["isStill"].toBool();
        if (o.contains("transcript"))
            m.transcript = std::make_shared<const Transcript>(transcriptFromJson(o["transcript"].toObject()));
        p.media.push_back(m);
    }
    for (const QJsonValue& v : root["videoTracks"].toArray())
        p.videoTracks.push_back(trackFromJson(v.toObject(), TrackKind::Video, projectDir, result.missingMedia));
    for (const QJsonValue& v : root["audioTracks"].toArray())
        p.audioTracks.push_back(trackFromJson(v.toObject(), TrackKind::Audio, projectDir, result.missingMedia));
    p.markers = markersFromJson(root["markers"]);
    for (const QJsonValue& v : root["sequences"].toArray()) {
        const QJsonObject o = v.toObject();
        Sequence seq;
        seq.id = Id(o["id"].toInteger());
        seq.name = o["name"].toString().toStdString();
        for (const QJsonValue& t : o["videoTracks"].toArray())
            seq.videoTracks.push_back(trackFromJson(t.toObject(), TrackKind::Video, projectDir, result.missingMedia));
        for (const QJsonValue& t : o["audioTracks"].toArray())
            seq.audioTracks.push_back(trackFromJson(t.toObject(), TrackKind::Audio, projectDir, result.missingMedia));
        seq.markers = markersFromJson(o["markers"]);
        p.sequences.push_back(std::move(seq));
    }
    if (root.contains("activeSequence")) {
        const QJsonObject active = root["activeSequence"].toObject();
        p.activeSequence = Id(active["id"].toInteger());
        p.activeName = active["name"].toString().toStdString();
        p.mainSequence = Id(root["mainSequence"].toInteger());
        for (const QJsonValue& v : root["sequencePath"].toArray())
            p.sequencePath.push_back(Id(v.toInteger()));
    } else { // versiones 1 a 3: solo la principal
        p.mainSequence = p.activeSequence = p.newId();
        p.activeName = "Principal";
    }

    std::string why;
    if (!p.validate(&why)) {
        result.error = QCoreApplication::translate("ProjectFile", "El proyecto está dañado: %1").arg(QString::fromStdString(why));
        return result;
    }
    result.project = std::move(p);
    return result;
}

bool saveProjectFile(const Project& project, const QString& path, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(serializeProject(project, path));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

LoadResult loadProjectFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        LoadResult r;
        r.error = QCoreApplication::translate("ProjectFile", "No se pudo abrir el proyecto: %1").arg(file.errorString());
        return r;
    }
    return deserializeProject(file.readAll(), path);
}

} // namespace movix::core
