#pragma once

#include "ai/ModelDownloader.h"

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace movix::updates {

// Versión publicada, leída de la API de versiones de GitHub (releases/latest).
struct ReleaseInfo {
    QString version; // sin la "v" inicial
    QString notes;   // Markdown
    QUrl pageUrl;
    // Instalador MSI de Windows (vacío si la versión no lo trae).
    QUrl installerUrl;
    QString installerName;
    QString installerSha256; // hexadecimal; vacío si GitHub no publicó el resumen
    qint64 installerSize = 0;
};

// <0, 0 o >0 como a<b, a==b o a>b. Versiones semánticas ("v1.2.3", "1.2.3-beta.2"): una versión
// preliminar va antes que la final.
int compareVersions(const QString& a, const QString& b);
// nullopt si el JSON no describe una versión publicada (borrador, preliminar o sin etiqueta).
std::optional<ReleaseInfo> parseRelease(const QByteArray& json);

} // namespace movix::updates

// Busca versiones nuevas (al iniciar, como mucho una vez al día, si el usuario no lo desactiva; o a
// petición) y, en una instalación con el MSI, descarga el instalador, comprueba su SHA-256 y lo
// abre para actualizar. Si no, ofrece la página de la versión.
class UpdateController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged)
    Q_PROPERTY(QString notes READ notes NOTIFY stateChanged)
    Q_PROPERTY(QUrl pageUrl READ pageUrl NOTIFY stateChanged)
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY stateChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY stateChanged)
    Q_PROPERTY(bool installerReady READ installerReady NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged) // 0..1, -1 = indeterminado
    Q_PROPERTY(bool autoCheck READ autoCheck WRITE setAutoCheck NOTIFY autoCheckChanged)
    Q_PROPERTY(bool enabled READ enabled CONSTANT) // hay una dirección de consulta configurada

public:
    explicit UpdateController(QObject* parent = nullptr);
    ~UpdateController() override;

    bool checking() const { return m_reply != nullptr; }
    bool available() const { return m_release.has_value(); }
    QString latestVersion() const { return m_release ? m_release->version : QString(); }
    QString notes() const { return m_release ? m_release->notes : QString(); }
    QUrl pageUrl() const { return m_release ? m_release->pageUrl : QUrl(); }
    bool canInstall() const;
    bool downloading() const { return m_downloader && m_downloader->busy(); }
    bool installerReady() const { return !m_installerPath.isEmpty(); }
    double progress() const { return m_progress; }
    bool autoCheck() const;
    void setAutoCheck(bool on);
    bool enabled() const { return m_feedUrl.isValid() && !m_feedUrl.isEmpty(); }

    // Al iniciar: solo si está activado, si ha pasado un día desde la última consulta y sin avisar
    // de errores ni de que no hay novedades.
    Q_INVOKABLE void checkAtStartup();
    Q_INVOKABLE void check(); // a petición: informa también de errores y de "ya está al día"
    Q_INVOKABLE void skipVersion();   // no volver a avisar de esta versión al iniciar
    Q_INVOKABLE void install();       // descarga y verifica el instalador → installerReady
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE bool launchInstaller(); // abre el MSI; la aplicación debe cerrarse a continuación

    // Para las pruebas.
    void setFeedUrl(const QUrl& url) { m_feedUrl = url; }
    void setCurrentVersion(const QString& version) { m_currentVersion = version; }
    void setInstalledWithMsi(bool installed) { m_installedWithMsi = installed; }
    void setInstallerLauncher(std::function<bool(const QString&)> launcher) { m_launcher = std::move(launcher); }

signals:
    void stateChanged();
    void progressChanged();
    void autoCheckChanged();
    void notice(const QString& message);
    void errorOccurred(const QString& message);

private:
    void startCheck(bool manual);
    void onCheckFinished();

    QNetworkAccessManager* m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    movix::ai::ModelDownloader* m_downloader = nullptr;
    QUrl m_feedUrl;
    QString m_currentVersion;
    bool m_installedWithMsi = false;
    bool m_manual = false;
    bool m_canceling = false;
    std::optional<movix::updates::ReleaseInfo> m_release;
    QString m_installerPath;
    double m_progress = -1;
    std::function<bool(const QString&)> m_launcher;
};
