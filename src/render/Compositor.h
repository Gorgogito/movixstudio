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
// un rectángulo del destino y apila encima las capas de abajo arriba. Cada capa parte ajustada al
// lienzo conservando su aspecto y se le aplica su transformación (desplazamiento, escala,
// rotación), su opacidad y su recorte; lo que sale del lienzo no se dibuja.
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

    // Rectángulo con relación de aspecto `aspect` centrado y ajustado dentro de `target`.
    static QRectF fitRect(QSizeF target, double aspect);

    // Geometría de una capa sobre el lienzo (en las mismas unidades que `canvas`): centro,
    // semiejes ya escalados y rotación en grados. La usa también la interfaz para los tiradores.
    struct LayerGeometry {
        QPointF center;
        QSizeF halfSize;
        double rotation = 0.0;
    };
    static LayerGeometry layerGeometry(const QRectF& canvas, double frameAspect, const core::LayerTransform& t);

private:
    struct Slot;
    void ensureCommon();
    void ensureSlotTextures(Slot& slot, const media::VideoFrame& frame);

    QRhi* m_rhi;
    QRhiRenderPassDescriptor* m_renderPass = nullptr;
    int m_sampleCount = 1;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiGraphicsPipeline> m_nv12Pipeline;
    std::unique_ptr<QRhiGraphicsPipeline> m_rgbaPipeline;
    std::unique_ptr<QRhiGraphicsPipeline> m_solidPipeline;
    std::unique_ptr<QRhiBuffer> m_solidUbuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_solidSrb;
    std::unique_ptr<QRhiTexture> m_dummy;
    std::vector<std::unique_ptr<Slot>> m_slots;
    size_t m_activeLayers = 0;
    QRectF m_canvas;
    bool m_vbufUploaded = false;
};

} // namespace movix::render
