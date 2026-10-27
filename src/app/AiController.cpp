#include "AiController.h"

#include "ai/Tracking.h"
#include "ai/WorkerProtocol.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"
#include "engine/MediaImport.h"
#include "media/AudioFilter.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QDateTime>
#include <QSaveFile>
#include <QStandardPaths>

#include <map>
#include <numbers>
#include <set>

using namespace movix;
using namespace movix::core;

// El audio (reproducción y exportación) usa el modelo de RNNoise si está instalado.
static void publishDenoiseModel()
{
    const ai::ModelInfo* m = ai::findModel(ai::defaultDenoiseModel());
    media::setDenoiseModel(m && ai::isModelInstalled(*m) ? QDir::toNativeSeparators(ai::modelPath(*m)).toStdString() : std::string());
}

AiController::AiController(QObject* parent)
    : QObject(parent)
{
    connect(&m_downloader, &ai::ModelDownloader::progress, this, [this](const QString& id, qint64 received, qint64 total) {
        const ai::ModelInfo* m = ai::findModel(id);
        const double p = total > 0 ? double(received) / double(total) : -1.0;
        setState(m_task, tr("Descargando %1… %2 de %3 MB").arg(m ? m->name : id).arg(received / 1e6, 0, 'f', 0).arg(total / 1e6, 0, 'f', 0),
                 p);
    });
    connect(&m_downloader, &ai::ModelDownloader::finished, this, &AiController::onDownloadFinished);
    connect(this, &AiController::modelsChanged, this, &publishDenoiseModel);
    publishDenoiseModel();
    connect(&m_worker, &ai::AiWorkerClient::finished, this, &AiController::onJobFinished);
    connect(&m_worker, &ai::AiWorkerClient::failed, this, &AiController::onJobFailed);
    connect(&m_worker, &ai::AiWorkerClient::progress, this, [this](int job, double value, const QString& stage) {
        if (job != m_currentJob)
            return;
        if (m_totalJobs <= 0) { // trabajos de una sola petición
            setState(m_task, m_status, value);
            return;
        }
        const int done = m_totalJobs - int(m_queue.size()) - 1;
        QString what;
        if (stage == QLatin1String("decode"))
            what = tr("Leyendo el audio de «%1»…");
        else if (m_task == Task::Silences)
            what = tr("Buscando la voz en «%1»…");
        else if (m_task == Task::Beats)
            what = tr("Buscando el ritmo de «%1»…");
        else if (m_task == Task::Scenes)
            what = tr("Buscando cambios de escena en «%1»…");
        else
            what = tr("Transcribiendo «%1»…");
        setState(m_task, what.arg(m_current.name) + (m_totalJobs > 1 ? QStringLiteral(" (%1/%2)").arg(done + 1).arg(m_totalJobs) : QString()),
                 (done + value) / m_totalJobs);
    });
}

bool AiController::denoiseAvailable() const
{
    const ai::ModelInfo* m = ai::findModel(ai::defaultDenoiseModel());
    return m && ai::isModelInstalled(*m);
}

AiController::~AiController()
{
    m_worker.shutdown();
}

void AiController::setProject(ProjectController* project)
{
    if (m_project == project)
        return;
    if (m_project)
        m_project->disconnect(this);
    m_project = project;
    if (m_project)
        connect(m_project, &ProjectController::timelineChanged, this, &AiController::rebuildWords);
    rebuildWords();
    emit projectChanged();
}

QVariantList AiController::models() const
{
    QVariantList list;
    for (const ai::ModelInfo& m : ai::modelCatalog()) {
        list.append(QVariantMap{
            {"id", m.id},
            {"name", m.name},
            {"description", m.description},
            {"task", m.task == ai::ModelTask::Transcription ? QStringLiteral("transcription")
                     : m.task == ai::ModelTask::Denoise     ? QStringLiteral("denoise")
                                                            : QStringLiteral("vad")},
            {"sizeMB", double(m.totalSize()) / 1e6},
            {"installed", ai::isModelInstalled(m)},
            {"downloading", m_downloader.busy() && m_downloader.currentModel() == m.id},
            {"license", m.license},
        });
    }
    return list;
}

QString AiController::modelsFolder() const
{
    return QDir::toNativeSeparators(ai::modelsDirectory());
}

void AiController::openModelsFolder() const
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(ai::modelsDirectory()));
}

QVariantList AiController::languages() const
{
    const std::pair<const char*, QString> list[] = {
        {"auto", tr("Detectar automáticamente")}, {"es", tr("Español")}, {"en", tr("Inglés")},   {"pt", tr("Portugués")},
        {"fr", tr("Francés")},                    {"it", tr("Italiano")}, {"de", tr("Alemán")},   {"ca", tr("Catalán")},
        {"gl", tr("Gallego")},                    {"eu", tr("Euskera")},  {"ja", tr("Japonés")},  {"zh", tr("Chino")},
    };
    QVariantList out;
    for (const auto& [code, name] : list)
        out.append(QVariantMap{{"code", QLatin1String(code)}, {"name", name}});
    return out;
}

double AiController::pendingDownloadMB(const QString& modelId) const
{
    double mb = 0;
    for (const QString& id : {modelId, ai::defaultVadModel()})
        if (const ai::ModelInfo* m = ai::findModel(id); m && !ai::isModelInstalled(*m))
            mb += double(m->totalSize()) / 1e6;
    return mb;
}

// --- Estado de la tarea ---

void AiController::setState(Task task, const QString& status, double progress)
{
    m_task = task;
    m_status = status;
    m_progress = progress;
    emit stateChanged();
}

void AiController::finishTask()
{
    m_queue.clear();
    m_downloads.clear();
    m_speech.clear();
    m_canceling = false;
    m_currentJob = -1;
    m_totalJobs = 0;
    setState(Task::None, {}, -1.0);
    emit modelsChanged();
}

void AiController::failTask(const QString& message)
{
    finishTask();
    emit errorOccurred(message);
}

void AiController::cancel()
{
    if (!busy())
        return;
    m_canceling = true;
    if (m_downloader.busy()) {
        m_downloader.cancel(); // onDownloadFinished termina la tarea (ahora o al volver al bucle)
    } else if (m_currentJob >= 0) {
        m_worker.cancel(m_currentJob); // onJobFailed(canceled) termina la tarea
        return;
    }
    if (busy()) {
        finishTask();
        emit notice(tr("Operación cancelada."));
    }
}

// --- Modelos ---

void AiController::downloadModel(const QString& id)
{
    const ai::ModelInfo* m = ai::findModel(id);
    if (!m || busy())
        return;
    setState(Task::Download, tr("Descargando %1…").arg(m->name), 0.0);
    m_downloader.start(*m); // si falla, finished() ya informó
    emit modelsChanged();
}

void AiController::removeModel(const QString& id)
{
    const ai::ModelInfo* m = ai::findModel(id);
    if (!m || (m_downloader.busy() && m_downloader.currentModel() == id))
        return;
    if (!QFile::remove(ai::modelPath(*m)))
        emit errorOccurred(tr("No se pudo eliminar el modelo (¿está en uso?)."));
    emit modelsChanged();
}

void AiController::onDownloadFinished(const QString& modelId, bool ok, const QString& error)
{
    emit modelsChanged();
    if (m_task == Task::None)
        return; // tarea ya cancelada
    if (!ok && m_canceling) {
        finishTask();
        emit notice(tr("Operación cancelada."));
        return;
    }
    if (!ok) {
        failTask(tr("No se pudo descargar el modelo: %1").arg(error));
        return;
    }
    if (m_task == Task::Download) {
        const ai::ModelInfo* m = ai::findModel(modelId);
        finishTask();
        emit notice(tr("Modelo %1 instalado.").arg(m ? m->name : modelId));
    } else {
        continueTask();
    }
}

// --- Tareas sobre los medios de la línea de tiempo ---

std::vector<AiController::MediaJob> AiController::audibleMedia(bool onlyUntranscribed) const
{
    std::vector<MediaJob> jobs;
    if (!m_project)
        return jobs;
    const Project& p = m_project->project();
    std::set<Id> seen;
    for (const AudioSegment& seg : audioSegmentsIn(p, 0, p.durationTime())) {
        if (!seg.media || !seg.media->hasAudio || !seen.insert(seg.media->id).second)
            continue;
        if (onlyUntranscribed && seg.media->transcript)
            continue;
        jobs.push_back({seg.media->id, QString::fromStdString(seg.media->path), QString::fromStdString(seg.media->name)});
    }
    return jobs;
}

bool AiController::startTask(Task task, std::vector<MediaJob> media, QStringList models)
{
    if (task != Task::Separate && task != Task::Matte && task != Task::Upscale && task != Task::SlowMotion && task != Task::Track
        && task != Task::Reframe)
        m_current = {};
    m_queue.assign(media.begin(), media.end());
    m_totalJobs = int(m_queue.size());
    m_downloads.clear();
    for (const QString& id : models)
        if (const ai::ModelInfo* m = ai::findModel(id); m && !ai::isModelInstalled(*m))
            m_downloads.append(id);
    m_speech.clear();
    setState(task, tr("Preparando…"), -1.0);
    continueTask();
    return true;
}

void AiController::continueTask()
{
    if (!busy() || !m_project)
        return;
    // 1. Modelos que falten.
    if (!m_downloads.isEmpty()) {
        const ai::ModelInfo* m = ai::findModel(m_downloads.takeFirst());
        setState(m_task, tr("Descargando %1…").arg(m->name), 0.0);
        m_downloader.start(*m);
        emit modelsChanged();
        return;
    }
    // 2. Medios por procesar, de uno en uno.
    if (!m_queue.empty()) {
        m_current = m_queue.front();
        m_queue.pop_front();
        const ai::ModelInfo* vad = ai::findModel(ai::defaultVadModel());
        QJsonObject request{{"media", m_current.path}};
        if (m_task == Task::Beats || m_task == Task::Scenes) {
            request["type"] = QLatin1String(m_task == Task::Beats ? ai::protocol::kDetectBeats : ai::protocol::kDetectScenes);
            request["from"] = qint64(m_clip.sourceIn);
            request["to"] = qint64(m_clip.sourceIn
                                   + TimeUs(double(m_project->project().settings.frameToTime(m_clip.duration)) * m_clip.speed));
        } else if (m_task == Task::Transcribe) {
            const ai::ModelInfo* model = ai::findModel(m_modelId);
            request["type"] = QLatin1String(ai::protocol::kTranscribe);
            request["model"] = ai::modelPath(*model);
            request["modelName"] = model->id;
            request["vadModel"] = ai::modelPath(*vad);
            request["language"] = m_language;
        } else {
            request["type"] = QLatin1String(ai::protocol::kDetectSpeech);
            request["model"] = ai::modelPath(*vad);
            request["threshold"] = m_vadThreshold;
            request["minSilenceMs"] = std::max(100, m_minSilenceMs / 2); // los silencios cortos los filtra silentRanges
            request["padMs"] = 0;
        }
        m_currentJob = m_worker.submit(request);
        const int done = m_totalJobs - int(m_queue.size()) - 1;
        setState(m_task, tr("Leyendo el audio de «%1»…").arg(m_current.name), double(done) / m_totalJobs);
        return;
    }
    // 3. Trabajos que no recorren medios (texto a voz, traducción, separación): una sola petición.
    if ((m_task == Task::Speak || m_task == Task::Translate || m_task == Task::Separate || m_task == Task::Matte
         || m_task == Task::Upscale || m_task == Task::SlowMotion || m_task == Task::Track || m_task == Task::Reframe)
        && !m_request.isEmpty()) {
        m_currentJob = m_worker.submit(m_request);
        m_request = {};
        setState(m_task, m_task == Task::Speak       ? tr("Generando la voz…")
                         : m_task == Task::Translate ? tr("Traduciendo los subtítulos…")
                         : m_task == Task::Matte     ? tr("Recortando el fondo de «%1»…").arg(m_current.name)
                         : m_task == Task::Upscale   ? tr("Aumentando la resolución de «%1»…").arg(m_current.name)
                         : m_task == Task::SlowMotion ? tr("Creando fotogramas intermedios de «%1»…").arg(m_current.name)
                         : m_task == Task::Track      ? tr("Siguiendo el movimiento en «%1»…").arg(m_current.name)
                         : m_task == Task::Reframe    ? tr("Buscando el sujeto de «%1»…").arg(m_current.name)
                                                     : tr("Separando la voz de la música…"),
                 0.0);
        return;
    }
    // 4. Resultado sobre la línea de tiempo.
    if (m_task == Task::Transcribe)
        completeTranscription();
    else if (m_task == Task::Silences)
        completeSilences();
    else
        finishTask(); // el medio cambió durante el análisis: no hay nada que aplicar
}

void AiController::onJobFinished(int job, const QJsonObject& result)
{
    if (job != m_currentJob || !m_project)
        return;
    m_currentJob = -1;
    // El medio puede haberse quitado o cambiado mientras tanto.
    const MediaItem* m = m_project->project().findMedia(m_current.mediaId);
    const bool sameMedia = m && QString::fromStdString(m->path) == m_current.path;
    if (m_task == Task::Transcribe && sameMedia) {
        auto transcript = std::make_shared<const Transcript>(transcriptFromJson(result["transcript"].toObject()));
        m_project->applyEdit(tr("Transcribir «%1»").arg(m_current.name), [&](Project& p) {
            return edit::setTranscript(p, m_current.mediaId, transcript);
        });
    } else if (m_task == Task::Speak) {
        const QString path = result["output"].toString();
        core::Id clip = kNoId;
        m_project->applyEdit(tr("Texto a voz"), [&](Project& p) {
            clip = addGeneratedAudio(p, path, m_atFrame, 1.0, tr("Voz").toStdString());
            return clip != kNoId;
        });
        finishTask();
        if (clip == kNoId)
            emit errorOccurred(tr("No se pudo añadir el audio generado."));
        else
            emit notice(tr("Voz generada (%1 s).").arg(toSeconds(result["duration"].toInteger()), 0, 'f', 1));
        return;
    } else if (m_task == Task::Translate) {
        const QJsonArray texts = result["texts"].toArray();
        int created = 0;
        m_project->applyEdit(tr("Traducir subtítulos"), [&](Project& p) {
            const Track* source = p.findTrack(m_sourceTrack);
            if (!source)
                return false;
            std::vector<Clip> titles;
            for (const Clip& c : source->clips)
                if (c.title && c.enabled)
                    titles.push_back(c);
            // addTrack puede reubicar las pistas: `source` deja de ser válido.
            const std::string name = source->name + " (" + m_trackSuffix.toStdString() + ")";
            const Id track = edit::addTrack(p, TrackKind::Video);
            p.findTrack(track)->name = name;
            for (size_t i = 0; i < titles.size() && i < size_t(texts.size()); ++i) {
                TitleStyle style = *titles[i].title;
                style.text = balanceLines(texts.at(qsizetype(i)).toString().toStdString(), 42, 2);
                const Id id = edit::placeTitle(p, track, titles[i].start, titles[i].duration, style);
                if (Clip* c = p.findClip(id)) {
                    c->transform = titles[i].transform;
                    ++created;
                }
            }
            // La pista original queda oculta: se ve la traducción.
            edit::setTrackHidden(p, m_sourceTrack, true);
            return created > 0;
        });
        finishTask();
        emit notice(tr("Se tradujeron %n subtítulos.", nullptr, created));
        return;
    } else if (m_task == Task::Separate && sameMedia) {
        const QString vocals = result["vocals"].toString(), music = result["accompaniment"].toString();
        bool ok = false;
        m_project->applyEdit(tr("Separar voz y música"), [&](Project& p) {
            const Id v = addGeneratedAudio(p, vocals, m_clip.start, m_clip.speed, tr("Voz").toStdString());
            const Id a = addGeneratedAudio(p, music, m_clip.start, m_clip.speed, tr("Música").toStdString());
            if (v == kNoId || a == kNoId)
                return false;
            // Mismo tramo que el original; el audio original deja de sonar.
            for (Id id : {v, a})
                if (Clip* c = p.findClip(id))
                    c->duration = std::min(c->duration, m_clip.duration);
            auto loc = p.locateClip(m_clip.clipId);
            if (Clip* c = loc.clip()) {
                if (loc.track->kind == TrackKind::Video)
                    c->audioDetached = true;
                else
                    c->enabled = false;
            }
            ok = true;
            return true;
        });
        finishTask();
        if (ok)
            emit notice(tr("Voz y música separadas en dos pistas nuevas."));
        else
            emit errorOccurred(tr("No se pudieron añadir las pistas separadas."));
        return;
    } else if (m_task == Task::Track && sameMedia) {
        const QJsonArray points = result["points"].toArray();
        int keys = 0;
        m_project->applyEdit(tr("Seguir el movimiento"), [&](Project& p) {
            Clip* tracked = p.findClip(m_clip.clipId);
            if (!tracked || tracked->masks.empty() || points.isEmpty())
                return false;
            const ProjectSettings& s = p.settings;
            const double W = s.width, H = s.height;
            // Ajuste de la capa seguida al lienzo, para llevar el movimiento a los clips adjuntos.
            const MediaItem* m = p.findMedia(tracked->mediaId);
            const double layerAspect = m && m->height ? double(m->width) / m->height : 16.0 / 9.0;
            const double canvasAspect = W / H;
            const double fitW = std::min(1.0, layerAspect / canvasAspect), fitH = std::min(1.0, canvasAspect / layerAspect);
            // Punto (cx, cy) del medio en píxeles del lienzo (desde su centro) en el fotograma `rel` de
            // la capa seguida, con su posición, escala y giro.
            auto onCanvas = [&](const Clip& c, FramePos rel, double cx, double cy) {
                const double sc = c.transform.scale.at(rel), r = c.transform.rotation.at(rel) * std::numbers::pi / 180.0;
                const double lx = (cx - 0.5) * fitW * W * sc, ly = (cy - 0.5) * fitH * H * sc;
                return std::pair{c.transform.x.at(rel) * W + lx * std::cos(r) - ly * std::sin(r),
                                 c.transform.y.at(rel) * H + lx * std::sin(r) + ly * std::cos(r)};
            };
            const QJsonArray first = points.at(0).toArray();
            const auto [ox, oy] = onCanvas(*tracked, m_trackStartRel, first.at(1).toDouble(), first.at(2).toDouble());
            const Mask& mask = tracked->masks.front();
            const double maskW = mask.width.at(m_trackStartRel), maskH = mask.height.at(m_trackStartRel);
            const double maskRotation = mask.rotation.at(m_trackStartRel);
            // Los clips adjuntos guardan su separación del objeto, que crece y gira con él.
            struct Attached {
                double dx, dy, scale, rotation;
            };
            std::map<Id, Attached> attached;
            for (Id id : m_clip.targets)
                if (const Clip* c = p.findClip(id); c && id != m_clip.clipId) {
                    const FramePos rel = m_trackStartRel + m_clip.start - c->start;
                    attached[id] = {c->transform.x.at(rel) * W - ox, c->transform.y.at(rel) * H - oy, c->transform.scale.at(rel),
                                    c->transform.rotation.at(rel)};
                }
            FramePos lastRel = -1;
            for (const QJsonValue& v : points) {
                const QJsonArray a = v.toArray();
                const FramePos rel = toTimeline(a.at(0).toInteger()) - m_clip.start;
                if (rel < 0 || rel >= m_clip.duration || rel == lastRel)
                    continue;
                lastRel = rel;
                const double cx = a.at(1).toDouble(), cy = a.at(2).toDouble();
                const double scale = m_trackScaleRotation ? a.at(4).toDouble(1.0) : 1.0;
                const double angle = m_trackScaleRotation ? a.at(5).toDouble(0.0) : 0.0;
                Clip* c = p.findClip(m_clip.clipId);
                std::vector<std::pair<edit::Property, double>> values{{edit::Property::MaskX, cx}, {edit::Property::MaskY, cy}};
                if (m_trackScaleRotation)
                    values.insert(values.end(), {{edit::Property::MaskWidth, maskW * scale},
                                                 {edit::Property::MaskHeight, maskH * scale},
                                                 {edit::Property::MaskRotation, maskRotation + angle}});
                for (auto [prop, value] : values)
                    edit::property(*c, prop, 0)->setKey(rel, value, Interpolation::Linear);
                const auto [px, py] = onCanvas(*c, rel, cx, cy);
                const double rad = angle * std::numbers::pi / 180.0;
                for (const auto& [id, base] : attached) {
                    Clip* clip = p.findClip(id);
                    const FramePos crel = rel + m_clip.start - clip->start;
                    if (crel < 0 || crel >= clip->duration)
                        continue;
                    const double dx = scale * (base.dx * std::cos(rad) - base.dy * std::sin(rad));
                    const double dy = scale * (base.dx * std::sin(rad) + base.dy * std::cos(rad));
                    clip->transform.x.setKey(crel, (px + dx) / W, Interpolation::Linear);
                    clip->transform.y.setKey(crel, (py + dy) / H, Interpolation::Linear);
                    if (m_trackScaleRotation) {
                        clip->transform.scale.setKey(crel, base.scale * scale, Interpolation::Linear);
                        clip->transform.rotation.setKey(crel, base.rotation + angle, Interpolation::Linear);
                    }
                }
                ++keys;
            }
            return keys > 0;
        });
        finishTask();
        emit notice(tr("Movimiento seguido en %n fotogramas.", nullptr, keys));
        return;
    } else if (m_task == Task::Reframe && sameMedia) {
        const QJsonArray points = result["points"].toArray();
        bool ok = false;
        m_project->applyEdit(tr("Reencuadre automático"), [&](Project& p) {
            // Nuevo formato ("ancho:alto"): se conserva el lado corto del proyecto.
            const QStringList ratio = m_aspect.split(QLatin1Char(':'));
            const int rw = ratio.size() == 2 ? std::max(1, ratio[0].toInt()) : 1, rh = ratio.size() == 2 ? std::max(1, ratio[1].toInt()) : 1;
            const int shortSide = std::min(p.settings.width, p.settings.height);
            int w = shortSide, h = shortSide;
            if (rw > rh)
                w = (shortSide * rw / rh) & ~1;
            else
                h = (shortSide * rh / rw) & ~1;
            edit::setResolution(p, w, h);
            Clip* c = p.findClip(m_clip.clipId);
            const MediaItem* m = c ? p.findMedia(c->mediaId) : nullptr;
            if (!c || !m || !m->width || !m->height)
                return false;
            std::vector<ai::TrackPoint> subject;
            for (const QJsonValue& v : points) {
                const QJsonArray a = v.toArray();
                ai::TrackPoint s{a.at(0).toInteger(), a.at(1).toDouble(0.5), a.at(2).toDouble(0.5), 1.0f};
                s.width = a.at(6).toDouble(0.0);
                s.height = a.at(7).toDouble(0.0);
                subject.push_back(s);
            }
            const auto keys = ai::planReframe(subject, double(m->width) / m->height, double(w) / h, m_reframeZoom);
            if (keys.empty())
                return false;
            Transform& t = c->transform;
            t.scale = Animated(keys.front().scale);
            t.x = Animated(0.0);
            t.y = Animated(0.0);
            for (const ai::FramingKey& k : keys) {
                const FramePos rel = toTimeline(k.time) - c->start;
                if (rel < 0 || rel >= c->duration)
                    continue;
                t.x.setKey(rel, k.x, Interpolation::Smooth);
                t.y.setKey(rel, k.y, Interpolation::Smooth);
                if (m_reframeZoom)
                    t.scale.setKey(rel, k.scale, Interpolation::Smooth);
            }
            // Un eje en el que no hay nada que desplazar (la capa ya llena el lienzo) queda fijo.
            for (Animated* a : {&t.x, &t.y, &t.scale})
                if (std::all_of(a->keys.begin(), a->keys.end(), [&](const Keyframe& k) { return std::abs(k.value - a->keys.front().value) < 1e-6; })) {
                    const double v = a->keys.empty() ? a->value : a->keys.front().value;
                    *a = Animated(v);
                }
            ok = true;
            return true;
        });
        finishTask();
        if (ok)
            emit notice(tr("Proyecto reencuadrado a %1.").arg(m_aspect));
        else
            emit errorOccurred(tr("No se pudo reencuadrar el clip."));
        return;
    } else if ((m_task == Task::Upscale || m_task == Task::SlowMotion) && sameMedia) {
        const QString path = result["output"].toString();
        bool ok = false;
        m_project->applyEdit(m_task == Task::Upscale ? tr("Aumentar la resolución") : tr("Cámara lenta suave"), [&](Project& p) {
            std::string err;
            const auto item = engine::probeMediaItem(QDir::toNativeSeparators(path).toStdString(), &err);
            if (!item)
                return false;
            // El video nuevo no tiene sonido: el audio original se separa antes a su pista.
            auto loc = p.locateClip(m_clip.clipId);
            if (!loc.clip())
                return false;
            const MediaItem* old = p.findMedia(loc.clip()->mediaId);
            if (old && old->hasAudio && loc.track->kind == TrackKind::Video && !loc.clip()->audioDetached)
                edit::detachAudio(p, m_clip.clipId);
            const Id media = edit::addMedia(p, *item);
            if (!edit::replaceClipMedia(p, m_clip.clipId, media, 0))
                return false;
            if (m_task == Task::SlowMotion)
                edit::setClipSpeed(p, m_clip.clipId, m_clip.speed / m_factor);
            ok = true;
            return true;
        });
        const Task task = m_task;
        finishTask();
        if (!ok)
            emit errorOccurred(tr("No se pudo usar el video generado."));
        else if (task == Task::Upscale)
            emit notice(tr("Resolución aumentada a %1 × %2.").arg(result["width"].toInt()).arg(result["height"].toInt()));
        else
            emit notice(tr("Cámara lenta suave a %1 fps.").arg(result["fps"].toDouble(), 0, 'f', 2));
        return;
    } else if (m_task == Task::Matte && sameMedia) {
        const AiMatte matte{QDir::toNativeSeparators(result["output"].toString()).toStdString(), result["start"].toInteger(), false};
        bool ok = false;
        m_project->applyEdit(tr("Recortar el fondo"), [&](Project& p) { return ok = edit::setMatte(p, m_clip.clipId, matte); });
        finishTask();
        if (ok)
            emit notice(tr("Fondo recortado (%n fotogramas).", nullptr, result["frames"].toInt()));
        else
            emit errorOccurred(tr("El clip ya no existe."));
        return;
    } else if (m_task == Task::Beats && sameMedia) {
        std::vector<FramePos> frames;
        for (const QJsonValue& v : result["beats"].toArray()) {
            const FramePos f = toTimeline(v.toInteger());
            if (f >= m_clip.start && f < m_clip.start + m_clip.duration)
                frames.push_back(f);
        }
        const QString label = tr("Pulso");
        int added = 0;
        m_project->applyEdit(tr("Marcar pulsos"), [&](Project& p) {
            const bool cleared = edit::clearMarkers(p, label.toStdString());
            added = edit::addMarkers(p, frames, label.toStdString(), 0xFF4FC3F7);
            return cleared || added > 0;
        });
        const double bpm = result["bpm"].toDouble();
        finishTask();
        if (result["variableTempo"].toBool())
            emit notice(tr("%n pulsos marcados (tempo variable, en torno a %1 BPM).", nullptr, added).arg(bpm, 0, 'f', 0));
        else
            emit notice(tr("%n pulsos marcados (%1 BPM).", nullptr, added).arg(bpm, 0, 'f', 1));
        return;
    } else if (m_task == Task::Scenes && sameMedia) {
        std::vector<FramePos> frames;
        for (const QJsonValue& v : result["cuts"].toArray()) {
            const FramePos f = toTimeline(v.toInteger());
            if (f > m_clip.start && f < m_clip.start + m_clip.duration)
                frames.push_back(f);
        }
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        // De derecha a izquierda: la parte izquierda conserva el id y sigue conteniendo los cortes anteriores.
        int pieces = 0;
        m_project->applyEdit(tr("Dividir en escenas"), [&](Project& p) {
            for (auto it = frames.rbegin(); it != frames.rend(); ++it)
                pieces += int(edit::splitAt(p, *it, m_clip.targets).size());
            return pieces > 0;
        });
        finishTask();
        emit notice(frames.empty() ? tr("No se encontraron cambios de escena en el clip.")
                                   : tr("Se encontraron %n cambios de escena.", nullptr, int(frames.size())));
        return;
    } else if (m_task == Task::Silences && sameMedia) {
        std::vector<SpeechRange>& spans = m_speech[m_current.mediaId];
        for (const QJsonValue& v : result["speech"].toArray()) {
            const QJsonArray a = v.toArray();
            spans.push_back({a.at(0).toInteger(), a.at(1).toInteger()});
        }
    }
    continueTask();
}

void AiController::onJobFailed(int job, const QString& error, bool canceled)
{
    if (job != m_currentJob)
        return;
    m_currentJob = -1;
    if (canceled) {
        finishTask();
        emit notice(tr("Operación cancelada."));
        return;
    }
    failTask(m_current.name.isEmpty() ? tr("La tarea de IA falló: %1").arg(error)
                                      : tr("No se pudo analizar «%1»: %2").arg(m_current.name, error));
}

// --- Transcripción y subtítulos ---

void AiController::transcribe(const QString& modelId, const QString& language, bool retranscribe, bool createTrack,
                              int maxCharsPerLine, int maxLines)
{
    if (busy() || !m_project)
        return;
    const ai::ModelInfo* model = ai::findModel(modelId);
    if (!model || model->task != ai::ModelTask::Transcription) {
        emit errorOccurred(tr("Modelo de transcripción desconocido."));
        return;
    }
    if (audibleMedia(false).empty()) {
        emit errorOccurred(tr("No hay audio en la línea de tiempo para transcribir."));
        return;
    }
    m_modelId = modelId;
    m_language = language.isEmpty() ? QStringLiteral("auto") : language;
    m_createTrack = createTrack;
    m_maxChars = std::clamp(maxCharsPerLine, 16, 80);
    m_maxLines = std::clamp(maxLines, 1, 3);
    startTask(Task::Transcribe, audibleMedia(!retranscribe), {modelId, ai::defaultVadModel()});
}

void AiController::completeTranscription()
{
    if (!m_createTrack) {
        finishTask();
        emit notice(m_words.empty() ? tr("No se reconoció voz en la línea de tiempo.")
                                    : tr("Transcripción lista: %n palabras.", nullptr, int(m_words.size())));
        return;
    }
    CueOptions options;
    options.maxCharsPerLine = m_maxChars;
    options.maxLines = m_maxLines;
    const auto cues = makeCues(timelineWords(m_project->project()), options);
    finishTask();
    if (cues.empty()) {
        emit notice(tr("No se reconoció voz en la línea de tiempo."));
        return;
    }
    const std::string name = tr("Subtítulos").toStdString();
    m_project->applyEdit(tr("Generar subtítulos"), [&](Project& p) {
        return edit::addSubtitleTrack(p, cues, subtitleStyle(), name) != kNoId;
    });
    emit notice(tr("Se crearon %n subtítulos.", nullptr, int(cues.size())));
}

// --- Silencios ---

void AiController::removeSilences(double threshold, int minSilenceMs, int paddingMs)
{
    if (busy() || !m_project)
        return;
    auto media = audibleMedia(false);
    if (media.empty()) {
        emit errorOccurred(tr("No hay audio en la línea de tiempo."));
        return;
    }
    m_vadThreshold = std::clamp(threshold, 0.05, 0.95);
    m_minSilenceMs = std::clamp(minSilenceMs, 100, 10000);
    m_paddingMs = std::clamp(paddingMs, 0, 2000);
    startTask(Task::Silences, std::move(media), {ai::defaultVadModel()});
}

void AiController::completeSilences()
{
    const auto ranges = silentRanges(m_project->project(), m_speech, TimeUs(m_minSilenceMs) * 1000, TimeUs(m_paddingMs) * 1000);
    finishTask();
    if (ranges.empty()) {
        emit notice(tr("No se encontraron silencios que eliminar."));
        return;
    }
    FramePos removed = 0;
    m_project->applyEdit(tr("Eliminar silencios"), [&](Project& p) { return (removed = edit::removeRanges(p, ranges)) > 0; });
    const double seconds = toSeconds(m_project->project().settings.frameToTime(removed));
    emit notice(tr("Se eliminaron %n silencios (%1 s).", nullptr, int(ranges.size())).arg(seconds, 0, 'f', 1));
}

// --- Pulsos y escenas ---

std::optional<AiController::ClipContext> AiController::selectedClip(
    const std::function<bool(const Clip&, const MediaItem&)>& accept) const
{
    if (!m_project)
        return std::nullopt;
    const Project& p = m_project->project();
    // En el orden de la línea de tiempo, para que el resultado no dependa del orden de selección.
    std::vector<const Clip*> selected;
    for (Id id : m_project->selectedClips())
        if (const Clip* c = p.findClip(id))
            selected.push_back(c);
    std::sort(selected.begin(), selected.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
    for (const Clip* c : selected) {
        const MediaItem* m = c->title || c->isCompound() ? nullptr : p.findMedia(c->mediaId);
        if (m && accept(*c, *m)) {
            ClipContext ctx{c->id, c->start, c->duration, c->sourceIn, c->speed, {}};
            for (const Clip* other : selected)
                ctx.targets.push_back(other->id);
            return ctx;
        }
    }
    return std::nullopt;
}

FramePos AiController::toTimeline(TimeUs source) const
{
    const ProjectSettings& s = m_project->project().settings;
    const TimeUs t = s.frameToTime(m_clip.start) + TimeUs(std::llround(double(source - m_clip.sourceIn) / m_clip.speed));
    return s.timeToFrame(t + s.frameDuration() / 2);
}

void AiController::markBeats()
{
    if (busy() || !m_project)
        return;
    const auto ctx = selectedClip([](const Clip&, const MediaItem& m) { return m.hasAudio; });
    if (!ctx) {
        emit errorOccurred(tr("Selecciona en la línea de tiempo un clip con música."));
        return;
    }
    m_clip = *ctx;
    const MediaItem* m = m_project->project().findMedia(m_project->project().findClip(ctx->clipId)->mediaId);
    startTask(Task::Beats, {{m->id, QString::fromStdString(m->path), QString::fromStdString(m->name)}}, {});
}

void AiController::splitScenes()
{
    if (busy() || !m_project)
        return;
    const auto ctx = selectedClip([](const Clip&, const MediaItem& m) { return m.hasVideo && !m.isStill; });
    if (!ctx) {
        emit errorOccurred(tr("Selecciona en la línea de tiempo un clip de video."));
        return;
    }
    m_clip = *ctx;
    const MediaItem* m = m_project->project().findMedia(m_project->project().findClip(ctx->clipId)->mediaId);
    startTask(Task::Scenes, {{m->id, QString::fromStdString(m->path), QString::fromStdString(m->name)}}, {});
}

// --- Texto a voz, traducción y separación ---

QString AiController::generatedFolder() const
{
    QString dir;
    if (m_project && m_project->fileUrl().isLocalFile()) {
        const QFileInfo file(m_project->fileUrl().toLocalFile());
        dir = file.absolutePath() + QLatin1Char('/') + file.completeBaseName() + QStringLiteral(" - IA");
    } else {
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/MovixStudio/generado");
    }
    QDir().mkpath(dir);
    return dir;
}

Id AiController::addGeneratedAudio(Project& p, const QString& path, FramePos frame, double speed, const std::string& trackName)
{
    std::string err;
    const auto item = engine::probeMediaItem(QDir::toNativeSeparators(path).toStdString(), &err);
    if (!item)
        return kNoId;
    const Id media = edit::addMedia(p, *item);
    const MediaItem* m = p.findMedia(media);
    const FramePos length = std::max<FramePos>(1, edit::availableFrames(p, *m, 0, speed));
    // Primera pista de audio sin nada en el tramo; si no hay, una nueva.
    Id track = kNoId;
    for (const Track& t : p.audioTracks) {
        const bool free = !t.locked && std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) {
            return c.start < frame + length && c.end() > frame;
        });
        if (free) {
            track = t.id;
            break;
        }
    }
    if (track == kNoId) {
        track = edit::addTrack(p, TrackKind::Audio);
        p.findTrack(track)->name = trackName;
    }
    const Id clip = edit::placeClip(p, track, media, frame, 0, length);
    if (Clip* c = p.findClip(clip); c && speed != 1.0)
        edit::setClipSpeed(p, clip, speed);
    return clip;
}

QVariantList AiController::voices() const
{
    const std::pair<const char*, QString> names[] = {
        {"ef_dora", tr("Dora (español, mujer)")},      {"em_alex", tr("Álex (español, hombre)")},
        {"em_santa", tr("Santa (español, hombre)")},   {"af_heart", tr("Heart (inglés, mujer)")},
        {"am_michael", tr("Michael (inglés, hombre)")},
    };
    QVariantList list;
    for (const auto& [id, name] : names)
        list.append(QVariantMap{{"id", QLatin1String(id)}, {"name", name}});
    return list;
}

void AiController::speak(const QString& text, const QString& voice, double speed)
{
    if (busy() || !m_project)
        return;
    if (text.trimmed().isEmpty()) {
        emit errorOccurred(tr("Escribe el texto que quieres convertir en voz."));
        return;
    }
    const ai::ModelInfo* model = ai::findModel(ai::speechModel());
    const QString output = generatedFolder() + QStringLiteral("/voz-%1.wav").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    m_atFrame = m_project->player()->positionFrame();
    m_request = QJsonObject{{"type", QLatin1String(ai::protocol::kSynthesize)},
                            {"model", ai::modelFolder(*model)},
                            {"text", text},
                            {"voice", voice},
                            {"speed", speed},
                            {"output", QDir::toNativeSeparators(output)}};
    startTask(Task::Speak, {}, {model->id});
}

void AiController::translateSubtitles(const QString& from, const QString& to)
{
    if (busy() || !m_project)
        return;
    const QString modelId = ai::translationModel(from, to);
    const Track* track = exportTrack();
    if (modelId.isEmpty() || !track) {
        emit errorOccurred(modelId.isEmpty() ? tr("No hay modelo de traducción para ese par de idiomas.")
                                             : tr("No hay ninguna pista con subtítulos que traducir."));
        return;
    }
    QJsonArray texts;
    for (const Clip& c : track->clips)
        if (c.title && c.enabled)
            texts.append(QString::fromStdString(c.title->text));
    m_sourceTrack = track->id;
    m_trackSuffix = to.toUpper();
    const ai::ModelInfo* model = ai::findModel(modelId);
    m_request = QJsonObject{{"type", QLatin1String(ai::protocol::kTranslate)}, {"model", ai::modelFolder(*model)}, {"texts", texts}};
    startTask(Task::Translate, {}, {modelId});
}

void AiController::separateVoice()
{
    if (busy() || !m_project)
        return;
    const auto ctx = selectedClip([](const Clip& c, const MediaItem& m) { return m.hasAudio && !c.audioDetached; });
    if (!ctx) {
        emit errorOccurred(tr("Selecciona en la línea de tiempo un clip con audio."));
        return;
    }
    m_clip = *ctx;
    const Project& p = m_project->project();
    const MediaItem* media = p.findMedia(p.findClip(ctx->clipId)->mediaId);
    const ai::ModelInfo* model = ai::findModel(ai::separationModel());
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = generatedFolder() + QLatin1Char('/') + QFileInfo(QString::fromStdString(media->path)).completeBaseName();
    m_request = QJsonObject{
        {"type", QLatin1String(ai::protocol::kSeparate)},
        {"model", ai::modelPath(*model)},
        {"media", QString::fromStdString(media->path)},
        {"from", qint64(ctx->sourceIn)},
        {"to", qint64(ctx->sourceIn + TimeUs(double(p.settings.frameToTime(ctx->duration)) * ctx->speed))},
        {"vocals", QDir::toNativeSeparators(base + QStringLiteral(" - voz %1.wav").arg(stamp))},
        {"accompaniment", QDir::toNativeSeparators(base + QStringLiteral(" - música %1.wav").arg(stamp))},
    };
    // Se trata como trabajo de una petición (sin cola de medios), pero se comprueba el medio al final.
    m_current = {media->id, QString::fromStdString(media->path), QString::fromStdString(media->name)};
    startTask(Task::Separate, {}, {model->id});
}

// --- Recorte de fondo ---

void AiController::removeBackground(const QString& kind)
{
    if (busy() || !m_project)
        return;
    const auto ctx = selectedClip([&](const Clip&, const MediaItem& m) { return m.hasVideo && !m.isStill; });
    if (!ctx || !m_project->project().trackOfClip(ctx->clipId) || m_project->project().trackOfClip(ctx->clipId)->kind != TrackKind::Video) {
        emit errorOccurred(tr("Selecciona en la línea de tiempo un clip de video."));
        return;
    }
    m_clip = *ctx;
    const Project& p = m_project->project();
    const MediaItem* media = p.findMedia(p.findClip(ctx->clipId)->mediaId);
    const bool general = kind == QLatin1String("general");
    const ai::ModelInfo* model = ai::findModel(general ? ai::generalMatteModel() : ai::portraitMatteModel());
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString output = generatedFolder() + QLatin1Char('/') + QFileInfo(QString::fromStdString(media->path)).completeBaseName()
        + QStringLiteral(" - recorte %1.mp4").arg(stamp);
    m_request = QJsonObject{
        {"type", QLatin1String(ai::protocol::kRemoveBackground)},
        {"model", ai::modelPath(*model)},
        {"kind", general ? QStringLiteral("general") : QStringLiteral("portrait")},
        {"media", QString::fromStdString(media->path)},
        {"from", qint64(ctx->sourceIn)},
        {"to", qint64(ctx->sourceIn + TimeUs(double(p.settings.frameToTime(ctx->duration)) * ctx->speed))},
        {"output", QDir::toNativeSeparators(output)},
    };
    m_current = {media->id, QString::fromStdString(media->path), QString::fromStdString(media->name)};
    startTask(Task::Matte, {}, {model->id});
}

void AiController::segmentObject(double u, double v)
{
    if (busy() || !m_project)
        return;
    const FramePos playhead = m_project->player()->positionFrame();
    const auto ctx = selectedClip([&](const Clip& c, const MediaItem& m) {
        return m.hasVideo && !m.isStill && playhead >= c.start && playhead < c.end();
    });
    if (!ctx) {
        emit errorOccurred(tr("Selecciona un clip de video y pon el cabezal sobre él."));
        return;
    }
    m_clip = *ctx;
    const Project& p = m_project->project();
    const Clip* c = p.findClip(ctx->clipId);
    const MediaItem* media = p.findMedia(c->mediaId);
    const ai::ModelInfo* model = ai::findModel(ai::segmentationModel());
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    m_request = QJsonObject{
        {"type", QLatin1String(ai::protocol::kSegmentObject)},
        {"model", ai::modelFolder(*model)},
        {"media", QString::fromStdString(media->path)},
        // Todo el clip: desde el clic hacia delante y hacia atrás (al revés, el principio del
        // clip es el final del tramo del medio).
        {"from", qint64(std::min(sourceTimeAt(p.settings, *c, c->start), sourceTimeAt(p.settings, *c, c->end())))},
        {"to", qint64(std::max(sourceTimeAt(p.settings, *c, c->start), sourceTimeAt(p.settings, *c, c->end())))},
        {"at", qint64(sourceTimeAt(p.settings, *c, playhead))},
        {"x", std::clamp(u, 0.0, 1.0)},
        {"y", std::clamp(v, 0.0, 1.0)},
        {"output", QDir::toNativeSeparators(generatedFolder() + QLatin1Char('/')
                                            + QFileInfo(QString::fromStdString(media->path)).completeBaseName()
                                            + QStringLiteral(" - objeto %1.mp4").arg(stamp))},
    };
    m_current = {media->id, QString::fromStdString(media->path), QString::fromStdString(media->name)};
    startTask(Task::Matte, {}, {model->id});
}

// --- Escalado y cámara lenta ---

void AiController::startVideoJob(Task task, const QString& modelId, const char* type, const QString& suffix, int factor)
{
    if (busy() || !m_project)
        return;
    const auto ctx = selectedClip([&](const Clip&, const MediaItem& m) { return m.hasVideo && !m.isStill; });
    if (!ctx) {
        emit errorOccurred(tr("Selecciona en la línea de tiempo un clip de video."));
        return;
    }
    m_clip = *ctx;
    m_factor = factor;
    const Project& p = m_project->project();
    const MediaItem* media = p.findMedia(p.findClip(ctx->clipId)->mediaId);
    const ai::ModelInfo* model = ai::findModel(modelId);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString output = generatedFolder() + QLatin1Char('/') + QFileInfo(QString::fromStdString(media->path)).completeBaseName()
        + QStringLiteral(" - %1 %2.mp4").arg(suffix, stamp);
    m_request = QJsonObject{
        {"type", QLatin1String(type)},
        {"model", ai::modelPath(*model)},
        {"media", QString::fromStdString(media->path)},
        {"from", qint64(ctx->sourceIn)},
        {"to", qint64(ctx->sourceIn + TimeUs(double(p.settings.frameToTime(ctx->duration)) * ctx->speed))},
        {"scale", factor},
        {"factor", factor},
        {"output", QDir::toNativeSeparators(output)},
    };
    m_current = {media->id, QString::fromStdString(media->path), QString::fromStdString(media->name)};
    startTask(task, {}, {model->id});
}

void AiController::upscale(int scale)
{
    startVideoJob(Task::Upscale, ai::upscaleModel(), ai::protocol::kUpscale, QStringLiteral("x%1").arg(scale), scale == 4 ? 4 : 2);
}

void AiController::slowMotion(int factor)
{
    factor = std::clamp(factor, 2, 8);
    startVideoJob(Task::SlowMotion, ai::interpolationModel(), ai::protocol::kInterpolate, QStringLiteral("lento x%1").arg(factor), factor);
}

// --- Seguimiento y reencuadre ---

void AiController::trackMotion(bool scaleRotation)
{
    if (busy() || !m_project)
        return;
    const FramePos playhead = m_project->player()->positionFrame();
    const auto ctx = selectedClip([&](const Clip& c, const MediaItem& m) {
        return m.hasVideo && !m.isStill && !c.masks.empty() && playhead >= c.start && playhead < c.end() - 1;
    });
    if (!ctx) {
        emit errorOccurred(tr("Selecciona un clip de video con una máscara (la región que se seguirá) y pon el cabezal sobre él."));
        return;
    }
    m_clip = *ctx;
    const Project& p = m_project->project();
    const Clip* c = p.findClip(ctx->clipId);
    const MediaItem* media = p.findMedia(c->mediaId);
    m_trackStartRel = playhead - c->start;
    const Mask& mask = c->masks.front();
    const double w = std::clamp(mask.width.at(m_trackStartRel), 0.02, 1.0), h = std::clamp(mask.height.at(m_trackStartRel), 0.02, 1.0);
    m_request = QJsonObject{
        {"type", QLatin1String(ai::protocol::kTrack)},
        {"media", QString::fromStdString(media->path)},
        {"from", qint64(sourceTimeAt(p.settings, *c, playhead))},
        {"to", qint64(sourceTimeAt(p.settings, *c, c->end()))},
        {"region", QJsonArray{mask.x.at(m_trackStartRel), mask.y.at(m_trackStartRel), w, h, mask.rotation.at(m_trackStartRel)}},
        {"scaleRotation", scaleRotation},
    };
    m_trackScaleRotation = scaleRotation;
    m_current = {media->id, QString::fromStdString(media->path), QString::fromStdString(media->name)};
    startTask(Task::Track, {}, {});
}

void AiController::autoReframe(const QString& aspect, bool zoom)
{
    if (busy() || !m_project)
        return;
    const auto ctx = selectedClip([](const Clip&, const MediaItem& m) { return m.hasVideo && !m.isStill; });
    if (!ctx) {
        emit errorOccurred(tr("Selecciona en la línea de tiempo un clip de video."));
        return;
    }
    m_clip = *ctx;
    m_aspect = aspect;
    m_reframeZoom = zoom;
    const Project& p = m_project->project();
    const MediaItem* media = p.findMedia(p.findClip(ctx->clipId)->mediaId);
    const ai::ModelInfo* modnet = ai::findModel(ai::portraitMatteModel());
    m_request = QJsonObject{
        {"type", QLatin1String(ai::protocol::kSubjectCenters)},
        {"media", QString::fromStdString(media->path)},
        {"from", qint64(ctx->sourceIn)},
        {"to", qint64(ctx->sourceIn + TimeUs(double(p.settings.frameToTime(ctx->duration)) * ctx->speed))},
        {"step", qint64(250'000)},
        {"model", ai::modelPath(*modnet)},
    };
    m_current = {media->id, QString::fromStdString(media->path), QString::fromStdString(media->name)};
    startTask(Task::Reframe, {}, {modnet->id});
}

// --- Edición por texto ---

void AiController::rebuildWords()
{
    m_words = m_project ? timelineWords(m_project->project()) : std::vector<TimedWord>{};
    m_wordsList.clear();
    m_wordsList.reserve(qsizetype(m_words.size()));
    const ProjectSettings settings = m_project ? m_project->project().settings : ProjectSettings{};
    for (const TimedWord& w : m_words) {
        m_wordsList.append(QVariantMap{
            {"text", QString::fromStdString(w.text)},
            {"start", toSeconds(w.start)},
            {"end", toSeconds(w.end)},
            {"startFrame", qint64(settings.timeToFrame(w.start))},
            {"endFrame", qint64(settings.timeToFrame(w.end))},
        });
    }
    m_untranscribed = !audibleMedia(true).empty();
    emit wordsChanged();
}

int AiController::wordAt(qint64 frame) const
{
    if (!m_project)
        return -1;
    const TimeUs t = m_project->project().settings.frameToTime(frame);
    // La última palabra que empieza antes del instante, si aún no ha terminado.
    const auto it = std::upper_bound(m_words.begin(), m_words.end(), t, [](TimeUs v, const TimedWord& w) { return v < w.start; });
    if (it == m_words.begin())
        return -1;
    const auto prev = std::prev(it);
    return t < prev->end ? int(prev - m_words.begin()) : -1;
}

void AiController::deleteWords(int first, int last)
{
    if (!m_project || m_words.empty())
        return;
    first = std::clamp(first, 0, int(m_words.size()) - 1);
    last = std::clamp(last, 0, int(m_words.size()) - 1);
    if (last < first)
        std::swap(first, last);
    const std::vector<TimedWord> selected(m_words.begin() + first, m_words.begin() + last + 1);
    const FrameRange range = wordsRange(m_project->project(), selected);
    m_project->applyEdit(tr("Eliminar texto"), [&](Project& p) { return edit::removeRanges(p, {range}) > 0; });
}

// --- SRT ---

bool AiController::importSrt(const QUrl& file)
{
    if (!m_project)
        return false;
    QFile in(file.toLocalFile());
    if (!in.open(QIODevice::ReadOnly)) {
        emit errorOccurred(tr("No se pudo abrir %1").arg(QDir::toNativeSeparators(file.toLocalFile())));
        return false;
    }
    const auto cues = parseSrt(in.readAll().toStdString());
    if (cues.empty()) {
        emit errorOccurred(tr("El archivo no contiene subtítulos válidos (SubRip .srt)."));
        return false;
    }
    const std::string name = QFileInfo(file.toLocalFile()).completeBaseName().toStdString();
    m_project->applyEdit(tr("Importar subtítulos"), [&](Project& p) {
        return edit::addSubtitleTrack(p, cues, subtitleStyle(), name) != kNoId;
    });
    emit notice(tr("Se importaron %n subtítulos.", nullptr, int(cues.size())));
    return true;
}

const Track* AiController::exportTrack() const
{
    const Project& p = m_project->project();
    for (Id id : m_project->selectedClips())
        if (const Clip* c = p.findClip(id); c && c->title)
            return p.trackOfClip(id);
    for (auto it = p.videoTracks.rbegin(); it != p.videoTracks.rend(); ++it)
        if (std::any_of(it->clips.begin(), it->clips.end(), [](const Clip& c) { return c.title.has_value(); }))
            return &*it;
    return nullptr;
}

bool AiController::hasSubtitleTrack() const
{
    return m_project && exportTrack();
}

bool AiController::exportSrt(const QUrl& file)
{
    if (!m_project)
        return false;
    const Track* track = exportTrack();
    if (!track) {
        emit errorOccurred(tr("No hay ninguna pista con subtítulos o títulos que exportar."));
        return false;
    }
    QSaveFile out(file.toLocalFile());
    const QByteArray data = QByteArray::fromStdString(toSrt(cuesOfTrack(m_project->project(), *track)));
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        emit errorOccurred(tr("No se pudo guardar %1").arg(QDir::toNativeSeparators(file.toLocalFile())));
        return false;
    }
    emit notice(tr("Subtítulos de la pista %1 exportados.").arg(QString::fromStdString(track->name)));
    return true;
}
