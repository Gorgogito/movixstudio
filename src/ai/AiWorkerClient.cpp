#include "ai/AiWorkerClient.h"

#include "ai/WorkerProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>

namespace movix::ai {

AiWorkerClient::AiWorkerClient(const QString& program, QObject* parent)
    : QObject(parent)
    , m_program(program.isEmpty() ? defaultProgram() : program)
{
}

AiWorkerClient::~AiWorkerClient()
{
    shutdown();
}

QString AiWorkerClient::defaultProgram()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("movix-ai-worker.exe");
#else
    const QString name = QStringLiteral("movix-ai-worker");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

bool AiWorkerClient::ensureRunning()
{
    if (m_process && m_process->state() != QProcess::NotRunning)
        return true;
    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_buffer.clear();
    m_process = new QProcess(this);
    m_process->setProgram(m_program);
    m_process->setReadChannel(QProcess::StandardOutput);
    // Los mensajes de diagnóstico del proceso van a la consola de la aplicación (si la hay).
    m_process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &AiWorkerClient::onReadyRead);
    connect(m_process, &QProcess::finished, this, &AiWorkerClient::onProcessFinished);
    m_process->start();
    return m_process->waitForStarted(10000);
}

int AiWorkerClient::submit(QJsonObject request)
{
    const int job = m_nextJob++;
    request["id"] = job;
    m_pending[job] = request;
    if (!ensureRunning()) {
        m_pending.erase(job);
        // Diferido: quien llama recibe el id antes que la señal.
        QMetaObject::invokeMethod(this, [this, job] {
            emit failed(job, tr("No se pudo iniciar el proceso de IA (%1).").arg(QDir::toNativeSeparators(m_program)), false);
        }, Qt::QueuedConnection);
        return job;
    }
    m_process->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    return job;
}

void AiWorkerClient::cancel(int job)
{
    if (!m_pending.count(job) || !m_process)
        return;
    m_process->write(QJsonDocument(QJsonObject{{"type", QLatin1String(protocol::kCancel)}, {"id", job}})
                         .toJson(QJsonDocument::Compact) + '\n');
}

void AiWorkerClient::shutdown()
{
    if (!m_process)
        return;
    m_process->disconnect(this);
    m_process->closeWriteChannel(); // el proceso abandona el trabajo y termina
    if (!m_process->waitForFinished(3000))
        m_process->kill();
    m_process->deleteLater();
    m_process = nullptr;
    const auto pending = std::exchange(m_pending, {});
    for (const auto& [job, request] : pending)
        emit failed(job, tr("Trabajo cancelado."), true);
}

void AiWorkerClient::onReadyRead()
{
    m_buffer += m_process->readAllStandardOutput();
    for (qsizetype nl; (nl = m_buffer.indexOf('\n')) >= 0;) {
        const QByteArray line = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            handle(doc.object());
    }
}

void AiWorkerClient::handle(const QJsonObject& e)
{
    const int job = e["id"].toInt();
    const QString name = e["event"].toString();
    if (name == QLatin1String("started")) {
        emit started(job);
    } else if (name == QLatin1String("progress")) {
        emit progress(job, e["value"].toDouble(), e["stage"].toString());
    } else if (name == QLatin1String("result")) {
        m_pending.erase(job);
        emit finished(job, e);
    } else if (name == QLatin1String("error")) {
        m_pending.erase(job);
        emit failed(job, e["message"].toString(), e["canceled"].toBool());
    }
}

void AiWorkerClient::onProcessFinished()
{
    if (m_process)
        onReadyRead(); // eventos que quedaran en la tubería
    const auto pending = std::exchange(m_pending, {});
    for (const auto& [job, request] : pending)
        emit failed(job, tr("El proceso de IA terminó inesperadamente."), false);
}

} // namespace movix::ai
