#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QTemporaryDir>

using namespace movix;
using namespace movix::core;
using Catch::Matchers::WithinAbs;

namespace {

MediaItem media(TimeUs duration = 10 * kUsPerSecond, bool video = true, bool audio = true)
{
    MediaItem m;
    m.path = "C:\\media\\m.mp4";
    m.name = "m.mp4";
    m.duration = duration;
    m.hasVideo = video;
    m.hasAudio = audio;
    m.width = 1920;
    m.height = 1080;
    m.frameRate = 30;
    return m;
}

struct F {
    Project p = Project::createDefault(); // 30 fps
    Id m = edit::addMedia(p, media());
    Id v1 = p.videoTracks[0].id;
    Id v2 = p.videoTracks[1].id;
    Id a1 = p.audioTracks[0].id;
    void check() const
    {
        std::string why;
        INFO(why);
        REQUIRE(p.validate(&why));
    }
};

} // namespace

TEST_CASE("Valores animables: interpolación y edición de keyframes", "[core][f2]")
{
    Animated a(5.0);
    CHECK(a.at(100) == 5.0);
    a.setKey(10, 0.0);
    a.setKey(20, 10.0);
    CHECK(a.at(0) == 0.0);            // antes del primero se mantiene
    CHECK(a.at(15) == 5.0);           // lineal
    CHECK(a.at(30) == 10.0);          // después del último se mantiene
    a.setKey(10, 0.0, Interpolation::Hold);
    CHECK(a.at(19) == 0.0);
    a.setKey(10, 0.0, Interpolation::Smooth);
    CHECK(a.at(15) == 5.0);           // simétrico en el centro
    CHECK(a.at(12) < 2.0);            // arranca suave (lineal daría 2)
    a.set(15, 7.0);                   // animado: crea un keyframe
    CHECK(a.keys.size() == 3);
    CHECK(a.removeKey(15));
    CHECK(a.removeKey(20));
    CHECK(a.removeKey(10));
    CHECK_FALSE(a.isAnimated());
    CHECK(a.value == 0.0);            // conserva el valor del último keyframe quitado
}

TEST_CASE("Propiedades de clip y keyframes que se mueven con el clip", "[core][f2]")
{
    F f;
    const Id c = edit::placeClip(f.p, f.v1, f.m, 30, 0, 90);
    REQUIRE(edit::setProperty(f.p, c, edit::Property::Scale, 0, 2.0));
    CHECK(f.p.findClip(c)->transform.scale.value == 2.0);
    REQUIRE(edit::toggleKeyframe(f.p, c, edit::Property::Opacity, 0));
    REQUIRE(edit::setProperty(f.p, c, edit::Property::Opacity, 60, 0.0)); // ahora animado: crea keyframe
    CHECK(f.p.findClip(c)->transform.opacity.keys.size() == 2);
    CHECK(edit::setProperty(f.p, c, edit::Property::Opacity, 60, 5.0)); // se limita a 1
    CHECK(f.p.findClip(c)->transform.opacity.at(60) == 1.0);

    // Recortar el inicio 30 fotogramas desplaza los keyframes para que sigan en el mismo instante.
    edit::trimClip(f.p, c, edit::Edge::Start, 60);
    CHECK(f.p.findClip(c)->transform.opacity.keys.front().frame == -30);
    // Al dividir, la parte derecha conserva sus keyframes en su propio origen.
    const Id right = edit::splitClip(f.p, c, 75);
    CHECK(f.p.findClip(right)->transform.opacity.keys.back().frame == 90 - 75); // instante 90 de la línea
    f.check();
}

TEST_CASE("Velocidad: duración, instante de fuente y límites", "[core][f2]")
{
    F f;
    const Id c = edit::placeClip(f.p, f.v1, f.m, 0, 0, 90); // 3 s
    edit::placeClip(f.p, f.v1, f.m, 200, 0, 30);
    REQUIRE(edit::setClipSpeed(f.p, c, 2.0));
    CHECK(f.p.findClip(c)->duration == 45);
    CHECK(sourceTimeAt(f.p.settings, *f.p.findClip(c), 30) == 2 * kUsPerSecond);
    // Ralentizar a 0,5: querría 180 fotogramas, pero el clip siguiente empieza en 200.
    REQUIRE(edit::setClipSpeed(f.p, c, 0.5));
    CHECK(f.p.findClip(c)->duration == 180);
    REQUIRE(edit::setClipSpeed(f.p, c, 0.25));
    CHECK(f.p.findClip(c)->duration == 200); // tope: inicio del siguiente
    // El medio disponible también se mide a la nueva velocidad: 10 s a 4x = 2,5 s = 75 fotogramas.
    REQUIRE(edit::setClipSpeed(f.p, c, 4.0));
    CHECK(*edit::trimClip(f.p, c, edit::Edge::End, 500) == 75);
    f.check();
}

TEST_CASE("Fundidos y transiciones: evaluación de capas", "[core][f2]")
{
    F f;
    const Id a = edit::placeClip(f.p, f.v1, f.m, 0, 0, 60);
    const Id b = edit::placeClip(f.p, f.v1, f.m, 60, 5 * kUsPerSecond, 60);

    SECTION("fundido de entrada")
    {
        REQUIRE(edit::setFades(f.p, a, 10, 0));
        CHECK(videoLayersAt(f.p, 0)[0].opacity == 0.0);
        CHECK_THAT(videoLayersAt(f.p, 5)[0].opacity, WithinAbs(0.5, 1e-9));
        CHECK(videoLayersAt(f.p, 10)[0].opacity == 1.0);
        edit::setFades(f.p, a, 100, 100); // se limitan a la duración del clip
        CHECK(f.p.findClip(a)->fadeIn + f.p.findClip(a)->fadeOut <= 60);
    }
    SECTION("disolver entre dos clips contiguos")
    {
        REQUIRE(edit::setTransition(f.p, b, TransitionType::Dissolve, 20));
        // Antes del tramo [50, 70): una sola capa.
        CHECK(videoLayersAt(f.p, 49).size() == 1);
        auto first = videoLayersAt(f.p, 50); // k = 0
        REQUIRE(first.size() == 2);
        CHECK(first[0].clipId == a);
        CHECK(first[1].clipId == b);
        CHECK_THAT(first[1].opacity, WithinAbs(0.5 / 20, 1e-9));
        // La entrante usa material anterior a su inicio: 10 fotogramas antes de 5 s.
        CHECK(first[1].sourceTime == 5 * kUsPerSecond - f.p.settings.frameToTime(10));
        auto last = videoLayersAt(f.p, 69); // k = 19, ya dentro de b
        REQUIRE(last.size() == 2);
        CHECK(last[0].sourceTime == f.p.settings.frameToTime(69)); // la saliente sigue más allá de su final
        CHECK(videoLayersAt(f.p, 70).size() == 1);
    }
    SECTION("empuje y barrido")
    {
        REQUIRE(edit::setTransition(f.p, b, TransitionType::PushLeft, 20));
        auto l = videoLayersAt(f.p, 60); // k = 10, progreso 0,525
        CHECK_THAT(l[0].transform.x, WithinAbs(-0.525, 1e-9));
        CHECK_THAT(l[1].transform.x, WithinAbs(0.475, 1e-9));
        REQUIRE(edit::setTransition(f.p, b, TransitionType::WipeLeft, 20));
        CHECK_THAT(videoLayersAt(f.p, 60)[1].crop.left, WithinAbs(0.475, 1e-9));
    }
    SECTION("la transición desaparece si los clips dejan de ser contiguos")
    {
        REQUIRE(edit::setTransition(f.p, b, TransitionType::Dissolve, 20));
        REQUIRE(edit::moveClips(f.p, {b}, 10));
        CHECK_FALSE(f.p.findClip(b)->transitionIn.active());
        CHECK_FALSE(edit::setTransition(f.p, b, TransitionType::Dissolve, 20)); // ya no hay corte
        f.check();
    }
}

TEST_CASE("Audio: volumen de pista y clip, fundidos y fundido cruzado", "[core][f2]")
{
    F f;
    const Id c = edit::placeClip(f.p, f.a1, f.m, 0, 0, 90);
    edit::setTrackVolume(f.p, f.a1, 0.5);
    edit::setProperty(f.p, c, edit::Property::Volume, 0, 0.8);
    auto seg = audioSegmentsIn(f.p, kUsPerSecond, kUsPerSecond + 20'000);
    REQUIRE(seg.size() == 1);
    CHECK_THAT(seg[0].gainStart, WithinAbs(0.4, 1e-6));

    edit::setAudioFades(f.p, c, 30, 0); // 1 s de fundido de entrada
    seg = audioSegmentsIn(f.p, 500'000, 520'000);
    CHECK_THAT(seg[0].gainStart, WithinAbs(0.2, 1e-3));

    // Transición: ambos clips suenan en el tramo, uno sube y otro baja (potencia constante).
    const Id d = edit::placeClip(f.p, f.a1, f.m, 90, 0, 60);
    REQUIRE(edit::setTransition(f.p, d, TransitionType::Dissolve, 30));
    seg = audioSegmentsIn(f.p, f.p.settings.frameToTime(90), f.p.settings.frameToTime(90) + 1000);
    REQUIRE(seg.size() == 2);
    const double sum = double(seg[0].gainStart) * seg[0].gainStart / 0.16 + double(seg[1].gainStart) * seg[1].gainStart / 0.25;
    CHECK_THAT(sum, WithinAbs(1.0, 0.01)); // sin² + cos² = 1 (normalizado por las ganancias de cada clip)
}

TEST_CASE("Títulos, imágenes, inserción, separar audio y portapapeles", "[core][f2]")
{
    F f;
    TitleStyle style;
    style.text = "Hola";
    const Id title = edit::placeTitle(f.p, f.v2, 0, 90, style);
    REQUIRE(title != kNoId);
    CHECK(edit::placeTitle(f.p, f.a1, 0, 90, style) == kNoId); // no en pistas de audio
    CHECK(videoLayersAt(f.p, 10).back().title->text == "Hola");
    REQUIRE(*edit::trimClip(f.p, title, edit::Edge::End, 100000) == 100000); // sin límite de medio

    MediaItem png = media(0, true, false);
    png.isStill = true;
    const Id still = edit::addMedia(f.p, png);
    const Id s = edit::placeClip(f.p, f.v1, still, 0);
    CHECK(f.p.findClip(s)->duration == 150); // 5 s por defecto
    CHECK_FALSE(edit::setClipSpeed(f.p, s, 2.0));

    // Inserción: los clips posteriores se desplazan.
    const Id later = edit::placeClip(f.p, f.v1, f.m, 300, 0, 30);
    edit::placeClip(f.p, f.v1, f.m, 100, 0, 50, edit::PlaceMode::Insert);
    CHECK(f.p.findClip(later)->start == 350);
    CHECK(f.p.findClip(s)->duration == 100); // la imagen se dividió en el punto de inserción

    // Separar audio: aparece un clip de audio con la misma posición y el de video deja de sonar.
    const Id audio = edit::detachAudio(f.p, later);
    REQUIRE(audio != kNoId);
    CHECK(f.p.findClip(audio)->start == 350);
    CHECK(f.p.findClip(later)->audioDetached);
    const auto segs = audioSegmentsIn(f.p, f.p.settings.frameToTime(355), f.p.settings.frameToTime(356));
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].clipId == audio);
    CHECK(edit::detachAudio(f.p, later) == kNoId);

    // Copiar y pegar conserva distancias y pistas, con ids nuevos.
    const auto cb = edit::copyClips(f.p, {title, later});
    const auto pasted = edit::pasteClips(f.p, cb, 1000);
    REQUIRE(pasted.size() == 2);
    CHECK(f.p.findClip(pasted[0])->start == 1000);
    CHECK(f.p.findClip(pasted[1])->start == 1350);
    f.check();
}

TEST_CASE("Archivo v2: ida y vuelta y compatibilidad con v1", "[core][f2][file]")
{
    F f;
    const Id c = edit::placeClip(f.p, f.v1, f.m, 0, 0, 90);
    const Id d = edit::placeClip(f.p, f.v1, f.m, 90, kUsPerSecond, 60);
    edit::toggleKeyframe(f.p, c, edit::Property::Scale, 0);
    edit::setProperty(f.p, c, edit::Property::Scale, 45, 1.5);
    edit::setKeyframeInterpolation(f.p, c, edit::Property::Scale, 0, Interpolation::Smooth);
    REQUIRE(edit::setClipSpeed(f.p, d, 1.5)); // conserva el inicio: sigue pegado a c
    edit::setFades(f.p, c, 5, 0);
    REQUIRE(edit::setTransition(f.p, d, TransitionType::PushRight, 12));
    edit::setTrackVolume(f.p, f.a1, 0.7);
    TitleStyle style;
    style.text = "Línea 1\nLínea 2";
    style.background = 0x80112233;
    edit::placeTitle(f.p, f.v2, 10, 40, style);

    QTemporaryDir dir;
    const QString path = dir.filePath("p.mxproj");
    REQUIRE(saveProjectFile(f.p, path));
    const LoadResult loaded = loadProjectFile(path);
    INFO(loaded.error.toStdString());
    REQUIRE(loaded.project);
    CHECK(*loaded.project == f.p);

    // Un proyecto de la versión 1 (con "gain") se abre con el volumen convertido.
    const auto v1 = deserializeProject(R"({"format":"movixstudio-project","version":1,"nextId":10,
        "media":[{"id":1,"path":"x","duration":5000000,"hasVideo":true,"hasAudio":true}],
        "videoTracks":[{"id":2,"name":"V1","clips":[{"id":3,"mediaId":1,"start":0,"duration":50,"sourceIn":0,"gain":0.25}]}],
        "audioTracks":[]})", "x.mxproj");
    REQUIRE(v1.project);
    const Clip* clip = v1.project->findClip(3);
    CHECK(clip->volume.value == 0.25);
    CHECK(clip->speed == 1.0);
    CHECK(clip->transform.scale.value == 1.0);
}
