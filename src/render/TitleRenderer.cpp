#include "render/TitleRenderer.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <cmath>
#include <cstring>

namespace movix::render {

namespace {
QColor fromArgb(std::uint32_t c)
{
    return QColor::fromRgba(QRgb(c));
}
} // namespace

std::shared_ptr<const media::VideoFrame> QtTitleRenderer::render(const core::TitleStyle& s, int w, int h)
{
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

        QFont font(QString::fromStdString(s.font));
        const double pixelSize = std::max(1.0, s.size * h);
        font.setPixelSize(int(std::lround(pixelSize)));
        font.setBold(s.bold);
        font.setItalic(s.italic);
        const QFontMetricsF metrics(font);

        // Bloque de líneas centrado en el lienzo; cada línea alineada dentro del bloque.
        const QStringList lines = QString::fromStdString(s.text).split(QLatin1Char('\n'));
        double blockWidth = 0;
        for (const QString& line : lines)
            blockWidth = std::max(blockWidth, metrics.horizontalAdvance(line));
        const double lineHeight = metrics.lineSpacing();
        const double blockHeight = lineHeight * double(lines.size());
        const double left = (w - blockWidth) / 2;
        const double top = (h - blockHeight) / 2;

        QPainterPath path;
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const double lineWidth = metrics.horizontalAdvance(lines[i]);
            double x = left;
            if (s.align == 1)
                x += (blockWidth - lineWidth) / 2;
            else if (s.align == 2)
                x += blockWidth - lineWidth;
            path.addText(QPointF(x, top + double(i) * lineHeight + metrics.ascent()), font, lines[i]);
        }

        if ((s.background >> 24) != 0) {
            const double pad = pixelSize * 0.3;
            painter.setPen(Qt::NoPen);
            painter.setBrush(fromArgb(s.background));
            painter.drawRoundedRect(QRectF(left - pad, top - pad * 0.6, blockWidth + 2 * pad, blockHeight + 1.2 * pad), pad * 0.5, pad * 0.5);
        }
        if (s.shadow) {
            const double offset = std::max(1.0, pixelSize * 0.05);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 150));
            painter.drawPath(path.translated(offset, offset));
        }
        if (s.outline > 0) {
            QPen pen(fromArgb(s.outlineColor), 2.0 * s.outline * pixelSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.strokePath(path, pen); // el relleno posterior tapa la mitad interior del trazo
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fromArgb(s.color));
        painter.drawPath(path);
    }

    // RGBA sin premultiplicar, como esperan el compositor y la mezcla "sobre".
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    auto frame = std::make_shared<media::VideoFrame>();
    frame->format = media::PixelFormat::RGBA;
    frame->width = w;
    frame->height = h;
    frame->rgba.resize(size_t(w) * size_t(h) * 4);
    for (int row = 0; row < h; ++row)
        std::memcpy(frame->rgba.data() + size_t(row) * size_t(w) * 4, rgba.constScanLine(row), size_t(w) * 4);
    return frame;
}

} // namespace movix::render
