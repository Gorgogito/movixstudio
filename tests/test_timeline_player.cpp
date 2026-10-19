#include "TestMedia.h"
#include "core/Edits.h"
#include "engine/MediaImport.h"
#include "playback/TimelinePlayer.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <cstdio>
#include <functional>
#include <thread>

using namespace movix;
using namespace movix::core;
using playback::TimelinePlayer;

namespace {

TimelinePlayer::Options testOptions()
{
    TimelinePlayer::Options o;
    o.nullAudio = true;
    return o;
}

struct TestProject {
    Project p = Project::createDefault();
    Id media = kNoId;

    TestProject(const char* name, Rational fps = {30, 1})
    {
        p.settings.frameRate = fps;
        auto item = engine::probeMediaItem(test::mediaPath(name));
        REQUIRE(item);
        media = edit::addMedia(p, *item);
    }
    std::shared_ptr<const Project> snapshot() const { return std::make_shared<const Project>(p); }
};

using BundleCallback = std::function<void(const engine::FrameBundle&, TimeUs clock)>;

void pump(TimelinePlayer& player, int ms, const BundleCallback& onFrame = {})
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        auto b = player.frameForPresentation();
        if (onFrame && b)
            onFrame(*b, player.clockUs());
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

std::shared_ptr<const engine::FrameBundle> waitFor(TimelinePlayer& player, int timeoutMs,
                                                   const std::function<bool(const engine::FrameBundle&)>& pred)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        auto b = player.frameForPresentation();
        if (b && pred(*b))
            return b;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return nullptr;
}

} // namespace

TEST_CASE("Reproducción en tiempo real con el fotograma correcto", "[timeline-player]")
{
    TestProject t("sync_720p30.mp4");
    edit::placeClip(t.p, t.p.videoTracks[0].id, t.media, 0);
    TimelinePlayer player(testOptions());
    player.setProject(t.snapshot());
    CHECK(player.durationFrames() == 150);
    pump(player, 200);

    player.play();
    QElapsedTimer wall;
    wall.start();
    int samples = 0, late = 0, mismatched = 0;
    const TimeUs frameDur = t.p.settings.frameDuration();
    pump(player, 2000, [&](const engine::FrameBundle& b, TimeUs clock) {
        if (wall.elapsed() < 300 || b.layers.empty())
            return;
        ++samples;
        const TimeUs lag = clock - b.time;
        if (lag < -frameDur / 2 || lag > frameDur + 8'000)
            ++late;
        // La capa muestra el fotograma del medio que corresponde a ese instante de la línea de tiempo.
        if (std::llabs(b.layers[0].frame->pts - b.time) > 1000)
            ++mismatched;
    });
    const double elapsed = wall.elapsed() / 1000.0;
    std::printf("[timeline-player] %.3f s → reloj %.3f s; fuera de tiempo %d/%d\n", elapsed, player.position(), late, samples);
    CHECK(player.position() > elapsed - 0.15);
    CHECK(player.position() < elapsed + 0.05);
    CHECK(samples > 200);
    CHECK(late <= samples / 100);
    CHECK(mismatched == 0);
}

TEST_CASE("Pausa, búsqueda y paso a paso en la línea de tiempo", "[timeline-player]")
{
    TestProject t("sync_720p30.mp4");
    edit::placeClip(t.p, t.p.videoTracks[0].id, t.media, 0);
    TimelinePlayer player(testOptions());
    player.setProject(t.snapshot());

    player.seekFrame(90);
    auto b = waitFor(player, 1500, [](const engine::FrameBundle& f) { return f.frame == 90 && !f.layers.empty(); });
    REQUIRE(b);
    CHECK(player.positionFrame() == 90);

    player.stepFrames(1);
    REQUIRE(waitFor(player, 1500, [](const engine::FrameBundle& f) { return f.frame == 91; }));
    player.stepFrames(-2);
    REQUIRE(waitFor(player, 1500, [](const engine::FrameBundle& f) { return f.frame == 89; }));
    CHECK(player.positionFrame() == 89);

    player.play();
    pump(player, 400);
    player.pause();
    const double t1 = player.position();
    CHECK(t1 > 89.0 / 30 + 0.25);
    pump(player, 200);
    CHECK(player.position() == t1);
}

TEST_CASE("60 fps a 60 Hz sin descartes en la línea de tiempo", "[timeline-player][cadence]")
{
    TestProject t("count_1080p60.mp4", {60, 1});
    edit::placeClip(t.p, t.p.videoTracks[0].id, t.media, 0);
    // Dispositivo de audio real (reproduce silencio: el clip no tiene audio). Su reloj lo marca el
    // hardware, como en la aplicación; el virtual depende de un temporizador del sistema.
    TimelinePlayer player;
    player.setProject(t.snapshot());
    pump(player, 200);
    player.play();

    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now();
    auto previous = deadline;
    FramePos last = -1;
    int jumps = 0, stalls = 0, distinct = 0;
    for (int i = 0; i < 150; ++i) {
        deadline += std::chrono::microseconds(16'667);
        // Un vsync real es regular: dormir hasta 2 ms antes y esperar activamente el resto
        // (sleep_until solo, con la resolución del temporizador de Windows, se pasa hasta 15 ms).
        std::this_thread::sleep_until(deadline - std::chrono::milliseconds(2));
        while (Clock::now() < deadline) {
        }
        const auto now = Clock::now();
        // Si el sistema detuvo este hilo de prueba, ese "refresco" no fue un vsync real.
        const bool stalled = now - previous > std::chrono::milliseconds(20);
        previous = now;
        QCoreApplication::processEvents();
        auto b = player.frameForPresentation();
        if (last >= 0 && b->frame - last > 1) {
            if (stalled)
                ++stalls;
            else if (i >= 5) // el primer avance tras arrancar puede saltar uno
                ++jumps;
        }
        if (b->frame != last)
            ++distinct;
        last = b->frame;
    }
    std::printf("[timeline-player] cadencia: 150 refrescos → %d fotogramas, %d saltos (%d por pausas del sistema)\n",
                distinct, jumps, stalls);
    CHECK(jumps == 0);
    CHECK(distinct >= 145);
}

TEST_CASE("Varios clips, huecos y final de la línea de tiempo", "[timeline-player]")
{
    TestProject t("sync_720p30.mp4");
    const Id v1 = t.p.videoTracks[0].id;
    edit::placeClip(t.p, v1, t.media, 0, 0, 15);                      // 0-15
    edit::placeClip(t.p, v1, t.media, 30, 2 * kUsPerSecond, 15);      // 30-45, desde 2 s
    TimelinePlayer player(testOptions());
    player.setProject(t.snapshot());
    QSignalSpy ended(&player, &TimelinePlayer::endReached);

    // En el hueco no hay capas; tras él, el segundo clip con su desplazamiento de fuente.
    player.seekFrame(20);
    auto gap = waitFor(player, 1500, [](const engine::FrameBundle& f) { return f.frame == 20; });
    REQUIRE(gap);
    CHECK(gap->layers.empty());
    player.seekFrame(32);
    auto second = waitFor(player, 1500, [](const engine::FrameBundle& f) { return f.frame == 32; });
    REQUIRE(second);
    REQUIRE(second->layers.size() == 1);
    CHECK(std::llabs(second->layers[0].frame->pts - (2 * kUsPerSecond + t.p.settings.frameToTime(2))) <= 1000);

    // Llega al final (con audio: el reloj termina con el reloj de pared de respaldo) y se detiene.
    player.play();
    pump(player, 900);
    CHECK(ended.count() == 1);
    CHECK_FALSE(player.isPlaying());
    CHECK(player.positionFrame() == 45);
}

TEST_CASE("Editar durante la reproducción conserva la posición", "[timeline-player]")
{
    TestProject t("sync_720p30.mp4");
    const Id clip = edit::placeClip(t.p, t.p.videoTracks[0].id, t.media, 0);
    TimelinePlayer player(testOptions());
    player.setProject(t.snapshot());
    player.play();
    pump(player, 600);

    // Ocultar V1 en plena reproducción: el resultado debe verse enseguida sin saltar de posición.
    const double before = player.position();
    edit::setTrackHidden(t.p, t.p.videoTracks[0].id, true);
    player.setProject(t.snapshot());
    auto hidden = waitFor(player, 500, [](const engine::FrameBundle& f) { return f.layers.empty(); });
    REQUIRE(hidden);
    CHECK(player.isPlaying());
    CHECK(player.position() >= before);
    CHECK(player.position() < before + 0.6);

    // Borrar el clip deja la línea vacía: se detiene y muestra el lienzo vacío.
    edit::deleteClips(t.p, {clip});
    player.setProject(t.snapshot());
    pump(player, 100);
    CHECK_FALSE(player.hasContent());
    CHECK_FALSE(player.isPlaying());
    CHECK(player.frameForPresentation()->layers.empty());
    CHECK(player.frameForPresentation()->canvasWidth == 1920);
}

TEST_CASE("Línea de tiempo de solo audio", "[timeline-player]")
{
    TestProject t("tone_44k_mono.wav"); // 3 s
    edit::placeClip(t.p, t.p.audioTracks[0].id, t.media, 0);
    TimelinePlayer player(testOptions());
    player.setProject(t.snapshot());
    QSignalSpy ended(&player, &TimelinePlayer::endReached);
    player.play();
    pump(player, 800);
    CHECK(player.position() > 0.6);
    CHECK(player.position() < 0.95);
    CHECK(player.frameForPresentation()->layers.empty());
    player.seek(2.6);
    pump(player, 700);
    CHECK(ended.count() == 1);
    CHECK_FALSE(player.isPlaying());
}

TEST_CASE("Proyecto vacío: no reproduce y muestra el lienzo", "[timeline-player]")
{
    TimelinePlayer player(testOptions());
    auto empty = std::make_shared<const Project>(Project::createDefault());
    player.setProject(empty);
    player.play();
    CHECK_FALSE(player.isPlaying());
    auto b = player.frameForPresentation();
    REQUIRE(b);
    CHECK(b->layers.empty());
}
