#pragma once

#include "engine/Sources.h"

#include <QRectF>
#include <QSize>

#include <memory>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiResourceUpdateBatch;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

namespace movix::render {

// Compositor de la línea de tiempo en la GPU (Qt RHI). Dibuja el lienzo del proyecto dentro de
// un rectángulo del destino y apila encima las capas de video de abajo arriba; cada capa se
// ajusta al lienzo conservando su relación de aspecto (las bandas dejan ver la capa inferior).
// Lo usan tanto el visor (destino = ventana) como la exportación (destino = textura).
class Compositor {
public:
    explicit Compositor(QRhi* rhi);
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Debe llamarse antes de dibujar y cada vez que cambie el formato del destino.
    void setRenderTarget(QRhiRenderPassDescriptor* renderPass, int sampleCount);

    // Sube texturas y uniformes (fuera del pase de render).
    void prepare(const engine::FrameBundle& bundle, QRhiResourceUpdateBatch* updates, QSize targetSize, QRectF canvasRect);
    // Registra el dibujo (dentro de beginPass/endPass).
    void draw(QRhiCommandBuffer* cb, QSize targetSize);

    // Rectángulo que ocupa un lienzo de canvasW×canvasH centrado y ajustado dentro de `target`.
    static QRectF fitRect(QSizeF target, double aspect);

private:
    struct Slot;
    void ensureCommon();
    void ensureSlotTextures(Slot& slot, int width, int height);

    QRhi* m_rhi;
    QRhiRenderPassDescriptor* m_renderPass = nullptr;
    int m_sampleCount = 1;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiGraphicsPipeline> m_layerPipeline;
    std::unique_ptr<QRhiGraphicsPipeline> m_solidPipeline;
    std::unique_ptr<QRhiBuffer> m_solidUbuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_solidSrb;
    std::unique_ptr<QRhiTexture> m_dummy;
    std::vector<std::unique_ptr<Slot>> m_slots;
    size_t m_activeLayers = 0;
    bool m_vbufUploaded = false;
    bool m_drawCanvas = false;
};

} // namespace movix::render
