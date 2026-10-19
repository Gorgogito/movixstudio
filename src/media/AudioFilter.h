#pragma once

#include <memory>
#include <string>
#include <vector>

namespace movix::media {

// Cadena de filtros de audio de FFmpeg (libavfilter) con entrada y salida en float intercalado a la
// misma frecuencia y canales. `chain` es la descripción de los filtros ("atempo=1.5", "arnndn=m=...").
class AudioFilter {
public:
    AudioFilter();
    ~AudioFilter();
    AudioFilter(const AudioFilter&) = delete;
    AudioFilter& operator=(const AudioFilter&) = delete;

    bool init(int sampleRate, int channels, const std::string& chain);
    bool reset(); // vacía el estado (tras una búsqueda) con la misma cadena
    bool isValid() const;
    bool push(const float* samples, int frames); // frames = 0 → fin de la entrada
    // Añade a `out` las muestras disponibles; devuelve cuántos frames añadió.
    int pull(std::vector<float>& out);

    int sampleRate() const { return m_rate; }
    int channels() const { return m_channels; }

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    int m_rate = 48000;
    int m_channels = 2;
    std::string m_chain;
};

// Ruta del modelo de RNNoise (.rnnn) que usa la reducción de ruido; vacía = no disponible.
// La fija la aplicación al arrancar (y cuando se instala el modelo).
void setDenoiseModel(const std::string& path);
std::string denoiseModel();

// Reducción de ruido con RNNoise (filtro arnndn de FFmpeg). Compensa el retardo del filtro:
// la salida está alineada con la entrada muestra a muestra.
class Denoiser {
public:
    // amount: 0..1 (proporción de señal filtrada mezclada con la original).
    bool init(int sampleRate, int channels, const std::string& model, double amount);
    void reset();
    bool isValid() const { return m_filter.isValid(); }
    double amount() const { return m_amount; }
    const std::string& model() const { return m_model; }
    // Procesa la entrada en orden y añade a `out` la salida disponible (alineada).
    void process(const float* in, int frames, std::vector<float>& out);
    // Fin de la entrada: añade el resto (en total, tantas muestras como entraron).
    void flush(std::vector<float>& out);

private:
    void collect(std::vector<float>& out);

    AudioFilter m_filter;
    std::string m_model;
    double m_amount = 0.0;
    int m_delay = 0;          // frames de salida que se descartan tras reiniciar
    int64_t m_in = 0;         // frames que entraron desde el reinicio
    int64_t m_out = 0;        // frames entregados desde el reinicio
    int m_toDrop = 0;
    std::vector<float> m_tmp;
};

} // namespace movix::media
