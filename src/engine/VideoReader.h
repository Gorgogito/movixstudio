#pragma once

#include "media/VideoDecoder.h"

#include <deque>
#include <memory>
#include <string>

namespace movix::engine {

// Lector de video de un clip: entrega el fotograma que se ve en cada instante del medio.
// Avanza decodificando en orden mientras las peticiones van hacia delante (reproducción) y
// solo busca cuando se salta hacia atrás o muy adelante (navegación). Hacia atrás en pasos cortos
// (clips al revés) decodifica por bloques y los guarda, para no buscar en cada fotograma.
class VideoReader {
public:
    // `gpuDevice` (ID3D11Device*): copia cero, los fotogramas se quedan en la GPU de ese dispositivo.
    bool open(const std::string& path, bool hardware, std::string* error = nullptr, void* gpuDevice = nullptr);

    // Fotograma visible en el instante `t` del medio (el último con pts ≤ t). Más allá del final
    // devuelve el último fotograma. nullptr solo si el medio no tiene ninguno.
    std::shared_ptr<const media::VideoFrame> frameAt(TimeUs t);

    const media::VideoDecoder& decoder() const { return m_decoder; }
    int seekCount() const { return m_seeks; } // para pruebas y diagnóstico

private:
    std::shared_ptr<media::VideoFrame> decodeNext();
    void seekTo(TimeUs t);
    // Al revés: se decodifica por bloques hacia atrás y se guardan los fotogramas (hasta 256 MB).
    std::shared_ptr<const media::VideoFrame> fromCache(TimeUs t) const;
    void fillBackward(TimeUs t);
    std::deque<std::shared_ptr<media::VideoFrame>> m_cache; // continuos, ordenados por pts
    size_t m_cacheBytes = 0;

    media::VideoDecoder m_decoder;
    std::shared_ptr<media::VideoFrame> m_current;
    std::shared_ptr<media::VideoFrame> m_next;
    bool m_nextValid = false; // m_next ya se intentó decodificar (nullptr = fin)
    int m_seeks = 0;
};

} // namespace movix::engine
