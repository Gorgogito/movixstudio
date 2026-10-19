#include "ai/ModelDownloader.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace movix::ai {

ModelDownloader::ModelDownloader(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

ModelDownloader::~ModelDownloader()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    if (m_file)
        m_file->remove();
}

bool ModelDownloader::start(const ModelInfo& model, const QString& folder)
{
    if (busy() || model.files.empty())
        return false;
    m_model = model;
    m_folder = folder.isEmpty() ? modelFolder(model) : folder;
    QDir().mkpath(m_folder);
    m_index = 0;
    m_doneBytes = 0;
    m_canceled = false;
    startNext();
    return true;
}

void ModelDownloader::startNext()
{
    // Los archivos ya completos se dan por buenos (su hash se comprobó al descargarlos).
    while (m_index < m_model.files.size()) {
        const ModelFile& f = m_model.files[m_index];
        if (QFileInfo(m_folder + QLatin1Char('/') + f.name).size() != f.size)
            break;
        m_doneBytes += f.size;
        ++m_index;
    }
    if (m_index >= m_model.files.size()) {
        emit progress(m_model.id, m_doneBytes, m_model.totalSize());
        emit finished(m_model.id, true, {});
        return;
    }

    const ModelFile& f = m_model.files[m_index];
    m_destination = m_folder + QLatin1Char('/') + f.name;
    m_file = std::make_unique<QFile>(m_destination + QStringLiteral(".part"));
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString error = tr("No se pudo escribir en %1").arg(QDir::toNativeSeparators(m_file->fileName()));
        m_file.reset();
        emit finished(m_model.id, false, error);
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_received = 0;

    QNetworkRequest request{QUrl(f.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MovixStudio"));
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &ModelDownloader::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &ModelDownloader::onFinished);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
        emit progress(m_model.id, m_doneBytes + received, m_model.totalSize());
    });
}

void ModelDownloader::cancel()
{
    if (!m_reply)
        return;
    m_canceled = true;
    m_reply->abort(); // emite finished → onFinished
}

void ModelDownloader::onReadyRead()
{
    if (!m_reply || !m_file)
        return;
    const QByteArray chunk = m_reply->readAll();
    if (m_file->write(chunk) != chunk.size()) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
        fail(tr("No hay espacio suficiente o no se pudo escribir el modelo."));
        return;
    }
    m_hash->addData(chunk);
    m_received += chunk.size();
}

void ModelDownloader::onFinished()
{
    QNetworkReply* reply = m_reply;
    if (!reply)
        return;
    reply->deleteLater();
    if (m_canceled) {
        m_reply = nullptr;
        fail(tr("Descarga cancelada."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        m_reply = nullptr;
        fail(tr("Error de red: %1").arg(reply->errorString()));
        return;
    }
    onReadyRead(); // lo que quede en el búfer
    m_reply = nullptr;
    if (!m_file)
        return;
    const ModelFile& f = m_model.files[m_index];
    const QString digest = QString::fromLatin1(m_hash->result().toHex());
    if (m_received != f.size || digest.compare(f.sha256, Qt::CaseInsensitive) != 0) {
        fail(tr("El archivo descargado no coincide con el esperado (tamaño o SHA-256)."));
        return;
    }
    m_file->close();
    QFile::remove(m_destination);
    if (!m_file->rename(m_destination)) {
        fail(tr("No se pudo guardar el modelo en %1").arg(QDir::toNativeSeparators(m_destination)));
        return;
    }
    m_file.reset();
    m_doneBytes += f.size;
    ++m_index;
    startNext();
}

void ModelDownloader::fail(const QString& error)
{
    if (m_file) {
        m_file->close();
        m_file->remove();
        m_file.reset();
    }
    emit finished(m_model.id, false, error);
}

} // namespace movix::ai
