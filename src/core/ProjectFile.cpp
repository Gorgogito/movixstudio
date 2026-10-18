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

QJsonObject trackToJson(const Track& t)
{
    QJsonArray clips;
    for (const Clip& c : t.clips) {
        clips.append(QJsonObject{
            {"id", qint64(c.id)},
            {"mediaId", qint64(c.mediaId)},
            {"start", qint64(c.start)},
            {"duration", qint64(c.duration)},
            {"sourceIn", qint64(c.sourceIn)},
            {"gain", double(c.gain)},
            {"enabled", c.enabled},
        });
    }
    return QJsonObject{
        {"id", qint64(t.id)},
        {"name", QString::fromStdString(t.name)},
        {"muted", t.muted},
        {"hidden", t.hidden},
        {"locked", t.locked},
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
    for (const QJsonValue& v : o["clips"].toArray()) {
        const QJsonObject c = v.toObject();
        Clip clip;
        clip.id = Id(c["id"].toInteger());
        clip.mediaId = Id(c["mediaId"].toInteger());
        clip.start = c["start"].toInteger();
        clip.duration = c["duration"].toInteger();
        clip.sourceIn = c["sourceIn"].toInteger();
        clip.gain = float(c["gain"].toDouble(1.0));
        clip.enabled = c["enabled"].toBool(true);
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
