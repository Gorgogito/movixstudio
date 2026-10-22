#pragma once

#include "ai/Speech.h"
#include "media/Time.h"

#include <string>
#include <vector>

// Sincronización de varias grabaciones del mismo suceso por su audio (multicámara): correlación
// cruzada generalizada con ponderación de fase (GCC-PHAT, Knapp y Carter 1976), robusta a
// micrófonos y niveles distintos. Sin modelos: DSP puro.
namespace movix::ai {

inline constexpr int kSyncSampleRate = 8000;

struct SyncResult {
    // Instante de la referencia (la primera grabación con audio) en que empieza cada grabación;
    // puede ser negativo (empezó antes que la referencia).
    std::vector<TimeUs> offsets;
    // Confianza de cada desfase (pico de la correlación frente al siguiente fuera de su entorno;
    // ≥ 2 es claro). 0 en las grabaciones sin audio o en la referencia.
    std::vector<double> confidence;
    std::vector<bool> synced; // false: sin audio (o confianza insuficiente): desfase 0
};

// `pcm`: grabaciones mono a `sampleRate`. Busca desfases de hasta `maxOffset` en ambos sentidos.
SyncResult syncByAudio(const std::vector<std::vector<float>>& pcm, int sampleRate, TimeUs maxOffset);

// Decodifica como mucho `maxSeconds` del audio de cada archivo (mono, 8 kHz) y lo sincroniza.
// Las rutas sin audio (o ilegibles) quedan sin sincronizar.
SyncResult syncFilesByAudio(const std::vector<std::string>& paths, double maxSeconds = 600.0, const ProgressFn& progress = {},
                            const CancelFn& cancel = {});

} // namespace movix::ai
