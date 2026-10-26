#pragma once

#include "core/Project.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace movix::core {

// Formato de archivo de proyecto (.mxproj): JSON legible y versionado.
// Las rutas de los medios se guardan absolutas y relativas al proyecto; al abrir se usa la
// absoluta si existe y, si no, la relativa (permite mover la carpeta del proyecto completa).
// Versión 2 (F2): velocidad, transformación y volumen animables, fundidos, títulos, transiciones.
// Versión 3 (F3): recorte, corrección de color (con LUT .cube), croma, máscara y modo de fusión.
// Versión 4: corrección de color con keyframes, varias máscaras (y de forma libre), clips compuestos.
// Versión 5 (F4): transcripción de los medios, reducción de ruido por clip, marcadores.
// Versión 6 (F5): mates de IA por clip.
// Versión 7 (F6): clips multicámara (ángulo y audio).
// Versión 8 (F6): efectos OpenFX por clip, velocidad variable, clips al revés, forma animada de las
// máscaras de forma libre y composición en luz lineal.
inline constexpr int kProjectFormatVersion = 8;
inline constexpr const char* kProjectExtension = "mxproj";

struct LoadResult {
    std::optional<Project> project;
    QString error;
    QStringList missingMedia; // medios o LUT cuyo archivo no se encontró (el proyecto se abre igualmente)
};

QByteArray serializeProject(const Project& project, const QString& projectFilePath);
LoadResult deserializeProject(const QByteArray& json, const QString& projectFilePath);

// Transcripción en JSON (la usan el archivo de proyecto y el proceso de IA).
QJsonObject transcriptToJson(const Transcript& t);
Transcript transcriptFromJson(const QJsonObject& o);

// Estilo de un título en JSON (lo usan el archivo de proyecto y el intercambio OTIO).
QJsonObject titleToJson(const TitleStyle& s);
TitleStyle titleFromJson(const QJsonObject& o);

// Escritura atómica: o se guarda completo o el archivo anterior queda intacto.
bool saveProjectFile(const Project& project, const QString& path, QString* error = nullptr);
LoadResult loadProjectFile(const QString& path);

} // namespace movix::core
