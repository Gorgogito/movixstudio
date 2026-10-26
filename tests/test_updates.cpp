#include "UpdateController.h"

#include <catch2/catch_test_macros.hpp>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using movix::updates::compareVersions;
using movix::updates::parseRelease;

namespace {

QByteArray releaseJson(const QString& tag, const QUrl& msiUrl, qint64 size, const QString& sha256, bool prerelease = false)
{
    return QStringLiteral(R"({"tag_name": "%1", "name": "MovixStudio %1", "draft": false, "prerelease": %2,
        "html_url": "https://example.org/releases/%1", "body": "- Novedad",
        "assets": [
          {"name": "MovixStudio-%1-win64.zip", "size": 10, "browser_download_url": "https://example.org/a.zip"},
          {"name": "MovixStudio-%1-win64.msi", "size": %3, "digest": "sha256:%4", "browser_download_url": "%5"}
        ]})")
        .arg(tag, prerelease ? QStringLiteral("true") : QStringLiteral("false"), QString::number(size), sha256, msiUrl.toString())
        .toUtf8();
}

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(data);
}

} // namespace

TEST_CASE("Actualizaciones: comparación de versiones", "[updates]")
{
    CHECK(compareVersions("0.1.0", "0.1.0") == 0);
    CHECK(compareVersions("v0.2.0", "0.1.9") > 0);
    CHECK(compareVersions("0.10.0", "0.9.0") > 0);
    CHECK(compareVersions("1.0", "1.0.0") == 0);
    CHECK(compareVersions("1.0.0-beta.2", "1.0.0") < 0);
    CHECK(compareVersions("1.0.0-beta.10", "1.0.0-beta.2") > 0);
    CHECK(compareVersions("1.0.0-alpha", "1.0.0-beta") < 0);
    CHECK(compareVersions("1.0.0-rc.1", "1.0.0-rc") > 0);
    CHECK(compareVersions("1.2.3+build.5", "1.2.3") == 0);
}

TEST_CASE("Actualizaciones: lectura de la versión publicada", "[updates]")
{
    const auto r = parseRelease(releaseJson("v0.3.0", QUrl("https://example.org/m.msi"), 1234, "ABCDEF"));
    REQUIRE(r);
    CHECK(r->version == "0.3.0");
    CHECK(r->notes == "- Novedad");
    CHECK(r->pageUrl == QUrl("https://example.org/releases/v0.3.0"));
    CHECK(r->installerName == "MovixStudio-v0.3.0-win64.msi");
    CHECK(r->installerUrl == QUrl("https://example.org/m.msi"));
    CHECK(r->installerSize == 1234);
    CHECK(r->installerSha256 == "abcdef");

    CHECK_FALSE(parseRelease(releaseJson("v0.3.0", QUrl(), 1, "00", true))); // preliminar
    CHECK_FALSE(parseRelease(R"({"tag_name": "v1.0.0", "draft": true})"));
    CHECK_FALSE(parseRelease(R"({"message": "Not Found"})"));
    CHECK_FALSE(parseRelease("no es json"));
}

TEST_CASE("Actualizaciones: buscar, descargar con verificación y abrir el instalador", "[updates]")
{
    QSettings().remove(QStringLiteral("updates"));
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QByteArray msi = QByteArray("instalador de prueba ").repeated(500);
    const QString msiPath = dir.filePath("origen.msi");
    writeFile(msiPath, msi);
    const QString sha = QString::fromLatin1(QCryptographicHash::hash(msi, QCryptographicHash::Sha256).toHex());
    const QString feed = dir.filePath("latest.json");
    writeFile(feed, releaseJson("v0.2.0", QUrl::fromLocalFile(msiPath), msi.size(), sha));

    UpdateController updates;
    updates.setFeedUrl(QUrl::fromLocalFile(feed));
    updates.setCurrentVersion("0.1.0");
    updates.setInstalledWithMsi(true);
    QString launched;
    updates.setInstallerLauncher([&](const QString& path) { launched = path; return true; });
    QSignalSpy errors(&updates, &UpdateController::errorOccurred);
    QSignalSpy notices(&updates, &UpdateController::notice);

    SECTION("versión nueva: se descarga, se verifica y se abre")
    {
        updates.check();
        REQUIRE(QTest::qWaitFor([&] { return !updates.checking(); }, 10000));
        REQUIRE(updates.available());
        CHECK(updates.latestVersion() == "0.2.0");
        CHECK(updates.notes() == "- Novedad");
        REQUIRE(updates.canInstall());

        updates.install();
        REQUIRE(QTest::qWaitFor([&] { return !updates.downloading(); }, 10000));
        REQUIRE(errors.isEmpty());
        REQUIRE(updates.installerReady());
        CHECK(updates.progress() == 1.0);
        REQUIRE(updates.launchInstaller());
        QFile downloaded(launched);
        REQUIRE(downloaded.open(QIODevice::ReadOnly));
        CHECK(downloaded.readAll() == msi);
        CHECK(launched.endsWith("MovixStudio-v0.2.0-win64.msi"));
        downloaded.close();

        // La sesión siguiente (ya actualizada) borra el instalador al iniciar, aunque no consulte.
        UpdateController next;
        next.setAutoCheck(false);
        next.checkAtStartup();
        CHECK_FALSE(QFileInfo::exists(launched));
        next.setAutoCheck(true);
    }
    SECTION("un instalador que no coincide con su SHA-256 se descarta")
    {
        writeFile(feed, releaseJson("v0.2.0", QUrl::fromLocalFile(msiPath), msi.size(), QString(64, QLatin1Char('0'))));
        updates.check();
        REQUIRE(QTest::qWaitFor([&] { return !updates.checking(); }, 10000));
        REQUIRE(updates.canInstall());
        updates.install();
        REQUIRE(QTest::qWaitFor([&] { return !updates.downloading(); }, 10000));
        CHECK(errors.size() == 1);
        CHECK_FALSE(updates.installerReady());
        CHECK_FALSE(updates.launchInstaller());
        CHECK(launched.isEmpty());
    }
    SECTION("sin instalación con el MSI solo se ofrece la página de la versión")
    {
        updates.setInstalledWithMsi(false);
        updates.check();
        REQUIRE(QTest::qWaitFor([&] { return !updates.checking(); }, 10000));
        CHECK(updates.available());
        CHECK_FALSE(updates.canInstall());
    }
    SECTION("ya al día: un aviso al buscar a mano y nada al iniciar")
    {
        updates.setCurrentVersion("0.2.0");
        updates.check();
        REQUIRE(QTest::qWaitFor([&] { return !updates.checking(); }, 10000));
        CHECK_FALSE(updates.available());
        CHECK(notices.size() == 1);
        // Al iniciar no se repite la consulta antes de un día.
        updates.setCurrentVersion("0.1.0");
        updates.checkAtStartup();
        CHECK_FALSE(updates.checking());
    }
    SECTION("una versión omitida no se anuncia al iniciar, pero sí al buscar a mano")
    {
        updates.check();
        REQUIRE(QTest::qWaitFor([&] { return !updates.checking(); }, 10000));
        updates.skipVersion();
        CHECK_FALSE(updates.available());
        QSettings().remove(QStringLiteral("updates/lastCheck"));
        updates.checkAtStartup();
        REQUIRE(QTest::qWaitFor([&] { return !updates.checking(); }, 10000));
        CHECK_FALSE(updates.available());
        updates.check();
        REQUIRE(QTest::qWaitFor([&] { return !updates.checking(); }, 10000));
        CHECK(updates.available());
    }
    SECTION("desactivada: no se consulta al iniciar")
    {
        updates.setAutoCheck(false);
        updates.checkAtStartup();
        CHECK_FALSE(updates.checking());
        updates.setAutoCheck(true);
    }
    QSettings().remove(QStringLiteral("updates"));
}
