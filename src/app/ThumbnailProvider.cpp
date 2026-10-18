#include "ThumbnailProvider.h"

#include "media/FrameConvert.h"
#include "media/VideoDecoder.h"

#include <QRunnable>
#include <QUrl>

namespace {

class ThumbnailResponse : public QQuickImageResponse, public QRunnable {
public:
    ThumbnailResponse(ThumbnailProvider* provider, QString id, QSize size)
        : m_provider(provider), m_id(std::move(id)), m_size(size)
    {
        setAutoDelete(false);
    }

    QQuickTextureFactory* textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(m_image); }

    void run() override
    {
        const QString key = m_id + QLatin1Char('@') + QString::number(m_size.width()) + 'x' + QString::number(m_size.height());
        m_image = m_provider->cached(key);
        if (m_image.isNull()) {
            m_image = render();
            if (!m_image.isNull())
                m_provider->store(key, m_image);
        }
        emit finished();
    }

private:
    QImage render() const
    {
        const qsizetype slash = m_id.indexOf(QLatin1Char('/'));
        if (slash < 0)
            return {};
        const qint64 timeUs = m_id.left(slash).toLongLong();
        const QString path = QUrl::fromPercentEncoding(m_id.mid(slash + 1).toUtf8());

        movix::media::VideoDecoder decoder;
        // En CPU: muchas miniaturas en paralelo no deben competir por el decodificador de la GPU.
        if (!decoder.open(path.toStdString(), movix::media::VideoDecoderOptions{false}))
            return {};
        decoder.seek(timeUs);
        auto frame = decoder.decodeNext();
        if (!frame)
            return {};

        // Tamaño pedido conservando la relación de aspecto: si solo se fija una dimensión, la
        // otra se deduce; sin ninguna, 160 px de ancho.
        const double aspect = frame->displayAspect();
        int w = 160, h = int(160 / aspect + 0.5);
        if (m_size.width() > 0 && m_size.height() <= 0) {
            w = m_size.width();
            h = int(w / aspect + 0.5);
        } else if (m_size.height() > 0 && m_size.width() <= 0) {
            h = m_size.height();
            w = int(h * aspect + 0.5);
        } else if (m_size.width() > 0 && m_size.height() > 0) {
            w = m_size.width();
            h = m_size.height();
            if (double(w) / h > aspect)
                w = int(h * aspect + 0.5);
            else
                h = int(w / aspect + 0.5);
        }
        std::vector<uint8_t> rgba;
        if (!movix::media::frameToRgba(*frame, std::max(1, w), std::max(1, h), rgba))
            return {};
        QImage image(rgba.data(), std::max(1, w), std::max(1, h), std::max(1, w) * 4, QImage::Format_RGBA8888);
        return image.copy(); // separarla del buffer temporal
    }

    ThumbnailProvider* m_provider;
    QString m_id;
    QSize m_size;
    QImage m_image;
};

} // namespace

ThumbnailProvider::ThumbnailProvider()
{
    m_pool.setMaxThreadCount(std::max(2, QThread::idealThreadCount() / 2));
    m_cache.setMaxCost(64 * 1024 * 1024); // bytes
}

QQuickImageResponse* ThumbnailProvider::requestImageResponse(const QString& id, const QSize& requestedSize)
{
    auto* response = new ThumbnailResponse(this, id, requestedSize);
    m_pool.start(response);
    return response;
}

QString ThumbnailProvider::idFor(const QString& path, qint64 timeUs)
{
    return QString::number(timeUs) + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(path));
}

QImage ThumbnailProvider::cached(const QString& key)
{
    QMutexLocker lock(&m_mutex);
    const QImage* image = m_cache.object(key);
    return image ? *image : QImage();
}

void ThumbnailProvider::store(const QString& key, const QImage& image)
{
    QMutexLocker lock(&m_mutex);
    m_cache.insert(key, new QImage(image), int(image.sizeInBytes()));
}
