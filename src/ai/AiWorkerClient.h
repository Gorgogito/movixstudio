#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>

#include <map>

// Cliente del proceso de IA: lo arranca al primer trabajo, le envía peticiones y convierte sus
// eventos en señales (en el hilo del objeto). Si el proceso muere, los trabajos pendientes fallan
// y el siguiente trabajo lo vuelve a arrancar.
namespace movix::ai {

class AiWorkerClient : public QObject {
    Q_OBJECT
public:
    // `program` vacío = movix-ai-worker junto al ejecutable actual.
    explicit AiWorkerClient(const QString& program = {}, QObject* parent = nullptr);
    ~AiWorkerClient() override;

    static QString defaultProgram();

    // Envía un trabajo (ver ai/WorkerProtocol.h; "id" lo pone el cliente). Devuelve su id.
    int submit(QJsonObject request);
    void cancel(int job);
    bool hasJobs() const { return !m_pending.empty(); }
    // Cierra el proceso (abandona los trabajos en curso).
    void shutdown();

signals:
    void started(int job);
    void progress(int job, double value, const QString& stage);
    void finished(int job, const QJsonObject& result);
    void failed(int job, const QString& error, bool canceled);

private:
    bool ensureRunning();
    void onReadyRead();
    void onProcessFinished();
    void handle(const QJsonObject& event);

    QString m_program;
    QProcess* m_process = nullptr;
    QByteArray m_buffer;
    int m_nextJob = 1;
    std::map<int, QJsonObject> m_pending; // trabajos enviados sin respuesta final
};

} // namespace movix::ai
