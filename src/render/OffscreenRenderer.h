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

    // `deep`: destino en coma flotante; render() entrega entonces RGBA de 16 bits por canal.
    bool init(int width, int height, QString* error = nullptr, bool deep = false);
    // Compone el fotograma y deja en `rgba` las filas de arriba abajo: width×height×4 bytes, o
    // width×height×4 valores de 16 bits (little endian) si se inició con `deep`.
    bool render(const engine::FrameBundle& bundle, std::vector<uint8_t>& rgba);
    bool isDeep() const { return m_deep; }
    QString backendName() const;
    int lastPassCount() const; // pasadas de GPU del último fotograma (para pruebas)

private:
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<Compositor> m_compositor;
    int m_width = 0;
    int m_height = 0;
    bool m_deep = false;
};

} // namespace movix::render
