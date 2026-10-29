#pragma once

#include "core/Project.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <string>

namespace movix::core::otio {

// Intercambio de la línea de tiempo con OpenTimelineIO (.otio, JSON). Se exporta la secuencia
// principal con sus pistas, clips, huecos, transiciones, títulos, compuestos (como pilas anidadas),
// multicámaras (pilas con todos los ángulos en las que solo están activos el que se ve y el audio
// que suena), velocidad (LinearTimeWarp) y marcadores. Los efectos propios de MovixStudio (color, máscaras,
// transformaciones...) no tienen equivalente en OTIO y no viajan.
// El audio de los clips de video se exporta en pistas de audio aparte, como lo esperan los demás
// editores; al importar, los clips de audio que coinciden con uno de video se vuelven a unir a él.
inline constexpr const char* kOtioExtension = "otio";
inline constexpr const char* kTitleGeneratorKind = "MovixStudioTitle";
inline constexpr const char* kOfxGeneratorKind = "MovixStudioOpenFX"; // generador OpenFX (plugin y parámetros)

// `name` es el nombre de la línea de tiempo (p. ej. el del archivo).
QByteArray exportTimeline(const Project& project, const QString& name);

// Sondea un archivo de medio (duración, flujos...). nullopt si no existe o no se puede leer.
using MediaProbe = std::function<std::optional<MediaItem>(const std::string& path)>;

struct ImportResult {
    std::optional<Project> project;
    QString error;
    QStringList warnings;     // elementos que no se pudieron importar
    QStringList missingMedia; // medios que no se encontraron (sus clips se conservan)
};

// Crea un proyecto con la primera línea de tiempo del archivo. Las rutas relativas se resuelven
// respecto a la carpeta de `otioPath`.
ImportResult importTimeline(const QByteArray& json, const QString& otioPath, const MediaProbe& probe);

ImportResult importFile(const QString& path, const MediaProbe& probe);
bool exportFile(const Project& project, const QString& path, QString* error = nullptr);

} // namespace movix::core::otio
