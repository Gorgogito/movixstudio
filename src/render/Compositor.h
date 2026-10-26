#pragma once

#include "engine/Sources.h"

#include <QColor>
#include <QRect>
#include <QRectF>
#include <QSize>

#include <memory>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiResourceUpdateBatch;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QRhiViewport;

namespace movix::render {

// Compositor de la línea de tiempo en la GPU (Qt RHI). Compone las capas, de abajo arriba, en una
// textura propia del tamaño del lienzo (en coma flotante si la GPU lo permite) y al final la copia
// al rectángulo del lienzo dentro del destino. Cada capa parte ajustada al lienzo conservando su
// aspecto y se le aplican recorte, clave de croma, corrección de color (LUT 3D), máscara,
// transformación, opacidad y modo de fusión. Los modos distintos de "normal" leen una copia del
// lienzo bajo la capa, así que cada uno abre una pasada más.
// Lo usan tanto el visor (destino = ventana) como la exportación (destino = textura).
class Compositor {
public:
    explicit Compositor(QRhi* rhi);
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Debe llamarse antes de dibujar y cada vez que cambie el formato del destino final.
    void setRenderTarget(QRhiRenderPassDescriptor* renderPass, int sampleCount);

    // Prepara texturas y uniformes en `updates` (que `render` envía en la primera pasada).
    // `canvasRect` es el lienzo dentro del destino, en píxeles (se redondea a píxeles enteros).
    void prepare(const engine::FrameBundle& bundle, QRhiResourceUpdateBatch* updates, QSize targetSize, QRectF canvasRect);
    // Registra todas las pasadas: las del lienzo y la final sobre `target`, que se borra con
    // `clearColor`. `afterFinal` (opcional) se envía al terminar la pasada final (lecturas).
    void render(QRhiCommandBuffer* cb, QRhiRenderTarget* target, const QColor& clearColor, QRhiResourceUpdateBatch* updates,
                QRhiResourceUpdateBatch* afterFinal = nullptr);

    // Dibuja el último lienzo compuesto (reducido o ampliado) en `target`, de formato RGBA8 y una
    // muestra, en una pasada propia; `afterPass` se envía al terminarla (lecturas para los scopes).
    // Debe llamarse después de render() en el mismo fotograma.
    void copyCanvas(QRhiCommandBuffer* cb, QRhiTextureRenderTarget* target, QRhiResourceUpdateBatch* afterPass);

    // Con `retain`, el lienzo intermedio se compone siempre (lo necesitan los scopes). Si no, y
    // ninguna capa usa un modo de fusión, las capas se dibujan directamente en el destino.
    void setRetainCanvas(bool retain) { m_retainCanvas = retain; }
    // Destino SDR (el visor): en proyectos HDR, la copia final convierte la señal a SDR para
    // mostrarla. La exportación no lo activa y conserva la señal HDR.
    void setSdrDisplay(bool sdr) { m_sdrDisplay = sdr; }
    // Destino HDR en scRGB (coma flotante, luz lineal con primarias Rec.709 y 1,0 = 80 nits): la
    // copia final da nits absolutos. Los proyectos SDR se ven con el blanco SDR de la pantalla
    // (`sdrWhiteNits`) y los HDR se comprimen por encima de su pico (`peakNits`). Tiene prioridad
    // sobre setSdrDisplay.
    void setHdrDisplay(bool hdr, float sdrWhiteNits = 200.0f, float peakNits = 1000.0f)
    {
        m_hdrDisplay = hdr;
        m_sdrWhiteNits = sdrWhiteNits;
        m_peakNits = peakNits;
    }

    // Textura con el último lienzo compuesto (para los scopes); nullptr si no hay lienzo o si el
    // último fotograma se dibujó directamente en el destino.
    QRhiTexture* canvasTexture() const;
    QRect canvasRect() const { return m_canvasRect; }
    // Número de pasadas del último fotograma (para pruebas).
    int lastPassCount() const { return m_lastPassCount; }

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
    bool ensureCanvas(QSize size);
    void ensureSlotTextures(Slot& slot, const media::VideoFrame& frame);
    void ensureSlotLut(Slot& slot, const std::shared_ptr<const color::Lut3D>& lut, QRhiResourceUpdateBatch* updates);
    void ensureSlotMask(Slot& slot, const std::shared_ptr<const engine::MaskImage>& mask, QRhiResourceUpdateBatch* updates);
    void prepareNested(Slot& slot, const engine::FrameBundle& nested, QSize size);
    void updateSlotBindings(Slot& slot);
    std::unique_ptr<QRhiShaderResourceBindings> layerLayout();
    void drawQuad(QRhiCommandBuffer* cb, QRhiGraphicsPipeline* pipeline, QRhiShaderResourceBindings* srb, QSize viewport);
    void drawQuad(QRhiCommandBuffer* cb, QRhiGraphicsPipeline* pipeline, QRhiShaderResourceBindings* srb, const QRhiViewport& viewport);

    QRhi* m_rhi;
    QRhiRenderPassDescriptor* m_targetPass = nullptr;
    int m_sampleCount = 1;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiTexture> m_dummy2D;
    std::unique_ptr<QRhiTexture> m_dummy3D;

    // Lienzo interno, su copia para los modos de fusión y sus pipelines.
    std::unique_ptr<QRhiTexture> m_canvas;
    std::unique_ptr<QRhiTexture> m_backdrop;
    std::unique_ptr<QRhiTextureRenderTarget> m_canvasTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_canvasPass;
    std::unique_ptr<QRhiGraphicsPipeline> m_layerPipeline;
    std::unique_ptr<QRhiGraphicsPipeline> m_solidPipeline;
    std::unique_ptr<QRhiBuffer> m_solidUbuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_solidSrb;
    int m_canvasFormat = 0; // QRhiTexture::Format

    // Pasada final (y dibujo directo en el destino).
    std::unique_ptr<QRhiGraphicsPipeline> m_blitPipeline;
    std::unique_ptr<QRhiGraphicsPipeline> m_layerPipelineDirect;
    std::unique_ptr<QRhiGraphicsPipeline> m_solidPipelineDirect;
    bool m_retainCanvas = false;
    bool m_transparent = false; // compositor de una secuencia anidada: fondo transparente
    bool m_sdrDisplay = false;
    bool m_displayConvert = false;
    bool m_hdrDisplay = false;
    float m_sdrWhiteNits = 200.0f;
    float m_peakNits = 1000.0f;
    bool m_linear = false;     // el lienzo guarda luz lineal (ajuste del proyecto)
    int m_projectSignal = 0;   // señal de trabajo del proyecto (core::ColorMode)
    bool m_canvasValid = false;
    std::unique_ptr<QRhiBuffer> m_blitUbuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_blitSrb;

    // Copia reducida del lienzo (scopes).
    std::unique_ptr<QRhiGraphicsPipeline> m_copyPipeline;
    QRhiRenderPassDescriptor* m_copyPass = nullptr;
    std::unique_ptr<QRhiBuffer> m_copyUbuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_copySrb;
    QRhiTexture* m_copySource = nullptr;

    std::vector<std::unique_ptr<Slot>> m_slots;
    size_t m_activeLayers = 0;
    QRect m_canvasRect;
    QSize m_targetSize;
    bool m_vbufUploaded = false;
    int m_lastPassCount = 0;
};

} // namespace movix::render
