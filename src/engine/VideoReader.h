#pragma once

#include "media/VideoDecoder.h"

#include <memory>
#include <string>

namespace movix::engine {

// Lector de video de un clip: entrega el fotograma que se ve en cada instante del medio.
// Avanza decodificando en orden mientras las peticiones van hacia delante (reproducción) y
// solo busca cuando se salta hacia atrás o muy adelante (navegación).
class VideoReader {
public:
    bool open(const std::string& path, bool hardware, std::string* error = nullptr);

    // Fotograma visible en el instante `t` del medio (el último con pts ≤ t). Más allá del final
    // devuelve el último fotograma. nullptr solo si el medio no tiene ninguno.
    std::shared_ptr<const media::VideoFrame> frameAt(TimeUs t);

    const media::VideoDecoder& decoder() const { return m_decoder; }
    int seekCount() const { return m_seeks; } // para pruebas y diagnóstico

private:
    std::shared_ptr<media::VideoFrame> decodeNext();
    void seekTo(TimeUs t);

    media::VideoDecoder m_decoder;
    std::shared_ptr<media::VideoFrame> m_current;
    std::shared_ptr<media::VideoFrame> m_next;
    bool m_nextValid = false; // m_next ya se intentó decodificar (nullptr = fin)
    int m_seeks = 0;
};

} // namespace movix::engine
