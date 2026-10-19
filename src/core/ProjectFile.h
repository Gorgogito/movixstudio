#pragma once

#include "core/Project.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace movix::core {

// Formato de archivo de proyecto (.mxproj): JSON legible y versionado.
// Las rutas de los medios se guardan absolutas y relativas al proyecto; al abrir se usa la
// absoluta si existe y, si no, la relativa (permite mover la carpeta del proyecto completa).
// Versión 2 (F2): velocidad, transformación y volumen animables, fundidos, títulos, transiciones.
inline constexpr int kProjectFormatVersion = 2;
inline constexpr const char* kProjectExtension = "mxproj";

struct LoadResult {
    std::optional<Project> project;
    QString error;
    QStringList missingMedia; // medios cuyo archivo no se encontró (el proyecto se abre igualmente)
};

QByteArray serializeProject(const Project& project, const QString& projectFilePath);
LoadResult deserializeProject(const QByteArray& json, const QString& projectFilePath);

// Escritura atómica: o se guarda completo o el archivo anterior queda intacto.
bool saveProjectFile(const Project& project, const QString& path, QString* error = nullptr);
LoadResult loadProjectFile(const QString& path);

} // namespace movix::core
