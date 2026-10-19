#include "render/MaskRenderer.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace movix::render {

namespace {

// Contorno de una máscara en píxeles de la imagen (mismo sistema que el shader: centro en x/y de
// la capa, caja de width × height, giro horario en grados).
QPainterPath outline(const core::LayerMask& m, int w, int h)
{
    QPainterPath path;
    const double bw = m.width * w, bh = m.height * h;
    switch (m.shape) {
    case core::MaskShape::Rectangle:
        path.addRect(QRectF(-bw / 2, -bh / 2, bw, bh));
        break;
    case core::MaskShape::Ellipse:
        path.addEllipse(QPointF(0, 0), bw / 2, bh / 2);
        break;
    case core::MaskShape::Linear: { // semiplano inferior de la línea
        const double big = 4.0 * (w + h);
        path.addRect(QRectF(-big, 0, 2 * big, big));
        break;
    }
    case core::MaskShape::Path: {
        const auto& pts = m.points;
        if (pts.size() < 3)
            break;
        auto at = [&](double x, double y) { return QPointF(x * bw, y * bh); };
        path.moveTo(at(pts[0].x, pts[0].y));
        for (size_t i = 1; i <= pts.size(); ++i) {
            const core::PathPoint& a = pts[i - 1];
            const core::PathPoint& b = pts[i % pts.size()];
            path.cubicTo(at(a.x + a.outX, a.y + a.outY), at(b.x + b.inX, b.y + b.inY), at(b.x, b.y));
        }
        path.closeSubpath();
        break;
    }
    case core::MaskShape::None:
        break;
    }
    QTransform t;
    t.translate(m.x * w, m.y * h);
    t.rotate(m.rotation);
    return t.map(path);
}

// Desenfoque de caja separable de ancho `radius * 2 + 1` (sumas acumuladas, coste independiente del radio).
void boxBlur(std::vector<float>& img, int w, int h, int radius)
{
    if (radius <= 0)
        return;
    std::vector<float> tmp(img.size());
    const float norm = 1.0f / float(2 * radius + 1);
    auto pass = [&](const std::vector<float>& src, std::vector<float>& dst, int len, int lines, int step, int stride) {
        for (int line = 0; line < lines; ++line) {
            const size_t base = size_t(line) * size_t(stride);
            auto px = [&](int i) { return src[base + size_t(std::clamp(i, 0, len - 1)) * size_t(step)]; };
            float sum = 0;
            for (int i = -radius; i <= radius; ++i)
                sum += px(i);
            for (int i = 0; i < len; ++i) {
                dst[base + size_t(i) * size_t(step)] = sum * norm;
                sum += px(i + radius + 1) - px(i - radius);
            }
        }
    };
    pass(img, tmp, w, h, 1, w); // horizontal
    pass(tmp, img, h, w, w, 1); // vertical
}

} // namespace

std::shared_ptr<const engine::MaskImage> QtMaskRenderer::render(const std::vector<core::LayerMask>& masks, int width, int height)
{
    auto out = std::make_shared<engine::MaskImage>();
    out->width = width;
    out->height = height;
    const size_t n = size_t(width) * size_t(height);
    std::vector<float> result(n, 0.0f);
    bool first = true;
    for (const core::LayerMask& m : masks) {
        if (m.shape == core::MaskShape::None)
            continue;
        QImage img(width, height, QImage::Format_Grayscale8);
        img.fill(0);
        {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            p.fillPath(outline(m, width, height), Qt::white);
        }
        std::vector<float> cov(n);
        for (int y = 0; y < height; ++y) {
            const uchar* line = img.constScanLine(y);
            for (int x = 0; x < width; ++x)
                cov[size_t(y) * size_t(width) + size_t(x)] = line[x] / 255.0f;
        }
        boxBlur(cov, width, height, int(std::lround(m.feather * height / 2)));
        for (size_t i = 0; i < n; ++i) {
            const float v = m.invert ? 1.0f - cov[i] : cov[i];
            if (first)
                result[i] = m.mode == core::MaskMode::Subtract ? 1.0f - v : v;
            else if (m.mode == core::MaskMode::Add)
                result[i] = std::max(result[i], v);
            else if (m.mode == core::MaskMode::Subtract)
                result[i] *= 1.0f - v;
            else
                result[i] *= v;
        }
        first = false;
    }
    if (first) // sin máscaras: todo visible
        std::fill(result.begin(), result.end(), 1.0f);
    out->alpha.resize(n);
    for (size_t i = 0; i < n; ++i)
        out->alpha[i] = uint8_t(std::lround(std::clamp(result[i], 0.0f, 1.0f) * 255));
    return out;
}

} // namespace movix::render
