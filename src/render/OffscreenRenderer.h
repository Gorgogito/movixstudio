#pragma once

#include "engine/Sources.h"

#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

class QRhi;
class QRhiRenderPassDescriptor;
class QRhiTexture;
class QRhiTextureRenderTarget;

namespace movix::render {

class Compositor;

// Composición fuera de pantalla con lectura de vuelta a memoria: el mismo compositor que el
// visor, pero sobre una textura del tamaño del proyecto. Se usa para exportar.
// Crea su propio dispositivo gráfico, así que puede vivir en un hilo de trabajo.
class OffscreenRenderer {
public:
    OffscreenRenderer();
    ~OffscreenRenderer();

    bool init(int width, int height, QString* error = nullptr);
    // Compone el fotograma y deja en `rgba` width×height×4 bytes (filas de arriba abajo).
    bool render(const engine::FrameBundle& bundle, std::vector<uint8_t>& rgba);
    QString backendName() const;

private:
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<Compositor> m_compositor;
    int m_width = 0;
    int m_height = 0;
};

} // namespace movix::render
