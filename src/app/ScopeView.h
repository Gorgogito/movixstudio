#pragma once

#include "VideoView.h"

#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

// Un scope (forma de onda, desfile RGB, vectorscopio o histograma) de lo que muestra un visor.
// Mientras es visible, pide al visor la copia reducida del lienzo y la analiza al llegar.
class ScopeView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(VideoView* source READ source WRITE setSource NOTIFY sourceChanged)
    // 0 forma de onda, 1 desfile RGB, 2 vectorscopio, 3 histograma (movix::color::ScopeType).
    Q_PROPERTY(int type READ type WRITE setType NOTIFY typeChanged)

public:
    explicit ScopeView(QQuickItem* parent = nullptr);

    VideoView* source() const { return m_source; }
    void setSource(VideoView* view);
    int type() const { return m_type; }
    void setType(int type);

    void paint(QPainter* painter) override;

signals:
    void sourceChanged();
    void typeChanged();

private:
    void poll();
    void recompute();
    void drawGraticule(QPainter* p, const QRectF& area) const;

    QPointer<VideoView> m_source;
    int m_type = 0;
    QTimer m_timer;
    uint64_t m_seq = 0;
    QByteArray m_rgba;
    QSize m_size;
    QImage m_image;
};
