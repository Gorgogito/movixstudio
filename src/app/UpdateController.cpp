#include "UpdateController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

#ifndef MOVIX_UPDATE_URL
#define MOVIX_UPDATE_URL ""
#endif

namespace movix::updates {

namespace {

QString stripPrefix(QString v)
{
    v = v.trimmed();
    if (v.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        v.remove(0, 1);
    return v;
}

// Compara identificadores separados por puntos: los numéricos como números y antes que los de texto.
int comparePreRelease(const QString& a, const QString& b)
{
    const QStringList pa = a.split(QLatin1Char('.'));
    const QStringList pb = b.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < std::min(pa.size(), pb.size()); ++i) {
        bool na = false, nb = false;
        const qlonglong ia = pa[i].toLongLong(&na);
        const qlonglong ib = pb[i].toLongLong(&nb);
        if (na && nb) {
            if (ia != ib)
                return ia < ib ? -1 : 1;
        } else if (na != nb) {
            return na ? -1 : 1;
        } else if (const int c = pa[i].compare(pb[i]); c != 0) {
            return c < 0 ? -1 : 1;
        }
    }
    return pa.size() == pb.size() ? 0 : (pa.size() < pb.size() ? -1 : 1);
}

} // namespace

int compareVersions(const QString& a, const QString& b)
{
    const QString va = stripPrefix(a).section(QLatin1Char('+'), 0, 0); // sin metadatos de compilación
    const QString vb = stripPrefix(b).section(QLatin1Char('+'), 0, 0);
    const QString coreA = va.section(QLatin1Char('-'), 0, 0), preA = va.section(QLatin1Char('-'), 1);
    const QString coreB = vb.section(QLatin1Char('-'), 0, 0), preB = vb.section(QLatin1Char('-'), 1);
    const QStringList pa = coreA.split(QLatin1Char('.'));
    const QStringList pb = coreB.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const qlonglong ia = i < pa.size() ? pa[i].toLongLong() : 0;
        const qlonglong ib = i < pb.size() ? pb[i].toLongLong() : 0;
        if (ia != ib)
            return ia < ib ? -1 : 1;
    }
    if (preA.isEmpty() || preB.isEmpty())
        return preA.isEmpty() == preB.isEmpty() ? 0 : (preA.isEmpty() ? 1 : -1);
    return comparePreRelease(preA, preB);
}

std::optional<ReleaseInfo> parseRelease(const QByteArray& json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.isEmpty() || o.value(QLatin1String("draft")).toBool() || o.value(QLatin1String("prerelease")).toBool())
        return std::nullopt;
    ReleaseInfo r;
    r.version = stripPrefix(o.value(QLatin1String("tag_name")).toString());
    if (r.version.isEmpty() || !r.version.front().isDigit())
        return std::nullopt;
    r.notes = o.value(QLatin1String("body")).toString();
    r.pageUrl = QUrl(o.value(QLatin1String("html_url")).toString());
    for (const QJsonValue& value : o.value(QLatin1String("assets")).toArray()) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QLatin1String("name")).toString();
        if (!name.endsWith(QLatin1String("-win64.msi"), Qt::CaseInsensitive))
            continue;
        r.installerName = QFileInfo(name).fileName(); // nunca una ruta
        if (r.installerName.isEmpty())
            continue;
        r.installerUrl = QUrl(asset.value(QLatin1String("browser_download_url")).toString());
        r.installerSize = asset.value(QLatin1String("size")).toInteger();
        const QString digest = asset.value(QLatin1String("digest")).toString(); // "sha256:<hex>"
        if (digest.startsWith(QLatin1String("sha256:"), Qt::CaseInsensitive))
            r.installerSha256 = digest.mid(7).toLower();
        break;
    }
    return r;
}

} // namespace movix::updates

namespace {

const QString kAutoCheck = QStringLiteral("updates/autoCheck");
const QString kLastCheck = QStringLiteral("updates/lastCheck");
const QString kSkipped = QStringLiteral("updates/skipped");

// Donde se descarga el instalador. Se vacía al iniciar: una descarga solo sirve para la sesión que
// la hizo (install() la repite siempre) y, tras actualizar, el MSI ya no hace falta.
QString downloadFolder()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("MovixStudio-actualizacion"));
}

bool runningFromProgramFiles()
{
    const QString dir = QDir::cleanPath(QCoreApplication::applicationDirPath());
    for (const char* var : {"ProgramW6432", "ProgramFiles"}) {
        const QString root = QDir::cleanPath(QDir::fromNativeSeparators(qEnvironmentVariable(var)));
        if (!root.isEmpty() && dir.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace

UpdateController::UpdateController(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_downloader(new movix::ai::ModelDownloader(this))
    , m_currentVersion(QCoreApplication::applicationVersion())
    , m_installedWithMsi(runningFromProgramFiles())
{
    const QString url = qEnvironmentVariable("MOVIX_UPDATE_URL", QStringLiteral(MOVIX_UPDATE_URL));
    m_feedUrl = QUrl(url);
    m_launcher = [](const QString& path) {
        return QProcess::startDetached(QStringLiteral("msiexec.exe"), {QStringLiteral("/i"), QDir::toNativeSeparators(path)});
    };
    connect(m_downloader, &movix::ai::ModelDownloader::progress, this, [this](const QString&, qint64 received, qint64 total) {
        m_progress = total > 0 ? double(received) / double(total) : -1;
        emit progressChanged();
    });
    connect(m_downloader, &movix::ai::ModelDownloader::finished, this, [this](const QString&, bool ok, const QString& error) {
        if (ok && m_release)
            m_installerPath = QDir(downloadFolder()).filePath(m_release->installerName);
        else if (!ok && !m_canceling)
            emit errorOccurred(tr("No se pudo descargar la actualización: %1").arg(error));
        emit stateChanged();
    });
}

UpdateController::~UpdateController()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
}

bool UpdateController::canInstall() const
{
    return m_installedWithMsi && m_release && m_release->installerUrl.isValid() && !m_release->installerSha256.isEmpty()
           && m_release->installerSize > 0;
}

bool UpdateController::autoCheck() const
{
    return QSettings().value(kAutoCheck, true).toBool();
}

void UpdateController::setAutoCheck(bool on)
{
    if (on == autoCheck())
        return;
    QSettings().setValue(kAutoCheck, on);
    emit autoCheckChanged();
}

void UpdateController::checkAtStartup()
{
    QDir(downloadFolder()).removeRecursively(); // si otra copia está instalando, el MSI abierto se queda
    if (!enabled() || !autoCheck())
        return;
    const QDateTime last = QSettings().value(kLastCheck).toDateTime();
    if (last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc()) < 24 * 3600)
        return;
    startCheck(false);
}

void UpdateController::check()
{
    if (!enabled()) {
        emit errorOccurred(tr("Esta compilación no tiene configurada una dirección para buscar actualizaciones."));
        return;
    }
    startCheck(true);
}

void UpdateController::startCheck(bool manual)
{
    if (m_reply) {
        m_manual = m_manual || manual;
        return;
    }
    m_manual = manual;
    QNetworkRequest request(m_feedUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MovixStudio/%1").arg(m_currentVersion));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(15000);
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateController::onCheckFinished);
    emit stateChanged();
}

void UpdateController::onCheckFinished()
{
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    const bool manual = m_manual;
    if (reply->error() != QNetworkReply::NoError) {
        if (manual)
            emit errorOccurred(tr("No se pudo comprobar si hay actualizaciones: %1").arg(reply->errorString()));
        emit stateChanged();
        return;
    }
    QSettings().setValue(kLastCheck, QDateTime::currentDateTimeUtc());
    const auto release = movix::updates::parseRelease(reply->readAll());
    const bool newer = release && movix::updates::compareVersions(release->version, m_currentVersion) > 0;
    const bool skipped = release && QSettings().value(kSkipped).toString() == release->version;
    if (newer && (manual || !skipped)) {
        if (!m_release || m_release->version != release->version)
            m_installerPath.clear();
        m_release = release;
    } else {
        m_release.reset();
        m_installerPath.clear();
        if (manual)
            emit notice(tr("MovixStudio está al día (versión %1).").arg(m_currentVersion));
    }
    emit stateChanged();
}

void UpdateController::skipVersion()
{
    if (!m_release)
        return;
    QSettings().setValue(kSkipped, m_release->version);
    cancelDownload();
    m_release.reset();
    m_installerPath.clear();
    emit stateChanged();
}

void UpdateController::install()
{
    if (!canInstall() || downloading() || installerReady())
        return;
    const QString folder = downloadFolder();
    // Sin reutilizar lo que hubiera: el instalador se verifica siempre al descargarlo.
    QFile::remove(QDir(folder).filePath(m_release->installerName));
    movix::ai::ModelInfo info;
    info.id = QStringLiteral("update");
    info.name = QStringLiteral("MovixStudio %1").arg(m_release->version);
    info.files.push_back({m_release->installerName, m_release->installerUrl.toString(), m_release->installerSha256,
                          m_release->installerSize});
    m_progress = 0;
    emit progressChanged();
    if (m_downloader->start(info, folder))
        emit stateChanged();
}

void UpdateController::cancelDownload()
{
    if (!m_downloader->busy())
        return;
    m_canceling = true;
    m_downloader->cancel(); // emite finished de inmediato
    m_canceling = false;
}

bool UpdateController::launchInstaller()
{
    if (!installerReady())
        return false;
    if (!m_launcher(m_installerPath)) {
        emit errorOccurred(tr("No se pudo abrir el instalador %1").arg(QDir::toNativeSeparators(m_installerPath)));
        return false;
    }
    return true;
}
