#include "ExportController.h"

#include "export/Exporter.h"
#include "media/Encoder.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

struct CodecDescription {
    const char* name;
    const char* label;     // nombre técnico, igual en todos los idiomas
    const char* extension;
    const char* audioCodec;
    int crfHigh;           // valor de calidad "alta"; media = +5, baja = +10
};

constexpr CodecDescription kCodecs[] = {
    {"libx264", "H.264", "mp4", "aac", 18},
    {"h264_nvenc", "H.264 · NVIDIA NVENC", "mp4", "aac", 19},
    {"h264_amf", "H.264 · AMD AMF", "mp4", "aac", 20},
    {"h264_qsv", "H.264 · Intel Quick Sync", "mp4", "aac", 20},
    {"libx265", "HEVC (H.265)", "mp4", "aac", 20},
    {"hevc_nvenc", "HEVC · NVIDIA NVENC", "mp4", "aac", 21},
    {"hevc_amf", "HEVC · AMD AMF", "mp4", "aac", 22},
    {"hevc_qsv", "HEVC · Intel Quick Sync", "mp4", "aac", 22},
    {"libsvtav1", "AV1 (SVT-AV1)", "mp4", "aac", 28},
    {"av1_nvenc", "AV1 · NVIDIA NVENC", "mp4", "aac", 28},
    {"av1_amf", "AV1 · AMD AMF", "mp4", "aac", 28},
    {"av1_qsv", "AV1 · Intel Quick Sync", "mp4", "aac", 28},
    {"libvpx-vp9", "VP9 (WebM)", "webm", "libopus", 28},
    {"prores_ks", "Apple ProRes 422 HQ", "mov", "pcm_s16le", 0},
};

const CodecDescription* describe(const QString& name)
{
    for (const auto& c : kCodecs)
        if (name == QLatin1String(c.name))
            return &c;
    return nullptr;
}

} // namespace

ExportController::ExportController(SnapshotSource source, QObject* parent) : QObject(parent), m_source(std::move(source)) {}

ExportController::~ExportController()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

QVariantList ExportController::videoCodecs() const
{
    QVariantList list;
    const auto& available = movix::media::Encoder::availableVideoEncoders();
    for (const auto& c : kCodecs) {
        const auto it = std::find_if(available.begin(), available.end(), [&](const auto& e) { return e.name == c.name; });
        if (it == available.end())
            continue;
        list.append(QVariantMap{
            {"name", QString::fromLatin1(c.name)},
            {"label", it->hardware ? QString::fromUtf8(c.label) + QStringLiteral(" (GPU)") : QString::fromUtf8(c.label)},
            {"hardware", it->hardware},
            {"extension", QString::fromLatin1(c.extension)},
            {"audioCodec", QString::fromLatin1(c.audioCodec)},
        });
    }
    return list;
}

QUrl ExportController::suggestedFile(const QString& projectName, const QString& extension) const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();
    QString base = projectName.isEmpty() ? QStringLiteral("MovixStudio") : projectName;
    QString path = QDir(dir).filePath(base + QLatin1Char('.') + extension);
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = QDir(dir).filePath(QStringLiteral("%1 (%2).%3").arg(base).arg(n).arg(extension));
    return QUrl::fromLocalFile(path);
}

void ExportController::start(const QVariantMap& s)
{
    if (m_running)
        return;
    if (m_thread.joinable())
        m_thread.join();
    auto project = m_source();
    if (!project)
        return;

    movix::exporting::ExportSettings settings;
    settings.path = s.value("path").toUrl().toLocalFile();
    settings.width = s.value("width").toInt() & ~1;
    settings.height = s.value("height").toInt() & ~1;
    settings.frameRate = {s.value("fpsNum").toLongLong(), std::max<qint64>(1, s.value("fpsDen").toLongLong())};
    settings.videoCodec = s.value("codec").toString().toStdString();
    const auto* codec = describe(s.value("codec").toString());
    const QString quality = s.value("quality").toString();
    const int base = codec ? codec->crfHigh : 20;
    settings.quality = quality == QLatin1String("baja") ? base + 10 : quality == QLatin1String("media") ? base + 5 : base;
    settings.videoBitRate = s.value("bitRateKbps").toLongLong() * 1000;
    settings.includeAudio = s.value("includeAudio", true).toBool();
    settings.audioCodec = codec ? codec->audioCodec : "aac";
    settings.audioBitRate = s.value("audioKbps", 192).toLongLong() * 1000;

    m_cancel = false;
    m_running = true;
    m_progress = m_speed = m_remaining = 0;
    m_bottleneck.clear();
    emit runningChanged();
    emit progressChanged();

    m_thread = std::thread([this, project, settings] {
        movix::exporting::Exporter exporter;
        QString error;
        const bool ok = exporter.run(*project, settings, m_cancel, [this](const movix::exporting::ExportProgress& p) {
            const double progress = p.framesTotal ? double(p.framesDone) / double(p.framesTotal) : 0.0;
            const double remaining = p.fps > 0 ? double(p.framesTotal - p.framesDone) / p.fps : 0.0;
            // Etapa más lenta: indica qué limita la velocidad (decodificar, componer o codificar).
            QString bottleneck = tr("codificación");
            double slowest = p.encodeMs;
            if (p.decodeMs > slowest) {
                bottleneck = tr("decodificación");
                slowest = p.decodeMs;
            }
            if (p.renderMs > slowest)
                bottleneck = tr("composición");
            QMetaObject::invokeMethod(this, [this, progress, fps = p.fps, remaining, bottleneck] {
                m_progress = progress;
                m_speed = fps;
                m_remaining = remaining;
                m_bottleneck = bottleneck;
                emit progressChanged();
            }, Qt::QueuedConnection);
        }, &error);
        const bool cancelled = exporter.wasCancelled();
        const QUrl file = QUrl::fromLocalFile(settings.path);
        QMetaObject::invokeMethod(this, [this, ok, cancelled, error, file] {
            m_running = false;
            emit runningChanged();
            emit finished(ok, ok ? QString() : (cancelled ? tr("Exportación cancelada.") : error), file);
        }, Qt::QueuedConnection);
    });
}

void ExportController::cancel()
{
    m_cancel = true;
}

void ExportController::revealInExplorer(const QUrl& file) const
{
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(file.toLocalFile())});
#else
    Q_UNUSED(file);
#endif
}
