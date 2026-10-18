#pragma once

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>

// Miniaturas de video para la biblioteca y la línea de tiempo, decodificadas en segundo plano.
// URL: image://thumbs/<instante en µs>/<ruta UTF-8 codificada en porcentaje>
class ThumbnailProvider : public QQuickAsyncImageProvider {
public:
    ThumbnailProvider();
    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    static QString idFor(const QString& path, qint64 timeUs);

    // Caché compartida entre peticiones (las miniaturas de un mismo clip se repiten mucho).
    QImage cached(const QString& key);
    void store(const QString& key, const QImage& image);

private:
    QThreadPool m_pool;
    QMutex m_mutex;
    QCache<QString, QImage> m_cache;
};
