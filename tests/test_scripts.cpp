#include "ProjectController.h"
#include "ScriptHost.h"
#include "core/Edits.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace movix;
using namespace movix::core;

namespace {

MediaItem fakeMedia(const char* name, double seconds)
{
    MediaItem m;
    m.path = name;
    m.name = name;
    m.duration = TimeUs(seconds * kUsPerSecond);
    m.hasVideo = m.hasAudio = true;
    m.width = 1920;
    m.height = 1080;
    return m;
}

// Dos clips con un hueco entre ellos en V1 y música en A1.
Project sample()
{
    Project p = Project::createDefault();
    p.settings.frameRate = {25, 1};
    const Id a = edit::addMedia(p, fakeMedia("a.mp4", 20));
    const Id m = edit::addMedia(p, fakeMedia("m.wav", 60));
    edit::placeClip(p, p.videoTracks[0].id, a, 0, 0, 100);
    edit::placeClip(p, p.videoTracks[0].id, a, 150, 0, 100);
    edit::placeClip(p, p.audioTracks[0].id, m, 0, 0, 200);
    return p;
}

QString builtin(const char* file)
{
    QFile f(QStringLiteral(":/scripts/") + QLatin1String(file));
    REQUIRE(f.open(QIODevice::ReadOnly));
    return QString::fromUtf8(f.readAll());
}

} // namespace

TEST_CASE("Scripts: la API consulta y edita el proyecto", "[scripts]")
{
    Project p = sample();
    const auto r = app::runScript(R"(
        const info = movix.info()
        if (info.fps !== 25 || info.playhead !== 30) throw new Error("info")
        const v1 = movix.tracks()[0]
        movix.log(v1.clips.length + " clips")
        const right = movix.split(v1.clips[0].id, 40)
        movix.set(right, "volume", 0.5)
        movix.set(right, "opacity", 0.25)
        movix.addMarker(10, "Inicio")
        movix.addTitle("Hola", 0, movix.secondsToFrames(2))
    )", p, 30, {});
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    CHECK(r.changed);
    CHECK(r.log == QStringList{"2 clips"});
    CHECK(p.videoTracks[0].clips.size() == 3);
    const Clip& right = p.videoTracks[0].clips[1];
    CHECK(right.start == 40);
    CHECK(right.volume.value == 0.5);
    CHECK(right.transform.opacity.value == 0.25);
    REQUIRE(p.markers.size() == 1);
    CHECK(p.markers[0].label == "Inicio");
    CHECK(p.videoTracks[1].clips.size() == 1); // el título, en la pista de arriba (libre)
    CHECK(p.videoTracks[1].clips[0].duration == 50);
    CHECK(p.validate());
}

TEST_CASE("Scripts: errores, sin cambios y bucles infinitos", "[scripts]")
{
    Project p = sample();
    const Project before = p;
    auto r = app::runScript("movix.splitAll(50)\nnoExiste()", p, 0, {});
    CHECK_FALSE(r.ok);
    CHECK(r.error.contains("2")); // la línea del error
    r = app::runScript("movix.log(movix.media().length)", p, 0, {});
    CHECK(r.ok);
    CHECK_FALSE(r.changed);
    r = app::runScript("while (true) {}", p, 0, {}, 300);
    CHECK_FALSE(r.ok);
    CHECK(r.error.contains("0.3"));
    // Sin acceso a lo que no expone la API.
    r = app::runScript("if (typeof require !== 'undefined' || typeof XMLHttpRequest !== 'undefined') throw new Error('acceso')", p, 0, {});
    CHECK(r.ok);
}

TEST_CASE("Scripts incluidos", "[scripts]")
{
    const auto scripts = app::listScripts({});
    REQUIRE(scripts.size() == 5);
    for (const auto& s : scripts) {
        CHECK(s.builtin);
        CHECK_FALSE(s.description.isEmpty());
    }

    SECTION("Quitar los huecos")
    {
        Project p = sample();
        const auto r = app::runScript(builtin("quitar-huecos.js"), p, 0, {});
        INFO(r.error.toStdString());
        REQUIRE(r.ok);
        // El hueco de V1 (100–150) tenía música debajo: no es un hueco de todas las pistas.
        CHECK_FALSE(r.changed);
        edit::deleteClips(p, {p.audioTracks[0].clips[0].id});
        REQUIRE(app::runScript(builtin("quitar-huecos.js"), p, 0, {}).changed);
        CHECK(p.videoTracks[0].clips[1].start == 100);
    }
    SECTION("Marcadores, cortes y títulos")
    {
        Project p = sample(); // 10 s a 25 fps
        REQUIRE(app::runScript(builtin("marcadores-cada-10-s.js"), p, 0, {}).ok);
        CHECK(p.markers.empty()); // dura 10 s: ningún marcador antes del final
        edit::addMarkers(p, {60, 120}, "Parte");
        REQUIRE(app::runScript(builtin("cortar-en-marcadores.js"), p, 0, {}).changed);
        CHECK(p.videoTracks[0].clips.size() == 3); // 60 corta el primero; 120 cae en el hueco
        CHECK(p.audioTracks[0].clips.size() == 3);
        REQUIRE(app::runScript(builtin("titulos-en-marcadores.js"), p, 0, {}).changed);
        int titles = 0; // se solapan (3 s cada 2,4 s): el segundo va a una pista nueva
        for (const Track& t : p.videoTracks)
            for (const Clip& c : t.clips)
                titles += c.title && c.title->text == "Parte" ? 1 : 0;
        CHECK(titles == 2);
        REQUIRE(app::runScript(builtin("musica-de-fondo.js"), p, 0, {}).changed);
        for (const Clip& c : p.audioTracks[0].clips)
            CHECK(c.volume.value == 0.3);
        CHECK(p.validate());
    }
}

TEST_CASE("Scripts desde la aplicación: un paso de deshacer", "[scripts][app]")
{
    QTemporaryDir dir;
    qputenv("MOVIX_SCRIPTS_DIR", dir.path().toUtf8());
    {
        QFile own(dir.filePath("mio.js"));
        REQUIRE(own.open(QIODevice::WriteOnly));
        own.write("// @name Mi script\n// @description Añade dos marcadores\nmovix.addMarker(5)\nmovix.addMarker(15)\n");
        own.close();
        ProjectController project;
        QSignalSpy errors(&project, &ProjectController::errorOccurred);
        project.newProject();
        project.reloadScripts();
        const QVariantList scripts = project.scripts();
        REQUIRE(scripts.size() == 6);
        const QVariantMap mine = scripts.last().toMap();
        CHECK(mine["name"].toString() == "Mi script");
        CHECK_FALSE(mine["builtin"].toBool());
        REQUIRE(project.runScript(mine["path"].toString()));
        CHECK(project.project().markers.size() == 2);
        project.undo(); // los dos marcadores a la vez
        CHECK(project.project().markers.empty());
        // Un script que falla no deja nada a medias.
        QFile bad(dir.filePath("malo.js"));
        REQUIRE(bad.open(QIODevice::WriteOnly));
        bad.write("movix.addMarker(5)\nthrow new Error('a medias')\n");
        bad.close();
        CHECK_FALSE(project.runScript(dir.filePath("malo.js")));
        CHECK(project.project().markers.empty());
        CHECK(errors.size() == 1);
    }
    qunsetenv("MOVIX_SCRIPTS_DIR");
}
