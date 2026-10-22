#include "TestMedia.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"
#include "engine/MediaImport.h"
#include "engine/Sources.h"
#include "engine/VideoReader.h"
#include "render/OffscreenRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace movix;
using namespace movix::core;

namespace {

// Proyecto a 25 fps con un clip de 10 s de un medio de 20 s (sin archivo).
struct Fixture {
    Project p = Project::createDefault();
    Id clip = kNoId;
    Fixture()
    {
        p.settings.frameRate = {25, 1};
        MediaItem m;
        m.path = "medio.mp4";
        m.name = "medio";
        m.duration = 20 * kUsPerSecond;
        m.hasVideo = m.hasAudio = true;
        m.width = 1920;
        m.height = 1080;
        const Id media = edit::addMedia(p, m);
        clip = edit::placeClip(p, p.videoTracks[0].id, media, 10, 2 * kUsPerSecond, 250);
        REQUIRE(clip != kNoId);
    }
    Clip& c() { return *p.findClip(clip); }
};

} // namespace

TEST_CASE("Velocidad variable: el medio avanza con la integral de la velocidad", "[core][speed]")
{
    Fixture f;
    auto& s = f.p.settings;
    // Rampa de 1× (fotograma 0) a 2× (fotograma 50); después se mantiene 2×.
    REQUIRE(edit::toggleKeyframe(f.p, f.clip, edit::Property::Speed, 0));
    REQUIRE(edit::setProperty(f.p, f.clip, edit::Property::Speed, 50, 2.0));
    REQUIRE(f.c().hasRamp());
    CHECK(f.c().speedRamp.keys.front().value == 1.0); // el primer keyframe parte de la velocidad fija
    // En 50 fotogramas (2 s) avanza 2 s × 1,5 de media = 3 s.
    CHECK(sourceTimeAt(s, f.c(), 10 + 50) == 5 * kUsPerSecond);
    CHECK(sourceTimeAt(s, f.c(), 10 + 25) == 2 * kUsPerSecond + 1'250'000); // 1 s a (1 + 1,5) / 2 de media
    // Medio disponible: 18 s desde sourceIn → 3 s de rampa + 15 s a 2× = 7,5 s más: 50 + 187 fotogramas.
    CHECK(edit::maxClipFrames(f.p, f.c()) == 237);
    CHECK(f.c().duration == 237); // el clip (250) se acortó para no pedir más medio del que hay
    CHECK(framesForSource(s, f.c(), clipSourceSpan(s, f.c())) == f.c().duration);
    // Dividir conserva lo que se ve.
    std::vector<TimeUs> before;
    for (FramePos fr = 10; fr < f.c().end(); ++fr)
        before.push_back(sourceTimeAt(s, f.c(), fr));
    const Id right = edit::splitClip(f.p, f.clip, 40);
    REQUIRE(right != kNoId);
    for (FramePos fr = 10; fr < 247; ++fr) {
        const Clip& part = fr < 40 ? f.c() : *f.p.findClip(right);
        INFO("fotograma " << fr);
        CHECK(std::abs(sourceTimeAt(s, part, fr) - before[size_t(fr - 10)]) <= 2);
    }
    // Una velocidad fija quita la variable.
    REQUIRE(edit::setClipSpeed(f.p, f.clip, 0.5));
    CHECK_FALSE(f.c().hasRamp());
    CHECK(f.p.validate());
}

TEST_CASE("Al revés: el mismo tramo del medio, del final al principio", "[core][speed]")
{
    Fixture f;
    auto& s = f.p.settings;
    REQUIRE(edit::setReverse(f.p, f.clip, true));
    // Primer fotograma: el último del tramo [2 s, 12 s); último fotograma: el primero (2 s).
    CHECK(sourceTimeAt(s, f.c(), 10) == 12 * kUsPerSecond - 40'000);
    CHECK(sourceTimeAt(s, f.c(), 259) == 2 * kUsPerSecond);
    std::vector<TimeUs> before;
    for (FramePos fr = 10; fr < 260; ++fr)
        before.push_back(sourceTimeAt(s, f.c(), fr));

    // Dividir y recortar el final conservan lo que se ve en el resto.
    const Id right = edit::splitClip(f.p, f.clip, 100);
    REQUIRE(right != kNoId);
    for (FramePos fr = 10; fr < 260; ++fr) {
        const Clip& part = fr < 100 ? f.c() : *f.p.findClip(right);
        CHECK(sourceTimeAt(s, part, fr) == before[size_t(fr - 10)]);
    }
    REQUIRE(edit::trimClip(f.p, right, edit::Edge::End, 200));
    for (FramePos fr = 100; fr < 200; ++fr)
        CHECK(sourceTimeAt(s, *f.p.findClip(right), fr) == before[size_t(fr - 10)]);
    // Alargar el final de un clip al revés muestra medio anterior: como mucho hasta el principio.
    REQUIRE(edit::trimClip(f.p, right, edit::Edge::End, 2000));
    CHECK(f.p.findClip(right)->sourceIn == 0);
    CHECK(f.p.findClip(right)->end() == 100 + 210); // su comienzo muestra 8,4 s: 8,4 s hacia atrás

    // En el archivo de proyecto, con una rampa.
    REQUIRE(edit::toggleKeyframe(f.p, f.clip, edit::Property::Speed, 0));
    REQUIRE(edit::setProperty(f.p, f.clip, edit::Property::Speed, 30, 1.5));
    const auto loaded = deserializeProject(serializeProject(f.p, "C:/x/p.mxproj"), "C:/x/p.mxproj");
    REQUIRE(loaded.project);
    CHECK(loaded.project->videoTracks == f.p.videoTracks);
    CHECK(f.p.validate());
}

TEST_CASE("Velocidad variable y al revés: tramos de audio", "[core][speed]")
{
    Fixture f;
    auto& s = f.p.settings;
    REQUIRE(edit::setReverse(f.p, f.clip, true));
    auto segs = audioSegmentsIn(f.p, s.frameToTime(10), s.frameToTime(20));
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].varispeed());
    CHECK(segs[0].sourceStart == 12 * kUsPerSecond);
    CHECK(segs[0].sourceEnd == 12 * kUsPerSecond - 400'000);
    CHECK(segs[0].timelineFor(11'800'000) == segs[0].timelineStart + 200'000);

    REQUIRE(edit::setReverse(f.p, f.clip, false));
    REQUIRE(edit::toggleKeyframe(f.p, f.clip, edit::Property::Speed, 0));
    REQUIRE(edit::setProperty(f.p, f.clip, edit::Property::Speed, 50, 2.0));
    segs = audioSegmentsIn(f.p, s.frameToTime(10), s.frameToTime(20));
    REQUIRE(segs.size() == 10); // uno por fotograma
    CHECK(segs[0].sourceEnd > segs[0].sourceStart);
    CHECK(segs[9].speed > segs[0].speed);
    CHECK(segs[9].sourceEnd == sourceTimeAt(s, f.c(), 20));
}

TEST_CASE("Video al revés: fotogramas exactos sin buscar en cada uno", "[engine][speed]")
{
    // count_1080p60: 180 fotogramas a 60 fps. Hacia atrás de 1 en 1 desde el final.
    engine::VideoReader reader;
    REQUIRE(reader.open(test::mediaPath("count_1080p60.mp4"), false));
    REQUIRE(reader.frameAt(0));
    const int seeksBefore = reader.seekCount();
    for (int i = 179; i >= 0; --i) {
        const TimeUs t = TimeUs(std::llround(i * 1e6 / 60.0));
        const auto f = reader.frameAt(t);
        REQUIRE(f);
        INFO("fotograma " << i);
        CHECK(std::abs(f->pts - t) <= 1000);
    }
    // Un bloque de 1 s por búsqueda (más la primera hasta el final).
    CHECK(reader.seekCount() - seeksBefore <= 5);
    // Y de nuevo hacia delante, en orden.
    for (int i = 0; i < 60; ++i) {
        const TimeUs t = TimeUs(std::llround(i * 1e6 / 60.0));
        CHECK(std::abs(reader.frameAt(t)->pts - t) <= 1000);
    }
}

TEST_CASE("Audio al revés: los pitidos suenan al final de su tramo", "[engine][speed]")
{
    // sync_720p30: pitido de 1 kHz en [k, k + 0,1) s, k = 0..4. Al revés, el de 1 s (medio)
    // suena en [3,9, 4,0) s de la línea de tiempo.
    Project p = Project::createDefault();
    auto item = engine::probeMediaItem(test::mediaPath("sync_720p30.mp4"));
    REQUIRE(item);
    const Id media = edit::addMedia(p, *item);
    p.settings.frameRate = {30, 1};
    const Id clip = edit::placeClip(p, p.audioTracks[0].id, media, 0, 0, 150);
    REQUIRE(clip != kNoId);
    REQUIRE(edit::setReverse(p, clip, true));
    engine::AudioMixer mixer(48000, 2);
    std::vector<float> all;
    std::vector<float> block(1024 * 2);
    for (int64_t pos = 0; pos < 48000 * 5; pos += 1024) {
        mixer.mix(p, pos, 1024, block.data());
        all.insert(all.end(), block.begin(), block.end());
    }
    // Energía de 1 kHz (correlación con seno y coseno) en un intervalo.
    auto tone = [&](double from, double to) {
        double re = 0, im = 0;
        const size_t a = size_t(from * 48000), b = size_t(to * 48000);
        for (size_t i = a; i < b; ++i) {
            const double w = 2 * std::numbers::pi * 1000.0 * double(i) / 48000.0;
            re += all[i * 2] * std::cos(w);
            im += all[i * 2] * std::sin(w);
        }
        return std::hypot(re, im) / double(b - a);
    };
    INFO("tono en [3,91, 3,99): " << tone(3.91, 3.99) << "; en [4,01, 4,09): " << tone(4.01, 4.09) << "; en [1,0, 1,08): " << tone(1.0, 1.08));
    CHECK(tone(3.91, 3.99) > 0.2);
    CHECK(tone(4.01, 4.09) < 0.02);
    CHECK(tone(1.0, 1.08) < 0.02); // donde sonaría sin invertir no hay nada (en [1, 1,1) del medio → 3,9 s)
    CHECK(tone(0.91, 0.99) > 0.2); // el de 4 s del medio, en [0,9, 1,0)
}

TEST_CASE("Copia cero: el fotograma decodificado en la GPU se compone igual", "[render][zerocopy]")
{
    Project p = Project::createDefault();
    auto item = engine::probeMediaItem(test::mediaPath("scenes_720p30.mp4"));
    REQUIRE(item);
    const Id media = edit::addMedia(p, *item);
    p.settings.width = 1280;
    p.settings.height = 720;
    REQUIRE(edit::placeClip(p, p.videoTracks[0].id, media, 0, 0, 120) != kNoId);

    render::OffscreenRenderer renderer;
    REQUIRE(renderer.init(1280, 720));
    void* device = renderer.d3d11Device();
    if (!device)
        SKIP("Sin Direct3D 11");
    engine::VideoSources cpu(false), gpu(true);
    gpu.setGpuDevice(device);
    for (FramePos f : {0, 45, 100}) {
        const auto a = cpu.build(p, f);
        const auto b = gpu.build(p, f);
        REQUIRE(b.layers.size() == 1);
        CHECK(b.layers[0].frame->onGpu());
        std::vector<uint8_t> pa, pb;
        REQUIRE(renderer.render(a, pa));
        REQUIRE(renderer.render(b, pb));
        double diff = 0;
        for (size_t i = 0; i < pa.size(); ++i)
            diff += std::abs(int(pa[i]) - int(pb[i]));
        INFO("fotograma " << f << ": diferencia media " << diff / double(pa.size()));
        CHECK(diff / double(pa.size()) < 1.5); // decodificador por CPU frente a GPU: casi idénticos
    }
}
