#include "Waveform.h"

#include "media/AudioDecoder.h"

#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QThreadPool>

#include <algorithm>
#include <cmath>

WaveformCache* WaveformCache::instance()
{
    static WaveformCache cache;
    return &cache;
}

std::shared_ptr<const std::vector<float>> WaveformCache::peaks(const QString& path)
{
    QMutexLocker lock(&m_mutex);
    if (auto it = m_peaks.find(path); it != m_peaks.end())
        return it.value();
    if (m_pending.contains(path) || path.isEmpty())
        return nullptr;
    m_pending.insert(path);
    lock.unlock();

    QThreadPool::globalInstance()->start([this, path] {
        // Mono a 8 kHz basta para los picos y decodifica muy rápido.
        constexpr int kRate = 8000;
        constexpr int kSamplesPerPeak = kRate / kPeaksPerSecond;
        auto result = std::make_shared<std::vector<float>>();
        movix::media::AudioDecoder decoder;
        if (decoder.open(path.toStdString(), kRate, 1)) {
            movix::media::AudioChunk chunk;
            float peak = 0;
            int count = 0;
            while (decoder.read(chunk)) {
                for (float s : chunk.samples) {
                    peak = std::max(peak, std::fabs(s));
                    if (++count == kSamplesPerPeak) {
                        result->push_back(std::min(peak, 1.0f));
                        peak = 0;
                        count = 0;
                    }
                }
            }
            if (count > 0)
                result->push_back(std::min(peak, 1.0f));
        }
        {
            QMutexLocker l(&m_mutex);
            m_peaks.insert(path, result);
            m_pending.remove(path);
        }
        QMetaObject::invokeMethod(this, [this, path] { emit ready(path); }, Qt::QueuedConnection);
    });
    return nullptr;
}

WaveformItem::WaveformItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    connect(WaveformCache::instance(), &WaveformCache::ready, this, [this](const QString& path) {
        if (path == m_source) {
            m_peaks = WaveformCache::instance()->peaks(path);
            update();
        }
    });
}

void WaveformItem::setSource(const QString& s)
{
    if (s == m_source)
        return;
    m_source = s;
    m_peaks = WaveformCache::instance()->peaks(s);
    update();
    emit changed();
}

void WaveformItem::setSourceIn(double s)
{
    if (qFuzzyCompare(s, m_sourceIn))
        return;
    m_sourceIn = s;
    update();
    emit changed();
}

void WaveformItem::setDuration(double seconds)
{
    if (qFuzzyCompare(seconds, m_duration))
        return;
    m_duration = seconds;
    update();
    emit changed();
}

void WaveformItem::setColor(const QColor& c)
{
    if (c == m_color)
        return;
    m_color = c;
    update();
    emit changed();
}

void WaveformItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update();
}

QSGNode* WaveformItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    auto* node = static_cast<QSGGeometryNode*>(old);
    const double w = width(), h = height();
    if (!m_peaks || m_peaks->empty() || w < 1 || h < 2 || m_duration <= 0) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawLines);
        geometry->setLineWidth(1);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* material = new QSGFlatColorMaterial;
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    static_cast<QSGFlatColorMaterial*>(node->material())->setColor(m_color);

    // Una línea vertical cada 2 px con el pico máximo del tramo de audio que cubre.
    const int step = 2;
    const int lines = int(w / step) + 1;
    QSGGeometry* geometry = node->geometry();
    geometry->allocate(lines * 2);
    auto* v = geometry->vertexDataAsPoint2D();
    const auto& peaks = *m_peaks;
    const double peaksPerPixel = m_duration * WaveformCache::kPeaksPerSecond / w;
    const double mid = h / 2;
    for (int i = 0; i < lines; ++i) {
        const double x = i * step;
        const double first = m_sourceIn * WaveformCache::kPeaksPerSecond + x * peaksPerPixel;
        const auto a = size_t(std::max(0.0, first));
        const auto b = std::min(peaks.size(), size_t(std::max(0.0, first + step * peaksPerPixel)) + 1);
        float peak = 0;
        for (size_t k = a; k < b; ++k)
            peak = std::max(peak, peaks[k]);
        const float half = float(std::max(0.5, peak * (mid - 1)));
        v[i * 2].set(float(x), float(mid) - half);
        v[i * 2 + 1].set(float(x), float(mid) + half);
    }
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    return node;
}
