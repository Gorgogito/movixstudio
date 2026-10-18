#include "core/Edits.h"
#include "core/Project.h"

#include <catch2/catch_test_macros.hpp>

using namespace movix;
using namespace movix::core;

namespace {

// Medio sintético de 10 s con video y audio (las ediciones no necesitan archivos reales).
MediaItem fakeMedia(TimeUs duration = 10 * kUsPerSecond, bool video = true, bool audio = true)
{
    MediaItem m;
    m.path = "C:\\media\\fake.mp4";
    m.name = "fake.mp4";
    m.duration = duration;
    m.hasVideo = video;
    m.hasAudio = audio;
    m.width = 1920;
    m.height = 1080;
    m.frameRate = 30;
    return m;
}

struct Fixture {
    Project p = Project::createDefault(); // 30 fps
    Id media = edit::addMedia(p, fakeMedia());
    Id v1 = p.videoTracks[0].id;
    Id v2 = p.videoTracks[1].id;
    Id a1 = p.audioTracks[0].id;

    const Track& track(Id id) const { return *p.findTrack(id); }
    void check() const
    {
        std::string why;
        INFO(why);
        REQUIRE(p.validate(&why));
    }
};

} // namespace

TEST_CASE("Conversión fotograma ↔ tiempo", "[core]")
{
    ProjectSettings s;
    s.frameRate = {30, 1};
    CHECK(s.frameToTime(0) == 0);
    CHECK(s.frameToTime(1) == 33'333);
    CHECK(s.frameToTime(3) == 100'000);
    CHECK(s.timeToFrame(33'333) == 1);
    CHECK(s.timeToFrame(33'332) == 0);
    CHECK(s.framesIn(10 * kUsPerSecond) == 300);

    s.frameRate = {30000, 1001}; // 29,97
    for (FramePos f : {0LL, 1LL, 29LL, 30LL, 1799LL, 107892LL}) // hasta 1 h
        CHECK(s.timeToFrame(s.frameToTime(f)) == f);
    CHECK(s.frameToTime(30000) == 1001 * kUsPerSecond);
}

TEST_CASE("Colocar clips sobrescribe lo que ocupa el destino", "[core][edits]")
{
    Fixture f;
    const Id a = edit::placeClip(f.p, f.v1, f.media, 0, 0, 90);   // 0-90
    REQUIRE(a != kNoId);
    CHECK(f.track(f.v1).clips[0].duration == 90);

    SECTION("toda la duración disponible por defecto")
    {
        const Id b = edit::placeClip(f.p, f.v2, f.media, 0);
        CHECK(f.p.findClip(b)->duration == 300);
    }
    SECTION("en medio de un clip lo divide en dos")
    {
        const Id b = edit::placeClip(f.p, f.v1, f.media, 30, 0, 30); // 30-60 dentro de 0-90
        const auto& clips = f.track(f.v1).clips;
        REQUIRE(clips.size() == 3);
        CHECK(clips[0].id == a);
        CHECK(clips[0].end() == 30);
        CHECK(clips[1].id == b);
        CHECK(clips[2].start == 60);
        CHECK(clips[2].duration == 30);
        CHECK(clips[2].sourceIn == f.p.settings.frameToTime(60)); // la parte derecha conserva su contenido
        f.check();
    }
    SECTION("solapando el final recorta el anterior")
    {
        edit::placeClip(f.p, f.v1, f.media, 60, 0, 60); // 60-120
        const auto& clips = f.track(f.v1).clips;
        REQUIRE(clips.size() == 2);
        CHECK(clips[0].end() == 60);
        f.check();
    }
    SECTION("no se coloca en una pista incompatible ni bloqueada")
    {
        const Id audioOnly = edit::addMedia(f.p, fakeMedia(5 * kUsPerSecond, false, true));
        CHECK(edit::placeClip(f.p, f.v1, audioOnly, 200) == kNoId);
        CHECK(edit::placeClip(f.p, f.a1, audioOnly, 0) != kNoId);
        edit::setTrackLocked(f.p, f.v2, true);
        CHECK(edit::placeClip(f.p, f.v2, f.media, 0) == kNoId);
    }
}

TEST_CASE("Dividir, recortar y eliminar", "[core][edits]")
{
    Fixture f;
    const Id a = edit::placeClip(f.p, f.v1, f.media, 0, 0, 90);
    const Id b = edit::placeClip(f.p, f.v1, f.media, 120, 2 * kUsPerSecond, 60); // 120-180, fuente desde 2 s

    SECTION("dividir conserva el contenido")
    {
        const Id right = edit::splitClip(f.p, b, 150);
        REQUIRE(right != kNoId);
        CHECK(f.p.findClip(b)->duration == 30);
        CHECK(f.p.findClip(right)->start == 150);
        CHECK(f.p.findClip(right)->sourceIn == 2 * kUsPerSecond + f.p.settings.frameToTime(30));
        CHECK(edit::splitClip(f.p, b, 120) == kNoId); // en el borde no se divide
        f.check();
    }
    SECTION("dividir en el cabezal afecta a todas las pistas no bloqueadas")
    {
        edit::placeClip(f.p, f.v2, f.media, 0, 0, 200);
        const auto created = edit::splitAt(f.p, 45);
        CHECK(created.size() == 2);
        f.check();
    }
    SECTION("recortar el inicio respeta el vecino y el medio disponible")
    {
        // b empieza en 120 con sourceIn = 2 s → se puede extender hasta 60 fotogramas antes,
        // pero el clip anterior termina en 90.
        auto applied = edit::trimClip(f.p, b, edit::Edge::Start, 0);
        REQUIRE(applied);
        CHECK(*applied == 90);
        CHECK(f.p.findClip(b)->sourceIn == 2 * kUsPerSecond - f.p.settings.frameToTime(30));
        // a empieza en 0 con sourceIn 0: no se puede extender hacia atrás.
        CHECK(*edit::trimClip(f.p, a, edit::Edge::Start, -10) == 0);
        f.check();
    }
    SECTION("recortar el final no supera el medio ni el siguiente clip")
    {
        CHECK(*edit::trimClip(f.p, a, edit::Edge::End, 500) == 120); // tope: inicio de b
        CHECK(*edit::trimClip(f.p, b, edit::Edge::End, 1000) == 120 + 240); // 10 s - 2 s = 240 fotogramas
        CHECK(*edit::trimClip(f.p, b, edit::Edge::End, 0) == 121);           // mínimo un fotograma
        f.check();
    }
    SECTION("eliminar con y sin cierre de hueco")
    {
        const Id c = edit::placeClip(f.p, f.v1, f.media, 200, 0, 30);
        CHECK(edit::deleteClips(f.p, {a}, false));
        CHECK(f.p.findClip(b)->start == 120);
        CHECK(edit::deleteClips(f.p, {b}, true));
        CHECK(f.p.findClip(c)->start == 140); // se desplaza los 60 fotogramas de b
        f.check();
    }
}

TEST_CASE("Mover clips en el tiempo y entre pistas", "[core][edits]")
{
    Fixture f;
    const Id a = edit::placeClip(f.p, f.v1, f.media, 0, 0, 60);
    const Id b = edit::placeClip(f.p, f.v1, f.media, 100, 0, 60);

    SECTION("mover sobre otro clip lo recorta")
    {
        CHECK(edit::moveClips(f.p, {a}, 80));
        const auto& clips = f.track(f.v1).clips;
        REQUIRE(clips.size() == 2);
        CHECK(clips[0].id == a);
        CHECK(clips[0].start == 80);
        CHECK(clips[1].start == 140);
        CHECK(clips[1].duration == 20);
        f.check();
    }
    SECTION("mover a otra pista")
    {
        CHECK(edit::moveClips(f.p, {a, b}, 10, 1));
        CHECK(f.track(f.v1).clips.empty());
        CHECK(f.track(f.v2).clips.size() == 2);
        CHECK(f.p.findClip(a)->start == 10);
        CHECK_FALSE(edit::moveClips(f.p, {a}, 0, 1)); // no hay V3
        f.check();
    }
    SECTION("el desplazamiento se limita al inicio de la línea de tiempo")
    {
        CHECK(edit::moveClips(f.p, {b}, -1000));
        CHECK(f.p.findClip(b)->start == 0);
        f.check();
    }
    SECTION("no se mueve desde ni hacia pistas bloqueadas")
    {
        edit::setTrackLocked(f.p, f.v2, true);
        CHECK_FALSE(edit::moveClips(f.p, {a}, 0, 1));
        CHECK(f.p.findClip(a)->start == 0);
    }
}

TEST_CASE("Cambiar los fps del proyecto conserva los instantes de los clips", "[core][edits]")
{
    Fixture f; // 30 fps
    const Id a = edit::placeClip(f.p, f.v1, f.media, 30, 0, 60);  // 1 s - 3 s
    const Id b = edit::placeClip(f.p, f.v1, f.media, 90, 0, 45);  // 3 s - 4,5 s (pegado a a)
    REQUIRE(edit::setFrameRate(f.p, {60, 1}));
    CHECK(f.p.findClip(a)->start == 60);
    CHECK(f.p.findClip(a)->duration == 120);
    CHECK(f.p.findClip(b)->start == 180);
    CHECK(f.p.findClip(b)->duration == 90);
    REQUIRE(edit::setFrameRate(f.p, {30000, 1001})); // 29,97: 1 s ≈ 29,97 fotogramas
    CHECK(f.p.findClip(a)->start == 30);
    CHECK(f.p.findClip(a)->end() == f.p.findClip(b)->start); // siguen pegados
    f.check();

    CHECK(edit::guessFrameRate(29.97) == Rational{30000, 1001});
    CHECK(edit::guessFrameRate(23.976) == Rational{24000, 1001});
    CHECK(edit::guessFrameRate(25.0) == Rational{25, 1});
    CHECK(edit::guessFrameRate(12.5) == Rational{13, 1});
}

TEST_CASE("El proyecto se ajusta al primer clip", "[core][edits]")
{
    Fixture f;
    MediaItem vertical = fakeMedia();
    vertical.width = 1081; // impar: se redondea a par
    vertical.height = 1920;
    vertical.frameRate = 59.94;
    const Id m = edit::addMedia(f.p, vertical);
    REQUIRE(edit::matchSettingsToMedia(f.p, *f.p.findMedia(m)));
    CHECK(f.p.settings.width == 1080);
    CHECK(f.p.settings.height == 1920);
    CHECK(f.p.settings.frameRate == Rational{60000, 1001});
    edit::placeClip(f.p, f.v1, m, 0);
    CHECK_FALSE(edit::matchSettingsToMedia(f.p, *f.p.findMedia(f.media))); // ya no está vacía
}

TEST_CASE("Pistas y medios", "[core][edits]")
{
    Fixture f;
    const Id v3 = edit::addTrack(f.p, TrackKind::Video);
    CHECK(f.p.videoTracks.back().name == "V3");
    CHECK(f.p.videoTracks.back().id == v3);
    const Id clip = edit::placeClip(f.p, v3, f.media, 0);
    CHECK(edit::removeMedia(f.p, f.media));
    CHECK(f.p.findClip(clip) == nullptr);
    CHECK(edit::removeTrack(f.p, v3));
    CHECK(f.p.videoTracks.size() == 2);
    f.check();
}
