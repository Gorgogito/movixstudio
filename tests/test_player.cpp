#include "TestMedia.h"
#include "playback/Player.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <cstdio>
#include <functional>
#include <thread>

using namespace movix;
using playback::Player;

namespace {

Player::Options testOptions()
{
    Player::Options o;
    o.nullAudio = true; // dispositivo de audio virtual: avanza en tiempo real sin sonar
    return o;
}

using FrameCallback = std::function<void(const media::VideoFrame*, TimeUs clock)>;

// Simula el bucle de render (~250 Hz) durante `ms` milisegundos, procesando eventos de Qt.
void pump(Player& player, int ms, const FrameCallback& onFrame = {})
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        auto frame = player.frameForPresentation();
        if (onFrame)
            onFrame(frame.get(), player.clockUs());
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

// Espera hasta que se muestre un fotograma que cumpla la condición (o se agote el tiempo).
std::shared_ptr<const media::VideoFrame> waitForFrame(Player& player, int timeoutMs,
                                                      const std::function<bool(const media::VideoFrame&)>& pred)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        auto frame = player.frameForPresentation();
        if (frame && pred(*frame))
            return frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return nullptr;
}

} // namespace

TEST_CASE("El reproductor avanza en tiempo real y muestra el fotograma correcto", "[player]")
{
    Player player(testOptions());
    REQUIRE(player.openFile(QString::fromStdString(test::mediaPath("sync_720p30.mp4"))));
    REQUIRE(player.hasVideo());
    REQUIRE(player.hasAudio());
    pump(player, 200); // primer fotograma decodificado

    player.play();
    QElapsedTimer wall;
    wall.start();
    TimeUs maxLag = 0;
    int samples = 0, late = 0;
    const TimeUs frameDur = player.frameDurationUs();
    pump(player, 2000, [&](const media::VideoFrame* f, TimeUs clock) {
        if (!f || wall.elapsed() < 300) // dejar que arranque el audio
            return;
        ++samples;
        const TimeUs lag = clock - f->pts; // cuánto hace que empezó el fotograma mostrado
        maxLag = std::max(maxLag, lag);
        // Se admite mostrar un fotograma hasta medio fotograma antes de su instante (tolerancia de refresco).
        if (lag < -frameDur / 2 || lag > frameDur + 8'000)
            ++late;
    });
    const double elapsed = wall.elapsed() / 1000.0;
    const double position = player.position();
    std::printf("[player] %.3f s de pared → reloj %.3f s; retraso máx. del fotograma %.1f ms; fuera de tiempo %d/%d\n",
                elapsed, position, maxLag / 1000.0, late, samples);

    CHECK(position > elapsed - 0.15);
    CHECK(position < elapsed + 0.05);
    CHECK(samples > 200);
    CHECK(late <= samples / 100); // ≥ 99 % de las muestras con el fotograma correcto
}

TEST_CASE("Pausa congela el reloj y la búsqueda muestra el fotograma pedido", "[player][seek]")
{
    Player player(testOptions());
    REQUIRE(player.openFile(QString::fromStdString(test::mediaPath("sync_720p30.mp4"))));
    player.play();
    pump(player, 500);
    player.pause();
    const double t1 = player.position();
    pump(player, 300);
    CHECK(player.position() == t1);

    player.seek(3.0);
    auto frame = waitForFrame(player, 1500, [](const media::VideoFrame& f) { return f.pts == fromSeconds(3.0); });
    REQUIRE(frame);
    CHECK(player.position() == 3.0);

    SECTION("avance y retroceso fotograma a fotograma")
    {
        player.stepFrames(1);
        auto next = waitForFrame(player, 1500, [](const media::VideoFrame& f) { return f.pts > fromSeconds(3.0); });
        REQUIRE(next);
        CHECK(std::llabs(next->pts - fromSeconds(3.0 + 1.0 / 30)) <= 1000);

        player.stepFrames(-1);
        auto back = waitForFrame(player, 1500, [](const media::VideoFrame& f) { return f.pts <= fromSeconds(3.0); });
        REQUIRE(back);
        CHECK(std::llabs(back->pts - fromSeconds(3.0)) <= 1000);
    }
    SECTION("búsqueda durante la reproducción")
    {
        player.play();
        player.seek(1.0);
        auto f = waitForFrame(player, 1500, [](const media::VideoFrame& fr) {
            return fr.pts >= fromSeconds(1.0) - 34'000 && fr.pts < fromSeconds(1.6);
        });
        REQUIRE(f);
        pump(player, 300);
        CHECK(player.position() > 1.0);
        CHECK(player.position() < 1.6);
    }
}

TEST_CASE("60 fps en una pantalla de 60 Hz no descarta fotogramas", "[player][cadence]")
{
    // Reproduce la situación más delicada: la cadencia del video coincide con la del refresco,
    // así que cada fotograma vence justo en un refresco y la fluctuación decide si se salta.
    Player player(testOptions());
    REQUIRE(player.openFile(QString::fromStdString(test::mediaPath("count_1080p60.mp4"))));
    pump(player, 200);
    player.play();

    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::microseconds(16'667);
    auto deadline = Clock::now();
    int renders = 0;
    std::vector<TimeUs> shown;
    while (renders < 150) { // 2,5 s de "vsync" a 60 Hz
        deadline += interval;
        std::this_thread::sleep_until(deadline);
        QCoreApplication::processEvents();
        auto f = player.frameForPresentation();
        if (f && (shown.empty() || shown.back() != f->pts))
            shown.push_back(f->pts);
        ++renders;
    }
    std::printf("[cadencia] %d refrescos → %zu fotogramas distintos, %d descartados\n", renders, shown.size(),
                player.droppedFrames());
    CHECK(player.droppedFrames() <= 1); // como mucho uno al arrancar
    for (size_t i = 1; i < shown.size(); ++i)
        CHECK(shown[i] - shown[i - 1] <= player.frameDurationUs() + 1000); // nunca se salta un fotograma
}

TEST_CASE("Video sin audio usa el reloj de pared y se detiene al final", "[player]")
{
    Player player(testOptions());
    REQUIRE(player.openFile(QString::fromStdString(test::mediaPath("count_1080p60.mp4"))));
    CHECK(player.hasVideo());
    CHECK_FALSE(player.hasAudio());
    QSignalSpy ended(&player, &Player::endReached);

    player.seek(2.0);
    pump(player, 200);
    player.play();
    pump(player, 500);
    CHECK(player.position() > 2.4);
    pump(player, 1000);
    CHECK(ended.count() == 1);
    CHECK_FALSE(player.isPlaying());
    CHECK(player.position() > 2.9);

    // Reproducir al final vuelve a empezar.
    player.play();
    pump(player, 300);
    CHECK(player.position() < 0.5);
}

TEST_CASE("Audio sin video", "[player]")
{
    Player player(testOptions());
    REQUIRE(player.openFile(QString::fromStdString(test::mediaPath("tone_44k_mono.wav"))));
    CHECK_FALSE(player.hasVideo());
    CHECK(player.hasAudio());
    player.play();
    pump(player, 800);
    CHECK(player.position() > 0.6);
    CHECK(player.position() < 0.9);
}

TEST_CASE("Un archivo inválido informa el error sin bloquear", "[player]")
{
    Player player(testOptions());
    QSignalSpy errors(&player, &Player::errorOccurred);
    CHECK_FALSE(player.openFile(QString::fromStdString(test::mediaPath("no_existe.mp4"))));
    CHECK(errors.count() == 1);
    CHECK_FALSE(player.hasMedia());
    player.play(); // no debe hacer nada
    CHECK_FALSE(player.isPlaying());
}
