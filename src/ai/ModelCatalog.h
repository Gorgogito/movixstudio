#pragma once

#include <QString>

#include <cstdint>
#include <vector>

// Modelos de IA que la aplicación sabe descargar y usar. Se guardan en la carpeta de datos local
// del usuario (o en MOVIX_MODELS_DIR, si está definida) y se verifican con SHA-256 al descargarlos.
// Un modelo puede tener varios archivos (p. ej. codificador, decodificador y tokenizador), que se
// guardan juntos en su subcarpeta.
namespace movix::ai {

enum class ModelTask {
    Transcription, // whisper.cpp (ggml)
    VoiceActivity, // Silero VAD (ggml)
    Denoise,       // RNNoise (.rnnn, filtro arnndn de FFmpeg)
    Speech,        // texto a voz: Kokoro (ONNX) + voces
    Translation,   // traducción: Opus-MT (Marian, ONNX) + SentencePiece
    Separation,    // separación de fuentes: HTDemucs (ONNX)
};

struct ModelFile {
    QString name;   // nombre del archivo dentro de la carpeta del modelo
    QString url;
    QString sha256; // hexadecimal en minúsculas
    std::int64_t size = 0;
};

struct ModelInfo {
    QString id;          // identificador estable ("whisper-base")
    ModelTask task = ModelTask::Transcription;
    QString name;        // nombre visible
    QString description; // traducible (ver ModelCatalog.cpp)
    QString license;
    QString folder;      // subcarpeta ("" = la raíz de los modelos)
    std::vector<ModelFile> files; // el primero es el principal
    std::int64_t totalSize() const;
};

const std::vector<ModelInfo>& modelCatalog();
const ModelInfo* findModel(const QString& id);
// Modelos que se proponen por defecto.
QString defaultTranscriptionModel();
QString defaultVadModel();
QString defaultDenoiseModel();
QString speechModel();
QString separationModel();
// Modelo de traducción de `from` a `to` ("es", "en"); vacío si no hay.
QString translationModel(const QString& from, const QString& to);

QString modelsDirectory(); // la crea si no existe
QString modelFolder(const ModelInfo& model);
QString modelPath(const ModelInfo& model); // el archivo principal
QString modelFilePath(const ModelInfo& model, const QString& fileName);
// Instalado = todos sus archivos existen con el tamaño esperado (el hash se comprobó al descargarlos).
bool isModelInstalled(const ModelInfo& model);

} // namespace movix::ai
