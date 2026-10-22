#include "ScriptHost.h"

#include "core/Edits.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace movix::core;

namespace movix::app {

namespace {

std::vector<Id> toIds(const QVariant& v)
{
    std::vector<Id> ids;
    if (v.canConvert<QVariantList>() && v.typeId() != QMetaType::Double && v.typeId() != QMetaType::QString) {
        for (const QVariant& x : v.toList())
            if (x.toDouble() > 0)
                ids.push_back(Id(x.toDouble()));
    } else if (v.toDouble() > 0) {
        ids.push_back(Id(v.toDouble()));
    }
    return ids;
}

} // namespace

// --- Lista de scripts ---

ScriptInfo scriptInfo(const QString& path, const QString& code)
{
    ScriptInfo info;
    info.path = path;
    info.builtin = path.startsWith(QLatin1Char(':'));
    info.name = QFileInfo(path).completeBaseName();
    static const QRegularExpression tag(QStringLiteral(R"(^\s*//\s*@(name|description)\s+(.+?)\s*$)"), QRegularExpression::MultilineOption);
    for (auto it = tag.globalMatch(code); it.hasNext();) {
        const auto m = it.next();
        (m.captured(1) == QLatin1String("name") ? info.name : info.description) = m.captured(2);
    }
    return info;
}

std::vector<ScriptInfo> listScripts(const QString& userDir)
{
    std::vector<ScriptInfo> out;
    for (const QString& dirPath : {QStringLiteral(":/scripts"), userDir}) {
        if (dirPath.isEmpty())
            continue;
        const QDir dir(dirPath);
        for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("*.js")}, QDir::Files, QDir::Name | QDir::IgnoreCase)) {
            QFile file(f.filePath());
            if (file.open(QIODevice::ReadOnly))
                out.push_back(scriptInfo(f.filePath(), QString::fromUtf8(file.readAll())));
        }
    }
    return out;
}

// --- Ejecución ---

ScriptResult runScript(const QString& code, Project& project, FramePos playhead, const std::vector<Id>& selection, int timeoutMs)
{
    ScriptResult result;
    QJSEngine engine;
    engine.installExtensions(QJSEngine::ConsoleExtension); // console.log → registro de Qt
    ScriptApi api(project, playhead, selection);
    engine.globalObject().setProperty(QStringLiteral("movix"), engine.newQObject(&api));
    QJSEngine::setObjectOwnership(&api, QJSEngine::CppOwnership);

    // Vigilante: un bucle infinito no debe colgar el editor.
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false, timedOut = false;
    std::thread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!done.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return finished; })) {
            timedOut = true;
            engine.setInterrupted(true);
        }
    });
    const QJSValue value = engine.evaluate(code, QStringLiteral("script.js"));
    {
        std::lock_guard lock(mutex);
        finished = true;
    }
    done.notify_all();
    watchdog.join();

    result.log = api.logLines();
    if (timedOut) {
        result.error = QCoreApplication::translate("ScriptHost", "El script tardó más de %1 s y se detuvo.").arg(timeoutMs / 1000.0);
        return result;
    }
    if (value.isError()) {
        result.error = QCoreApplication::translate("ScriptHost", "Línea %1: %2").arg(value.property(QStringLiteral("lineNumber")).toInt()).arg(value.toString());
        return result;
    }
    result.ok = true;
    result.changed = api.changed();
    return result;
}

// --- API ---

ScriptApi::ScriptApi(Project& project, FramePos playhead, std::vector<Id> selection)
    : m_p(project), m_playhead(playhead), m_selection(std::move(selection))
{
}

QVariantMap ScriptApi::clipMap(const Clip& c, const Track& t) const
{
    const MediaItem* m = c.isGenerated() ? nullptr : m_p.findMedia(c.mediaId);
    return {
        {"id", double(c.id)},
        {"trackId", double(t.id)},
        {"start", double(c.start)},
        {"duration", double(c.duration)},
        {"end", double(c.end())},
        {"mediaId", double(c.mediaId)},
        {"name", c.title ? QString::fromStdString(c.title->text)
                 : c.isCompound() ? QString::fromStdString(m_p.sequenceName(c.sequenceId))
                 : m ? QString::fromStdString(m->name) : QString()},
        {"enabled", c.enabled},
        {"speed", c.speed},
        {"sourceIn", double(c.sourceIn) / double(kUsPerSecond)},
        {"isTitle", c.title.has_value()},
        {"isCompound", c.isCompound()},
        {"volume", c.volume.value},
        {"opacity", c.transform.opacity.value},
    };
}

QVariantMap ScriptApi::info() const
{
    QVariantList sel;
    for (Id id : m_selection)
        if (m_p.findClip(id))
            sel.append(double(id));
    const auto& s = m_p.settings;
    return {{"width", s.width},          {"height", s.height}, {"fps", s.frameRate.value()}, {"duration", double(m_p.duration())},
            {"playhead", double(m_playhead)}, {"selection", sel}};
}

QVariantList ScriptApi::tracks() const
{
    QVariantList out;
    for (const auto* list : {&m_p.videoTracks, &m_p.audioTracks}) {
        for (const Track& t : *list) {
            QVariantList clips;
            for (const Clip& c : t.clips)
                clips.append(clipMap(c, t));
            out.append(QVariantMap{{"id", double(t.id)},
                                   {"kind", t.kind == TrackKind::Video ? QStringLiteral("video") : QStringLiteral("audio")},
                                   {"name", QString::fromStdString(t.name)},
                                   {"muted", t.muted},
                                   {"hidden", t.hidden},
                                   {"locked", t.locked},
                                   {"clips", clips}});
        }
    }
    return out;
}

QVariant ScriptApi::clip(double clipId) const
{
    const Track* t = m_p.trackOfClip(Id(clipId));
    const Clip* c = m_p.findClip(Id(clipId));
    return c && t ? QVariant(clipMap(*c, *t)) : QVariant();
}

QVariantList ScriptApi::media() const
{
    QVariantList out;
    for (const MediaItem& m : m_p.media)
        out.append(QVariantMap{{"id", double(m.id)},
                               {"name", QString::fromStdString(m.name)},
                               {"path", QString::fromStdString(m.path)},
                               {"duration", double(m.duration) / double(kUsPerSecond)},
                               {"hasVideo", m.hasVideo},
                               {"hasAudio", m.hasAudio}});
    return out;
}

QVariantList ScriptApi::markers() const
{
    QVariantList out;
    for (const Marker& m : m_p.markers)
        out.append(QVariantMap{{"frame", double(m.frame)}, {"label", QString::fromStdString(m.label)}});
    return out;
}

double ScriptApi::secondsToFrames(double seconds) const
{
    return double(m_p.settings.timeToFrame(TimeUs(std::llround(seconds * kUsPerSecond))));
}

double ScriptApi::framesToSeconds(double frames) const
{
    return double(m_p.settings.frameToTime(FramePos(frames))) / double(kUsPerSecond);
}

double ScriptApi::split(double clipId, double frame)
{
    const Id id = edit::splitClip(m_p, Id(clipId), FramePos(frame));
    mark(id != kNoId);
    return id == kNoId ? -1 : double(id);
}

QVariantList ScriptApi::splitAll(double frame)
{
    QVariantList out;
    for (Id id : edit::splitAt(m_p, FramePos(frame)))
        out.append(double(id));
    mark(!out.isEmpty());
    return out;
}

bool ScriptApi::remove(const QVariant& clipIds, bool ripple)
{
    return mark(edit::deleteClips(m_p, toIds(clipIds), ripple));
}

bool ScriptApi::move(const QVariant& clipIds, double delta, int trackOffset)
{
    return mark(edit::moveClips(m_p, toIds(clipIds), FramePos(delta), trackOffset));
}

double ScriptApi::trim(double clipId, const QString& edge, double frame)
{
    const auto applied = edit::trimClip(m_p, Id(clipId), edge == QLatin1String("start") ? edit::Edge::Start : edit::Edge::End,
                                        FramePos(frame));
    mark(applied.has_value());
    return applied ? double(*applied) : -1;
}

double ScriptApi::place(double mediaId, double trackId, double frame, double durationFrames)
{
    const Id id = edit::placeClip(m_p, Id(trackId), Id(mediaId), FramePos(frame), 0, FramePos(durationFrames));
    mark(id != kNoId);
    return id == kNoId ? -1 : double(id);
}

double ScriptApi::addTitle(const QString& text, double frame, double durationFrames, double trackId)
{
    Id target = trackId > 0 ? Id(trackId) : kNoId;
    const FramePos at = FramePos(frame), duration = std::max<FramePos>(1, FramePos(durationFrames));
    if (target == kNoId) { // la pista de video superior si está libre; si no, una nueva encima
        const Track& top = m_p.videoTracks.back();
        const bool free = std::none_of(top.clips.begin(), top.clips.end(), [&](const Clip& c) { return c.start < at + duration && c.end() > at; });
        target = free && !top.locked ? top.id : edit::addTrack(m_p, TrackKind::Video);
    }
    TitleStyle style;
    style.text = text.toStdString();
    const Id id = edit::placeTitle(m_p, target, at, duration, style);
    mark(id != kNoId);
    return id == kNoId ? -1 : double(id);
}

double ScriptApi::addTrack(const QString& kind)
{
    mark(true);
    return double(edit::addTrack(m_p, kind == QLatin1String("audio") ? TrackKind::Audio : TrackKind::Video));
}

bool ScriptApi::addMarker(double frame, const QString& label)
{
    return mark(edit::addMarkers(m_p, {FramePos(frame)}, label.toStdString()) > 0);
}

bool ScriptApi::removeMarker(double frame)
{
    return mark(edit::removeMarker(m_p, FramePos(frame)));
}

double ScriptApi::removeRanges(const QVariantList& ranges)
{
    std::vector<FrameRange> list;
    for (const QVariant& r : ranges) {
        const QVariantList pair = r.toList();
        if (pair.size() == 2)
            list.push_back({FramePos(pair[0].toDouble()), FramePos(pair[1].toDouble())});
    }
    const FramePos removed = edit::removeRanges(m_p, std::move(list));
    mark(removed > 0);
    return double(removed);
}

bool ScriptApi::set(double clipId, const QString& property, const QVariant& value)
{
    const Id id = Id(clipId);
    const Clip* c = m_p.findClip(id);
    if (!c)
        return false;
    static const std::pair<const char*, edit::Property> kProps[] = {
        {"volume", edit::Property::Volume}, {"opacity", edit::Property::Opacity},   {"scale", edit::Property::Scale},
        {"x", edit::Property::PositionX},   {"y", edit::Property::PositionY},       {"rotation", edit::Property::Rotation},
    };
    for (const auto& [name, prop] : kProps)
        if (property == QLatin1String(name))
            return mark(edit::setProperty(m_p, id, prop, 0, value.toDouble()));
    if (property == QLatin1String("speed"))
        return mark(edit::setClipSpeed(m_p, id, value.toDouble()));
    if (property == QLatin1String("enabled"))
        return mark(edit::setClipEnabled(m_p, id, value.toBool()));
    if (property == QLatin1String("text") && c->title) {
        TitleStyle style = *c->title;
        style.text = value.toString().toStdString();
        return mark(edit::setTitleStyle(m_p, id, style));
    }
    return false;
}

void ScriptApi::log(const QString& text)
{
    m_log.append(text);
}

} // namespace movix::app
