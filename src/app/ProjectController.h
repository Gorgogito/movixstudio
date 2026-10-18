#pragma once

#include "ExportController.h"
#include "TimelineModels.h"
#include "core/History.h"
#include "playback/TimelinePlayer.h"

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

// Controlador del proyecto abierto: une la interfaz con el núcleo (historial, ediciones, archivo),
// mantiene sincronizados los modelos y el reproductor, y se ocupa del autoguardado.
class ProjectController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(MediaListModel* media READ media CONSTANT)
    Q_PROPERTY(TrackListModel* tracks READ tracks CONSTANT)
    Q_PROPERTY(ClipListModel* clips READ clips CONSTANT)
    Q_PROPERTY(movix::playback::TimelinePlayer* player READ player CONSTANT)
    Q_PROPERTY(movix::playback::TimelinePlayer* sourcePlayer READ sourcePlayer CONSTANT)
    Q_PROPERTY(ExportController* exporter READ exporter CONSTANT)
    Q_PROPERTY(QString projectName READ projectName NOTIFY fileChanged)
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileChanged)
    Q_PROPERTY(bool modified READ isModified NOTIFY historyChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY historyChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY historyChanged)
    Q_PROPERTY(int canvasWidth READ canvasWidth NOTIFY settingsChanged)
    Q_PROPERTY(int canvasHeight READ canvasHeight NOTIFY settingsChanged)
    Q_PROPERTY(double frameRate READ frameRate NOTIFY settingsChanged)
    Q_PROPERTY(QString frameRateText READ frameRateText NOTIFY settingsChanged)
    Q_PROPERTY(qint64 frameRateNum READ frameRateNum NOTIFY settingsChanged)
    Q_PROPERTY(qint64 frameRateDen READ frameRateDen NOTIFY settingsChanged)
    Q_PROPERTY(qint64 durationFrames READ durationFrames NOTIFY timelineChanged)
    Q_PROPERTY(int videoTrackCount READ videoTrackCount NOTIFY timelineChanged)
    Q_PROPERTY(int audioTrackCount READ audioTrackCount NOTIFY timelineChanged)
    Q_PROPERTY(QVariantList selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(int importing READ importing NOTIFY importingChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY sourceChanged)

public:
    explicit ProjectController(QObject* parent = nullptr);
    ~ProjectController() override;

    MediaListModel* media() { return &m_media; }
    TrackListModel* tracks() { return &m_tracks; }
    ClipListModel* clips() { return &m_clips; }
    movix::playback::TimelinePlayer* player() { return m_player.get(); }
    movix::playback::TimelinePlayer* sourcePlayer() { return m_sourcePlayer.get(); }
    ExportController* exporter() { return &m_exporter; }

    QString projectName() const;
    QUrl fileUrl() const { return m_fileUrl; }
    bool isModified() const { return !m_history.isClean(); }
    bool canUndo() const { return m_history.canUndo(); }
    bool canRedo() const { return m_history.canRedo(); }
    QString undoText() const { return QString::fromStdString(m_history.undoText()); }
    QString redoText() const { return QString::fromStdString(m_history.redoText()); }
    int canvasWidth() const { return project().settings.width; }
    int canvasHeight() const { return project().settings.height; }
    double frameRate() const { return project().settings.frameRate.value(); }
    QString frameRateText() const;
    qint64 frameRateNum() const { return project().settings.frameRate.num; }
    qint64 frameRateDen() const { return project().settings.frameRate.den; }
    qint64 durationFrames() const { return project().duration(); }
    int videoTrackCount() const { return int(project().videoTracks.size()); }
    int audioTrackCount() const { return int(project().audioTracks.size()); }
    QVariantList selection() const;
    int importing() const { return m_importing; }
    QString sourceName() const { return m_sourceName; }

    const movix::core::Project& project() const { return m_history.project(); }
    std::shared_ptr<const movix::core::Project> snapshot() const { return m_snapshot; }

    // --- Proyecto ---
    Q_INVOKABLE void newProject();
    Q_INVOKABLE bool openProject(const QUrl& url);
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool saveAs(const QUrl& url);
    Q_INVOKABLE void setProjectSettings(int width, int height, int fpsNum, int fpsDen);
    Q_INVOKABLE void checkRecovery();
    Q_INVOKABLE void recoverAutosave();
    Q_INVOKABLE void discardAutosave();
    Q_INVOKABLE void shutdown(); // cierre ordenado: elimina el autoguardado

    // --- Medios ---
    Q_INVOKABLE void importFiles(const QList<QUrl>& urls);
    Q_INVOKABLE void removeMedia(qint64 mediaId);
    Q_INVOKABLE void previewMedia(qint64 mediaId);
    Q_INVOKABLE QVariantMap mediaInfo(qint64 mediaId) const;

    // --- Línea de tiempo ---
    Q_INVOKABLE bool appendToTimeline(qint64 mediaId);
    Q_INVOKABLE bool placeMedia(qint64 mediaId, qint64 trackId, qint64 frame);
    Q_INVOKABLE bool moveClips(const QVariantList& clipIds, qint64 delta, int trackOffset);
    Q_INVOKABLE qint64 trimClip(qint64 clipId, bool startEdge, qint64 frame);
    Q_INVOKABLE void splitAtPlayhead();
    Q_INVOKABLE void deleteSelection(bool ripple);
    Q_INVOKABLE void setClipGain(qint64 clipId, double gain);
    Q_INVOKABLE void setClipEnabled(qint64 clipId, bool enabled);
    Q_INVOKABLE void endInteraction();
    Q_INVOKABLE void addTrack(const QString& kind);
    Q_INVOKABLE void removeTrack(qint64 trackId);
    Q_INVOKABLE void setTrackFlag(qint64 trackId, const QString& flag, bool value);
    Q_INVOKABLE QVariantMap clipInfo(qint64 clipId) const;
    Q_INVOKABLE QVariantList snapPoints(const QVariantList& excludeClipIds) const;
    Q_INVOKABLE QString timecode(qint64 frame) const;

    // --- Selección ---
    Q_INVOKABLE void select(qint64 clipId, bool additive);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void selectAll();
    // "video", "audio", "mixed" o "" según las pistas de los clips seleccionados.
    Q_INVOKABLE QString selectionKind() const;

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

signals:
    void fileChanged();
    void historyChanged();
    void settingsChanged();
    void timelineChanged();
    void selectionChanged();
    void importingChanged();
    void sourceChanged();
    void errorOccurred(const QString& message);
    void notice(const QString& message);
    void recoveryAvailable(const QString& when);

private:
    bool apply(const QString& text, const std::function<bool(movix::core::Project&)>& edit, const QString& mergeKey = {});
    void refresh();
    void resetTo(movix::core::Project project, const QUrl& url);
    void autosave();
    QString autosavePath() const;
    void finishImport();

    movix::core::History m_history;
    std::shared_ptr<const movix::core::Project> m_snapshot;
    QUrl m_fileUrl;
    MediaListModel m_media;
    TrackListModel m_tracks;
    ClipListModel m_clips;
    QSet<movix::core::Id> m_selection;
    std::unique_ptr<movix::playback::TimelinePlayer> m_player;
    std::unique_ptr<movix::playback::TimelinePlayer> m_sourcePlayer;
    ExportController m_exporter;
    QString m_sourceName;
    QTimer m_autosaveTimer;
    uint64_t m_autosavedRevision = 0;
    int m_importing = 0;
    std::vector<std::pair<int, movix::core::MediaItem>> m_imported; // (orden, medio)
    QStringList m_importErrors;
    int m_importOrder = 0;
};
