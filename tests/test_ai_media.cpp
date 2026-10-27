#include "TestMedia.h"
#include "ai/Beats.h"
#include "ai/Scenes.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <functional>
#include <numbers>
#include <random>

using namespace movix;

namespace {

// Pista de percusión sintética en 4/4: charles en cada corchea, bombo en los tiempos 1 y 3 y caja
// en el 2 y el 4. Los pulsos son las negras; el tempo de cada una, el de `bpmAt` en su instante.
std::vector<float> drumTrack(const std::function<double(double)>& bpmAt, double offset, double seconds, int rate, std::vector<double>& beats)
{
    std::vector<float> pcm(size_t(seconds * rate), 0.0f);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (float& v : pcm)
        v = 0.01f * noise(rng);
    enum Kind { Kick, Snare, Hat };
    auto hit = [&](double at, Kind kind) {
        const size_t start = size_t(at * rate);
        for (size_t i = 0; i < size_t(0.25 * rate) && start + i < pcm.size(); ++i) {
            const double t = double(i) / rate;
            double s = 0;
            if (kind == Kick)
                s = 0.8 * std::sin(2 * std::numbers::pi * 60 * t) * std::exp(-t / 0.08);
            else if (kind == Snare)
                s = (0.3 * std::sin(2 * std::numbers::pi * 200 * t) + 0.4 * noise(rng)) * std::exp(-t / 0.05);
            else
                s = 0.12 * noise(rng) * std::exp(-t / 0.015);
            pcm[start + i] += float(s);
        }
    };
    int n = 0;
    for (double t = offset, period = 60.0 / bpmAt(t); t < seconds - 0.3; t += period, period = 60.0 / bpmAt(t), ++n) {
        beats.push_back(t);
        hit(t, n % 2 == 0 ? Kick : Snare);
        hit(t, Hat);
        hit(t + period / 2, Hat);
    }
    return pcm;
}

// Pulsos reales encontrados (a menos de `tolerance` s de uno detectado) y detectados que caen
// sobre uno real.
std::pair<int, int> matchBeats(const std::vector<double>& truth, const std::vector<TimeUs>& found, double tolerance)
{
    auto near = [&](double t, const auto& list, auto toS) {
        for (const auto& x : list)
            if (std::abs(toS(x) - t) < tolerance)
                return true;
        return false;
    };
    int recalled = 0, precise = 0;
    for (double t : truth)
        recalled += near(t, found, [](TimeUs b) { return toSeconds(b); });
    for (TimeUs b : found)
        precise += near(toSeconds(b), truth, [](double x) { return x; });
    return {recalled, precise};
}

void checkBeats(double bpm, double offset)
{
    std::vector<double> truth;
    const auto pcm = drumTrack([&](double) { return bpm; }, offset, 20.0, ai::kBeatSampleRate, truth);
    ai::BeatResult r;
    REQUIRE(ai::detectBeats(pcm, ai::kBeatSampleRate, r));
    INFO(bpm << " BPM → " << r.bpm << ", " << r.beats.size() << " pulsos; primeros: " << toSeconds(r.beats[0]) << " "
              << toSeconds(r.beats[1]) << " " << toSeconds(r.beats[2]) << " (reales " << truth[0] << " " << truth[1] << ")");
    CHECK(r.bpm == Catch::Approx(bpm).margin(1.0));
    CHECK(std::abs(int(r.beats.size()) - int(truth.size())) <= 2);
    // Cada pulso detectado cae a menos de 35 ms de uno real.
    int aligned = 0;
    for (TimeUs b : r.beats) {
        double nearest = 1e9;
        for (double t : truth)
            nearest = std::min(nearest, std::abs(toSeconds(b) - t));
        aligned += nearest < 0.035;
    }
    CHECK(aligned >= int(r.beats.size()) - 1);
    CHECK_FALSE(r.variableTempo);
}

void checkVaryingBeats(const char* name, const std::function<double(double)>& bpmAt, double seconds, double medianBpm)
{
    std::vector<double> truth;
    const auto pcm = drumTrack(bpmAt, 0.2, seconds, ai::kBeatSampleRate, truth);
    ai::BeatResult r;
    REQUIRE(ai::detectBeats(pcm, ai::kBeatSampleRate, r));
    const auto [recalled, precise] = matchBeats(truth, r.beats, 0.05);
    std::printf("[beats] %s: %zu pulsos reales, %zu detectados; %d encontrados, %d bien colocados; %.1f BPM típico\n", name, truth.size(),
                r.beats.size(), recalled, precise, r.bpm);
    INFO(name);
    CHECK(r.variableTempo);
    CHECK(recalled >= int(0.9 * double(truth.size())));
    CHECK(precise >= int(0.9 * double(r.beats.size())));
    CHECK(r.bpm == Catch::Approx(medianBpm).epsilon(0.06));
}

} // namespace

TEST_CASE("Ritmo: tempo y pulsos de una pista de percusión", "[ai][beats]")
{
    checkBeats(120.0, 0.30);
    checkBeats(92.0, 0.10);
    checkBeats(150.0, 0.55);

    // Ruido sin pulsos: no hay ritmo que detectar.
    std::vector<float> noise(size_t(ai::kBeatSampleRate * 10));
    std::mt19937 rng(1);
    std::normal_distribution<float> n(0.0f, 0.1f);
    for (float& v : noise)
        v = n(rng);
    ai::BeatResult r;
    CHECK_FALSE(ai::detectBeats(noise, ai::kBeatSampleRate, r));
    CHECK_FALSE(ai::detectBeats(std::vector<float>(ai::kBeatSampleRate * 10, 0.0f), ai::kBeatSampleRate, r));
}

TEST_CASE("Ritmo: pulsos con el tempo cambiando", "[ai][beats]")
{
    // Accelerando de 90 a 150 BPM en 40 s, ritardando de 140 a 100 y un salto de 100 a 128 a los 12 s.
    checkVaryingBeats("accelerando", [](double t) { return 90.0 + 60.0 * std::min(1.0, t / 40.0); }, 40.0, 120.0);
    checkVaryingBeats("ritardando", [](double t) { return 140.0 - 40.0 * std::min(1.0, t / 30.0); }, 30.0, 120.0);
    checkVaryingBeats("cambio brusco", [](double t) { return t < 12.0 ? 100.0 : 128.0; }, 30.0, 128.0);
}

TEST_CASE("Escenas: picos aislados por encima de su entorno", "[ai][scenes]")
{
    std::vector<TimeUs> times;
    std::vector<float> d;
    for (int i = 0; i < 300; ++i) {
        times.push_back(TimeUs(i) * 33'333);
        d.push_back(0.03f + 0.02f * float(i % 3)); // movimiento: variación pequeña constante
    }
    d[100] = 0.6f; // corte
    d[110] = 0.5f; // otro corte demasiado cerca (< 0,5 s): se ignora
    d[200] = 0.12f; // salto pequeño: por debajo del umbral
    for (int i = 240; i < 260; ++i)
        d[size_t(i)] = 0.3f; // movimiento muy rápido y sostenido: no destaca sobre su entorno
    const auto cuts = ai::findCuts(times, d, {});
    REQUIRE(cuts.size() == 1);
    CHECK(cuts[0] == times[100]);
}

TEST_CASE("Escenas: cortes de un video real", "[ai][scenes]")
{
    std::vector<TimeUs> cuts;
    std::string error;
    double lastProgress = 0;
    REQUIRE(ai::detectScenes(test::mediaPath("scenes_720p30.mp4"), 0, 0, {}, cuts, &error,
                             [&](double p) { lastProgress = p; }));
    REQUIRE(cuts.size() == 2);
    CHECK(toSeconds(cuts[0]) == Catch::Approx(2.0).margin(0.001));
    CHECK(toSeconds(cuts[1]) == Catch::Approx(4.0).margin(0.001));
    CHECK(lastProgress > 0.8);

    // Solo un tramo: [1 s, 3 s) contiene el primer corte.
    REQUIRE(ai::detectScenes(test::mediaPath("scenes_720p30.mp4"), kUsPerSecond, 3 * kUsPerSecond, {}, cuts, &error));
    REQUIRE(cuts.size() == 1);
    CHECK(toSeconds(cuts[0]) == Catch::Approx(2.0).margin(0.001));

    // Cancelación y archivo sin video.
    CHECK_FALSE(ai::detectScenes(test::mediaPath("scenes_720p30.mp4"), 0, 0, {}, cuts, &error, {}, [] { return true; }));
    CHECK_FALSE(ai::detectScenes(test::mediaPath("tone_44k_mono.wav"), 0, 0, {}, cuts, &error));
}

TEST_CASE("Escenas: disolvencias y fundidos", "[ai][scenes]")
{
    // Ver scenes_fades.mp4 en GenerateTestMedia.cmake: disolvencia centrada en 2,5 s, fundido a
    // negro y desde negro con el negro hacia 4,05 s, corte en 7,8 s y fundido a negro, negro y
    // aparición entre 8,5 y 10,6 s (negro de ~9,3 a ~9,8 s). El zoom con paneo de 5 a 7,8 s no es
    // un cambio de escena.
    std::vector<TimeUs> cuts;
    std::string error;
    REQUIRE(ai::detectScenes(test::mediaPath("scenes_fades.mp4"), 0, 0, {}, cuts, &error));
    std::string list;
    for (const TimeUs c : cuts)
        list += std::to_string(toSeconds(c)) + " ";
    INFO("cambios: " << list);
    REQUIRE(cuts.size() == 4);
    CHECK(toSeconds(cuts[0]) == Catch::Approx(2.5).margin(0.15));
    CHECK(toSeconds(cuts[1]) == Catch::Approx(4.05).margin(0.1));
    CHECK(toSeconds(cuts[2]) == Catch::Approx(7.8).margin(0.001));
    CHECK(toSeconds(cuts[3]) == Catch::Approx(9.55).margin(0.25));

    // Solo cortes: está el de 7,8 s, y también saltos dentro de las transiciones (en los colores
    // lisos el histograma cambia a golpes), que el análisis gradual descarta.
    ai::SceneOptions cutsOnly;
    cutsOnly.gradual = false;
    REQUIRE(ai::detectScenes(test::mediaPath("scenes_fades.mp4"), 0, 0, cutsOnly, cuts, &error));
    CHECK(std::count(cuts.begin(), cuts.end(), fromSeconds(7.8)) == 1);
    CHECK(cuts.size() > 4);

    // Un tramo que acaba en pleno negro: el fundido del final no separa escenas.
    REQUIRE(ai::detectScenes(test::mediaPath("scenes_fades.mp4"), 6 * kUsPerSecond, fromSeconds(9.5), {}, cuts, &error));
    REQUIRE(cuts.size() == 1);
    CHECK(toSeconds(cuts[0]) == Catch::Approx(7.8).margin(0.001));
}

TEST_CASE("Marcadores: orden, cortes, compuestos, cambio de fps y archivo", "[ai][markers]")
{
    using namespace movix::core;
    Project p = Project::createDefault(); // 30 fps
    MediaItem m;
    m.path = "clip.mp4";
    m.duration = 20 * kUsPerSecond;
    m.hasVideo = m.hasAudio = true;
    m.width = 1280;
    m.height = 720;
    m.frameRate = 30;
    const Id media = edit::addMedia(p, m);
    const Id a = edit::placeClip(p, p.videoTracks[0].id, media, 0, 0, 300);

    CHECK(edit::addMarkers(p, {90, 30, 60, 30, -5}, "Pulso") == 3); // sin repetidos ni negativos
    REQUIRE(p.markers.size() == 3);
    CHECK(p.markers[0].frame == 30);
    CHECK(p.markers[2].frame == 90);
    CHECK(p.markers[0].label == "Pulso");
    CHECK(edit::addMarkers(p, {150}, "") == 1);

    // Cortar [40, 70) quita el marcador de 60 y adelanta los posteriores 30 fotogramas.
    REQUIRE(edit::removeRanges(p, {{40, 70}}) == 30);
    REQUIRE(p.markers.size() == 3);
    CHECK(p.markers[0].frame == 30);
    CHECK(p.markers[1].frame == 60);
    CHECK(p.markers[2].frame == 120);

    // Los marcadores pertenecen a su secuencia: dentro de un compuesto no se ven los de fuera.
    const Id compound = edit::createCompound(p, {a}, "Compuesto");
    REQUIRE(compound != kNoId);
    const Id seq = p.findClip(compound)->sequenceId;
    REQUIRE(edit::openSequence(p, seq));
    CHECK(p.markers.empty());
    edit::addMarkers(p, {10}, "dentro");
    REQUIRE(edit::closeSequence(p));
    CHECK(p.markers.size() == 3);
    CHECK(p.findSequence(seq)->markers.size() == 1);

    // Pasar a 60 fps duplica los fotogramas de todos los marcadores y de los clips de los compuestos.
    const FramePos innerEnd = p.findSequence(seq)->videoTracks[0].clips[0].end();
    REQUIRE(edit::setFrameRate(p, {60, 1}));
    CHECK(p.markers[1].frame == 120);
    CHECK(p.findSequence(seq)->markers[0].frame == 20);
    CHECK(p.findSequence(seq)->videoTracks[0].clips[0].end() == innerEnd * 2);
    std::string why;
    CHECK(p.validate(&why));

    QTemporaryDir dir;
    REQUIRE(saveProjectFile(p, dir.filePath("m.mxproj")));
    const auto loaded = loadProjectFile(dir.filePath("m.mxproj"));
    REQUIRE(loaded.project);
    CHECK(loaded.project->markers == p.markers);
    CHECK(*loaded.project == p);

    CHECK(edit::removeMarker(p, 120));
    CHECK_FALSE(edit::removeMarker(p, 121));
    CHECK(edit::clearMarkers(p, "Pulso"));
    REQUIRE(p.markers.size() == 1); // el que no tiene etiqueta
    CHECK(edit::clearMarkers(p));
    CHECK(p.markers.empty());
}
