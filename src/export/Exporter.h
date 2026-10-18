#pragma once

#include "core/Project.h"
#include "media/Encoder.h"

#include <QString>

#include <atomic>
#include <functional>

namespace movix::exporting {

struct ExportSettings {
    QString path;
    std::string container;            // vacío = según la extensión
    int width = 1920;
    int height = 1080;
    core::Rational frameRate{30, 1};
    std::string videoCodec = "libx264";
    int quality = 20;                 // CRF/CQ (menor = mejor)
    int64_t videoBitRate = 0;         // > 0: tasa de bits objetivo
    std::string preset;
    bool includeAudio = true;
    std::string audioCodec = "aac";
    int64_t audioBitRate = 192'000;
    int sampleRate = 48000;
    // Intervalo de la línea de tiempo (en fotogramas del proyecto); end < 0 = hasta el final.
    core::FramePos rangeStart = 0;
    core::FramePos rangeEnd = -1;
    bool hardwareDecoding = true;
};

struct ExportProgress {
    int64_t framesDone = 0;
    int64_t framesTotal = 0;
    double elapsedSeconds = 0.0;
    double fps = 0.0;                 // velocidad de exportación
    // Tiempo medio por fotograma de cada etapa (ms), para localizar el cuello de botella.
    double decodeMs = 0.0;
    double renderMs = 0.0;
    double encodeMs = 0.0;
    double audioMs = 0.0;
};

// Exporta la línea de tiempo: compone cada fotograma con el mismo compositor GPU que la vista
// previa, mezcla el audio y codifica. Es bloqueante: se ejecuta en un hilo de trabajo.
class Exporter {
public:
    using ProgressCallback = std::function<void(const ExportProgress&)>;

    // Devuelve false si falla o se cancela (en ese caso se borra el archivo parcial).
    bool run(const core::Project& project, const ExportSettings& settings, const std::atomic<bool>& cancel,
             const ProgressCallback& progress, QString* error);

    bool wasCancelled() const { return m_cancelled; }

private:
    bool m_cancelled = false;
};

} // namespace movix::exporting
