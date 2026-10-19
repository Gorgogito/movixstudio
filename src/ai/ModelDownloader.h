#pragma once

#include "ai/ModelCatalog.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>

#include <memory>

class QNetworkAccessManager;

namespace movix::ai {

// Descarga los archivos de un modelo, uno tras otro, a "<archivo>.part" calculando su SHA-256 sobre
// la marcha; solo si el hash y el tamaño coinciden lo renombra al nombre definitivo. Los archivos
// que ya están completos no se vuelven a descargar. Un modelo cada vez.
class ModelDownloader : public QObject {
    Q_OBJECT
public:
    explicit ModelDownloader(QObject* parent = nullptr);
    ~ModelDownloader() override;

    bool busy() const { return m_reply != nullptr; }
    QString currentModel() const { return m_model.id; }

    // `folder` vacío = modelFolder(model).
    bool start(const ModelInfo& model, const QString& folder = {});
    void cancel();

signals:
    // Progreso del modelo completo (todos sus archivos).
    void progress(const QString& modelId, qint64 received, qint64 total);
    void finished(const QString& modelId, bool ok, const QString& error);

private:
    void startNext();
    void onReadyRead();
    void onFinished();
    void fail(const QString& error);

    QNetworkAccessManager* m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    ModelInfo m_model;
    QString m_folder;
    size_t m_index = 0;        // archivo en curso
    qint64 m_doneBytes = 0;    // bytes de los archivos ya terminados
    QString m_destination;     // ruta final del archivo en curso
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    qint64 m_received = 0;
    bool m_canceled = false;
};

} // namespace movix::ai
