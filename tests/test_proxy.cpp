#include "ProjectController.h"
#include "TestMedia.h"
#include "engine/Proxy.h"
#include "engine/VideoReader.h"
#include "media/MediaInfo.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <cmath>

using namespace movix;

TEST_CASE("Proxy: 540 líneas, mismos instantes y una clave cada medio segundo", "[engine][proxy]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    core::MediaItem m;
    m.path = test::mediaPath("count_1080p60.mp4");
    m.hasVideo = true;
    m.width = 1920;
    m.height = 1080;
    CHECK_FALSE(engine::needsProxy(m)); // 1080p se edita bien sin proxy
    m.width = 3840;
    m.height = 2160;
    CHECK(engine::needsProxy(m));

    const std::string out = engine::proxyPathFor(dir.path().toStdString(), m);
    CHECK(out == engine::proxyPathFor(dir.path().toStdString(), m)); // estable
    double last = 0;
    std::string error;
    REQUIRE(engine::buildProxy(m, out, &error, [&](double f) { last = f; }, {}));
    CHECK(last > 0.9);
    REQUIRE(QFileInfo::exists(QString::fromStdString(out)));
    CHECK(QDir(dir.path()).entryList(QDir::Files).size() == 1); // sin el temporal

    const auto info = media::probe(out);
    REQUIRE(info);
    REQUIRE(info->video);
    CHECK(info->video->width == 960);
    CHECK(info->video->height == 540);

    // Los mismos fotogramas en los mismos instantes, también al saltar hacia atrás.
    engine::VideoReader reader;
    REQUIRE(reader.open(out, false));
    for (int i : {0, 59, 179, 90, 30, 120}) {
        const TimeUs t = TimeUs(std::llround(i * 1e6 / 60.0));
        const auto f = reader.frameAt(t);
        REQUIRE(f);
        INFO("fotograma " << i);
        CHECK(std::abs(f->pts - t) <= 1000);
    }

    // Cancelar no deja nada.
    const std::string other = dir.filePath(QStringLiteral("cancelado.mp4")).toStdString();
    CHECK_FALSE(engine::buildProxy(m, other, &error, {}, [] { return true; }));
    CHECK(QDir(dir.path()).entryList(QDir::Files).size() == 1);
}

TEST_CASE("Proxy: el controlador lo crea, lo reconoce al volver y lo borra", "[app][proxy]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    qputenv("MOVIX_PROXY_DIR", dir.path().toUtf8());
    {
        ProjectController project;
        project.newProject();
        project.importFiles({QUrl::fromLocalFile(QString::fromStdString(test::mediaPath("count_1080p60.mp4")))});
        REQUIRE(QTest::qWaitFor([&] { return project.importing() == 0 && !project.project().media.empty(); }, 10000));
        const qint64 id = qint64(project.project().media[0].id);
        CHECK(project.proxyState(id).isEmpty()); // 1080p: no se crea solo

        project.createProxy(id);
        CHECK_FALSE(project.proxyState(id).isEmpty());
        REQUIRE(QTest::qWaitFor([&] { return project.proxyState(id) == QLatin1String("ready"); }, 60000));
        CHECK(QDir(dir.path()).entryList(QDir::Files).size() == 1);

        // Otro proyecto con el mismo medio lo encuentra sin volver a crearlo.
        ProjectController again;
        again.newProject();
        again.importFiles({QUrl::fromLocalFile(QString::fromStdString(test::mediaPath("count_1080p60.mp4")))});
        REQUIRE(QTest::qWaitFor([&] { return again.importing() == 0 && !again.project().media.empty(); }, 10000));
        CHECK(again.proxyState(qint64(again.project().media[0].id)) == QLatin1String("ready"));

        project.removeProxy(id);
        CHECK(project.proxyState(id).isEmpty());
        REQUIRE(QTest::qWaitFor([&] { return QDir(dir.path()).entryList(QDir::Files).isEmpty(); }, 5000));
    }
    qunsetenv("MOVIX_PROXY_DIR");
}
