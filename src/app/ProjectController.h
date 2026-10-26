#pragma once

#include "ExportController.h"
#include "TemplateStore.h"
#include "TimelineModels.h"
#include "core/Edits.h"
#include "core/History.h"
#include "playback/TimelinePlayer.h"

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>

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
    // Edición desde otros controladores (IA): se registra en el historial como cualquier otra.
    bool applyEdit(const QString& text, const std::function<bool(movix::core::Project&)>& edit) { return apply(text, edit); }
    QSet<movix::core::Id> selectedClips() const { return m_selection; }

    // --- Proyecto ---
    Q_INVOKABLE void newProject();
    Q_INVOKABLE bool openProject(const QUrl& url);
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool saveAs(const QUrl& url);
    Q_INVOKABLE void setProjectSettings(int width, int height, int fpsNum, int fpsDen);
    // Espacio de color: "sdr" (Rec.709), "pq" (HDR10, Rec.2020) o "hlg" (Rec.2020).
    Q_PROPERTY(QString colorMode READ colorMode NOTIFY settingsChanged)
    QString colorMode() const;
    Q_INVOKABLE void setColorMode(const QString& mode);
    // Componer en luz lineal: fusión, opacidad y fundidos mezclan como la luz real.
    Q_PROPERTY(bool linearLight READ linearLight NOTIFY settingsChanged)
    bool linearLight() const { return project().settings.linearLight; }
    Q_INVOKABLE void setLinearLight(bool on);
    Q_INVOKABLE void checkRecovery();
    Q_INVOKABLE void recoverAutosave();
    Q_INVOKABLE void discardAutosave();
    Q_INVOKABLE void shutdown(); // cierre ordenado: elimina el autoguardado
    // Intercambio con OpenTimelineIO: importar crea un proyecto nuevo (sin guardar) con la línea
    // de tiempo; exportar escribe la secuencia principal.
    Q_INVOKABLE void importOtio(const QUrl& url);
    Q_INVOKABLE bool exportOtio(const QUrl& url);

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
    Q_INVOKABLE void setClipEnabled(qint64 clipId, bool enabled);

    // --- Propiedades animables (evaluadas en el cabezal) ---
    // prop: "x", "y", "scale", "rotation", "opacity", "volume".
    Q_INVOKABLE void setClipProperty(qint64 clipId, const QString& prop, double value);
    Q_INVOKABLE void toggleKeyframe(qint64 clipId, const QString& prop);
    Q_INVOKABLE void resetClipProperty(qint64 clipId, const QString& prop);
    // Cómo se llega desde el keyframe del cabezal al siguiente: "linear", "hold" o "smooth".
    // `prop` es una propiedad animable o "color" (keyframes de color).
    Q_INVOKABLE void setKeyInterpolation(qint64 clipId, const QString& prop, const QString& interp);
    // {value, animated, keyHere, interp, hasPrev, hasNext} de la propiedad en el cabezal.
    Q_INVOKABLE QVariantMap propertyState(qint64 clipId, const QString& prop) const;
    // Lleva el cabezal al keyframe anterior/siguiente de cualquier propiedad del clip.
    Q_INVOKABLE void jumpToKeyframe(qint64 clipId, bool forward);
    // Fotogramas (relativos a la línea de tiempo) con keyframes en el clip: para dibujarlos.
    Q_INVOKABLE QVariantList keyframeFrames(qint64 clipId) const;
    // Geometría del clip en el cabezal, en fracciones del lienzo: {cx, cy, hw, hh, rotation}.
    Q_INVOKABLE QVariantMap clipGeometry(qint64 clipId) const;

    // --- Velocidad, fundidos, transiciones, títulos, audio ---
    Q_INVOKABLE void setClipSpeed(qint64 clipId, double speed); // fija (quita la variable)
    Q_INVOKABLE void setReverse(qint64 clipId, bool reverse);
    Q_INVOKABLE void setFades(qint64 clipId, double inSeconds, double outSeconds, bool audio);
    Q_INVOKABLE void setDenoise(qint64 clipId, double amount);

    // --- Marcadores ---
    // [{frame, label, color}] de la secuencia activa.
    Q_PROPERTY(QVariantList markers READ markers NOTIFY timelineChanged)
    QVariantList markers() const;
    Q_INVOKABLE void addMarker();                       // en el cabezal
    Q_INVOKABLE void removeMarker(qint64 frame);
    Q_INVOKABLE void clearMarkers();
    Q_INVOKABLE void jumpToMarker(bool forward);
    // type: "none", "dissolve", "dipToBlack", "wipeLeft", "wipeRight", "pushLeft", "pushRight", "slideLeft".
    Q_INVOKABLE bool setTransition(qint64 clipId, const QString& type, double seconds);
    // Aplica una transición en el corte más cercano a `frame` en la pista de la fila `trackRow`.
    Q_INVOKABLE bool dropTransition(const QString& type, qint64 trackId, qint64 frame);
    Q_INVOKABLE bool addTitle(int preset, qint64 trackId = -1, qint64 frame = -1);
    Q_INVOKABLE void setTitleStyle(qint64 clipId, const QVariantMap& style);
    Q_INVOKABLE void detachAudio(qint64 clipId);
    Q_INVOKABLE void setTrackVolume(qint64 trackId, double volume);
    // Títulos predefinidos seguidos de las plantillas del usuario ({name, text, user}); el índice es
    // el que recibe addTitle().
    Q_PROPERTY(QVariantList titlePresets READ titlePresets NOTIFY templatesChanged)
    QVariantList titlePresets() const;

    // --- Plantillas propias ---
    Q_PROPERTY(QStringList projectTemplates READ projectTemplates NOTIFY templatesChanged)
    QStringList projectTemplates() const;
    Q_INVOKABLE bool saveTitleTemplate(qint64 clipId, const QString& name);
    Q_INVOKABLE void removeTitleTemplate(int presetIndex);
    Q_INVOKABLE bool saveProjectTemplate(const QString& name);
    Q_INVOKABLE void removeProjectTemplate(const QString& name);
    // Proyecto nuevo (sin guardar) a partir de la plantilla.
    Q_INVOKABLE bool newFromTemplate(const QString& name);

    // --- Scripts (JavaScript, ver ScriptHost.h y docs/SCRIPTS.md) ---
    // Los incluidos y los de la carpeta del usuario: {name, description, path, builtin}.
    Q_PROPERTY(QVariantList scripts READ scripts NOTIFY scriptsChanged)
    QVariantList scripts() const { return m_scripts; }
    Q_INVOKABLE void reloadScripts();
    Q_INVOKABLE bool runScript(const QString& path); // un solo paso de deshacer
    Q_INVOKABLE void openScriptsFolder();
    QString scriptsFolder() const;

    // --- Color (F3) ---
    // Corrección completa del clip: valores básicos, ruedas [r, g, b, master], curvas [[x, y]...],
    // hsl [[tono, saturación, luminancia] × 8], lutPath/lutName/lutIntensity e "identity".
    Q_INVOKABLE QVariantMap colorGrade(qint64 clipId) const;
    // key: "exposure", "contrast"..., "lift.r", "gamma.master", "hsl.3.saturation", "lutIntensity".
    Q_INVOKABLE void setColorValue(qint64 clipId, const QString& key, double value);
    // Mueve una rueda ("lift", "gamma", "gain") de una vez (r, g, b sin tocar su master).
    Q_INVOKABLE void setColorWheel(qint64 clipId, const QString& wheel, double r, double g, double b);
    // channel: "master", "red", "green", "blue"; points: [[x, y], ...].
    Q_INVOKABLE void setColorCurve(qint64 clipId, const QString& channel, const QVariantList& points);
    Q_INVOKABLE bool setColorLut(qint64 clipId, const QUrl& file);
    // Espacios de cámara de OpenColorIO: [{name, family}]; "" quita la transformación de entrada.
    Q_INVOKABLE QVariantList ocioInputSpaces() const;
    Q_INVOKABLE void setColorInputSpace(qint64 clipId, const QString& space);
    // section: "basic", "wheels", "curves", "hsl", "lut" o "all".
    Q_INVOKABLE void resetColor(qint64 clipId, const QString& section);
    Q_INVOKABLE void toggleColorKeyframe(qint64 clipId);
    Q_INVOKABLE void copyColor(qint64 clipId);
    Q_INVOKABLE void pasteColor(); // a todos los clips de video seleccionados
    Q_PROPERTY(bool canPasteColor READ canPasteColor NOTIFY clipboardChanged)
    bool canPasteColor() const { return m_colorClipboard.has_value(); }

    // --- Croma, máscara y fusión (F3) ---
    // Cambia los campos presentes de {enabled, color, tolerance, softness, spill}.
    Q_INVOKABLE void setChromaKey(qint64 clipId, const QVariantMap& values);
    // Toma como color clave el del clip en (u, v), coordenadas de la propia capa (0..1).
    Q_INVOKABLE bool pickKeyColor(qint64 clipId, double u, double v);
    // Máscaras (shape: "rectangle", "ellipse", "linear", "path"; mode: "add", "subtract", "intersect").
    // Sus valores animables son las propiedades "mask.<índice>.x" (y, width, height, rotation, feather).
    Q_INVOKABLE int addMask(qint64 clipId, const QString& shape);
    Q_INVOKABLE void removeMask(qint64 clipId, int index);
    Q_INVOKABLE void setMaskShape(qint64 clipId, int index, const QString& shape);
    Q_INVOKABLE void setMaskOptions(qint64 clipId, int index, const QString& mode, bool invert);
    // points: [[x, y, inX, inY, outX, outY], ...] en el sistema de la máscara.
    Q_INVOKABLE void setMaskPoints(qint64 clipId, int index, const QVariantList& points);
    // Puntos de una máscara de forma libre con tangentes suaves (Catmull-Rom) o con esquinas.
    Q_INVOKABLE QVariantList smoothMaskPoints(const QVariantList& points, bool smooth) const;
    Q_INVOKABLE void setBlendMode(qint64 clipId, const QString& mode);
    // Mate de IA (recorte de fondo): invertir o quitar.
    Q_INVOKABLE void setMatteInvert(qint64 clipId, bool invert);
    Q_INVOKABLE void removeMatte(qint64 clipId);

    // --- Clips compuestos ---
    // Nombres de las secuencias abiertas, de la principal a la activa (para la ruta de navegación).
    Q_PROPERTY(QStringList sequencePath READ sequencePath NOTIFY timelineChanged)
    Q_PROPERTY(bool inCompound READ inCompound NOTIFY timelineChanged)
    QStringList sequencePath() const;
    bool inCompound() const { return !project().sequencePath.empty(); }
    Q_INVOKABLE void createCompound();              // con los clips seleccionados
    Q_INVOKABLE void openCompound(qint64 clipId);    // editar su secuencia
    Q_INVOKABLE void openSequenceLevel(int level);   // volver a un nivel de la ruta (0 = principal)
    Q_INVOKABLE void breakCompound(qint64 clipId);   // devolver su contenido a la línea de tiempo

    // --- Portapapeles e inserción ---
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void cutSelection();
    Q_INVOKABLE void paste();
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged)
    Q_PROPERTY(bool insertMode READ insertMode WRITE setInsertMode NOTIFY insertModeChanged)
    bool canPaste() const { return !m_clipboard.empty(); }
    bool insertMode() const { return m_insertMode; }
    void setInsertMode(bool on);
    Q_INVOKABLE void endInteraction();
    Q_INVOKABLE void addTrack(const QString& kind);
    Q_INVOKABLE void removeTrack(qint64 trackId);
    Q_INVOKABLE void setTrackFlag(qint64 trackId, const QString& flag, bool value);
    Q_INVOKABLE QVariantMap clipInfo(qint64 clipId) const;
    Q_INVOKABLE QVariantList snapPoints(const QVariantList& excludeClipIds) const;
    Q_INVOKABLE QString timecode(qint64 frame) const;

    // --- Multicámara ---
    // Vista del visor con todos los ángulos de los clips multicámara en cuadrícula.
    Q_PROPERTY(bool multicamView READ multicamView WRITE setMulticamView NOTIFY multicamViewChanged)
    bool multicamView() const { return m_multicamView; }
    void setMulticamView(bool on);
    // Crea un clip multicámara con los medios (sincronizados por su audio o alineados al inicio) al
    // final de la línea de tiempo. Se hace en segundo plano (`importing`).
    Q_INVOKABLE void createMulticam(const QVariantList& mediaIds, bool syncByAudio);
    // Clip multicámara visible en el fotograma (el de la pista de video más alta): {clipId, angle,
    // audioAngle, angles: [nombres], audioSources: [nombres]}; vacío si no hay.
    Q_INVOKABLE QVariantMap multicamAt(qint64 frame) const;
    // Corta en el cabezal al ángulo indicado del clip multicámara visible (también reproduciendo).
    Q_INVOKABLE bool switchAngle(int angle);
    Q_INVOKABLE void setMulticamAngle(qint64 clipId, int angle);
    Q_INVOKABLE void setMulticamAudio(qint64 clipId, int audioAngle); // −1 = el del ángulo que se ve

    // --- Proxies: copias ligeras de los medios pesados para la vista previa ---
    Q_PROPERTY(bool useProxies READ useProxies WRITE setUseProxies NOTIFY proxiesChanged)
    Q_PROPERTY(int proxyRevision READ proxyRevision NOTIFY proxiesChanged) // cambia con cada estado
    bool useProxies() const;
    void setUseProxies(bool on);
    int proxyRevision() const { return m_proxyRevision; }
    // "" (sin proxy), "ready" o el porcentaje mientras se crea ("37").
    Q_INVOKABLE QString proxyState(qint64 mediaId) const;
    Q_INVOKABLE void createProxy(qint64 mediaId);
    Q_INVOKABLE void removeProxy(qint64 mediaId);
    QString proxyFolder() const;

    // --- Efectos OpenFX (ProjectControllerOfx.cpp) ---
    // Plugins instalados: {id, label, group, description}.
    Q_PROPERTY(QVariantList ofxPlugins READ ofxPlugins NOTIFY ofxPluginsChanged)
    QVariantList ofxPlugins() const;
    Q_INVOKABLE void rescanOfx();
    Q_INVOKABLE void openOfxFolder();
    QString ofxFolder() const;
    // Efectos del clip con la definición de sus parámetros y su valor en el cabezal: {index, pluginId,
    // label, enabled, installed, params: [{name, label, hint, type, value, defaults, min, max, options,
    // animates, animated, keyHere, interp, hasPrev, hasNext}]}.
    Q_INVOKABLE QVariantList ofxEffects(qint64 clipId) const;
    Q_INVOKABLE void addOfxEffect(qint64 clipId, const QString& pluginId);
    Q_INVOKABLE void removeOfxEffect(qint64 clipId, int index);
    Q_INVOKABLE void setOfxEnabled(qint64 clipId, int index, bool enabled);
    Q_INVOKABLE void moveOfxEffect(qint64 clipId, int from, int to);
    Q_INVOKABLE void setOfxParam(qint64 clipId, int index, const QString& name, const QVariantList& value);
    // Keyframe del parámetro en el cabezal (añadir o quitar) y cómo se llega al siguiente.
    Q_INVOKABLE void toggleOfxKeyframe(qint64 clipId, int index, const QString& name);
    Q_INVOKABLE void setOfxKeyInterpolation(qint64 clipId, int index, const QString& name, const QString& interp);

    // --- Selección ---
    Q_INVOKABLE void select(qint64 clipId, bool additive);
    Q_INVOKABLE void clearSelection();
    // Selección por recuadro: los clips que tocan [fromFrame, toFrame) en las filas firstRow..lastRow
    // de la línea de tiempo (las de video de arriba abajo y luego las de audio).
    Q_INVOKABLE void selectRange(qint64 fromFrame, qint64 toFrame, int firstRow, int lastRow, bool additive);
    // Varios clips a la vez: {count, video, audio, opacity, volume, scale, speed, enabled, reverse,
    // fadeIn, fadeOut, audioFadeIn, audioFadeOut} (los valores, del primero).
    Q_INVOKABLE QVariantMap selectionSummary() const;
    // Aplica a todos los seleccionados en un paso de deshacer: "opacity", "volume", "scale" (en el
    // cabezal, como keyframe si están animados), "speed", "enabled", "reverse" o los fundidos
    // ("fadeIn", "fadeOut", "audioFadeIn", "audioFadeOut", en segundos).
    Q_INVOKABLE void editSelection(const QString& what, const QVariant& value);
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
    void clipboardChanged();
    void insertModeChanged();
    void multicamViewChanged();
    void templatesChanged();
    void scriptsChanged();
    void ofxPluginsChanged();
    void proxiesChanged();

private:
    movix::core::FramePos playheadRel(const movix::core::Clip& c) const;
    bool apply(const QString& text, const std::function<bool(movix::core::Project&)>& edit, const QString& mergeKey = {});
    void refresh();
    void resetTo(movix::core::Project project, const QUrl& url);
    void autosave();
    QString autosavePath() const;
    void finishImport();

    movix::core::History m_history;
    std::shared_ptr<const movix::core::Project> m_snapshot;
    bool m_multicamView = false;
    // Proxies listos (ruta del original → proxy) y si se usan: lo consulta el hilo del reproductor.
    struct ProxyIndex {
        std::mutex mutex;
        bool enabled = true;
        std::map<std::string, std::string> ready;
    };
    std::shared_ptr<ProxyIndex> m_proxies = std::make_shared<ProxyIndex>();
    std::map<std::string, int> m_proxyBuilding; // ruta → porcentaje
    int m_proxyRevision = 0;
    void queueProxies(); // los que faltan de los medios pesados
    void startProxy(const movix::core::MediaItem& media);
    void runNextProxy();
    std::vector<movix::core::MediaItem> m_proxyQueue;
    bool m_proxyRunning = false;
    std::shared_ptr<std::atomic<bool>> m_proxyCancel = std::make_shared<std::atomic<bool>>(false);
    TemplateStore m_templates;
    QVariantList m_scripts;
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
    movix::core::edit::Clipboard m_clipboard;
    std::optional<movix::core::AnimatedGrade> m_colorClipboard;
    bool m_insertMode = false;
};
