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
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;

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
    // Lo mismo convertido en la GPU a NV12 (BT.709, rango limitado): plano Y de width×height y
    // UV intercalado de la mitad. Menos de la mitad de datos que leer y sin conversión en la CPU.
    // Solo SDR (sin `deep`) y con ancho y alto pares; si no, devuelve false.
    bool renderNv12(const engine::FrameBundle& bundle, std::vector<uint8_t>& nv12);
    bool supportsNv12() const { return !m_deep && m_width % 2 == 0 && m_height % 2 == 0; }
    // Dispositivo Direct3D 11 del compositor (ID3D11Device*), preparado para decodificar en él desde
    // otro hilo (copia cero); nullptr si no es D3D11.
    void* d3d11Device() const;
    bool isDeep() const { return m_deep; }
    QString backendName() const;
    int lastPassCount() const; // pasadas de GPU del último fotograma (para pruebas)

private:
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<Compositor> m_compositor;
    bool initNv12();
    bool prepareFrame(const engine::FrameBundle& bundle, QRhiCommandBuffer*& cb);
    struct Plane {
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiTextureRenderTarget> target;
        std::unique_ptr<QRhiRenderPassDescriptor> pass;
        std::unique_ptr<QRhiBuffer> ubo;
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    };
    Plane m_y, m_uv;
    std::unique_ptr<QRhiSampler> m_sampler;
    int m_width = 0;
    int m_height = 0;
    bool m_deep = false;
};

} // namespace movix::render
