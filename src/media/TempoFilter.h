#pragma once

#include <memory>
#include <vector>

namespace movix::media {

// Cambio de velocidad del audio conservando el tono (filtro atempo de FFmpeg, encadenado
// cuando la velocidad sale de su rango). Entrada y salida: float intercalado.
class TempoFilter {
public:
    TempoFilter();
    ~TempoFilter();
    TempoFilter(const TempoFilter&) = delete;
    TempoFilter& operator=(const TempoFilter&) = delete;

    bool init(int sampleRate, int channels, double speed);
    void reset();                                // vacía el estado (tras una búsqueda)
    bool push(const float* samples, int frames); // frames = 0 → fin de la entrada
    // Añade a `out` las muestras disponibles; devuelve cuántos frames añadió.
    int pull(std::vector<float>& out);
    double speed() const { return m_speed; }

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    int m_rate = 48000;
    int m_channels = 2;
    double m_speed = 1.0;
};

} // namespace movix::media
