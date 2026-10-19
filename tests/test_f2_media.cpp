#include "TestMedia.h"
#include "core/Edits.h"
#include "engine/MediaImport.h"
#include "engine/Sources.h"
#include "render/OffscreenRenderer.h"
#include "render/TitleRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>

using namespace movix;
using namespace movix::core;

namespace {

std::shared_ptr<const media::VideoFrame> solid(int w, int h, uint8_t y, uint8_t cb, uint8_t cr)
{
    auto f = std::make_shared<media::VideoFrame>();
    f->width = w;
    f->height = h;
    f->y.assign(size_t(w) * size_t(h), y);
    f->uv.resize(size_t(f->chromaWidth()) * 2 * size_t(f->chromaHeight()));
    for (size_t i = 0; i < f->uv.size(); i += 2) {
        f->uv[i] = cb;
        f->uv[i + 1] = cr;
    }
    return f;
}
auto red() { return solid(64, 36, 63, 102, 240); }
auto blue() { return solid(64, 36, 32, 240, 118); }

struct Rgb {
    int r, g, b;
};
Rgb at(const std::vector<uint8_t>& px, int w, int x, int y)
{
    const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 4;
    return {px[i], px[i + 1], px[i + 2]};
}
bool near(Rgb a, Rgb b, int tol = 8)
{
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

constexpr int W = 320, H = 180;

engine::LayerFrame layer(std::shared_ptr<const media::VideoFrame> f)
{
    engine::LayerFrame l;
    l.frame = std::move(f);
    return l;
}

} // namespace

TEST_CASE("Compositor: transformación, opacidad y recorte de capas", "[render][f2]")
{
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    engine::FrameBundle b;
    b.canvasWidth = W;
    b.canvasHeight = H;
    std::vector<uint8_t> px;

    SECTION("escala a la mitad: la capa ocupa el centro")
    {
        auto l = layer(red());
        l.transform.scale = 0.5;
        b.layers.push_back(l);
        REQUIRE(r.render(b, px));
        CHECK(near(at(px, W, W / 2, H / 2), {255, 0, 0}));
        CHECK(near(at(px, W, 10, 10), {0, 0, 0}));
        CHECK(near(at(px, W, 85, 50), {255, 0, 0}));  // dentro de [80, 240] × [45, 135]
        CHECK(near(at(px, W, 75, 50), {0, 0, 0}));
    }
    SECTION("desplazamiento horizontal de un cuarto del lienzo")
    {
        auto l = layer(red());
        l.transform.x = 0.25;
        b.layers.push_back(l);
        REQUIRE(r.render(b, px));
        CHECK(near(at(px, W, 70, H / 2), {0, 0, 0}));    // [0, 80) queda vacío
        CHECK(near(at(px, W, 90, H / 2), {255, 0, 0}));
        CHECK(near(at(px, W, W - 3, H / 2), {255, 0, 0})); // lo que sale del lienzo se recorta
    }
    SECTION("rotación de 90 grados")
    {
        auto l = layer(red());
        l.transform.rotation = 90;
        b.layers.push_back(l);
        REQUIRE(r.render(b, px));
        // Rotada, la capa 16:9 de 320×180 mide 180 de ancho y 320 de alto, centrada: x ∈ [70, 250].
        CHECK(near(at(px, W, W / 2, 5), {255, 0, 0}));
        CHECK(near(at(px, W, 60, H / 2), {0, 0, 0}));
        CHECK(near(at(px, W, 80, H / 2), {255, 0, 0}));
    }
    SECTION("opacidad: azul al 50 % sobre rojo")
    {
        b.layers.push_back(layer(red()));
        auto l = layer(blue());
        l.opacity = 0.5;
        b.layers.push_back(l);
        REQUIRE(r.render(b, px));
        CHECK(near(at(px, W, W / 2, H / 2), {128, 0, 128}, 10));
    }
    SECTION("recorte: solo la mitad derecha de la capa superior")
    {
        b.layers.push_back(layer(red()));
        auto l = layer(blue());
        l.crop.left = 0.5;
        b.layers.push_back(l);
        REQUIRE(r.render(b, px));
        CHECK(near(at(px, W, 40, H / 2), {255, 0, 0}));
        CHECK(near(at(px, W, W - 40, H / 2), {0, 0, 255}));
    }
}

TEST_CASE("Imagen PNG con transparencia sobre video", "[engine][f2]")
{
    auto png = engine::probeMediaItem(test::mediaPath("half_alpha.png"));
    REQUIRE(png);
    CHECK(png->isStill);
    CHECK(png->width == 320);
    auto video = engine::probeMediaItem(test::mediaPath("sync_720p30.mp4"));
    REQUIRE(video);

    Project p = Project::createDefault();
    p.settings.width = W;
    p.settings.height = H;
    const Id v = edit::addMedia(p, *video);
    const Id s = edit::addMedia(p, *png);
    edit::placeClip(p, p.videoTracks[0].id, v, 0);       // fotograma 0: destello blanco
    const Id still = edit::placeClip(p, p.videoTracks[1].id, s, 0);
    CHECK(p.findClip(still)->duration == 150);           // 5 s por defecto

    engine::VideoSources sources(false);
    const engine::FrameBundle bundle = sources.build(p, 0);
    REQUIRE(bundle.layers.size() == 2);
    CHECK(bundle.layers[1].frame->format == media::PixelFormat::RGBA);
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    std::vector<uint8_t> px;
    REQUIRE(r.render(bundle, px));
    CHECK(near(at(px, W, 40, H / 2), {0, 255, 0}));        // mitad opaca de la imagen
    CHECK(near(at(px, W, W - 40, H / 2), {255, 255, 255})); // mitad transparente: el video de debajo
}

TEST_CASE("Títulos generados con QPainter", "[engine][render][f2]")
{
    Project p = Project::createDefault();
    p.settings.width = W;
    p.settings.height = H;
    TitleStyle style;
    style.text = "MMMM";
    style.size = 0.4;
    style.shadow = false;
    style.color = 0xFFFFFFFF;
    edit::placeTitle(p, p.videoTracks[0].id, 0, 30, style);

    engine::VideoSources sources(false);
    sources.setTitleRenderer(std::make_shared<render::QtTitleRenderer>());
    const engine::FrameBundle bundle = sources.build(p, 0);
    REQUIRE(bundle.layers.size() == 1);
    const auto& f = *bundle.layers[0].frame;
    REQUIRE(f.format == media::PixelFormat::RGBA);
    CHECK(f.width == W);
    // Esquina transparente; en el centro del texto hay píxeles blancos opacos.
    CHECK(f.rgba[3] == 0);
    int opaqueWhite = 0;
    for (int y = H / 2 - 20; y < H / 2 + 20; ++y)
        for (int x = W / 2 - 60; x < W / 2 + 60; ++x) {
            const size_t i = (size_t(y) * W + size_t(x)) * 4;
            if (f.rgba[i + 3] > 250 && f.rgba[i] > 250)
                ++opaqueWhite;
        }
    CHECK(opaqueWhite > 500);
    // Se reutiliza de la caché mientras el estilo no cambie.
    CHECK(sources.build(p, 10).layers[0].frame == bundle.layers[0].frame);
}

TEST_CASE("Audio a doble velocidad conserva el tono", "[engine][audio][f2]")
{
    Project p = Project::createDefault();
    auto tone = engine::probeMediaItem(test::mediaPath("tone_44k_mono.wav")); // 440 Hz, 3 s
    REQUIRE(tone);
    const Id m = edit::addMedia(p, *tone);
    const Id c = edit::placeClip(p, p.audioTracks[0].id, m, 0);
    REQUIRE(edit::setClipSpeed(p, c, 2.0));
    CHECK(p.findClip(c)->duration == 45); // 1,5 s

    engine::AudioMixer mixer(48000, 2);
    std::vector<float> all;
    std::vector<float> block(1024 * 2);
    for (int64_t pos = 0; pos < 48000 * 2; pos += 1024) {
        mixer.mix(p, pos, 1024, block.data());
        all.insert(all.end(), block.begin(), block.end());
    }
    // Frecuencia por cruces por cero en [0,2 s, 1,2 s): debe seguir siendo ~440 Hz.
    int crossings = 0;
    for (size_t i = size_t(0.2 * 48000); i < size_t(1.2 * 48000); ++i)
        if ((all[i * 2] >= 0) != (all[(i + 1) * 2] >= 0))
            ++crossings;
    const double freq = crossings / 2.0;
    std::printf("[f2] tono a 2x: %.1f Hz (original 440 Hz)\n", freq);
    CHECK(std::abs(freq - 440.0) < 440.0 * 0.03);
    // Suena hasta 1,5 s y después hay silencio.
    double late = 0, early = 0;
    for (size_t i = size_t(1.3 * 48000); i < size_t(1.45 * 48000); ++i)
        early = std::max(early, double(std::fabs(all[i * 2])));
    for (size_t i = size_t(1.55 * 48000); i < size_t(1.9 * 48000); ++i)
        late = std::max(late, double(std::fabs(all[i * 2])));
    CHECK(early > 0.05);
    CHECK(late == 0.0);
}
