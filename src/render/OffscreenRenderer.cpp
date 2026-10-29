#include "render/OffscreenRenderer.h"

#include "render/Compositor.h"
#include "media/FrameConvert.h"
#include "media/GpuFrames.h"

#include <QCoreApplication>
#include <QFile>
#include <rhi/qrhi.h>

#include <QFloat16>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace movix::render {

OffscreenRenderer::OffscreenRenderer() = default;

OffscreenRenderer::~OffscreenRenderer()
{
    // Los recursos deben liberarse antes que el dispositivo.
    m_y = {};
    m_uv = {};
    m_sampler.reset();
    m_compositor.reset();
    m_target.reset();
    m_renderPass.reset();
    m_texture.reset();
    m_rhi.reset();
}

bool OffscreenRenderer::init(int width, int height, QString* error, bool deep)
{
    m_width = width;
    m_height = height;
    m_deep = deep;
#ifdef Q_OS_WIN
    QRhiD3D11InitParams params;
    m_rhi.reset(QRhi::create(QRhi::D3D11, &params));
    if (!m_rhi) // sin GPU utilizable: rasterizador por software de Direct3D (WARP)
        m_rhi.reset(QRhi::create(QRhi::D3D11, &params, QRhi::PreferSoftwareRenderer));
#endif
    if (!m_rhi) {
        if (error)
            *error = QCoreApplication::translate("OffscreenRenderer", "No se pudo inicializar la GPU para renderizar.");
        return false;
    }

    m_texture.reset(m_rhi->newTexture(deep ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8, QSize(width, height), 1,
                                      QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!m_texture->create()) {
        if (error)
            *error = QCoreApplication::translate("OffscreenRenderer", "La GPU no admite una textura de %1×%2.").arg(width).arg(height);
        return false;
    }
    m_target.reset(m_rhi->newTextureRenderTarget({m_texture.get()}));
    m_renderPass.reset(m_target->newCompatibleRenderPassDescriptor());
    m_target->setRenderPassDescriptor(m_renderPass.get());
    m_target->create();

    m_compositor = std::make_unique<Compositor>(m_rhi.get());
    m_compositor->setRenderTarget(m_renderPass.get(), 1);
    return true;
}

bool OffscreenRenderer::render(const engine::FrameBundle& bundle, std::vector<uint8_t>& rgba)
{
    if (!m_rhi)
        return false;
    QRhiCommandBuffer* cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return false;

    const QSize size(m_width, m_height);
    QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
    // Si la salida tiene otra relación de aspecto que el proyecto, el lienzo se centra con bandas.
    const double aspect = bundle.canvasHeight > 0 ? double(bundle.canvasWidth) / bundle.canvasHeight : double(m_width) / m_height;
    m_compositor->prepare(bundle, updates, size, Compositor::fitRect(QSizeF(size), aspect));
    QRhiReadbackResult result;
    QRhiResourceUpdateBatch* readback = m_rhi->nextResourceUpdateBatch();
    readback->readBackTexture(QRhiReadbackDescription(m_texture.get()), &result);
    m_compositor->render(cb, m_target.get(), m_transparent ? QColor(0, 0, 0, 0) : QColor(Qt::black), updates, readback);
    // En un fotograma fuera de pantalla, endOffscreenFrame espera a que la GPU termine.
    if (m_rhi->endOffscreenFrame() != QRhi::FrameOpSuccess)
        return false;

    if (m_deep) { // medios flotantes → enteros de 16 bits
        const size_t values = size_t(m_width) * size_t(m_height) * 4;
        if (size_t(result.data.size()) < values * sizeof(qfloat16))
            return false;
        rgba.resize(values * 2);
        const auto* half = reinterpret_cast<const qfloat16*>(result.data.constData());
        auto* out = reinterpret_cast<uint16_t*>(rgba.data());
        const size_t rowValues = size_t(m_width) * 4;
        for (int row = 0; row < m_height; ++row) {
            const int srcRow = m_rhi->isYUpInFramebuffer() ? m_height - 1 - row : row;
            for (size_t i = 0; i < rowValues; ++i) {
                const float v = std::clamp(float(half[size_t(srcRow) * rowValues + i]), 0.0f, 1.0f);
                out[size_t(row) * rowValues + i] = uint16_t(std::lround(v * 65535.0f));
            }
        }
        return true;
    }
    const size_t rowBytes = size_t(m_width) * 4;
    rgba.resize(rowBytes * size_t(m_height));
    const auto* src = reinterpret_cast<const uint8_t*>(result.data.constData());
    if (size_t(result.data.size()) < rgba.size())
        return false;
    if (m_rhi->isYUpInFramebuffer()) { // OpenGL: la primera fila leída es la inferior
        for (int row = 0; row < m_height; ++row)
            std::memcpy(rgba.data() + size_t(row) * rowBytes, src + size_t(m_height - 1 - row) * rowBytes, rowBytes);
    } else {
        std::memcpy(rgba.data(), src, rgba.size());
    }
    return true;
}

bool OffscreenRenderer::initNv12()
{
    auto shader = [](const QString& name) {
        QFile f(QStringLiteral(":/movix/shaders/") + name);
        return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
    };
    const QShader vert = shader(QStringLiteral("fullscreen.vert.qsb"));
    const QShader frag = shader(QStringLiteral("nv12.frag.qsb"));
    if (!vert.isValid() || !frag.isValid())
        return false;
    m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
    if (!m_sampler->create())
        return false;
    auto make = [&](Plane& plane, QRhiTexture::Format format, QSize size) {
        plane.texture.reset(m_rhi->newTexture(format, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!plane.texture->create())
            return false;
        plane.target.reset(m_rhi->newTextureRenderTarget({plane.texture.get()}));
        plane.pass.reset(plane.target->newCompatibleRenderPassDescriptor());
        plane.target->setRenderPassDescriptor(plane.pass.get());
        plane.ubo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
        plane.srb.reset(m_rhi->newShaderResourceBindings());
        plane.srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                                         plane.ubo.get()),
                                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_texture.get(),
                                                                          m_sampler.get())});
        plane.pipeline.reset(m_rhi->newGraphicsPipeline());
        plane.pipeline->setShaderStages({{QRhiShaderStage::Vertex, vert}, {QRhiShaderStage::Fragment, frag}});
        plane.pipeline->setVertexInputLayout({});
        plane.pipeline->setShaderResourceBindings(plane.srb.get());
        plane.pipeline->setRenderPassDescriptor(plane.pass.get());
        return plane.target->create() && plane.ubo->create() && plane.srb->create() && plane.pipeline->create();
    };
    if (!make(m_y, QRhiTexture::R8, QSize(m_width, m_height)) || !make(m_uv, QRhiTexture::RG8, QSize(m_width / 2, m_height / 2))) {
        m_y = {};
        m_uv = {};
        return false;
    }
    return true;
}

bool OffscreenRenderer::renderNv12(const engine::FrameBundle& bundle, std::vector<uint8_t>& nv12)
{
    if (!m_rhi || !supportsNv12())
        return false;
    if (!m_y.pipeline && !initNv12())
        return false;
    QRhiCommandBuffer* cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return false;
    const QSize size(m_width, m_height);
    QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
    const double aspect = bundle.canvasHeight > 0 ? double(bundle.canvasWidth) / bundle.canvasHeight : double(m_width) / m_height;
    m_compositor->prepare(bundle, updates, size, Compositor::fitRect(QSizeF(size), aspect));
    m_compositor->render(cb, m_target.get(), Qt::black, updates);

    // Dos pasadas sobre el lienzo: luma a tamaño completo y croma a la mitad.
    const float flip = m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f;
    QRhiReadbackResult yResult, uvResult;
    for (Plane* plane : {&m_y, &m_uv}) {
        QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();
        const float params[4] = {plane == &m_y ? 0.0f : 1.0f, flip, 0.0f, 0.0f};
        u->updateDynamicBuffer(plane->ubo.get(), 0, sizeof params, params);
        QRhiResourceUpdateBatch* after = nullptr;
        if (plane == &m_uv) { // al final, las dos lecturas
            after = m_rhi->nextResourceUpdateBatch();
            after->readBackTexture(QRhiReadbackDescription(m_y.texture.get()), &yResult);
            after->readBackTexture(QRhiReadbackDescription(m_uv.texture.get()), &uvResult);
        }
        const QSize ps = plane->texture->pixelSize();
        cb->beginPass(plane->target.get(), Qt::black, {1.0f, 0}, u);
        cb->setGraphicsPipeline(plane->pipeline.get());
        cb->setViewport({0, 0, float(ps.width()), float(ps.height())});
        cb->setShaderResources();
        cb->draw(3);
        cb->endPass(after);
    }
    if (m_rhi->endOffscreenFrame() != QRhi::FrameOpSuccess)
        return false;

    const size_t ySize = size_t(m_width) * size_t(m_height);
    if (size_t(yResult.data.size()) < ySize || size_t(uvResult.data.size()) < ySize / 2)
        return false;
    nv12.resize(ySize + ySize / 2);
    std::memcpy(nv12.data(), yResult.data.constData(), ySize);
    std::memcpy(nv12.data() + ySize, uvResult.data.constData(), ySize / 2);
    return true;
}

void* OffscreenRenderer::d3d11Device() const
{
#ifdef Q_OS_WIN
    if (m_rhi && m_rhi->backend() == QRhi::D3D11) {
        const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(m_rhi->nativeHandles());
        if (handles && handles->dev && media::enableMultithreadProtection(handles->dev))
            return handles->dev;
    }
#endif
    return nullptr;
}

void OffscreenRenderer::setTransparent(bool transparent)
{
    m_transparent = transparent;
    if (m_compositor)
        m_compositor->setTransparent(transparent);
}

void OffscreenRenderer::setHdrDisplay(bool hdr, float sdrWhiteNits, float peakNits)
{
    if (m_compositor)
        m_compositor->setHdrDisplay(hdr, sdrWhiteNits, peakNits);
}

int OffscreenRenderer::lastPassCount() const
{
    return m_compositor ? m_compositor->lastPassCount() : 0;
}

QString OffscreenRenderer::backendName() const
{
    return m_rhi ? QString::fromLatin1(m_rhi->backendName()) + QStringLiteral(" · ") + QString::fromUtf8(m_rhi->driverInfo().deviceName)
                 : QString();
}

QtBundleRenderer::QtBundleRenderer() = default;
QtBundleRenderer::~QtBundleRenderer() = default;

std::shared_ptr<const media::VideoFrame> QtBundleRenderer::render(const engine::FrameBundle& bundle, bool deep)
{
    const int w = bundle.canvasWidth, h = bundle.canvasHeight;
    if (w <= 0 || h <= 0)
        return nullptr;
    if (!m_renderer || m_renderer->width() != w || m_renderer->height() != h || m_renderer->isDeep() != deep) {
        m_renderer = std::make_unique<OffscreenRenderer>();
        m_failed = !m_renderer->init(w, h, nullptr, deep);
        m_renderer->setTransparent(true);
    }
    if (m_failed || !m_renderer->render(bundle, m_pixels))
        return nullptr;

    // Color premultiplicado → sin premultiplicar, como las demás imágenes con transparencia.
    auto frame = std::make_shared<media::VideoFrame>();
    frame->width = w;
    frame->height = h;
    frame->fullRange = true;
    const bool hdr = bundle.colorMode != core::ColorMode::SDR;
    frame->colorSpace = hdr ? media::ColorSpace::BT2020 : media::ColorSpace::BT709;
    frame->transfer = bundle.colorMode == core::ColorMode::PQ    ? media::Transfer::PQ
                      : bundle.colorMode == core::ColorMode::HLG ? media::Transfer::HLG
                                                                 : media::Transfer::SDR;
    frame->pts = bundle.time;
    const size_t pixels = size_t(w) * size_t(h);
    if (deep) {
        frame->format = media::PixelFormat::RGBA16F;
        std::vector<float> values(pixels * 4);
        const auto* src = reinterpret_cast<const uint16_t*>(m_pixels.data());
        for (size_t i = 0; i < pixels; ++i) {
            const float a = src[i * 4 + 3] / 65535.0f;
            for (size_t c = 0; c < 3; ++c)
                values[i * 4 + c] = a > 0.0f ? std::min(1.0f, src[i * 4 + c] / 65535.0f / a) : 0.0f;
            values[i * 4 + 3] = a;
        }
        media::floatToHalf(values, frame->rgba);
    } else {
        frame->format = media::PixelFormat::RGBA;
        frame->rgba = std::move(m_pixels);
        uint8_t* p = frame->rgba.data();
        for (size_t i = 0; i < pixels; ++i, p += 4)
            if (const int a = p[3]; a > 0 && a < 255)
                for (int c = 0; c < 3; ++c)
                    p[c] = uint8_t(std::min(255, (p[c] * 255 + a / 2) / a));
        m_pixels = {};
    }
    return frame;
}

} // namespace movix::render
