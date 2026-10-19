#include "ScopeView.h"

#include "color/Scopes.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace {

using movix::color::ScopeType;

const QColor kBackground(12, 13, 16);
const QColor kGrid(255, 255, 255, 38);
const QColor kLabel(255, 255, 255, 110);

} // namespace

ScopeView::ScopeView(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setFillColor(kBackground);
    m_timer.setInterval(66);
    connect(&m_timer, &QTimer::timeout, this, &ScopeView::poll);
    m_timer.start();
    connect(this, &QQuickItem::widthChanged, this, &ScopeView::recompute);
    connect(this, &QQuickItem::heightChanged, this, &ScopeView::recompute);
}

void ScopeView::setSource(VideoView* view)
{
    if (m_source == view)
        return;
    m_source = view;
    m_seq = 0;
    emit sourceChanged();
}

void ScopeView::setType(int type)
{
    type = std::clamp(type, 0, 3);
    if (m_type == type)
        return;
    m_type = type;
    recompute();
    emit typeChanged();
}

void ScopeView::poll()
{
    if (!m_source || !isVisible())
        return;
    const auto feed = m_source->scopeFeed();
    if (feed->sequence() == m_seq)
        return;
    if (feed->latest(m_rgba, m_size, m_seq))
        recompute();
}

void ScopeView::recompute()
{
    const int w = int(width()), h = int(height());
    if (w <= 0 || h <= 0 || m_rgba.isEmpty() || m_size.isEmpty()
        || m_rgba.size() < qsizetype(m_size.width()) * m_size.height() * 4) {
        m_image = {};
        update();
        return;
    }
    const auto scope = movix::color::computeScope(ScopeType(m_type), reinterpret_cast<const uint8_t*>(m_rgba.constData()),
                                                  m_size.width(), m_size.height(), m_size.width() * 4, w, h);
    m_image = QImage(reinterpret_cast<const uchar*>(scope.pixels.data()), scope.width, scope.height, scope.width * 4,
                     QImage::Format_ARGB32_Premultiplied)
                  .copy();
    update();
}

void ScopeView::paint(QPainter* p)
{
    const QRectF area(0, 0, width(), height());
    p->setRenderHint(QPainter::Antialiasing);
    if (!m_image.isNull())
        p->drawImage(area, m_image);
    drawGraticule(p, area);
    if (m_image.isNull()) {
        p->setPen(kLabel);
        p->drawText(area, Qt::AlignCenter, tr("Sin imagen"));
    }
}

void ScopeView::drawGraticule(QPainter* p, const QRectF& a) const
{
    QFont font = p->font();
    font.setPixelSize(9);
    p->setFont(font);
    p->setPen(QPen(kGrid, 1));
    const auto type = ScopeType(m_type);
    if (type == ScopeType::Waveform || type == ScopeType::Parade) {
        for (int pct = 0; pct <= 100; pct += 25) {
            const double y = a.top() + (100 - pct) / 100.0 * (a.height() - 1);
            p->setPen(QPen(kGrid, 1));
            p->drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
            p->setPen(kLabel);
            p->drawText(QPointF(a.left() + 3, std::clamp(y - 2, a.top() + 10, a.bottom() - 2)), QString::number(pct));
        }
        if (type == ScopeType::Parade) {
            p->setPen(QPen(kGrid, 1));
            for (int i = 1; i < 3; ++i) {
                const double x = a.left() + a.width() * i / 3;
                p->drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
            }
        }
    } else if (type == ScopeType::Vectorscope) {
        const double side = std::min(a.width(), a.height());
        const QRectF sq(a.center().x() - side / 2, a.center().y() - side / 2, side, side);
        const QPointF c = sq.center();
        p->setPen(QPen(kGrid, 1));
        p->drawEllipse(c, side * 0.45, side * 0.45);
        p->drawLine(QPointF(c.x() - side * 0.45, c.y()), QPointF(c.x() + side * 0.45, c.y()));
        p->drawLine(QPointF(c.x(), c.y() - side * 0.45), QPointF(c.x(), c.y() + side * 0.45));
        // Línea del tono de piel: desde el centro hacia un tono de piel medio.
        const auto skin = movix::color::vectorscopePosition(224, 172, 138);
        const QPointF dir(skin.x - 0.5, skin.y - 0.5);
        const double len = std::hypot(dir.x(), dir.y());
        if (len > 0) {
            p->setPen(QPen(QColor(255, 200, 150, 70), 1, Qt::DashLine));
            p->drawLine(c, c + QPointF(dir.x() / len, dir.y() / len) * side * 0.45);
        }
        // Objetivos de las barras de color al 75 %.
        struct Target {
            const char* name;
            int r, g, b;
        };
        // Iniciales de rojo, amarillo, verde, cian, azul y magenta.
        static const Target kTargets[] = {{QT_TR_NOOP("R"), 191, 0, 0},   {QT_TR_NOOP("Am"), 191, 191, 0},
                                          {QT_TR_NOOP("V"), 0, 191, 0},   {QT_TR_NOOP("Ci"), 0, 191, 191},
                                          {QT_TR_NOOP("Az"), 0, 0, 191},  {QT_TR_NOOP("Mg"), 191, 0, 191}};
        for (const Target& t : kTargets) {
            const auto pt = movix::color::vectorscopePosition(t.r, t.g, t.b);
            const QPointF q(sq.left() + pt.x * side, sq.top() + pt.y * side);
            p->setPen(QPen(QColor(t.r ? 255 : 120, t.g ? 255 : 120, t.b ? 255 : 120, 150), 1));
            p->drawRect(QRectF(q.x() - 4, q.y() - 4, 8, 8));
            p->setPen(kLabel);
            p->drawText(q + QPointF(6, 3), tr(t.name));
        }
    } else {
        for (int pct = 25; pct < 100; pct += 25) {
            const double x = a.left() + pct / 100.0 * a.width();
            p->drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
        }
    }
}
