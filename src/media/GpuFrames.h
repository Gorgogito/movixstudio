#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Fotogramas decodificados que se quedan en la GPU (copia cero, Windows/Direct3D 11): el
// decodificador escribe en sus propias texturas y cada fotograma se copia, dentro de la GPU, a una
// textura NV12 (o P010) de un grupo que el compositor puede leer directamente. La textura vuelve al
// grupo cuando se libera el último fotograma que la usa.
namespace movix::media {

class GpuTexturePool {
public:
    // `device`: ID3D11Device*. `deep`: P010 (10 bits o más) en lugar de NV12.
    GpuTexturePool(void* device, int width, int height, bool deep);
    ~GpuTexturePool();
    GpuTexturePool(const GpuTexturePool&) = delete;
    GpuTexturePool& operator=(const GpuTexturePool&) = delete;

    bool matches(int width, int height, bool deep) const { return m_width == width && m_height == height && m_deep == deep; }
    // Copia la capa `index` de la textura del decodificador (`source`, ID3D11Texture2D*) a una
    // textura libre. Devuelve la textura y lo que la mantiene ocupada; nullptr si falla.
    void* copyFrom(void* source, int index, std::shared_ptr<void>& hold);

private:
    struct Shared;
    std::shared_ptr<Shared> m_shared; // también lo comparten los fotogramas en vuelo
    int m_width = 0;
    int m_height = 0;
    bool m_deep = false;
};

// Prepara un dispositivo para usarse a la vez desde el hilo que decodifica y el que compone.
bool enableMultithreadProtection(void* device);

} // namespace movix::media
