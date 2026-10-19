#pragma once

#include <QQuickImageProvider>

#include <algorithm>
#include <cmath>

// Disco de las ruedas de color ("image://colorwheel/<lado>"): cada punto muestra el color de su
// posición en el plano Cb/Cr (el mismo que el vectorscopio), con luminancia media.
class ColorWheelProvider : public QQuickImageProvider {
public:
    ColorWheelProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override
    {
        int n = requested.width() > 0 ? requested.width() : id.toInt();
        n = std::clamp(n, 8, 1024);
        QImage img(n, n, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        const double r0 = n / 2.0;
        for (int y = 0; y < n; ++y) {
            auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < n; ++x) {
                const double px = (x + 0.5 - r0) / r0, py = (y + 0.5 - r0) / r0;
                const double d = std::hypot(px, py);
                if (d > 1.0)
                    continue;
                const double cb = px * 0.45, cr = -py * 0.45, luma = 0.5;
                const double r = luma + 1.5748 * cr, b = luma + 1.8556 * cb;
                const double g = (luma - 0.2126 * r - 0.0722 * b) / 0.7152;
                const double a = std::min(1.0, (1.0 - d) * r0); // borde suavizado
                auto ch = [&](double v) { return int(std::lround(std::clamp(v * 0.8, 0.0, 1.0) * 255 * a)); };
                line[x] = qRgba(ch(r), ch(g), ch(b), int(std::lround(255 * a)));
            }
        }
        if (size)
            *size = img.size();
        return img;
    }
};
