#pragma once

#include "media/AudioDecoder.h"
#include "media/AudioFilter.h"
#include "media/TempoFilter.h"

#include <string>
#include <vector>

namespace movix::engine {

// Lector de audio de un clip: suma al buffer de salida las muestras de un intervalo del medio.
// Mantiene la continuidad entre peticiones consecutivas (sin buscar), busca exacto a la muestra
// cuando se pide otro punto y, si el clip va a otra velocidad, la aplica conservando el tono.
class AudioReader {
public:
    bool open(const std::string& path, int sampleRate, int channels, std::string* error = nullptr);

    // Suma `frames` muestras de salida que empiezan en el instante `t` del medio, reproducido a
    // `speed`, con una ganancia que va de `gainStart` a `gainEnd`. Fuera del medio hay silencio.
    // `denoise` (0..1) aplica la reducción de ruido (RNNoise) si hay modelo (media::denoiseModel()).
    void mixInto(TimeUs t, int frames, double speed, float gainStart, float gainEnd, float* out, double denoise = 0.0);

    int seekCount() const { return m_seeks; }

private:
    // Etapa de fuente: escribe (no suma) `frames` muestras del medio desde `t`.
    void readSource(TimeUs t, int frames, float* dst);
    // Etapa de tempo: escribe `frames` muestras de salida a otra velocidad desde `t`.
    void readTempo(TimeUs t, int frames, double speed, float* dst);
    TimeUs nextPts() const { return m_basePts + m_consumed * kUsPerSecond / m_rate; }
    void seekTo(TimeUs t);
    bool fill(int frames);
    int available() const { return int(m_fifo.size() / size_t(m_channels) - m_fifoRead); }

    media::AudioDecoder m_decoder;
    int m_rate = 48000;
    int m_channels = 2;
    std::vector<float> m_fifo;     // muestras pendientes del medio (intercaladas)
    size_t m_fifoRead = 0;         // primer frame no consumido del FIFO
    TimeUs m_basePts = kNoTime;    // pts de la primera muestra tras la última búsqueda
    int64_t m_consumed = 0;        // frames consumidos desde m_basePts
    bool m_eof = false;
    int m_seeks = 0;
    media::AudioChunk m_chunk;
    std::vector<float> m_scratch;

    // Reducción de ruido entre el decodificador y el FIFO (se reinicia con cada búsqueda).
    void configureDenoise(double amount);
    media::Denoiser m_denoiser;
    bool m_denoiseOn = false;
    std::vector<float> m_denoised;

    media::TempoFilter m_tempo;
    double m_tempoSpeed = 0.0;     // 0 = sin inicializar
    std::vector<float> m_tempoOut; // salida del filtro pendiente
    size_t m_tempoRead = 0;
    TimeUs m_tempoBase = kNoTime;  // instante del medio de la primera salida tras reiniciar
    int64_t m_tempoProduced = 0;   // frames de salida entregados desde m_tempoBase
    TimeUs m_tempoFeed = 0;        // instante del medio de la próxima muestra que entra al filtro
    std::vector<float> m_feed;
};

} // namespace movix::engine
