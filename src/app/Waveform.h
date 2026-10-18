#pragma once

#include <QColor>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQuickItem>
#include <QSet>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

// Picos de audio de cada archivo (100 por segundo), calculados una vez en segundo plano.
class WaveformCache : public QObject {
    Q_OBJECT
public:
    static WaveformCache* instance();
    static constexpr int kPeaksPerSecond = 100;

    // Picos del archivo o nullptr si aún no están (se calculan y luego se emite ready()).
    std::shared_ptr<const std::vector<float>> peaks(const QString& path);

signals:
    void ready(const QString& path);

private:
    QMutex m_mutex;
    QHash<QString, std::shared_ptr<const std::vector<float>>> m_peaks;
    QSet<QString> m_pending;
};

// Forma de onda de un tramo de un archivo, dibujada con geometría de la GPU (sin texturas),
// así que funciona igual con clips de pocos píxeles o de decenas de miles.
class WaveformItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY changed)
    Q_PROPERTY(double sourceIn READ sourceIn WRITE setSourceIn NOTIFY changed)       // segundos
    Q_PROPERTY(double duration READ duration WRITE setDuration NOTIFY changed)       // segundos
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY changed)

public:
    explicit WaveformItem(QQuickItem* parent = nullptr);

    QString source() const { return m_source; }
    void setSource(const QString& s);
    double sourceIn() const { return m_sourceIn; }
    void setSourceIn(double s);
    double duration() const { return m_duration; }
    void setDuration(double seconds);
    QColor color() const { return m_color; }
    void setColor(const QColor& c);

signals:
    void changed();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    QString m_source;
    double m_sourceIn = 0;
    double m_duration = 0;
    QColor m_color = QColor(255, 255, 255, 170);
    std::shared_ptr<const std::vector<float>> m_peaks;
};
