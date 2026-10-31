#include "ProjectController.h"
#include "TestMedia.h"
#include "core/Edits.h"
#include "core/Otio.h"
#include "engine/MediaImport.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <map>

using namespace movix;
using namespace movix::core;

namespace {

// Sondeo falso: devuelve los medios registrados (sin archivos reales).
struct FakeProbe {
    std::map<std::string, MediaItem> items;

    void add(const MediaItem& m) { items[m.path] = m; }
    otio::MediaProbe probe() const
    {
        return [this](const std::string& path) -> std::optional<MediaItem> {
            auto it = items.find(path);
            if (it == items.end())
                return std::nullopt;
            return it->second;
        };
    }
};

MediaItem fakeMedia(const QString& path, double seconds, bool video, bool audio)
{
    MediaItem m;
    m.path = QDir::toNativeSeparators(path).toStdString();
    m.name = path.section(QLatin1Char('/'), -1).toStdString();
    m.duration = TimeUs(seconds * kUsPerSecond);
    m.hasVideo = video;
    m.hasAudio = audio;
    if (video) {
        m.width = 1920;
        m.height = 1080;
        m.frameRate = 25;
    }
    return m;
}

// Resumen de un clip independiente de los ids.
std::string describe(const Project& p, const Clip& c)
{
    std::string s = std::to_string(c.start) + "+" + std::to_string(c.duration) + " in" + std::to_string(c.sourceIn / 1000)
                  + " x" + std::to_string(c.speed);
    if (const MediaItem* m = p.findMedia(c.mediaId))
        s += " " + m->name;
    if (c.title)
        s += " título:" + c.title->text;
    if (c.isCompound())
        s += " compuesto:" + p.sequenceName(c.sequenceId);
    if (c.transitionIn.active())
        s += " tr" + std::to_string(int(c.transitionIn.type)) + "/" + std::to_string(c.transitionIn.duration);
    if (c.audioDetached)
        s += " separado";
    if (!c.enabled)
        s += " desactivado";
    if (c.volume.value != 1.0)
        s += " vol" + std::to_string(c.volume.value);
    return s;
}

std::vector<std::string> describe(const Project& p, const std::vector<Track>& tracks)
{
    std::vector<std::string> out;
    for (const Track& t : tracks) {
        out.push_back("pista " + t.name + (t.hidden ? " oculta" : "") + (t.muted ? " silenciada" : ""));
        for (const Clip& c : t.clips)
            out.push_back("  " + describe(p, c));
    }
    return out;
}

} // namespace

TEST_CASE("OTIO: exportar e importar conserva la línea de tiempo", "[core][otio]")
{
    FakeProbe probe;
    const MediaItem video = fakeMedia("C:/medios/entrevista.mp4", 60, true, true);
    const MediaItem music = fakeMedia("C:/medios/música de fondo.wav", 120, false, true);
    MediaItem logo = fakeMedia("C:/medios/logo.png", 0, true, false);
    logo.isStill = true;
    logo.duration = kUnlimitedDuration;
    for (const MediaItem& m : {video, music, logo})
        probe.add(m);

    Project p = Project::createDefault();
    p.settings.width = 1280;
    p.settings.height = 720;
    p.settings.frameRate = {25, 1};
    const Id mv = edit::addMedia(p, video);
    const Id mm = edit::addMedia(p, music);
    const Id ml = edit::addMedia(p, logo);
    const Id v1 = p.videoTracks[0].id, v2 = p.videoTracks[1].id, a1 = p.audioTracks[0].id;

    const Id c1 = edit::placeClip(p, v1, mv, 0, 2 * kUsPerSecond, 100);
    const Id c2 = edit::placeClip(p, v1, mv, 100, 10 * kUsPerSecond, 50);
    REQUIRE(edit::setClipSpeed(p, c2, 2.0));
    REQUIRE(edit::setTransition(p, c2, TransitionType::DipToBlack, 10));
    const Id c3 = edit::placeClip(p, v1, mv, 200, 30 * kUsPerSecond, 50);
    REQUIRE(edit::detachAudio(p, c3) != kNoId);
    REQUIRE(edit::setClipEnabled(p, c1, false));
    TitleStyle style;
    style.text = "Hola, mundo";
    style.color = 0xFFFF0000;
    REQUIRE(edit::placeTitle(p, v2, 20, 50, style) != kNoId);
    const Id music1 = edit::placeClip(p, a1, mm, 0, 0, 200);
    REQUIRE(edit::setProperty(p, music1, edit::Property::Volume, 0, 0.5));
    const Id lg = edit::placeClip(p, v2, ml, 150, 0, 25);
    REQUIRE(edit::createCompound(p, {lg}, "Logo") != kNoId);
    REQUIRE(edit::addMarkers(p, {10}, "Inicio") == 1);
    REQUIRE(edit::addMarkers(p, {60}, "Pulso", 0xFF4FC3F7) == 1);
    REQUIRE(edit::setTrackHidden(p, v2, true));
    REQUIRE(p.validate());

    const QByteArray json = otio::exportTimeline(p, QStringLiteral("Mi montaje"));
    // Para validarlo con la biblioteca oficial de OpenTimelineIO (fuera de las pruebas).
    if (const QString dump = qEnvironmentVariable("MOVIX_OTIO_DUMP"); !dump.isEmpty()) {
        QFile out(dump);
        REQUIRE(out.open(QIODevice::WriteOnly));
        out.write(json);
    }

    SECTION("estructura OTIO")
    {
        const QJsonObject tl = QJsonDocument::fromJson(json).object();
        CHECK(tl["OTIO_SCHEMA"].toString() == "Timeline.1");
        CHECK(tl["name"].toString() == "Mi montaje");
        const QJsonArray tracks = tl["tracks"].toObject()["children"].toArray();
        // V1, V2, A1, A2 y el audio de V1 (los clips de V1 que conservan su audio).
        REQUIRE(tracks.size() == 5);
        CHECK(tracks[4].toObject()["kind"].toString() == "Audio");
        CHECK(tracks[1].toObject()["enabled"].toBool() == false); // V2 oculta
        const QJsonArray v1Items = tracks[0].toObject()["children"].toArray();
        REQUIRE(v1Items.size() == 5); // clip, transición, clip, hueco, clip
        CHECK(v1Items[1].toObject()["OTIO_SCHEMA"].toString() == "Transition.1");
        CHECK(v1Items[3].toObject()["OTIO_SCHEMA"].toString() == "Gap.1");
        const QJsonObject second = v1Items[2].toObject();
        CHECK(second["effects"].toArray()[0].toObject()["OTIO_SCHEMA"].toString() == "LinearTimeWarp.1");
        const QJsonObject ref = second["media_references"].toObject()["DEFAULT_MEDIA"].toObject();
        CHECK(ref["target_url"].toString() == "C:/medios/entrevista.mp4"); // sin file://, como Kdenlive y Resolve
        const QJsonArray linked = tracks[4].toObject()["children"].toArray();
        CHECK(linked.size() == 3); // los dos primeros clips con su transición (el tercero tiene el audio separado)
    }

    SECTION("ida y vuelta")
    {
        const auto result = otio::importTimeline(json, QStringLiteral("C:/proyectos/montaje.otio"), probe.probe());
        REQUIRE(result.project);
        INFO(result.warnings.join(QLatin1Char('\n')).toStdString());
        CHECK(result.warnings.isEmpty());
        CHECK(result.missingMedia.isEmpty());
        const Project& q = *result.project;
        CHECK(q.settings == p.settings);
        CHECK(q.activeName == "Mi montaje");
        CHECK(q.media.size() == 3);
        CHECK(describe(q, q.videoTracks) == describe(p, p.videoTracks));
        CHECK(describe(q, q.audioTracks) == describe(p, p.audioTracks));
        CHECK(q.markers == p.markers);
        // El compuesto conserva su contenido.
        const Clip& compound = q.videoTracks[1].clips.back();
        REQUIRE(compound.isCompound());
        const Clip& original = p.videoTracks[1].clips.back();
        CHECK(describe(q, *q.videoTracksOf(compound.sequenceId)) == describe(p, *p.videoTracksOf(original.sequenceId)));
    }
}

TEST_CASE("OTIO: importar una línea de tiempo de otro editor", "[core][otio]")
{
    // Al estilo de DaVinci Resolve: 24 fps, códigos de tiempo desde 01:00:00:00, rutas relativas,
    // audio en pistas aparte y un medio que no existe.
    const QByteArray json = R"({
      "OTIO_SCHEMA": "Timeline.1", "name": "Edición", "metadata": {},
      "global_start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 86400.0},
      "tracks": {"OTIO_SCHEMA": "Stack.1", "name": "tracks", "metadata": {}, "source_range": null, "effects": [],
        "markers": [{"OTIO_SCHEMA": "Marker.1", "name": "Ojo", "metadata": {}, "color": "RED",
                     "marked_range": {"OTIO_SCHEMA": "TimeRange.1",
                       "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 12.0},
                       "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 0.0}}}],
        "children": [
          {"OTIO_SCHEMA": "Track.1", "name": "Video 1", "kind": "Video", "metadata": {}, "source_range": null,
           "effects": [], "markers": [], "children": [
            {"OTIO_SCHEMA": "Gap.1", "name": "", "metadata": {}, "effects": [], "markers": [],
             "source_range": {"OTIO_SCHEMA": "TimeRange.1",
               "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 0.0},
               "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 24.0}}},
            {"OTIO_SCHEMA": "Clip.1", "name": "toma", "metadata": {}, "effects": [], "markers": [],
             "media_reference": {"OTIO_SCHEMA": "ExternalReference.1", "name": "", "metadata": {},
               "target_url": "medios/toma.mov",
               "available_range": {"OTIO_SCHEMA": "TimeRange.1",
                 "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 86400.0},
                 "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 240.0}}},
             "source_range": {"OTIO_SCHEMA": "TimeRange.1",
               "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 86448.0},
               "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 48.0}}},
            {"OTIO_SCHEMA": "Transition.1", "name": "Cross Dissolve", "metadata": {}, "transition_type": "SMPTE_Dissolve",
             "in_offset": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 6.0},
             "out_offset": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 6.0}},
            {"OTIO_SCHEMA": "Clip.2", "name": "toma rápida", "metadata": {}, "markers": [],
             "effects": [{"OTIO_SCHEMA": "LinearTimeWarp.1", "name": "", "metadata": {}, "effect_name": "LinearTimeWarp", "time_scalar": 2.0},
                         {"OTIO_SCHEMA": "Effect.1", "name": "", "metadata": {}, "effect_name": "Blur"}],
             "active_media_reference_key": "DEFAULT_MEDIA",
             "media_references": {"DEFAULT_MEDIA": {"OTIO_SCHEMA": "ExternalReference.1", "name": "", "metadata": {},
               "target_url": "medios/toma.mov",
               "available_range": {"OTIO_SCHEMA": "TimeRange.1",
                 "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 86400.0},
                 "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 240.0}}}},
             "source_range": {"OTIO_SCHEMA": "TimeRange.1",
               "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 86500.0},
               "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 48.0}}},
            {"OTIO_SCHEMA": "Clip.1", "name": "perdido", "metadata": {}, "effects": [], "markers": [],
             "media_reference": {"OTIO_SCHEMA": "ExternalReference.1", "name": "", "metadata": {},
               "target_url": "file:///C:/no/existe.mov", "available_range": null},
             "source_range": {"OTIO_SCHEMA": "TimeRange.1",
               "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 24.0},
               "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 24.0}}},
            {"OTIO_SCHEMA": "Clip.1", "name": "sin medio", "metadata": {}, "effects": [], "markers": [],
             "media_reference": {"OTIO_SCHEMA": "MissingReference.1", "name": "", "metadata": {}, "available_range": null},
             "source_range": {"OTIO_SCHEMA": "TimeRange.1",
               "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 0.0},
               "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 24.0}}}
          ]},
          {"OTIO_SCHEMA": "Track.1", "name": "Audio 1", "kind": "Audio", "metadata": {}, "source_range": null,
           "effects": [], "markers": [], "children": [
            {"OTIO_SCHEMA": "Gap.1", "name": "", "metadata": {}, "effects": [], "markers": [],
             "source_range": {"OTIO_SCHEMA": "TimeRange.1",
               "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 0.0},
               "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 24.0}}},
            {"OTIO_SCHEMA": "Clip.1", "name": "toma", "metadata": {}, "effects": [], "markers": [],
             "media_reference": {"OTIO_SCHEMA": "ExternalReference.1", "name": "", "metadata": {},
               "target_url": "medios/toma.mov",
               "available_range": {"OTIO_SCHEMA": "TimeRange.1",
                 "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 86400.0},
                 "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 240.0}}},
             "source_range": {"OTIO_SCHEMA": "TimeRange.1",
               "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 86448.0},
               "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 48.0}}}
          ]},
          {"OTIO_SCHEMA": "Track.1", "name": "Audio 2", "kind": "Audio", "metadata": {}, "source_range": null,
           "effects": [], "markers": [], "children": [
            {"OTIO_SCHEMA": "Clip.1", "name": "toma", "metadata": {}, "effects": [], "markers": [],
             "media_reference": {"OTIO_SCHEMA": "ExternalReference.1", "name": "", "metadata": {},
               "target_url": "medios/toma.mov", "available_range": null},
             "source_range": {"OTIO_SCHEMA": "TimeRange.1",
               "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 0.0},
               "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 96.0}}}
          ]}
        ]}
    })";

    const QString otioPath = QStringLiteral("C:/proyectos/edición/montaje.otio");
    FakeProbe probe;
    probe.add(fakeMedia(QStringLiteral("C:/proyectos/edición/medios/toma.mov"), 10, true, true));
    const auto result = otio::importTimeline(json, otioPath, probe.probe());
    REQUIRE(result.project);
    const Project& p = *result.project;
    CHECK(p.validate());
    CHECK(p.settings.frameRate == Rational{24, 1});
    CHECK(p.settings.width == 1920); // la del primer medio de video
    CHECK(p.activeName == "Edición");

    REQUIRE(p.videoTracks.size() == 1);
    const auto& v = p.videoTracks[0].clips;
    REQUIRE(v.size() == 3); // el clip sin medio se omite
    CHECK(v[0].start == 24);
    CHECK(v[0].duration == 48);
    CHECK(v[0].sourceIn == 2 * kUsPerSecond); // relativo al código de tiempo de inicio del medio
    CHECK_FALSE(v[0].audioDetached);          // su audio de "Audio 1" se volvió a unir
    CHECK(v[1].start == 72);
    CHECK(v[1].speed == 2.0);
    CHECK(std::abs(v[1].sourceIn - TimeUs(std::llround(100.0 / 24 * kUsPerSecond))) <= 1);
    CHECK(v[1].transitionIn.type == TransitionType::Dissolve);
    CHECK(v[1].transitionIn.duration == 12);
    CHECK(v[1].audioDetached); // ningún clip de audio coincide con él
    CHECK(v[2].start == 120);

    // "Audio 1" se vació al unir su clip y desaparece; "Audio 2" se conserva.
    REQUIRE(p.audioTracks.size() == 1);
    CHECK(p.audioTracks[0].name == "Audio 2");
    CHECK(p.audioTracks[0].clips.size() == 1);

    REQUIRE(p.markers.size() == 1);
    CHECK(p.markers[0].frame == 12);
    CHECK(p.markers[0].label == "Ojo");
    CHECK(p.markers[0].color == 0xFFE5484D);

    CHECK(result.missingMedia == QStringList{QDir::toNativeSeparators(QStringLiteral("C:/no/existe.mov"))});
    const MediaItem* missing = p.findMedia(v[2].mediaId);
    REQUIRE(missing);
    CHECK(missing->duration >= 2 * kUsPerSecond); // alcanza para lo que usa su clip
    INFO(result.warnings.join(QLatin1Char('\n')).toStdString());
    CHECK(result.warnings.size() == 2); // el efecto "Blur" y el clip sin medio
}

TEST_CASE("OTIO: archivo escrito por la biblioteca oficial", "[core][otio]")
{
    // tests/data/otio/oficial.otio lo generó OpenTimelineIO 0.18 (Python): 23,976 fps, medios con
    // código de tiempo, audio en su pista, fundido encadenado y un clip a media velocidad.
    const QString path = QStringLiteral(MOVIX_TEST_DATA_DIR "/otio/oficial.otio");
    FakeProbe probe;
    probe.add(fakeMedia(QStringLiteral(MOVIX_TEST_DATA_DIR "/otio/medios/toma.mov"), 100, true, true));
    const auto result = otio::importFile(path, probe.probe());
    REQUIRE(result.project);
    INFO(result.warnings.join(QLatin1Char('\n')).toStdString());
    CHECK(result.warnings.isEmpty());
    CHECK(result.missingMedia.isEmpty());
    const Project& p = *result.project;
    CHECK(p.settings.frameRate == Rational{24000, 1001});

    REQUIRE(p.videoTracks.size() == 1);
    const auto& v = p.videoTracks[0].clips;
    REQUIRE(v.size() == 2);
    CHECK(v[0].start == 48);
    CHECK(v[0].duration == 96);
    CHECK(std::abs(v[0].sourceIn - 1001000) <= 1); // 24 fotogramas desde el inicio del medio
    CHECK_FALSE(v[0].audioDetached);
    CHECK(v[1].start == 144);
    CHECK(v[1].duration == 72);
    CHECK(v[1].speed == 0.5);
    CHECK(v[1].transitionIn.type == TransitionType::Dissolve);
    CHECK(v[1].transitionIn.duration == 24);
    CHECK(v[1].audioDetached); // su audio va a otra velocidad: queda como clip aparte

    REQUIRE(p.audioTracks.size() == 1);
    const auto& a = p.audioTracks[0].clips;
    REQUIRE(a.size() == 1);
    CHECK(a[0].start == 144);
    CHECK_FALSE(a[0].transitionIn.active()); // su clip anterior pasó a ser el audio del video

    REQUIRE(p.markers.size() == 1);
    CHECK(p.markers[0].frame == 30);
    CHECK(p.markers[0].label == "Revisar");

    // Y lo que exportamos lo vuelve a leer igual.
    const auto again = otio::importTimeline(otio::exportTimeline(p, QStringLiteral("x")), path, probe.probe());
    REQUIRE(again.project);
    CHECK(describe(*again.project, again.project->videoTracks) == describe(p, p.videoTracks));
    CHECK(describe(*again.project, again.project->audioTracks) == describe(p, p.audioTracks));
}

TEST_CASE("OTIO: archivos de DaVinci Resolve y Kdenlive", "[core][otio]")
{
    // tests/data/otio/resolve.otio y kdenlive.otio: un montaje exportado por MovixStudio, abierto en
    // DaVinci Resolve 21.1 y Kdenlive 26.08 y vuelto a exportar por ellos (con las rutas de los
    // medios hechas relativas). Resolve conserva la disolvencia y el nombre del marcador; Kdenlive
    // pierde la transición y guarda el texto del marcador en el comentario. El título es propio de
    // MovixStudio y ninguno de los dos lo trae.
    FakeProbe probe;
    probe.add(fakeMedia(QStringLiteral(MOVIX_TEST_DATA_DIR "/otio/medios/scenes_720p30.mp4"), 10, true, false)); // sin audio
    probe.add(fakeMedia(QStringLiteral(MOVIX_TEST_DATA_DIR "/otio/medios/sync_720p30.mp4"), 10, true, true));
    probe.add(fakeMedia(QStringLiteral(MOVIX_TEST_DATA_DIR "/otio/medios/tone_44k_mono.wav"), 10, false, true));
    const std::string transition = " tr" + std::to_string(int(TransitionType::Dissolve)) + "/15";

    for (const char* editor : {"resolve", "kdenlive"}) {
        DYNAMIC_SECTION(editor)
        {
            const bool resolve = std::string(editor) == "resolve";
            const auto result = otio::importFile(QStringLiteral(MOVIX_TEST_DATA_DIR "/otio/%1.otio").arg(QLatin1String(editor)), probe.probe());
            REQUIRE(result.project);
            INFO(result.warnings.join(QLatin1Char('\n')).toStdString());
            CHECK(result.missingMedia.isEmpty());
            // Solo el aviso del título (Resolve lo deja como clip sin medio; los efectos que pone
            // desactivados en cada clip no cuentan).
            CHECK(result.warnings.size() == (resolve ? 1 : 0));
            const Project& p = *result.project;
            CHECK(p.settings.frameRate == Rational{30, 1});
            const auto video = describe(p, p.videoTracks);
            REQUIRE(video.size() >= 4);
            CHECK(video[1] == "  0+90 in1000 x1.000000 scenes_720p30.mp4");
            CHECK(video[2] == "  90+90 in0 x1.000000 sync_720p30.mp4" + (resolve ? transition : std::string()));
            CHECK(video[3] == "  210+30 in5000 x1.000000 scenes_720p30.mp4");
            const auto audio = describe(p, p.audioTracks);
            CHECK(std::count(audio.begin(), audio.end(), "  0+90 in0 x1.000000 tone_44k_mono.wav") == 1);
            CHECK(audio.size() == 3); // A1 con el tono y A2; el audio del clip va con él
            REQUIRE(p.markers.size() == 1);
            CHECK(p.markers[0].frame == 45);
            CHECK(p.markers[0].label == (resolve ? "Marca de prueba" : "Marker"));
        }
    }
}

TEST_CASE("OTIO: archivos que no son líneas de tiempo", "[core][otio]")
{
    CHECK_FALSE(otio::importTimeline("no es json", QStringLiteral("x.otio"), {}).project);
    const auto r = otio::importTimeline(R"({"OTIO_SCHEMA": "Clip.1"})", QStringLiteral("x.otio"), {});
    CHECK_FALSE(r.project);
    CHECK_FALSE(r.error.isEmpty());
    // Una colección con una línea de tiempo dentro sí sirve.
    const auto c = otio::importTimeline(
        R"({"OTIO_SCHEMA": "SerializableCollection.1", "children": [{"OTIO_SCHEMA": "Timeline.1", "name": "T",
            "tracks": {"OTIO_SCHEMA": "Stack.1", "children": []}}]})",
        QStringLiteral("x.otio"), {});
    REQUIRE(c.project);
    CHECK(c.project->videoTracks.size() == 1);
    CHECK(c.project->audioTracks.size() == 1);
}

TEST_CASE("OTIO: exportar e importar desde la aplicación", "[otio][app]")
{
    ProjectController project;
    QSignalSpy errors(&project, &ProjectController::errorOccurred);
    project.newProject();
    project.importFiles({QUrl::fromLocalFile(QString::fromStdString(test::mediaPath("sync_720p30.mp4")))});
    REQUIRE(QTest::qWaitFor([&] { return project.importing() == 0 && !project.project().media.empty(); }, 10000));
    REQUIRE(project.appendToTimeline(qint64(project.project().media[0].id)));
    REQUIRE(project.appendToTimeline(qint64(project.project().media[0].id)));
    const Project before = project.project();

    QTemporaryDir dir;
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("montaje"))); // sin extensión: se añade
    REQUIRE(project.exportOtio(url));
    REQUIRE(QFileInfo::exists(dir.filePath(QStringLiteral("montaje.otio"))));

    project.newProject();
    project.importFiles({QUrl::fromLocalFile(dir.filePath(QStringLiteral("montaje.otio")))}); // como al soltarlo
    REQUIRE(QTest::qWaitFor([&] { return project.importing() == 0 && !project.project().media.empty(); }, 10000));
    INFO((errors.isEmpty() ? QString() : errors.last().at(0).toString()).toStdString());
    CHECK(errors.isEmpty());
    const Project& after = project.project();
    CHECK(after.settings == before.settings);
    CHECK(describe(after, after.videoTracks) == describe(before, before.videoTracks));
    CHECK(describe(after, after.audioTracks) == describe(before, before.audioTracks));
    CHECK(project.isModified()); // sin archivo de proyecto todavía
    CHECK(project.fileUrl().isEmpty());
}

// Para probar a mano con otros editores (no corre con las demás). MOVIX_OTIO_OUT: carpeta donde se
// copian los medios de prueba y se escribe movix.otio con un montaje que los usa.
TEST_CASE("OTIO: montaje de prueba para otros editores", "[.][otio-fuera]")
{
    const QString out = qEnvironmentVariable("MOVIX_OTIO_OUT");
    if (out.isEmpty())
        SKIP("Sin MOVIX_OTIO_OUT");
    QDir().mkpath(out + "/medios");
    auto media = [&](Project& p, const char* name) {
        const QString path = out + "/medios/" + QString::fromLatin1(name);
        if (!QFileInfo::exists(path)) // si ya está, puede tenerlo abierto el otro editor
            REQUIRE(QFile::copy(QString::fromStdString(test::mediaPath(name)), path));
        auto item = engine::probeMediaItem(path.toStdString());
        REQUIRE(item);
        return edit::addMedia(p, *item);
    };
    Project p = Project::createDefault();
    p.settings.width = 1280;
    p.settings.height = 720;
    p.settings.frameRate = {30, 1};
    const Id scenes = media(p, "scenes_720p30.mp4"), sync = media(p, "sync_720p30.mp4"), tone = media(p, "tone_44k_mono.wav");
    const Id v1 = p.videoTracks[0].id, v2 = p.videoTracks[1].id, a1 = p.audioTracks[0].id;
    REQUIRE(edit::placeClip(p, v1, scenes, 0, 1 * kUsPerSecond, 90) != kNoId);
    const Id second = edit::placeClip(p, v1, sync, 90, 0, 90);
    REQUIRE(edit::setTransition(p, second, TransitionType::Dissolve, 15));
    REQUIRE(edit::placeClip(p, v1, scenes, 210, 5 * kUsPerSecond, 60) != kNoId); // tras un hueco de 1 s
    TitleStyle style;
    style.text = "MovixStudio";
    REQUIRE(edit::placeTitle(p, v2, 30, 60, style) != kNoId);
    REQUIRE(edit::placeClip(p, a1, tone, 0, 0, 150) != kNoId);
    REQUIRE(edit::addMarkers(p, {45}, "Marca de prueba") == 1);
    QFile file(out + "/movix.otio");
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(otio::exportTimeline(p, QStringLiteral("Prueba MovixStudio")));
    for (const auto& line : describe(p, p.videoTracks))
        std::printf("[otio] %s\n", line.c_str());
    for (const auto& line : describe(p, p.audioTracks))
        std::printf("[otio] %s\n", line.c_str());
}

// MOVIX_OTIO_IN: un .otio de otro editor; muestra lo que entra.
TEST_CASE("OTIO: leer un archivo de otro editor", "[.][otio-dentro]")
{
    const QString in = qEnvironmentVariable("MOVIX_OTIO_IN");
    if (in.isEmpty())
        SKIP("Sin MOVIX_OTIO_IN");
    QFile file(in);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const auto result = otio::importTimeline(file.readAll(), in, [](const std::string& path) { return engine::probeMediaItem(path); });
    INFO(result.error.toStdString());
    REQUIRE(result.project);
    const Project& p = *result.project;
    std::printf("[otio] %dx%d a %.3f fps, %zu medios, %zu marcadores\n", p.settings.width, p.settings.height, p.settings.frameRate.value(),
                p.media.size(), p.markers.size());
    for (const auto& m : result.missingMedia)
        std::printf("[otio] falta el medio: %s\n", m.toStdString().c_str());
    for (const auto& w : result.warnings)
        std::printf("[otio] aviso: %s\n", w.toStdString().c_str());
    for (const auto& line : describe(p, p.videoTracks))
        std::printf("[otio] %s\n", line.c_str());
    for (const auto& line : describe(p, p.audioTracks))
        std::printf("[otio] %s\n", line.c_str());
}
