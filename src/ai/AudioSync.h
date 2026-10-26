#pragma once

#include "ai/Speech.h"
#include "media/Time.h"

#include <functional>
#include <string>
#include <vector>

// Sincronización de varias grabaciones del mismo suceso por su audio (multicámara): correlación
// cruzada generalizada con ponderación de fase (GCC-PHAT, Knapp y Carter 1976), robusta a
// micrófonos y niveles distintos. Sin modelos: DSP puro.
// Las grabaciones largas (más de kExactSyncSeconds) se sincronizan en dos pasos: uno grueso con la
// envolvente de inicios de sonido (100 por segundo, cualquier desfase) y uno fino a 8 kHz en varios
// puntos del solape, que mide también la deriva entre los relojes de las grabadoras.
namespace movix::ai {

inline constexpr int kSyncSampleRate = 8000;
inline constexpr int kEnvelopeRate = 100;            // valores por segundo de la envolvente
inline constexpr double kExactSyncSeconds = 600.0;  // hasta aquí, correlación directa de todo el audio

struct SyncResult {
    // Instante de la referencia (la primera grabación con audio) en que empieza cada grabación;
    // puede ser negativo (empezó antes que la referencia).
    std::vector<TimeUs> offsets;
    // Confianza de cada desfase (pico de la correlación frente al siguiente fuera de su entorno;
    // ≥ 2 es claro). 0 en las grabaciones sin audio o en la referencia.
    std::vector<double> confidence;
    std::vector<bool> synced; // false: sin audio (o confianza insuficiente): desfase 0
    // Velocidad a la que reproducir cada grabación para que no se desincronice con la referencia
    // (su reloj adelanta o atrasa); 1 si no hay deriva medible.
    std::vector<double> speed;
};

// `pcm`: grabaciones mono a `sampleRate`. Busca desfases de hasta `maxOffset` en ambos sentidos.
SyncResult syncByAudio(const std::vector<std::vector<float>>& pcm, int sampleRate, TimeUs maxOffset);

// Envolvente de inicios de sonido (aumentos de energía en cuatro bandas, en escala logarítmica) de
// audio mono a kSyncSampleRate, kEnvelopeRate valores por segundo. Se alimenta por bloques.
class OnsetEnvelope {
public:
    void push(const float* samples, size_t count);
    const std::vector<float>& values() const { return m_values; }

private:
    struct Band {
        double b0 = 0, b2 = 0, a1 = 0, a2 = 0; // pasabanda (b1 = 0)
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        double energy = 0, previous = 0;
    };
    Band m_bands[4];
    bool m_ready = false;
    int m_count = 0; // muestras del marco en curso
    std::vector<float> m_values;
};

// Grabación larga: su envolvente y una lectura de tramos de su audio (mono, kSyncSampleRate; las
// muestras fuera de la grabación, a cero).
struct SyncSource {
    std::vector<float> envelope;
    TimeUs duration = 0;
    std::function<std::vector<float>(TimeUs start, TimeUs length)> read;
};

// Sincronización en dos pasos (grueso con la envolvente, fino en varios puntos del solape, con la
// deriva). Las fuentes sin envolvente o sin sonido quedan sin sincronizar.
SyncResult syncSources(const std::vector<SyncSource>& sources);

// Decodifica el audio de cada archivo (mono, 8 kHz) y lo sincroniza: si todos duran como mucho
// kExactSyncSeconds, con la correlación directa; si no, en dos pasos (syncSources). Las rutas sin
// audio (o ilegibles) quedan sin sincronizar.
SyncResult syncFilesByAudio(const std::vector<std::string>& paths, const ProgressFn& progress = {}, const CancelFn& cancel = {});

} // namespace movix::ai
