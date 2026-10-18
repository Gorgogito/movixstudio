#pragma once

#include "media/AudioDecoder.h"

#include <string>
#include <vector>

namespace movix::engine {

// Lector de audio de un clip: suma al buffer de salida las muestras de un intervalo del medio.
// Mantiene la continuidad entre peticiones consecutivas (sin buscar) y busca exacto a la muestra
// cuando se pide otro punto.
class AudioReader {
public:
    bool open(const std::string& path, int sampleRate, int channels, std::string* error = nullptr);

    // Suma `frames` muestras desde el instante `t` del medio, multiplicadas por `gain`.
    // Lo que quede fuera del medio se considera silencio.
    void mixInto(TimeUs t, int frames, float gain, float* out);

    int seekCount() const { return m_seeks; }

private:
    TimeUs nextPts() const { return m_basePts + m_consumed * kUsPerSecond / m_rate; }
    void seekTo(TimeUs t);
    bool fill(int frames); // asegura `frames` muestras disponibles en el FIFO (salvo fin del medio)

    media::AudioDecoder m_decoder;
    int m_rate = 48000;
    int m_channels = 2;
    std::vector<float> m_fifo;     // muestras pendientes (intercaladas)
    size_t m_fifoRead = 0;         // primer frame no consumido del FIFO
    TimeUs m_basePts = kNoTime;    // pts de la primera muestra tras la última búsqueda
    int64_t m_consumed = 0;        // frames consumidos desde m_basePts
    bool m_eof = false;
    int m_seeks = 0;
    media::AudioChunk m_chunk;
};

} // namespace movix::engine
