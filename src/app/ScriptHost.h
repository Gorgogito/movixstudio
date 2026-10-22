#pragma once

#include "core/Project.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <vector>

// Scripts en JavaScript (QJSEngine) que automatizan la edición: un objeto global `movix` consulta y
// edita el proyecto. Un script trabaja sobre una copia: si termina bien, sus cambios se aplican de una
// vez (un solo paso de deshacer); si falla, no cambia nada. Sin acceso a archivos ni a la red, y se
// interrumpe si tarda demasiado.
namespace movix::app {

struct ScriptInfo {
    QString name;        // "// @name ..." en la cabecera (o el nombre del archivo)
    QString description; // "// @description ..."
    QString path;        // ruta del archivo (":/scripts/..." los incluidos)
    bool builtin = false;
};

// Scripts incluidos y los de la carpeta del usuario (por nombre).
std::vector<ScriptInfo> listScripts(const QString& userDir);
ScriptInfo scriptInfo(const QString& path, const QString& code);

struct ScriptResult {
    bool ok = false;
    bool changed = false; // el proyecto se modificó
    QString error;        // con la línea, si falló
    QStringList log;      // lo escrito con movix.log()
};

// Ejecuta `code` sobre `project` (el cabezal y la selección son los del editor).
ScriptResult runScript(const QString& code, core::Project& project, core::FramePos playhead, const std::vector<core::Id>& selection,
                       int timeoutMs = 10000);

// La API `movix` que ven los scripts. Los tiempos son fotogramas del proyecto; los ids, números.
class ScriptApi : public QObject {
    Q_OBJECT
public:
    ScriptApi(core::Project& project, core::FramePos playhead, std::vector<core::Id> selection);

    // Consultas.
    Q_INVOKABLE QVariantMap info() const; // {width, height, fps, duration, playhead, selection}
    Q_INVOKABLE QVariantList tracks() const;
    Q_INVOKABLE QVariant clip(double clipId) const;
    Q_INVOKABLE QVariantList media() const;
    Q_INVOKABLE QVariantList markers() const;
    Q_INVOKABLE double secondsToFrames(double seconds) const;
    Q_INVOKABLE double framesToSeconds(double frames) const;

    // Ediciones (devuelven el id creado, true/false o −1 si no se pudo).
    Q_INVOKABLE double split(double clipId, double frame);
    Q_INVOKABLE QVariantList splitAll(double frame);
    Q_INVOKABLE bool remove(const QVariant& clipIds, bool ripple = false);
    Q_INVOKABLE bool move(const QVariant& clipIds, double delta, int trackOffset = 0);
    Q_INVOKABLE double trim(double clipId, const QString& edge, double frame);
    Q_INVOKABLE double place(double mediaId, double trackId, double frame, double durationFrames = 0);
    Q_INVOKABLE double addTitle(const QString& text, double frame, double durationFrames, double trackId = -1);
    Q_INVOKABLE double addTrack(const QString& kind);
    Q_INVOKABLE bool addMarker(double frame, const QString& label = {});
    Q_INVOKABLE bool removeMarker(double frame);
    Q_INVOKABLE double removeRanges(const QVariantList& ranges); // [[desde, hasta], ...]
    Q_INVOKABLE bool set(double clipId, const QString& property, const QVariant& value);
    Q_INVOKABLE void log(const QString& text);

    bool changed() const { return m_changed; }
    const QStringList& logLines() const { return m_log; }

private:
    bool mark(bool changed)
    {
        m_changed = m_changed || changed;
        return changed;
    }
    QVariantMap clipMap(const core::Clip& c, const core::Track& t) const;

    core::Project& m_p;
    core::FramePos m_playhead;
    std::vector<core::Id> m_selection;
    bool m_changed = false;
    QStringList m_log;
};

} // namespace movix::app
