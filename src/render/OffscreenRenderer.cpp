#include "render/OffscreenRenderer.h"

#include "render/Compositor.h"

#include <QCoreApplication>
#include <rhi/qrhi.h>

#include <cstring>

namespace movix::render {

OffscreenRenderer::OffscreenRenderer() = default;

OffscreenRenderer::~OffscreenRenderer()
{
    // Los recursos deben liberarse antes que el dispositivo.
    m_compositor.reset();
    m_target.reset();
    m_renderPass.reset();
    m_texture.reset();
    m_rhi.reset();
}

bool OffscreenRenderer::init(int width, int height, QString* error)
{
    m_width = width;
    m_height = height;
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

    m_texture.reset(m_rhi->newTexture(QRhiTexture::RGBA8, QSize(width, height), 1,
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
    cb->beginPass(m_target.get(), Qt::black, {1.0f, 0}, updates);
    m_compositor->draw(cb, size);

    QRhiReadbackResult result;
    QRhiResourceUpdateBatch* readback = m_rhi->nextResourceUpdateBatch();
    readback->readBackTexture(QRhiReadbackDescription(m_texture.get()), &result);
    cb->endPass(readback);
    // En un fotograma fuera de pantalla, endOffscreenFrame espera a que la GPU termine.
    if (m_rhi->endOffscreenFrame() != QRhi::FrameOpSuccess)
        return false;

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

QString OffscreenRenderer::backendName() const
{
    return m_rhi ? QString::fromLatin1(m_rhi->backendName()) + QStringLiteral(" · ") + QString::fromUtf8(m_rhi->driverInfo().deviceName)
                 : QString();
}

} // namespace movix::render
