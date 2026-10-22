#include "ProjectController.h"

#include "ScriptHost.h"
#include "ofx/OfxHost.h"
#include "ai/AudioSync.h"

#include "EffectNames.h"

#include "core/Edits.h"
#include "core/Otio.h"
#include "core/Timeline.h"
#include "core/ProjectFile.h"
#include "engine/MediaImport.h"
#include "media/MediaInfo.h"

#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
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

constexpr int kBuiltinTitles = 4; // títulos predefinidos (luego van las plantillas del usuario)

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
    reloadScripts();
    movix::ofx::Registry::instance().setExtraDirs({ofxFolder().toStdString()}); // se buscan al pedirlos
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
    if (m_multicamView) { // solo la vista previa: la exportación usa m_snapshot
        auto preview = std::make_shared<Project>(*m_snapshot);
        preview->multicamGrid = true;
        m_player->setProject(preview);
    } else {
        m_player->setProject(m_snapshot);
    }
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

QString ProjectController::colorMode() const
{
    switch (project().settings.colorMode) {
    case ColorMode::PQ: return QStringLiteral("pq");
    case ColorMode::HLG: return QStringLiteral("hlg");
    case ColorMode::SDR: break;
    }
    return QStringLiteral("sdr");
}

void ProjectController::setColorMode(const QString& mode)
{
    const ColorMode m = mode == QLatin1String("pq") ? ColorMode::PQ : mode == QLatin1String("hlg") ? ColorMode::HLG : ColorMode::SDR;
    apply(tr("Espacio de color del proyecto"), [&](Project& p) { return edit::setColorMode(p, m); });
}

void ProjectController::setProjectSettings(int width, int height, int fpsNum, int fpsDen)
{
    apply(tr("Ajustes del proyecto"), [&](Project& p) {
        const bool a = edit::setResolution(p, width, height);
        const bool b = edit::setFrameRate(p, {fpsNum, std::max(1, fpsDen)});
        return a || b;
    });
}

// --- OpenTimelineIO ---

void ProjectController::importOtio(const QUrl& url)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return;
    ++m_importing;
    emit importingChanged();
    // Se sondean todos sus medios: en segundo plano, como al importarlos.
    QThreadPool::globalInstance()->start([this, path, url] {
        auto result = std::make_shared<otio::ImportResult>(otio::importFile(path, [](const std::string& media) {
            return engine::probeMediaItem(media);
        }));
        QMetaObject::invokeMethod(this, [this, url, result] {
            if (result->project) {
                resetTo(std::move(*result->project), QUrl());
                m_history.markDirty(); // aún no tiene archivo de proyecto
                emit historyChanged();
                discardAutosave();
                QStringList problems;
                if (!result->missingMedia.isEmpty())
                    problems.append(tr("No se encontraron %n archivos de medios:\n%1", nullptr, int(result->missingMedia.size()))
                                        .arg(result->missingMedia.join(QLatin1Char('\n'))));
                if (!result->warnings.isEmpty())
                    problems.append(tr("Algunos elementos no se importaron:\n%1").arg(result->warnings.join(QLatin1Char('\n'))));
                if (problems.isEmpty())
                    emit notice(tr("Línea de tiempo importada de «%1».").arg(QFileInfo(url.toLocalFile()).fileName()));
                else
                    emit errorOccurred(problems.join(QStringLiteral("\n\n")));
            } else {
                emit errorOccurred(tr("No se pudo importar la línea de tiempo: %1").arg(result->error));
            }
            if (--m_importing == 0)
                finishImport();
            emit importingChanged();
        }, Qt::QueuedConnection);
    });
}

bool ProjectController::exportOtio(const QUrl& url)
{
    QString path = url.toLocalFile();
    if (QFileInfo(path).suffix().compare(QLatin1String(otio::kOtioExtension), Qt::CaseInsensitive) != 0)
        path += QLatin1Char('.') + QLatin1String(otio::kOtioExtension);
    QString error;
    if (!otio::exportFile(project(), path, &error)) {
        emit errorOccurred(tr("No se pudo exportar la línea de tiempo: %1").arg(error));
        return false;
    }
    emit notice(tr("Línea de tiempo exportada a «%1». Los efectos de imagen (color, máscaras, transformaciones…) no se incluyen.")
                    .arg(QFileInfo(path).fileName()));
    return true;
}

// --- Plantillas propias ---

QStringList ProjectController::projectTemplates() const
{
    QStringList names;
    for (const auto& t : m_templates.projects())
        names.append(t.name);
    return names;
}

bool ProjectController::saveTitleTemplate(qint64 clipId, const QString& name)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c || !c->title)
        return false;
    const auto& s = project().settings;
    TemplateStore::Title t;
    t.name = name;
    t.style = *c->title;
    t.x = c->transform.x.at(0);
    t.y = c->transform.y.at(0);
    t.scale = c->transform.scale.at(0);
    t.rotation = c->transform.rotation.at(0);
    t.opacity = c->transform.opacity.at(0);
    t.seconds = toSeconds(s.frameToTime(c->duration));
    t.fadeIn = toSeconds(s.frameToTime(c->fadeIn));
    t.fadeOut = toSeconds(s.frameToTime(c->fadeOut));
    QString error;
    if (!m_templates.addTitle(t, &error)) {
        emit errorOccurred(tr("No se pudo guardar la plantilla: %1").arg(error));
        return false;
    }
    emit templatesChanged();
    emit notice(tr("Plantilla de título «%1» guardada: está en la pestaña Títulos.").arg(name.trimmed()));
    return true;
}

void ProjectController::removeTitleTemplate(int presetIndex)
{
    if (m_templates.removeTitle(presetIndex - kBuiltinTitles))
        emit templatesChanged();
}

bool ProjectController::saveProjectTemplate(const QString& name)
{
    QString error;
    if (!m_templates.saveProject(project(), name, &error)) {
        emit errorOccurred(tr("No se pudo guardar la plantilla: %1").arg(error));
        return false;
    }
    emit templatesChanged();
    emit notice(tr("Plantilla de proyecto «%1» guardada: está en Archivo → Nuevo desde plantilla.").arg(name.trimmed()));
    return true;
}

void ProjectController::removeProjectTemplate(const QString& name)
{
    if (m_templates.removeProject(name))
        emit templatesChanged();
}

bool ProjectController::newFromTemplate(const QString& name)
{
    const LoadResult result = m_templates.loadProject(name);
    if (!result.project) {
        emit errorOccurred(result.error);
        return false;
    }
    resetTo(*result.project, QUrl()); // sin archivo: guardarlo no pisa la plantilla
    m_history.markDirty();
    emit historyChanged();
    discardAutosave();
    if (!result.missingMedia.isEmpty())
        emit errorOccurred(tr("No se encontraron %n archivos de medios:\n%1", nullptr, int(result.missingMedia.size()))
                               .arg(result.missingMedia.join(QLatin1Char('\n'))));
    return true;
}

// --- Scripts ---

QString ProjectController::scriptsFolder() const
{
    return qEnvironmentVariableIsSet("MOVIX_SCRIPTS_DIR") ? qEnvironmentVariable("MOVIX_SCRIPTS_DIR")
                                                         : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/scripts");
}

void ProjectController::reloadScripts()
{
    m_scripts.clear();
    for (const movix::app::ScriptInfo& s : movix::app::listScripts(scriptsFolder()))
        m_scripts.append(QVariantMap{{"name", s.name}, {"description", s.description}, {"path", s.path}, {"builtin", s.builtin}});
    emit scriptsChanged();
}

bool ProjectController::runScript(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit errorOccurred(tr("No se pudo abrir el script: %1").arg(file.errorString()));
        return false;
    }
    const QString code = QString::fromUtf8(file.readAll());
    const QString name = movix::app::scriptInfo(path, code).name;
    std::vector<Id> selection(m_selection.begin(), m_selection.end());
    movix::app::ScriptResult result;
    apply(tr("Script «%1»").arg(name), [&](Project& p) {
        result = movix::app::runScript(code, p, m_player->positionFrame(), selection);
        return result.ok && result.changed; // si falla, el proyecto queda como estaba
    });
    if (!result.ok) {
        emit errorOccurred(tr("El script «%1» falló. %2").arg(name, result.error));
        return false;
    }
    const QString log = result.log.join(QStringLiteral(" · "));
    emit notice(result.changed ? (log.isEmpty() ? tr("«%1» terminado.").arg(name) : QStringLiteral("%1: %2").arg(name, log))
                               : tr("«%1» no cambió nada.").arg(name) + (log.isEmpty() ? QString() : QStringLiteral(" ") + log));
    return true;
}

void ProjectController::openScriptsFolder()
{
    QDir().mkpath(scriptsFolder());
    QDesktopServices::openUrl(QUrl::fromLocalFile(scriptsFolder()));
}

// --- Multicámara ---

namespace {

QVariantMap multicamInfo(const Project& p, const Clip& c)
{
    QVariantList angles, audio;
    if (const auto* v = p.videoTracksOf(c.sequenceId))
        for (const Track& t : *v)
            angles.append(QString::fromStdString(t.name));
    audio = angles;
    if (const auto* a = p.audioTracksOf(c.sequenceId)) // todas: el índice es el de audioAngle
        for (const Track& t : *a)
            audio.append(t.clips.empty() ? ProjectController::tr("%1 (vacía)").arg(QString::fromStdString(t.name))
                                         : QString::fromStdString(t.name));
    // Rectángulo de cada ángulo en la cuadrícula (fracciones del lienzo) para el visor.
    QVariantList cells;
    const MulticamGrid grid = multicamGridFor(int(angles.size()));
    for (int a = 0; a < int(angles.size()); ++a) {
        const auto r = grid.image(a);
        cells.append(QVariant(QVariantList{r.x, r.y, r.width, r.height})); // un elemento, no cuatro
    }
    return {{"clipId", qint64(c.id)}, {"angle", c.angle},   {"audioAngle", c.audioAngle},
            {"angles", angles},       {"audioSources", audio}, {"cells", cells}};
}

const Clip* multicamClipAt(const Project& p, FramePos frame)
{
    for (auto it = p.videoTracks.rbegin(); it != p.videoTracks.rend(); ++it) {
        if (it->hidden)
            continue;
        if (const Clip* c = edit::clipAt(*it, frame); c && c->isMulticam())
            return c;
    }
    return nullptr;
}

} // namespace

void ProjectController::setMulticamView(bool on)
{
    if (m_multicamView == on)
        return;
    m_multicamView = on;
    refresh();
    emit multicamViewChanged();
}

QVariantMap ProjectController::multicamAt(qint64 frame) const
{
    const Clip* c = multicamClipAt(project(), FramePos(frame));
    return c ? multicamInfo(project(), *c) : QVariantMap();
}

bool ProjectController::switchAngle(int angle)
{
    const FramePos frame = m_player->positionFrame();
    const Clip* c = multicamClipAt(project(), frame);
    if (!c)
        return false;
    const Id clipId = c->id;
    const int before = c->angle;
    Id result = kNoId;
    apply(tr("Cambiar de ángulo"), [&](Project& p) {
        result = edit::switchAngle(p, clipId, frame, angle);
        return result != kNoId && (result != clipId || before != angle);
    });
    return result != kNoId;
}

void ProjectController::setMulticamAngle(qint64 clipId, int angle)
{
    apply(tr("Ángulo de la multicámara"), [&](Project& p) { return edit::setMulticamAngle(p, Id(clipId), angle); });
}

void ProjectController::setMulticamAudio(qint64 clipId, int audioAngle)
{
    apply(tr("Audio de la multicámara"), [&](Project& p) { return edit::setMulticamAudio(p, Id(clipId), audioAngle); });
}

void ProjectController::createMulticam(const QVariantList& mediaIds, bool syncByAudio)
{
    std::vector<Id> ids;
    std::vector<std::string> paths;
    for (const QVariant& v : mediaIds) {
        const MediaItem* m = project().findMedia(Id(v.toLongLong()));
        if (!m || m->isStill || std::find(ids.begin(), ids.end(), m->id) != ids.end())
            continue;
        ids.push_back(m->id);
        paths.push_back(m->path);
    }
    const bool anyVideo = std::any_of(ids.begin(), ids.end(), [&](Id id) { return project().findMedia(id)->hasVideo; });
    if (ids.size() < 2 || !anyVideo) {
        emit errorOccurred(tr("Para una multicámara hacen falta al menos dos grabaciones (de video o audio, no imágenes) y una de ellas con video."));
        return;
    }
    ++m_importing;
    emit importingChanged();
    QThreadPool::globalInstance()->start([this, ids, paths, syncByAudio] {
        ai::SyncResult sync;
        if (syncByAudio)
            sync = ai::syncFilesByAudio(paths);
        QMetaObject::invokeMethod(this, [this, ids, syncByAudio, sync] {
            if (--m_importing == 0)
                finishImport();
            emit importingChanged();
            // Los medios pudieron quitarse mientras se sincronizaba.
            std::vector<edit::MulticamSource> sources;
            QStringList unsynced;
            for (size_t i = 0; i < ids.size(); ++i) {
                const MediaItem* m = project().findMedia(ids[i]);
                if (!m)
                    continue;
                const bool ok = syncByAudio && i < sync.synced.size() && sync.synced[i];
                sources.push_back({ids[i], ok ? sync.offsets[i] : 0});
                if (syncByAudio && !ok)
                    unsynced.append(QString::fromStdString(m->name));
            }
            int count = 1;
            for (const Sequence& seq : project().sequences)
                count += QString::fromStdString(seq.name).startsWith(tr("Multicámara")) ? 1 : 0;
            const std::string name = tr("Multicámara %1").arg(count).toStdString();
            Id clip = kNoId;
            const bool ok = apply(tr("Crear multicámara"), [&](Project& p) {
                for (const auto& src : sources)
                    if (const MediaItem* m = p.findMedia(src.mediaId); m && m->hasVideo) {
                        edit::matchSettingsToMedia(p, *m); // solo si la línea de tiempo está vacía
                        break;
                    }
                const Track* track = firstUnlocked(p.videoTracks);
                if (!track)
                    return false;
                clip = edit::createMulticam(p, sources, name, track->id, p.duration());
                return clip != kNoId;
            });
            if (!ok) {
                emit errorOccurred(tr("No se pudo crear la multicámara."));
                return;
            }
            m_selection = {clip};
            m_clips.sync(project(), m_selection);
            emit selectionChanged();
            m_player->seekFrame(project().findClip(clip)->start);
            setMulticamView(true);
            const int angles = edit::multicamAngles(project(), *project().findClip(clip));
            QString text = tr("Multicámara con %n ángulos. Reproduce y pulsa 1–%1 (o haz clic en un ángulo) para cortar a él.", nullptr, angles)
                               .arg(std::min(angles, 9));
            if (!unsynced.isEmpty())
                emit errorOccurred(text + QLatin1Char('\n') + tr("No se pudo sincronizar por el audio: %1 (se alineó al inicio).").arg(unsynced.join(QStringLiteral(", "))));
            else
                emit notice(text);
        }, Qt::QueuedConnection);
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
        if (QFileInfo(path).suffix().compare(QLatin1String(otio::kOtioExtension), Qt::CaseInsensitive) == 0) {
            importOtio(url);
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
        clip = edit::placeClip(p, target, Id(mediaId), p.settings.timeToFrame(at), 0, 0,
                               m_insertMode ? edit::PlaceMode::Insert : edit::PlaceMode::Overwrite);
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

namespace {

// Propiedad animable por nombre de QML: "x", "opacity", "cropLeft"... o "mask.<índice>.<campo>"
// (campo: x, y, width, height, rotation, feather).
struct NamedProperty {
    edit::Property prop;
    int maskIndex = 0;
};

QString interpolationName(Interpolation i)
{
    return i == Interpolation::Hold ? QStringLiteral("hold") : i == Interpolation::Smooth ? QStringLiteral("smooth") : QStringLiteral("linear");
}

std::optional<NamedProperty> propertyFromName(const QString& name)
{
    static const QHash<QString, edit::Property> kMap{
        {"x", edit::Property::PositionX}, {"y", edit::Property::PositionY}, {"scale", edit::Property::Scale},
        {"rotation", edit::Property::Rotation}, {"opacity", edit::Property::Opacity}, {"volume", edit::Property::Volume},
        {"cropLeft", edit::Property::CropLeft}, {"cropTop", edit::Property::CropTop}, {"cropRight", edit::Property::CropRight},
        {"cropBottom", edit::Property::CropBottom}, {"speed", edit::Property::Speed},
    };
    static const QHash<QString, edit::Property> kMask{
        {"x", edit::Property::MaskX}, {"y", edit::Property::MaskY}, {"width", edit::Property::MaskWidth},
        {"height", edit::Property::MaskHeight}, {"rotation", edit::Property::MaskRotation}, {"feather", edit::Property::MaskFeather},
    };
    if (auto it = kMap.find(name); it != kMap.end())
        return NamedProperty{it.value(), 0};
    const QStringList parts = name.split(QLatin1Char('.'));
    bool ok = false;
    if (parts.size() == 3 && parts[0] == QLatin1String("mask")) {
        const int index = parts[1].toInt(&ok);
        if (auto it = kMask.find(parts[2]); ok && index >= 0 && it != kMask.end())
            return NamedProperty{it.value(), index};
    }
    return std::nullopt;
}

const char* const kTransitionNames[] = {"none", "dissolve", "dipToBlack", "wipeLeft", "wipeRight", "pushLeft", "pushRight", "slideLeft"};

TransitionType transitionFromName(const QString& name)
{
    for (int i = 0; i < int(std::size(kTransitionNames)); ++i)
        if (name == QLatin1String(kTransitionNames[i]))
            return TransitionType(i);
    return TransitionType::None;
}

using movix::app::colorName;
using movix::app::colorValue;

} // namespace

FramePos ProjectController::playheadRel(const Clip& c) const
{
    return m_player->positionFrame() - c.start;
}

void ProjectController::setClipProperty(qint64 clipId, const QString& prop, double value)
{
    const auto property = propertyFromName(prop);
    const Clip* c = project().findClip(Id(clipId));
    if (!property || !c)
        return;
    const FramePos rel = playheadRel(*c);
    // Arrastrar un control agrupa todos los cambios en un único paso de deshacer.
    apply(tr("Cambiar propiedad del clip"),
          [&](Project& p) { return edit::setProperty(p, Id(clipId), property->prop, rel, value, property->maskIndex); },
          QStringLiteral("prop:%1:%2").arg(clipId).arg(prop));
}

void ProjectController::toggleKeyframe(qint64 clipId, const QString& prop)
{
    const auto property = propertyFromName(prop);
    const Clip* c = project().findClip(Id(clipId));
    if (!property || !c)
        return;
    const FramePos rel = playheadRel(*c);
    apply(tr("Keyframe"), [&](Project& p) { return edit::toggleKeyframe(p, Id(clipId), property->prop, rel, property->maskIndex); });
}

void ProjectController::resetClipProperty(qint64 clipId, const QString& prop)
{
    if (const auto property = propertyFromName(prop))
        apply(tr("Restablecer propiedad"),
              [&](Project& p) { return edit::resetProperty(p, Id(clipId), property->prop, property->maskIndex); });
}

QVariantMap ProjectController::propertyState(qint64 clipId, const QString& prop) const
{
    const auto property = propertyFromName(prop);
    const Clip* c = project().findClip(Id(clipId));
    if (!property || !c)
        return {};
    const Animated* animated = edit::property(*c, property->prop, property->maskIndex);
    if (!animated)
        return {};
    const Animated& a = *animated;
    const FramePos rel = playheadRel(*c);
    bool prev = false, next = false;
    for (const Keyframe& k : a.keys) {
        prev = prev || k.frame < rel;
        next = next || k.frame > rel;
    }
    const Keyframe* here = a.keyAt(rel);
    return {{"value", a.at(rel)},           {"animated", a.isAnimated()}, {"keyHere", here != nullptr},
            {"interp", here ? interpolationName(here->interp) : QString()}, {"hasPrev", prev}, {"hasNext", next}};
}

void ProjectController::setKeyInterpolation(qint64 clipId, const QString& prop, const QString& interp)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const Interpolation mode = interp == QLatin1String("hold") ? Interpolation::Hold
                             : interp == QLatin1String("smooth") ? Interpolation::Smooth
                                                                 : Interpolation::Linear;
    const FramePos rel = playheadRel(*c);
    if (prop == QLatin1String("color")) {
        const FramePos r = std::clamp<FramePos>(rel, 0, c->duration - 1);
        apply(tr("Interpolación del keyframe"), [&](Project& p) { return edit::setColorKeyInterpolation(p, Id(clipId), r, mode); });
        return;
    }
    const auto property = propertyFromName(prop);
    if (!property)
        return;
    apply(tr("Interpolación del keyframe"), [&](Project& p) {
        return edit::setKeyframeInterpolation(p, Id(clipId), property->prop, rel, mode, property->maskIndex);
    });
}

QVariantList ProjectController::keyframeFrames(qint64 clipId) const
{
    const Clip* c = project().findClip(Id(clipId));
    QSet<qint64> frames;
    if (c) {
        for (int prop = 0; prop < int(edit::Property::MaskX); ++prop)
            for (const Keyframe& k : edit::property(*c, edit::Property(prop))->keys)
                frames.insert(c->start + k.frame);
        for (int mask = 0; mask < int(c->masks.size()); ++mask)
            for (int prop = int(edit::Property::MaskX); prop <= int(edit::Property::MaskFeather); ++prop)
                for (const Keyframe& k : edit::property(*c, edit::Property(prop), mask)->keys)
                    frames.insert(c->start + k.frame);
        for (const GradeKey& k : c->color.keys)
            frames.insert(c->start + k.frame);
    }
    QVariantList out;
    for (qint64 f : frames)
        out.append(f);
    return out;
}

void ProjectController::jumpToKeyframe(qint64 clipId, bool forward)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const qint64 now = m_player->positionFrame();
    qint64 best = -1;
    for (const QVariant& v : keyframeFrames(clipId)) {
        const qint64 f = v.toLongLong();
        if (forward ? (f > now && (best < 0 || f < best)) : (f < now && (best < 0 || f > best)))
            best = f;
    }
    if (best >= 0) {
        m_player->pause();
        m_player->seekFrame(best);
    }
}

QVariantMap ProjectController::clipGeometry(qint64 clipId) const
{
    const Clip* c = project().findClip(Id(clipId));
    const Track* t = project().trackOfClip(Id(clipId));
    if (!c || !t || t->kind != TrackKind::Video)
        return {};
    const auto& s = project().settings;
    // Relación de aspecto propia de la capa: la del medio o, en títulos, la del lienzo.
    double aspect = double(s.width) / s.height;
    if (const MediaItem* m = c->title ? nullptr : project().findMedia(c->mediaId); m && m->width > 0 && m->height > 0)
        aspect = double(m->width) / m->height;
    const double canvasAspect = double(s.width) / s.height;
    const double fitW = aspect > canvasAspect ? 1.0 : aspect / canvasAspect; // fracción del ancho del lienzo
    const double fitH = aspect > canvasAspect ? canvasAspect / aspect : 1.0;
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    const double scale = c->transform.scale.at(rel);
    QVariantList masks;
    for (const Mask& m : c->masks) {
        QVariantList points;
        for (const PathPoint& pt : m.points)
            points.append(QVariant(QVariantList{pt.x, pt.y, pt.inX, pt.inY, pt.outX, pt.outY}));
        masks.append(QVariantMap{{"shape", QLatin1String(movix::app::kMaskShapeNames[int(m.shape)])},
                                 {"x", m.x.at(rel)},
                                 {"y", m.y.at(rel)},
                                 {"width", m.width.at(rel)},
                                 {"height", m.height.at(rel)},
                                 {"rotation", m.rotation.at(rel)},
                                 {"points", points}});
    }
    return {{"cx", 0.5 + c->transform.x.at(rel)},
            {"cy", 0.5 + c->transform.y.at(rel)},
            {"hw", fitW / 2 * scale},
            {"hh", fitH / 2 * scale},
            {"rotation", c->transform.rotation.at(rel)},
            {"visible", playheadRel(*c) >= 0 && playheadRel(*c) < c->duration},
            // Máscaras en coordenadas de la capa (0..1), para dibujarlas y moverlas sobre el visor.
            {"masks", masks}};
}

void ProjectController::setClipSpeed(qint64 clipId, double speed)
{
    apply(tr("Velocidad del clip"), [&](Project& p) { return edit::setClipSpeed(p, Id(clipId), speed); });
}

void ProjectController::setReverse(qint64 clipId, bool reverse)
{
    apply(reverse ? tr("Reproducir al revés") : tr("Reproducir hacia delante"),
          [&](Project& p) { return edit::setReverse(p, Id(clipId), reverse); });
}

void ProjectController::setFades(qint64 clipId, double inSeconds, double outSeconds, bool audio)
{
    const auto& s = project().settings;
    const FramePos in = s.framesIn(fromSeconds(std::max(0.0, inSeconds)));
    const FramePos out = s.framesIn(fromSeconds(std::max(0.0, outSeconds)));
    apply(audio ? tr("Fundidos de audio") : tr("Fundidos"),
          [&](Project& p) { return audio ? edit::setAudioFades(p, Id(clipId), in, out) : edit::setFades(p, Id(clipId), in, out); },
          QStringLiteral("fade:%1:%2").arg(clipId).arg(audio));
}

QVariantList ProjectController::markers() const
{
    QVariantList list;
    for (const Marker& m : project().markers)
        list.append(QVariantMap{{"frame", qint64(m.frame)},
                                {"label", QString::fromStdString(m.label)},
                                {"color", QColor::fromRgba(QRgb(m.color))}});
    return list;
}

void ProjectController::addMarker()
{
    const FramePos frame = m_player->positionFrame();
    apply(tr("Añadir marcador"), [&](Project& p) { return edit::addMarkers(p, {frame}, {}) > 0; });
}

void ProjectController::removeMarker(qint64 frame)
{
    apply(tr("Quitar marcador"), [&](Project& p) { return edit::removeMarker(p, frame); });
}

void ProjectController::clearMarkers()
{
    apply(tr("Quitar los marcadores"), [&](Project& p) { return edit::clearMarkers(p); });
}

void ProjectController::jumpToMarker(bool forward)
{
    const FramePos now = m_player->positionFrame();
    std::optional<FramePos> target;
    for (const Marker& m : project().markers) {
        if (forward && m.frame > now) {
            target = m.frame;
            break;
        }
        if (!forward && m.frame < now)
            target = m.frame;
    }
    if (target)
        m_player->seekFrame(*target);
}

void ProjectController::setMatteInvert(qint64 clipId, bool invert)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c || !c->matte.active())
        return;
    AiMatte m = c->matte;
    m.invert = invert;
    apply(tr("Invertir el recorte"), [&](Project& p) { return edit::setMatte(p, Id(clipId), m); });
}

void ProjectController::removeMatte(qint64 clipId)
{
    apply(tr("Quitar el recorte"), [&](Project& p) { return edit::setMatte(p, Id(clipId), AiMatte{}); });
}

void ProjectController::setDenoise(qint64 clipId, double amount)
{
    apply(tr("Reducción de ruido"), [&](Project& p) { return edit::setDenoise(p, Id(clipId), amount); },
          QStringLiteral("denoise:%1").arg(clipId));
}

bool ProjectController::setTransition(qint64 clipId, const QString& type, double seconds)
{
    const FramePos frames = project().settings.framesIn(fromSeconds(std::max(0.0, seconds)));
    return apply(tr("Transición"), [&](Project& p) { return edit::setTransition(p, Id(clipId), transitionFromName(type), frames); },
                 QStringLiteral("transition:%1").arg(clipId));
}

bool ProjectController::dropTransition(const QString& type, qint64 trackId, qint64 frame)
{
    const Track* t = project().findTrack(Id(trackId));
    if (!t)
        return false;
    // Corte (inicio de un clip pegado al anterior) más cercano al punto de soltado.
    const Clip* best = nullptr;
    for (size_t i = 1; i < t->clips.size(); ++i) {
        const Clip& c = t->clips[i];
        if (t->clips[i - 1].end() != c.start)
            continue;
        if (!best || std::llabs(c.start - frame) < std::llabs(best->start - frame))
            best = &c;
    }
    if (!best) {
        emit notice(tr("Las transiciones se colocan en el corte entre dos clips contiguos de la misma pista."));
        return false;
    }
    const Id id = best->id;
    const bool ok = setTransition(qint64(id), type, 1.0);
    if (ok) {
        m_selection = {id};
        m_clips.sync(project(), m_selection);
        emit selectionChanged();
    }
    return ok;
}

QVariantList ProjectController::titlePresets() const
{
    QVariantList list{
        QVariantMap{{"name", tr("Título")}, {"text", tr("Título")}, {"user", false}},
        QVariantMap{{"name", tr("Rótulo inferior")}, {"text", tr("Nombre Apellido")}, {"user", false}},
        QVariantMap{{"name", tr("Título con fondo")}, {"text", tr("Capítulo 1")}, {"user", false}},
        QVariantMap{{"name", tr("Subtítulo")}, {"text", tr("Texto del subtítulo")}, {"user", false}},
    };
    static_assert(kBuiltinTitles == 4);
    for (const TemplateStore::Title& t : m_templates.titles())
        list.append(QVariantMap{{"name", t.name}, {"text", QString::fromStdString(t.style.text)}, {"user", true}});
    return list;
}

bool ProjectController::addTitle(int preset, qint64 trackId, qint64 frame)
{
    TitleStyle style;
    Transform transform;
    FramePos duration = project().settings.framesIn(5 * kUsPerSecond);
    FramePos fadeIn = 0, fadeOut = 0;
    if (preset >= kBuiltinTitles) { // plantilla del usuario
        const auto& titles = m_templates.titles();
        if (preset - kBuiltinTitles >= int(titles.size()))
            return false;
        const TemplateStore::Title& t = titles[size_t(preset - kBuiltinTitles)];
        const auto& s = project().settings;
        style = t.style;
        transform.x = t.x;
        transform.y = t.y;
        transform.scale = t.scale;
        transform.rotation = t.rotation;
        transform.opacity = t.opacity;
        duration = std::max<FramePos>(1, s.framesIn(TimeUs(t.seconds * kUsPerSecond)));
        fadeIn = std::min(duration, s.timeToFrame(TimeUs(t.fadeIn * kUsPerSecond)));
        fadeOut = std::min(duration - fadeIn, s.timeToFrame(TimeUs(t.fadeOut * kUsPerSecond)));
    }
    switch (preset < kBuiltinTitles ? preset : -1) {
    case -1:
        break;
    case 1: // rótulo inferior a la izquierda
        style.text = tr("Nombre Apellido").toStdString();
        style.size = 0.06;
        style.align = 0;
        style.background = 0xB0101420;
        transform.x = -0.22;
        transform.y = 0.32;
        break;
    case 2:
        style.text = tr("Capítulo 1").toStdString();
        style.size = 0.1;
        style.background = 0xC07C6CFF;
        break;
    case 3: // subtítulo
        style.text = tr("Texto del subtítulo").toStdString();
        style.size = 0.055;
        style.bold = false;
        style.outline = 0.08;
        transform.y = 0.38;
        break;
    default:
        style.text = tr("Título").toStdString();
        break;
    }
    const FramePos at = frame >= 0 ? frame : m_player->positionFrame();
    // Pista: la indicada o la superior si está libre en ese tramo; si no, una nueva encima de
    // todo (un título debajo de otro video quedaría tapado).
    Id target = trackId > 0 ? Id(trackId) : kNoId;
    Id clip = kNoId;
    const bool ok = apply(tr("Añadir título"), [&](Project& p) {
        if (target == kNoId) {
            const Track& top = p.videoTracks.back();
            const bool free = std::none_of(top.clips.begin(), top.clips.end(),
                                           [&](const Clip& c) { return c.start < at + duration && c.end() > at; });
            target = free && !top.locked ? top.id : edit::addTrack(p, TrackKind::Video);
        }
        clip = edit::placeTitle(p, target, at, duration, style, m_insertMode ? edit::PlaceMode::Insert : edit::PlaceMode::Overwrite);
        if (clip == kNoId)
            return false;
        p.findClip(clip)->transform = transform;
        if (fadeIn > 0 || fadeOut > 0)
            edit::setFades(p, clip, fadeIn, fadeOut);
        return true;
    });
    if (ok) {
        m_selection = {clip};
        m_clips.sync(project(), m_selection);
        emit selectionChanged();
    }
    return ok;
}

void ProjectController::setTitleStyle(qint64 clipId, const QVariantMap& v)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c || !c->title)
        return;
    TitleStyle s = *c->title;
    if (v.contains("text")) s.text = v["text"].toString().toStdString();
    if (v.contains("font")) s.font = v["font"].toString().toStdString();
    if (v.contains("size")) s.size = std::clamp(v["size"].toDouble(), 0.01, 0.5);
    if (v.contains("color")) s.color = colorValue(v["color"], s.color);
    if (v.contains("bold")) s.bold = v["bold"].toBool();
    if (v.contains("italic")) s.italic = v["italic"].toBool();
    if (v.contains("align")) s.align = std::clamp(v["align"].toInt(), 0, 2);
    if (v.contains("outline")) s.outline = std::clamp(v["outline"].toDouble(), 0.0, 0.3);
    if (v.contains("outlineColor")) s.outlineColor = colorValue(v["outlineColor"], s.outlineColor);
    if (v.contains("shadow")) s.shadow = v["shadow"].toBool();
    if (v.contains("background")) s.background = v["background"].toString().isEmpty() ? 0 : colorValue(v["background"], s.background);
    if (v.contains("animIn")) s.animIn = TextAnimation(std::clamp(v["animIn"].toInt(), 0, int(TextAnimation::Typewriter)));
    if (v.contains("animOut")) s.animOut = TextAnimation(std::clamp(v["animOut"].toInt(), 0, int(TextAnimation::Typewriter)));
    if (v.contains("animSeconds")) s.animSeconds = std::clamp(v["animSeconds"].toDouble(), 0.05, 10.0);
    apply(tr("Editar título"), [&](Project& p) { return edit::setTitleStyle(p, Id(clipId), s); }, QStringLiteral("title:%1").arg(clipId));
}

void ProjectController::detachAudio(qint64 clipId)
{
    apply(tr("Separar audio"), [&](Project& p) { return edit::detachAudio(p, Id(clipId)) != kNoId; });
}

void ProjectController::setTrackVolume(qint64 trackId, double volume)
{
    apply(tr("Volumen de la pista"), [&](Project& p) { return edit::setTrackVolume(p, Id(trackId), volume); },
          QStringLiteral("trackvol:%1").arg(trackId));
}

void ProjectController::copySelection()
{
    m_clipboard = edit::copyClips(project(), std::vector<Id>(m_selection.begin(), m_selection.end()));
    emit clipboardChanged();
}

void ProjectController::cutSelection()
{
    copySelection();
    deleteSelection(false);
}

void ProjectController::paste()
{
    if (m_clipboard.empty())
        return;
    const FramePos at = m_player->positionFrame();
    std::vector<Id> created;
    apply(tr("Pegar"), [&](Project& p) {
        created = edit::pasteClips(p, m_clipboard, at);
        return !created.empty();
    });
    m_selection = QSet<Id>(created.begin(), created.end());
    m_clips.sync(project(), m_selection);
    emit selectionChanged();
}

void ProjectController::setInsertMode(bool on)
{
    if (on == m_insertMode)
        return;
    m_insertMode = on;
    emit insertModeChanged();
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
    const MediaItem* m = c->title ? nullptr : project().findMedia(c->mediaId);
    const Track* t = project().trackOfClip(c->id);
    const auto& s = project().settings;
    const auto& clips = t->clips;
    const auto idx = size_t(c - clips.data());
    const bool hasCut = idx > 0 && clips[idx - 1].end() == c->start;
    QVariantMap info{
        {"clipId", qint64(c->id)},
        {"mediaId", qint64(c->mediaId)},
        {"name", c->title          ? tr("Título")
                 : c->isCompound() ? QString::fromStdString(project().sequenceName(c->sequenceId))
                 : m               ? QString::fromStdString(m->name)
                                   : QString()},
        {"isCompound", c->isCompound()},
        {"isMulticam", c->isMulticam()},
        {"track", t ? QString::fromStdString(t->name) : QString()},
        {"trackId", qint64(t->id)},
        {"trackVolume", t->volume},
        {"isVideoTrack", t->kind == TrackKind::Video},
        {"start", qint64(c->start)},
        {"duration", qint64(c->duration)},
        {"sourceIn", toSeconds(c->sourceIn)},
        {"speed", c->speed},
        {"reverse", c->reverse},
        {"speedRamp", c->hasRamp()},
        {"canRamp", m && !m->isStill && !c->isGenerated()},
        {"enabled", c->enabled},
        {"hasAudio", m && m->hasAudio && !(t->kind == TrackKind::Video && c->audioDetached)},
        {"canDetachAudio", m && m->hasAudio && t->kind == TrackKind::Video && !c->audioDetached},
        {"isStill", m && m->isStill},
        {"isTitle", c->title.has_value()},
        {"fadeIn", toSeconds(s.frameToTime(c->fadeIn))},
        {"fadeOut", toSeconds(s.frameToTime(c->fadeOut))},
        {"audioFadeIn", toSeconds(s.frameToTime(c->audioFadeIn))},
        {"denoise", c->denoise},
        {"audioFadeOut", toSeconds(s.frameToTime(c->audioFadeOut))},
        {"hasCut", hasCut},
        {"transition", QLatin1String(kTransitionNames[int(c->transitionIn.type)])},
        {"transitionSeconds", toSeconds(s.frameToTime(c->transitionIn.duration))},
        {"durationSeconds", toSeconds(s.frameToTime(c->duration))},
        {"blend", QLatin1String(movix::app::kBlendModeNames[int(c->blend)])},
        {"masks", [&] {
             QVariantList list;
             for (const Mask& m : c->masks)
                 list.append(QVariantMap{{"shape", QLatin1String(movix::app::kMaskShapeNames[int(m.shape)])},
                                         {"mode", QLatin1String(movix::app::kMaskModeNames[int(m.mode)])},
                                         {"invert", m.invert}});
             return list;
         }()},
        {"colorActive", !c->color.isIdentity()},
        {"hasMatte", c->matte.active()},
        {"matteInvert", c->matte.invert},
        {"chromaKey", QVariantMap{{"enabled", c->chromaKey.enabled},
                                  {"color", colorName(c->chromaKey.color)},
                                  {"tolerance", c->chromaKey.tolerance},
                                  {"softness", c->chromaKey.softness},
                                  {"spill", c->chromaKey.spill}}},
    };
    if (c->title) {
        const TitleStyle& ts = *c->title;
        info["title"] = QVariantMap{
            {"text", QString::fromStdString(ts.text)}, {"font", QString::fromStdString(ts.font)}, {"size", ts.size},
            {"color", colorName(ts.color)}, {"bold", ts.bold}, {"italic", ts.italic}, {"align", ts.align},
            {"outline", ts.outline}, {"outlineColor", colorName(ts.outlineColor)}, {"shadow", ts.shadow},
            {"background", ts.background ? colorName(ts.background) : QString()},
            {"animIn", int(ts.animIn)}, {"animOut", int(ts.animOut)}, {"animSeconds", ts.animSeconds},
        };
    }
    if (c->isMulticam())
        info["multicam"] = multicamInfo(project(), *c);
    return info;
}

QVariantList ProjectController::snapPoints(const QVariantList& excludeClipIds) const
{
    QSet<Id> exclude;
    for (const QVariant& v : excludeClipIds)
        exclude.insert(Id(v.toLongLong()));
    QSet<qint64> points{0, m_player->positionFrame()};
    for (const Marker& m : project().markers)
        points.insert(m.frame);
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

void ProjectController::selectRange(qint64 fromFrame, qint64 toFrame, int firstRow, int lastRow, bool additive)
{
    const auto& p = project();
    if (!additive)
        m_selection.clear();
    const FramePos a = std::min(fromFrame, toFrame), b = std::max(fromFrame, toFrame);
    for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = p.tracksOf(kind);
        for (int i = 0; i < int(tracks.size()); ++i) {
            const int row = TrackListModel::displayRow(p, kind, i);
            if (row < std::min(firstRow, lastRow) || row > std::max(firstRow, lastRow) || tracks[size_t(i)].locked)
                continue;
            for (const Clip& c : tracks[size_t(i)].clips)
                if (c.start < b && c.end() > a)
                    m_selection.insert(c.id);
        }
    }
    m_clips.sync(project(), m_selection);
    emit selectionChanged();
}

QVariantMap ProjectController::selectionSummary() const
{
    const auto& p = project();
    const auto& s = p.settings;
    int video = 0, audio = 0;
    const Clip* first = nullptr;
    for (Id id : m_selection) {
        const Clip* c = p.findClip(id);
        const Track* t = p.trackOfClip(id);
        if (!c || !t)
            continue;
        (t->kind == TrackKind::Video ? video : audio) += 1;
        if (!first || c->start < first->start)
            first = c;
    }
    if (!first)
        return {{"count", 0}};
    const FramePos rel = std::clamp<FramePos>(playheadRel(*first), 0, first->duration - 1);
    return {{"count", video + audio},
            {"video", video},
            {"audio", audio},
            {"opacity", first->transform.opacity.at(rel)},
            {"volume", first->volume.at(rel)},
            {"scale", first->transform.scale.at(rel)},
            {"speed", first->speed},
            {"enabled", first->enabled},
            {"reverse", first->reverse},
            {"fadeIn", toSeconds(s.frameToTime(first->fadeIn))},
            {"fadeOut", toSeconds(s.frameToTime(first->fadeOut))},
            {"audioFadeIn", toSeconds(s.frameToTime(first->audioFadeIn))},
            {"audioFadeOut", toSeconds(s.frameToTime(first->audioFadeOut))}};
}

void ProjectController::editSelection(const QString& what, const QVariant& value)
{
    const std::vector<Id> ids(m_selection.begin(), m_selection.end());
    if (ids.empty())
        return;
    const FramePos playhead = m_player->positionFrame();
    apply(tr("Editar %n clips", nullptr, int(ids.size())), [&](Project& p) {
        bool changed = false;
        const auto& s = p.settings;
        for (Id id : ids) {
            const Clip* c = p.findClip(id);
            const Track* t = p.trackOfClip(id);
            if (!c || !t || t->locked)
                continue;
            const bool videoTrack = t->kind == TrackKind::Video;
            const FramePos rel = std::clamp<FramePos>(playhead - c->start, 0, c->duration - 1);
            const double v = value.toDouble();
            const FramePos frames = s.timeToFrame(TimeUs(std::max(0.0, v) * kUsPerSecond));
            if (what == QLatin1String("opacity") && videoTrack)
                changed |= edit::setProperty(p, id, edit::Property::Opacity, rel, v);
            else if (what == QLatin1String("scale") && videoTrack)
                changed |= edit::setProperty(p, id, edit::Property::Scale, rel, v);
            else if (what == QLatin1String("volume"))
                changed |= edit::setProperty(p, id, edit::Property::Volume, rel, v);
            else if (what == QLatin1String("speed"))
                changed |= edit::setClipSpeed(p, id, v);
            else if (what == QLatin1String("enabled"))
                changed |= edit::setClipEnabled(p, id, value.toBool());
            else if (what == QLatin1String("reverse"))
                changed |= edit::setReverse(p, id, value.toBool());
            else if (what == QLatin1String("fadeIn") && videoTrack)
                changed |= edit::setFades(p, id, frames, c->fadeOut);
            else if (what == QLatin1String("fadeOut") && videoTrack)
                changed |= edit::setFades(p, id, c->fadeIn, frames);
            else if (what == QLatin1String("audioFadeIn"))
                changed |= edit::setAudioFades(p, id, frames, c->audioFadeOut);
            else if (what == QLatin1String("audioFadeOut"))
                changed |= edit::setAudioFades(p, id, c->audioFadeIn, frames);
        }
        return changed;
    }, QStringLiteral("sel:%1").arg(what));
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

// --- Clips compuestos ---

QStringList ProjectController::sequencePath() const
{
    const Project& p = project();
    auto name = [&](Id seq) {
        return seq == p.mainSequence ? tr("Principal") : QString::fromStdString(p.sequenceName(seq));
    };
    QStringList out;
    for (Id seq : p.sequencePath)
        out.append(name(seq));
    out.append(name(p.activeSequence));
    return out;
}

void ProjectController::createCompound()
{
    if (m_selection.isEmpty())
        return;
    const std::vector<Id> ids(m_selection.begin(), m_selection.end());
    const std::string name = tr("Compuesto %1").arg(project().sequences.size() + 1).toStdString();
    Id created = kNoId;
    if (apply(tr("Crear clip compuesto"), [&](Project& p) { return (created = edit::createCompound(p, ids, name)) != kNoId; })) {
        m_selection = {created};
        emit selectionChanged();
        refresh();
    }
}

void ProjectController::openCompound(qint64 clipId)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c || !c->isCompound())
        return;
    const Id seq = c->sequenceId;
    m_player->pause();
    if (apply(tr("Abrir clip compuesto"), [&](Project& p) { return edit::openSequence(p, seq); })) {
        clearSelection();
        m_player->seekFrame(0);
    }
}

void ProjectController::openSequenceLevel(int level)
{
    const auto& path = project().sequencePath;
    if (level < 0 || size_t(level) >= path.size())
        return;
    const Id seq = path[size_t(level)];
    m_player->pause();
    if (apply(tr("Volver a la secuencia"), [&](Project& p) { return edit::openSequence(p, seq); }))
        clearSelection();
}

void ProjectController::breakCompound(qint64 clipId)
{
    std::vector<Id> created;
    if (apply(tr("Deshacer clip compuesto"), [&](Project& p) { return !(created = edit::breakCompound(p, Id(clipId))).empty(); })) {
        m_selection = QSet<Id>(created.begin(), created.end());
        emit selectionChanged();
        refresh();
    } else if (const Clip* c = project().findClip(Id(clipId)); c && c->isCompound() && c->speed != 1.0) {
        emit notice(tr("Para deshacer un clip compuesto, su velocidad debe ser 1×."));
    }
}
