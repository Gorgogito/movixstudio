#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace movix;
using namespace movix::core;

namespace {

Project sampleProject(const QString& mediaPath)
{
    Project p = Project::createDefault();
    p.settings.frameRate = {30000, 1001};
    p.settings.width = 3840;
    p.settings.height = 2160;
    MediaItem m;
    m.path = QDir::toNativeSeparators(mediaPath).toStdString();
    m.name = "clip ñandú.mp4";
    m.duration = 12'345'678;
    m.hasVideo = m.hasAudio = true;
    m.width = 3840;
    m.height = 2160;
    m.frameRate = 29.97;
    const Id id = edit::addMedia(p, m);
    const Id clip = edit::placeClip(p, p.videoTracks[0].id, id, 10, 500'000, 120);
    edit::placeClip(p, p.audioTracks[1].id, id, 50);
    edit::setProperty(p, clip, edit::Property::Volume, 0, 0.25f);
    edit::setTrackHidden(p, p.videoTracks[1].id, true);
    return p;
}

} // namespace

TEST_CASE("Guardar y abrir un proyecto conserva todo", "[core][file]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString mediaPath = dir.filePath("medios/clip ñandú.mp4");
    QDir().mkpath(dir.filePath("medios"));
    QFile(mediaPath).open(QIODevice::WriteOnly);

    const Project original = sampleProject(mediaPath);
    const QString projectPath = dir.filePath("proyecto.mxproj");
    QString error;
    REQUIRE(saveProjectFile(original, projectPath, &error));

    const LoadResult loaded = loadProjectFile(projectPath);
    INFO(loaded.error.toStdString());
    REQUIRE(loaded.project);
    CHECK(loaded.missingMedia.isEmpty());
    CHECK(*loaded.project == original);
}

TEST_CASE("Los medios se encuentran por ruta relativa si se mueve la carpeta", "[core][file]")
{
    QTemporaryDir a, b;
    QDir().mkpath(a.filePath("medios"));
    QFile(a.filePath("medios/v.mp4")).open(QIODevice::WriteOnly);
    REQUIRE(saveProjectFile(sampleProject(a.filePath("medios/v.mp4")), a.filePath("p.mxproj")));

    // "Mover" el proyecto con su carpeta de medios a otra ubicación.
    QDir().mkpath(b.filePath("medios"));
    QFile::copy(a.filePath("medios/v.mp4"), b.filePath("medios/v.mp4"));
    QFile::copy(a.filePath("p.mxproj"), b.filePath("p.mxproj"));
    QFile::remove(a.filePath("medios/v.mp4"));

    const LoadResult loaded = loadProjectFile(b.filePath("p.mxproj"));
    REQUIRE(loaded.project);
    CHECK(loaded.missingMedia.isEmpty());
    CHECK(QDir::fromNativeSeparators(QString::fromStdString(loaded.project->media[0].path)) == b.filePath("medios/v.mp4"));

    // Si no está en ningún sitio, se abre igualmente y se informa.
    QFile::remove(b.filePath("medios/v.mp4"));
    const LoadResult missing = loadProjectFile(b.filePath("p.mxproj"));
    REQUIRE(missing.project);
    CHECK(missing.missingMedia.size() == 1);
}

TEST_CASE("Archivos de proyecto inválidos se rechazan con un mensaje", "[core][file]")
{
    CHECK_FALSE(deserializeProject("no es json", "x.mxproj").project);
    CHECK_FALSE(deserializeProject(R"({"format":"otro"})", "x.mxproj").project);
    CHECK_FALSE(deserializeProject(R"({"format":"movixstudio-project","version":99})", "x.mxproj").project);
    // Clips solapados: estructura válida pero proyecto dañado.
    const auto bad = deserializeProject(R"({"format":"movixstudio-project","version":1,"nextId":10,
        "media":[{"id":1,"path":"x","duration":5000000,"hasVideo":true}],
        "videoTracks":[{"id":2,"name":"V1","clips":[{"id":3,"mediaId":1,"start":0,"duration":50},
                                                     {"id":4,"mediaId":1,"start":40,"duration":50}]}],
        "audioTracks":[]})", "x.mxproj");
    CHECK_FALSE(bad.project);
    CHECK(bad.error.contains("dañado"));
}

TEST_CASE("Evaluación de capas de video y tramos de audio", "[core][timeline]")
{
    Project p = Project::createDefault(); // 30 fps
    MediaItem m;
    m.path = "x";
    m.duration = 10 * kUsPerSecond;
    m.hasVideo = m.hasAudio = true;
    const Id media = edit::addMedia(p, m);
    const Id low = edit::placeClip(p, p.videoTracks[0].id, media, 0, 0, 90);           // V1 0-90
    const Id high = edit::placeClip(p, p.videoTracks[1].id, media, 30, kUsPerSecond, 30); // V2 30-60, fuente desde 1 s

    auto layers = videoLayersAt(p, 45);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].clipId == low);
    CHECK(layers[0].sourceTime == p.settings.frameToTime(45));
    CHECK(layers[1].clipId == high);
    CHECK(layers[1].sourceTime == kUsPerSecond + p.settings.frameToTime(15));
    CHECK(videoLayersAt(p, 60).size() == 1);
    CHECK(videoLayersAt(p, 90).empty());

    edit::setTrackHidden(p, p.videoTracks[1].id, true);
    CHECK(videoLayersAt(p, 45).size() == 1);

    // Audio: ambos clips de video aportan su audio; la intersección es exacta.
    auto segs = audioSegmentsIn(p, 900'000, 1'100'000);
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].timelineStart == 900'000);
    CHECK(segs[0].sourceStart == 900'000);
    CHECK(segs[1].clipId == high);
    CHECK(segs[1].timelineStart == 1'000'000);
    CHECK(segs[1].sourceStart == kUsPerSecond);

    edit::setTrackMuted(p, p.videoTracks[0].id, true);
    CHECK(audioSegmentsIn(p, 0, 3 * kUsPerSecond).size() == 1);
}
