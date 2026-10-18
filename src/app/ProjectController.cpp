#include "ProjectController.h"

#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "engine/MediaImport.h"
#include "media/MediaInfo.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>

using namespace movix;
using namespace movix::core;

namespace {

std::vector<Id> toIds(const QVariantList& list)
{
    std::vector<Id> ids;
    for (const QVariant& v : list)
        ids.push_back(Id(v.toLongLong()));
    return ids;
}

const Track* firstUnlocked(const std::vector<Track>& tracks)
{
    for (const Track& t : tracks)
        if (!t.locked)
            return &t;
    return nullptr;
}

} // namespace

ProjectController::ProjectController(QObject* parent)
    : QObject(parent)
    , m_player(std::make_unique<playback::TimelinePlayer>())
    , m_sourcePlayer(std::make_unique<playback::TimelinePlayer>())
    , m_exporter([this] { return m_snapshot; })
{
    m_autosaveTimer.setInterval(30'000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &ProjectController::autosave);
    m_autosaveTimer.start();
    refresh();
}

ProjectController::~ProjectController() = default;

// --- Estado ---

QString ProjectController::projectName() const
{
    return m_fileUrl.isEmpty() ? tr("Sin título") : QFileInfo(m_fileUrl.toLocalFile()).completeBaseName();
}

QString ProjectController::frameRateText() const
{
    const Rational r = project().settings.frameRate;
    const double v = r.value();
    return r.den == 1 ? QString::number(r.num) : QLocale().toString(v, 'f', 3);
}

QVariantList ProjectController::selection() const
{
    QVariantList list;
    for (Id id : m_selection)
        list.append(qint64(id));
    return list;
}

QString ProjectController::timecode(qint64 frame) const
{
    const Rational r = project().settings.frameRate;
    const qint64 fpsInt = std::max<qint64>(1, std::llround(r.value()));
    const TimeUs t = project().settings.frameToTime(std::max<qint64>(0, frame));
    const qint64 secs = t / kUsPerSecond;
    // Fotograma dentro del segundo, a partir del tiempo real (correcto también a 29,97).
    const qint64 ff = std::min(fpsInt - 1, (frame - project().settings.timeToFrame(secs * kUsPerSecond)));
    return QStringLiteral("%1:%2:%3:%4")
        .arg(secs / 3600, 2, 10, QLatin1Char('0'))
        .arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(secs % 60, 2, 10, QLatin1Char('0'))
        .arg(std::max<qint64>(0, ff), 2, 10, QLatin1Char('0'));
}

// --- Núcleo de las ediciones ---

bool ProjectController::apply(const QString& text, const std::function<bool(Project&)>& edit, const QString& mergeKey)
{
    if (!m_history.apply(text.toStdString(), edit, mergeKey.toStdString()))
        return false;
    refresh();
    return true;
}

void ProjectController::refresh()
{
    const bool settingsChangedNow = !m_snapshot || !(m_snapshot->settings == project().settings);
    m_snapshot = std::make_shared<const Project>(project());
    // La selección solo puede contener clips que existen.
    for (auto it = m_selection.begin(); it != m_selection.end();)
        it = project().findClip(*it) ? std::next(it) : m_selection.erase(it);
    m_media.sync(project());
    m_tracks.sync(project());
    m_clips.sync(project(), m_selection);
    m_player->setProject(m_snapshot);
    emit historyChanged();
    emit timelineChanged();
    emit selectionChanged();
    if (settingsChangedNow)
        emit settingsChanged();
}

void ProjectController::resetTo(Project project, const QUrl& url)
{
    m_player->pause();
    m_player->seek(0);
    m_selection.clear();
    m_history.reset(std::move(project));
    m_fileUrl = url;
    m_autosavedRevision = m_history.revision();
    refresh();
    emit fileChanged();
}

void ProjectController::undo()
{
    if (m_history.undo())
        refresh();
}

void ProjectController::redo()
{
    if (m_history.redo())
        refresh();
}

void ProjectController::endInteraction()
{
    m_history.breakMerge();
}

// --- Proyecto ---

void ProjectController::newProject()
{
    resetTo(Project::createDefault(), QUrl());
    discardAutosave();
}

bool ProjectController::openProject(const QUrl& url)
{
    const LoadResult result = loadProjectFile(url.toLocalFile());
    if (!result.project) {
        emit errorOccurred(result.error);
        return false;
    }
    resetTo(*result.project, url);
    discardAutosave();
    if (!result.missingMedia.isEmpty())
        emit errorOccurred(tr("No se encontraron %n archivos de medios:\n%1", nullptr, int(result.missingMedia.size()))
                               .arg(result.missingMedia.join(QLatin1Char('\n'))));
    return true;
}

bool ProjectController::save()
{
    if (m_fileUrl.isEmpty())
        return false;
    return saveAs(m_fileUrl);
}

bool ProjectController::saveAs(const QUrl& url)
{
    QString path = url.toLocalFile();
    if (QFileInfo(path).suffix().compare(QLatin1String(kProjectExtension), Qt::CaseInsensitive) != 0)
        path += QLatin1Char('.') + QLatin1String(kProjectExtension);
    QString error;
    if (!saveProjectFile(project(), path, &error)) {
        emit errorOccurred(tr("No se pudo guardar el proyecto: %1").arg(error));
        return false;
    }
    m_history.markClean();
    m_fileUrl = QUrl::fromLocalFile(path);
    discardAutosave();
    emit fileChanged();
    emit historyChanged();
    return true;
}

void ProjectController::setProjectSettings(int width, int height, int fpsNum, int fpsDen)
{
    apply(tr("Ajustes del proyecto"), [&](Project& p) {
        const bool a = edit::setResolution(p, width, height);
        const bool b = edit::setFrameRate(p, {fpsNum, std::max(1, fpsDen)});
        return a || b;
    });
}

// --- Autoguardado y recuperación ---

QString ProjectController::autosavePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/autosave");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/recuperacion.") + QLatin1String(kProjectExtension);
}

void ProjectController::autosave()
{
    if (m_history.isClean() || m_history.revision() == m_autosavedRevision)
        return;
    if (saveProjectFile(project(), autosavePath())) {
        m_autosavedRevision = m_history.revision();
        // Junto a la copia se recuerda qué archivo era, para que al recuperar se pueda guardar en él.
        QSaveFile origin(autosavePath() + QStringLiteral(".origen"));
        if (origin.open(QIODevice::WriteOnly)) {
            origin.write(m_fileUrl.toString().toUtf8());
            origin.commit();
        }
    }
}

void ProjectController::checkRecovery()
{
    const QFileInfo info(autosavePath());
    if (info.exists())
        emit recoveryAvailable(QLocale().toString(info.lastModified(), QLocale::ShortFormat));
}

void ProjectController::recoverAutosave()
{
    const LoadResult result = loadProjectFile(autosavePath());
    if (!result.project) {
        emit errorOccurred(result.error);
        return;
    }
    QUrl origin;
    QFile originFile(autosavePath() + QStringLiteral(".origen"));
    if (originFile.open(QIODevice::ReadOnly))
        origin = QUrl(QString::fromUtf8(originFile.readAll()));
    resetTo(*result.project, origin);
    m_history.markDirty(); // recuperado pero aún no guardado
    emit historyChanged();
}

void ProjectController::discardAutosave()
{
    QFile::remove(autosavePath());
    QFile::remove(autosavePath() + QStringLiteral(".origen"));
}

void ProjectController::shutdown()
{
    m_player->pause();
    m_sourcePlayer->pause();
    m_exporter.cancel();
    discardAutosave();
}

// --- Medios ---

void ProjectController::importFiles(const QList<QUrl>& urls)
{
    for (const QUrl& url : urls) {
        const QString path = QDir::toNativeSeparators(url.toLocalFile());
        if (path.isEmpty())
            continue;
        if (QFileInfo(path).suffix().compare(QLatin1String(kProjectExtension), Qt::CaseInsensitive) == 0) {
            openProject(url);
            continue;
        }
        const bool already = std::any_of(project().media.begin(), project().media.end(),
                                          [&](const MediaItem& m) { return QString::fromStdString(m.path) == path; });
        if (already)
            continue;
        ++m_importing;
        const int order = m_importOrder++;
        // Sondear en segundo plano: un archivo en red o dañado no debe congelar la interfaz.
        QThreadPool::globalInstance()->start([this, path, order] {
            std::string err;
            auto item = engine::probeMediaItem(path.toStdString(), &err);
            QMetaObject::invokeMethod(this, [this, path, order, item, err] {
                if (item)
                    m_imported.emplace_back(order, *item);
                else
                    m_importErrors.append(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), QString::fromStdString(err)));
                if (--m_importing == 0)
                    finishImport();
                emit importingChanged();
            }, Qt::QueuedConnection);
        });
    }
    emit importingChanged();
}

void ProjectController::finishImport()
{
    std::sort(m_imported.begin(), m_imported.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto items = std::move(m_imported);
    m_imported.clear();
    if (!items.empty()) {
        apply(tr("Importar medios"), [&](Project& p) {
            for (auto& [order, item] : items)
                edit::addMedia(p, item);
            return true;
        });
    }
    if (!m_importErrors.isEmpty()) {
        emit errorOccurred(tr("No se pudieron importar algunos archivos:\n%1").arg(m_importErrors.join(QLatin1Char('\n'))));
        m_importErrors.clear();
    }
}

void ProjectController::removeMedia(qint64 mediaId)
{
    apply(tr("Quitar medio"), [&](Project& p) { return edit::removeMedia(p, Id(mediaId)); });
}

void ProjectController::previewMedia(qint64 mediaId)
{
    const MediaItem* m = project().findMedia(Id(mediaId));
    if (!m)
        return;
    // La vista previa de un medio es un proyecto de un solo clip con sus propios ajustes.
    Project preview = Project::createDefault();
    const Id id = edit::addMedia(preview, *m);
    edit::matchSettingsToMedia(preview, *preview.findMedia(id));
    edit::placeClip(preview, m->hasVideo ? preview.videoTracks[0].id : preview.audioTracks[0].id, id, 0);
    m_sourcePlayer->pause();
    m_sourcePlayer->setProject(std::make_shared<const Project>(std::move(preview)));
    m_sourcePlayer->seek(0);
    m_sourceName = QString::fromStdString(m->name);
    emit sourceChanged();
}

QVariantMap ProjectController::mediaInfo(qint64 mediaId) const
{
    const MediaItem* m = project().findMedia(Id(mediaId));
    if (!m)
        return {};
    QVariantMap map{
        {"name", QString::fromStdString(m->name)},
        {"path", QString::fromStdString(m->path)},
        {"duration", toSeconds(m->duration)},
        {"hasVideo", m->hasVideo},
        {"hasAudio", m->hasAudio},
        {"width", m->width},
        {"height", m->height},
        {"frameRate", m->frameRate},
    };
    if (auto info = media::probe(m->path)) {
        map["container"] = QString::fromStdString(info->container);
        if (info->video) {
            map["videoCodec"] = QString::fromStdString(info->video->codec);
            map["pixelFormat"] = QString::fromStdString(info->video->pixelFormat);
        }
        if (info->audio) {
            map["audioCodec"] = QString::fromStdString(info->audio->codec);
            map["sampleRate"] = info->audio->sampleRate;
            map["channels"] = info->audio->channels;
        }
    }
    return map;
}

// --- Línea de tiempo ---

bool ProjectController::appendToTimeline(qint64 mediaId)
{
    const MediaItem* m = project().findMedia(Id(mediaId));
    if (!m)
        return false;
    const Track* track = m->hasVideo ? firstUnlocked(project().videoTracks) : firstUnlocked(project().audioTracks);
    if (!track)
        return false;
    const FramePos start = track->clips.empty() ? 0 : track->clips.back().end();
    return placeMedia(mediaId, qint64(track->id), start);
}

bool ProjectController::placeMedia(qint64 mediaId, qint64 trackId, qint64 frame)
{
    const MediaItem* m = project().findMedia(Id(mediaId));
    const Track* t = project().findTrack(Id(trackId));
    if (!m || !t)
        return false;
    // Un medio que no encaja en la pista se lleva a la primera pista compatible.
    Id target = t->id;
    if (t->kind == TrackKind::Video && !m->hasVideo)
        target = firstUnlocked(project().audioTracks) ? firstUnlocked(project().audioTracks)->id : kNoId;
    if (target == kNoId)
        return false;

    const Rational before = project().settings.frameRate;
    const int beforeW = project().settings.width, beforeH = project().settings.height;
    Id clip = kNoId;
    const bool ok = apply(tr("Añadir clip"), [&](Project& p) {
        // La posición llega en fotogramas de los ajustes actuales; si el proyecto se ajusta al
        // medio (otra velocidad), se convierte conservando el instante.
        const TimeUs at = p.settings.frameToTime(std::max<qint64>(0, frame));
        edit::matchSettingsToMedia(p, *p.findMedia(Id(mediaId)));
        clip = edit::placeClip(p, target, Id(mediaId), p.settings.timeToFrame(at));
        return clip != kNoId;
    });
    if (ok) {
        m_selection = {clip};
        m_clips.sync(project(), m_selection);
        emit selectionChanged();
        const auto& s = project().settings;
        if (!(s.frameRate == before) || s.width != beforeW || s.height != beforeH)
            emit notice(tr("El proyecto se ajustó al clip: %1×%2 a %3 fps.").arg(s.width).arg(s.height).arg(frameRateText()));
    }
    return ok;
}

bool ProjectController::moveClips(const QVariantList& clipIds, qint64 delta, int trackOffset)
{
    const auto ids = toIds(clipIds);
    return apply(tr("Mover clips"), [&](Project& p) { return edit::moveClips(p, ids, delta, trackOffset); });
}

qint64 ProjectController::trimClip(qint64 clipId, bool startEdge, qint64 frame)
{
    std::optional<FramePos> applied;
    apply(tr("Recortar clip"), [&](Project& p) {
        applied = edit::trimClip(p, Id(clipId), startEdge ? edit::Edge::Start : edit::Edge::End, frame);
        return applied.has_value();
    });
    return applied.value_or(-1);
}

void ProjectController::splitAtPlayhead()
{
    const FramePos at = m_player->positionFrame();
    std::vector<Id> targets;
    for (Id id : m_selection)
        targets.push_back(id);
    apply(tr("Dividir"), [&](Project& p) { return !edit::splitAt(p, at, targets).empty(); });
}

void ProjectController::deleteSelection(bool ripple)
{
    if (m_selection.isEmpty())
        return;
    const std::vector<Id> ids(m_selection.begin(), m_selection.end());
    apply(ripple ? tr("Eliminar y cerrar hueco") : tr("Eliminar"), [&](Project& p) { return edit::deleteClips(p, ids, ripple); });
}

void ProjectController::setClipGain(qint64 clipId, double gain)
{
    apply(tr("Volumen del clip"), [&](Project& p) { return edit::setClipGain(p, Id(clipId), float(gain)); },
          QStringLiteral("gain:%1").arg(clipId));
}

void ProjectController::setClipEnabled(qint64 clipId, bool enabled)
{
    apply(enabled ? tr("Activar clip") : tr("Desactivar clip"), [&](Project& p) { return edit::setClipEnabled(p, Id(clipId), enabled); });
}

void ProjectController::addTrack(const QString& kind)
{
    const TrackKind k = kind == QLatin1String("audio") ? TrackKind::Audio : TrackKind::Video;
    apply(tr("Añadir pista"), [&](Project& p) { return edit::addTrack(p, k) != kNoId; });
}

void ProjectController::removeTrack(qint64 trackId)
{
    apply(tr("Eliminar pista"), [&](Project& p) { return edit::removeTrack(p, Id(trackId)); });
}

void ProjectController::setTrackFlag(qint64 trackId, const QString& flag, bool value)
{
    if (flag == QLatin1String("muted"))
        apply(value ? tr("Silenciar pista") : tr("Activar sonido de pista"), [&](Project& p) { return edit::setTrackMuted(p, Id(trackId), value); });
    else if (flag == QLatin1String("hidden"))
        apply(value ? tr("Ocultar pista") : tr("Mostrar pista"), [&](Project& p) { return edit::setTrackHidden(p, Id(trackId), value); });
    else if (flag == QLatin1String("locked"))
        apply(value ? tr("Bloquear pista") : tr("Desbloquear pista"), [&](Project& p) { return edit::setTrackLocked(p, Id(trackId), value); });
}

QVariantMap ProjectController::clipInfo(qint64 clipId) const
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return {};
    const MediaItem* m = project().findMedia(c->mediaId);
    const Track* t = project().trackOfClip(c->id);
    return {
        {"clipId", qint64(c->id)},
        {"mediaId", qint64(c->mediaId)},
        {"name", m ? QString::fromStdString(m->name) : QString()},
        {"track", t ? QString::fromStdString(t->name) : QString()},
        {"start", qint64(c->start)},
        {"duration", qint64(c->duration)},
        {"sourceIn", toSeconds(c->sourceIn)},
        {"gain", double(c->gain)},
        {"enabled", c->enabled},
        {"hasAudio", m && m->hasAudio},
    };
}

QVariantList ProjectController::snapPoints(const QVariantList& excludeClipIds) const
{
    QSet<Id> exclude;
    for (const QVariant& v : excludeClipIds)
        exclude.insert(Id(v.toLongLong()));
    QSet<qint64> points{0, m_player->positionFrame()};
    for (const auto* list : {&project().videoTracks, &project().audioTracks})
        for (const auto& t : *list)
            for (const auto& c : t.clips)
                if (!exclude.contains(c.id)) {
                    points.insert(c.start);
                    points.insert(c.end());
                }
    QVariantList out;
    for (qint64 p : points)
        out.append(p);
    return out;
}

// --- Selección ---

void ProjectController::select(qint64 clipId, bool additive)
{
    const Id id = Id(clipId);
    if (additive) {
        if (!m_selection.remove(id))
            m_selection.insert(id);
    } else {
        m_selection = {id};
    }
    m_clips.sync(project(), m_selection);
    emit selectionChanged();
}

void ProjectController::clearSelection()
{
    if (m_selection.isEmpty())
        return;
    m_selection.clear();
    m_clips.sync(project(), m_selection);
    emit selectionChanged();
}

QString ProjectController::selectionKind() const
{
    bool video = false, audio = false;
    for (Id id : m_selection) {
        if (const Track* t = project().trackOfClip(id))
            (t->kind == TrackKind::Video ? video : audio) = true;
    }
    return video && audio ? QStringLiteral("mixed") : video ? QStringLiteral("video") : audio ? QStringLiteral("audio") : QString();
}

void ProjectController::selectAll()
{
    for (const auto* list : {&project().videoTracks, &project().audioTracks})
        for (const auto& t : *list)
            for (const auto& c : t.clips)
                m_selection.insert(c.id);
    m_clips.sync(project(), m_selection);
    emit selectionChanged();
}
