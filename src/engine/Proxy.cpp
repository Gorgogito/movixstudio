#include "engine/Proxy.h"

#include "core/Edits.h"
#include "media/Encoder.h"
#include "media/FrameConvert.h"
#include "media/VideoDecoder.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace movix::engine {

bool needsProxy(const core::MediaItem& media)
{
    return media.hasVideo && !media.isStill && int64_t(media.width) * media.height > 1920LL * 1088;
}

std::string proxyPathFor(const std::string& dir, const core::MediaItem& media)
{
    const QFileInfo info(QString::fromStdString(media.path));
    const QByteArray key = QStringLiteral("%1|%2|%3")
                               .arg(info.absoluteFilePath())
                               .arg(info.size())
                               .arg(info.lastModified().toMSecsSinceEpoch())
                               .toUtf8();
    const QString name = QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(20));
    return QDir(QString::fromStdString(dir)).filePath(name + QStringLiteral(".mp4")).toStdString();
}

bool buildProxy(const core::MediaItem& item, const std::string& outPath, std::string* error,
                const std::function<void(double)>& progress, const std::function<bool()>& cancel)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return false;
    };
    media::VideoDecoder decoder;
    std::string err;
    if (!decoder.open(item.path, media::VideoDecoderOptions{true, true}, &err))
        return fail(err);
    // 540 líneas (o las del original si tiene menos), con su relación de aspecto y medidas pares.
    const int srcW = std::max(1, decoder.width()), srcH = std::max(1, decoder.height());
    const int outH = std::min(540, srcH) / 2 * 2;
    const int outW = std::max(2, int(std::lround(double(outH) * srcW / srcH)) / 2 * 2);
    const core::Rational fps = core::edit::guessFrameRate(decoder.frameRate() > 0 ? decoder.frameRate() : 30.0);

    const QString finalPath = QString::fromStdString(outPath);
    QDir().mkpath(QFileInfo(finalPath).absolutePath());
    const QString tmpPath = finalPath + QStringLiteral(".tmp.mp4");
    QFile::remove(tmpPath);

    media::EncoderSettings es;
    es.path = tmpPath.toStdString();
    es.container = "mp4";
    es.hasAudio = false;
    es.video.codec = "libx264";
    es.video.width = outW;
    es.video.height = outH;
    es.video.fpsNum = int(fps.num);
    es.video.fpsDen = int(fps.den);
    es.video.quality = 23;
    es.video.preset = "veryfast";
    es.video.gop = std::max(1, int(std::lround(fps.value() / 2))); // buscar rápido: una clave cada medio segundo
    media::Encoder encoder;
    if (!encoder.open(es, &err))
        return fail(err);

    std::vector<uint8_t> rgba;
    const double total = double(std::max<TimeUs>(1, decoder.duration()));
    while (auto frame = decoder.decodeNext()) {
        if (cancel && cancel()) {
            encoder.abort();
            QFile::remove(tmpPath);
            return fail("cancelado");
        }
        if (!media::frameToRgba(*frame, outW, outH, rgba) || !encoder.writeVideo(rgba.data(), outW * 4)) {
            encoder.abort();
            QFile::remove(tmpPath);
            return fail(encoder.lastError());
        }
        if (progress)
            progress(std::clamp(double(frame->pts) / total, 0.0, 1.0));
    }
    if (!encoder.finish(&err)) {
        QFile::remove(tmpPath);
        return fail(err);
    }
    QFile::remove(finalPath);
    if (!QFile::rename(tmpPath, finalPath))
        return fail("no se pudo guardar el proxy");
    return true;
}

} // namespace movix::engine
