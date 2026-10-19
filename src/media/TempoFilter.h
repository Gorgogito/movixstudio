#pragma once

#include "media/AudioFilter.h"

#include <vector>

namespace movix::media {

// Cambio de velocidad del audio conservando el tono (filtro atempo de FFmpeg, encadenado
// cuando la velocidad sale de su rango). Entrada y salida: float intercalado.
class TempoFilter {
public:
    bool init(int sampleRate, int channels, double speed);
    void reset();                                // vacía el estado (tras una búsqueda)
    bool push(const float* samples, int frames); // frames = 0 → fin de la entrada
    // Añade a `out` las muestras disponibles; devuelve cuántos frames añadió.
    int pull(std::vector<float>& out) { return m_filter.pull(out); }
    double speed() const { return m_speed; }

private:
    AudioFilter m_filter;
    double m_speed = 1.0;
};

} // namespace movix::media
