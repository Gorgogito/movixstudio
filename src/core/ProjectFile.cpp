#include "core/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace movix::core {

namespace {

const QString kFormatName = QStringLiteral("movixstudio-project");

// Nombres estables de los enumerados en el archivo (no dependen del orden en el código).
const char* const kTransitionNames[] = {"none", "dissolve", "dipToBlack", "wipeLeft", "wipeRight", "pushLeft", "pushRight", "slideLeft"};
const char* const kInterpolationNames[] = {"linear", "hold", "smooth"};

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

QJsonObject trackToJson(const Track& t)
{
    QJsonArray clips;
    for (const Clip& c : t.clips) {
        QJsonObject o{
            {"id", qint64(c.id)},
            {"mediaId", qint64(c.mediaId)},
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
                                      {"opacity", animatedToJson(c.transform.opacity)}}},
            {"fades", QJsonArray{qint64(c.fadeIn), qint64(c.fadeOut)}},
            {"audioFades", QJsonArray{qint64(c.audioFadeIn), qint64(c.audioFadeOut)}},
            {"audioDetached", c.audioDetached},
        };
        if (c.title)
            o["title"] = titleToJson(*c.title);
        if (c.transitionIn.active())
            o["transitionIn"] = QJsonObject{{"type", QLatin1String(kTransitionNames[int(c.transitionIn.type)])},
                                            {"duration", qint64(c.transitionIn.duration)}};
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

Track trackFromJson(const QJsonObject& o, TrackKind kind)
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
        clip.fadeIn = c["fades"].toArray().at(0).toInteger();
        clip.fadeOut = c["fades"].toArray().at(1).toInteger();
        clip.audioFadeIn = c["audioFades"].toArray().at(0).toInteger();
        clip.audioFadeOut = c["audioFades"].toArray().at(1).toInteger();
        clip.audioDetached = c["audioDetached"].toBool();
        if (c.contains("title"))
            clip.title = titleFromJson(c["title"].toObject());
        if (c.contains("transitionIn")) {
            const QJsonObject ti = c["transitionIn"].toObject();
            clip.transitionIn.type = enumFromName(ti["type"].toString(), kTransitionNames, TransitionType::None);
            clip.transitionIn.duration = ti["duration"].toInteger();
        }
        t.clips.push_back(clip);
    }
    return t;
}

} // namespace

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
    }
    QJsonArray video, audio;
    for (const Track& t : p.videoTracks)
        video.append(trackToJson(t));
    for (const Track& t : p.audioTracks)
        audio.append(trackToJson(t));

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
         }},
        {"nextId", qint64(p.nextId)},
        {"media", media},
        {"videoTracks", video},
        {"audioTracks", audio},
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
        p.media.push_back(m);
    }
    for (const QJsonValue& v : root["videoTracks"].toArray())
        p.videoTracks.push_back(trackFromJson(v.toObject(), TrackKind::Video));
    for (const QJsonValue& v : root["audioTracks"].toArray())
        p.audioTracks.push_back(trackFromJson(v.toObject(), TrackKind::Audio));

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
