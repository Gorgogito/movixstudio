#pragma once

#include "ProjectController.h"
#include "ai/AiWorkerClient.h"
#include "ai/ModelDownloader.h"
#include "core/Project.h"
#include "core/Transcript.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <deque>
#include <functional>
#include <optional>
#include <map>

// Funciones de IA de la interfaz: gestor de modelos, transcripción y subtítulos automáticos,
// edición por texto y eliminación de silencios (todo en el proceso de IA, en local). Una tarea
// cada vez; el progreso y el estado se muestran en la UI.
class AiController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(ProjectController* project READ project WRITE setProject NOTIFY projectChanged)
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged) // 0..1, -1 = indeterminado
    Q_PROPERTY(QString modelsFolder READ modelsFolder CONSTANT)
    // Palabras que se oyen en la línea de tiempo: [{text, start, end (segundos), startFrame, endFrame}].
    Q_PROPERTY(QVariantList words READ words NOTIFY wordsChanged)
    // Hay medios audibles sin transcribir (el panel de transcripción ofrece hacerlo).
    Q_PROPERTY(bool untranscribed READ untranscribed NOTIFY wordsChanged)
    // El modelo de reducción de ruido está instalado (y en uso por el audio).
    Q_PROPERTY(bool denoiseAvailable READ denoiseAvailable NOTIFY modelsChanged)

public:
    explicit AiController(QObject* parent = nullptr);
    ~AiController() override;

    ProjectController* project() const { return m_project; }
    void setProject(ProjectController* project);
    // [{id, name, description, task, sizeMB, installed, downloading, license}]
    QVariantList models() const;
    bool busy() const { return m_task != Task::None; }
    QString status() const { return m_status; }
    double progress() const { return m_progress; }
    QString modelsFolder() const;
    QVariantList words() const { return m_wordsList; }
    bool untranscribed() const { return m_untranscribed; }
    bool denoiseAvailable() const;

    Q_INVOKABLE void downloadModel(const QString& id);
    Q_INVOKABLE void removeModel(const QString& id);
    Q_INVOKABLE void openModelsFolder() const;
    // Idiomas de transcripción: [{code, name}] ("auto" = detectar).
    Q_INVOKABLE QVariantList languages() const;
    // ¿Cuántos MB hay que descargar para transcribir con este modelo ("" = solo detección de voz)?
    Q_INVOKABLE double pendingDownloadMB(const QString& modelId) const;

    // Transcribe los medios que se oyen en la línea de tiempo (los que aún no tienen transcripción,
    // o todos con `retranscribe`) y, con `createTrack`, crea una pista de subtítulos. Descarga los
    // modelos que falten.
    Q_INVOKABLE void transcribe(const QString& modelId, const QString& language, bool retranscribe, bool createTrack,
                                int maxCharsPerLine = 42, int maxLines = 2);
    // Detecta la voz de los medios audibles y elimina los silencios de la línea de tiempo (en todas
    // las pistas desbloqueadas, cerrando huecos).
    Q_INVOKABLE void removeSilences(double threshold, int minSilenceMs, int paddingMs);
    // Marca los pulsos de la música del clip seleccionado (con audio) en la línea de tiempo.
    Q_INVOKABLE void markBeats();
    // Divide los clips seleccionados en los cambios de escena del primero de video.
    Q_INVOKABLE void splitScenes();

    // Texto a voz (Kokoro): el audio se añade en el cabezal, en una pista de audio libre.
    // [{id, name, language}]
    Q_INVOKABLE QVariantList voices() const;
    Q_INVOKABLE void speak(const QString& text, const QString& voice, double speed);
    // Traduce la pista de subtítulos (ver exportSrt) a una pista nueva con los mismos tiempos.
    Q_INVOKABLE void translateSubtitles(const QString& from, const QString& to);
    // Separa la voz del resto del audio del clip seleccionado en dos pistas nuevas.
    Q_INVOKABLE void separateVoice();
    Q_INVOKABLE void cancel();

    // Edición por texto: elimina de la línea de tiempo lo que ocupan las palabras [first, last].
    Q_INVOKABLE void deleteWords(int first, int last);
    // Índice de la palabra que suena en el fotograma (o -1).
    Q_INVOKABLE int wordAt(qint64 frame) const;

    Q_INVOKABLE bool importSrt(const QUrl& file);
    // Exporta la pista de subtítulos: la del clip de título seleccionado o, si no, la superior con títulos.
    Q_INVOKABLE bool exportSrt(const QUrl& file);
    Q_INVOKABLE bool hasSubtitleTrack() const;

signals:
    void projectChanged();
    void modelsChanged();
    void stateChanged();
    void wordsChanged();
    void errorOccurred(const QString& message);
    void notice(const QString& message);

private:
    enum class Task { None, Download, Transcribe, Silences, Beats, Scenes, Speak, Translate, Separate };
    // Clip sobre el que se analiza (pulsos, escenas) y cómo se lleva su tiempo a la línea de tiempo.
    struct ClipContext {
        movix::core::Id clipId = movix::core::kNoId;
        movix::core::FramePos start = 0;
        movix::core::FramePos duration = 0;
        movix::TimeUs sourceIn = 0;
        double speed = 1.0;
        std::vector<movix::core::Id> targets; // clips que se dividen (escenas)
    };
    // Primer clip seleccionado que cumple `accept`.
    std::optional<ClipContext> selectedClip(const std::function<bool(const movix::core::Clip&, const movix::core::MediaItem&)>& accept) const;
    movix::core::FramePos toTimeline(movix::TimeUs source) const;
    struct MediaJob {
        movix::core::Id mediaId = movix::core::kNoId;
        QString path;
        QString name;
    };

    void setState(Task task, const QString& status, double progress);
    void finishTask();
    void failTask(const QString& message);
    // Medios con audio que se oyen en la línea de tiempo activa.
    std::vector<MediaJob> audibleMedia(bool onlyUntranscribed) const;
    bool startTask(Task task, std::vector<MediaJob> media, QStringList models);
    void continueTask();
    void completeTranscription();
    void completeSilences();
    void onDownloadFinished(const QString& modelId, bool ok, const QString& error);
    void onJobFinished(int job, const QJsonObject& result);
    void onJobFailed(int job, const QString& error, bool canceled);
    void rebuildWords();
    const movix::core::Track* exportTrack() const;

    QPointer<ProjectController> m_project;
    movix::ai::ModelDownloader m_downloader;
    movix::ai::AiWorkerClient m_worker;
    Task m_task = Task::None;
    QString m_status;
    double m_progress = -1.0;
    bool m_canceling = false;

    // Tarea en curso
    QStringList m_downloads;        // modelos pendientes de descargar
    std::deque<MediaJob> m_queue;   // medios pendientes de procesar
    int m_totalJobs = 0;
    int m_currentJob = -1;
    MediaJob m_current;
    QString m_modelId;
    QString m_language;
    bool m_createTrack = false;
    int m_maxChars = 42;
    int m_maxLines = 2;
    double m_vadThreshold = 0.5;
    int m_minSilenceMs = 500;
    int m_paddingMs = 150;
    std::map<movix::core::Id, std::vector<movix::core::SpeechRange>> m_speech;
    ClipContext m_clip;
    // Texto a voz, traducción y separación
    QJsonObject m_request;              // petición preparada (se envía tras descargar los modelos)
    movix::core::FramePos m_atFrame = 0;
    movix::core::Id m_sourceTrack = movix::core::kNoId;
    QString m_trackSuffix;
    // Carpeta para los audios generados: junto al proyecto guardado o en los datos de la aplicación.
    QString generatedFolder() const;
    // Añade un archivo de audio generado como medio y lo coloca en `frame` en una pista de audio
    // libre (o nueva). Devuelve el id del clip.
    movix::core::Id addGeneratedAudio(movix::core::Project& p, const QString& path, movix::core::FramePos frame,
                                      double speed, const std::string& trackName);

    // Palabras de la línea de tiempo (edición por texto)
    std::vector<movix::core::TimedWord> m_words;
    QVariantList m_wordsList;
    bool m_untranscribed = false;
};
