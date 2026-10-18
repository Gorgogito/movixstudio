#include "render/OffscreenRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>

using namespace movix;

namespace {

// Fotograma NV12 de un color uniforme (Y, Cb, Cr de 8 bits, BT.709 rango limitado).
std::shared_ptr<const media::VideoFrame> solidFrame(int w, int h, uint8_t y, uint8_t cb, uint8_t cr)
{
    auto f = std::make_shared<media::VideoFrame>();
    f->width = w;
    f->height = h;
    f->colorSpace = media::ColorSpace::BT709;
    f->y.assign(size_t(w) * size_t(h), y);
    f->uv.resize(size_t(f->chromaWidth()) * 2 * size_t(f->chromaHeight()));
    for (size_t i = 0; i < f->uv.size(); i += 2) {
        f->uv[i] = cb;
        f->uv[i + 1] = cr;
    }
    return f;
}

const auto kRed = [](int w, int h) { return solidFrame(w, h, 63, 102, 240); };
const auto kBlue = [](int w, int h) { return solidFrame(w, h, 32, 240, 118); };

struct Rgb {
    int r, g, b;
};

Rgb pixel(const std::vector<uint8_t>& rgba, int width, int x, int y)
{
    const size_t i = (size_t(y) * size_t(width) + size_t(x)) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2]};
}

bool near(Rgb a, Rgb b, int tol = 6)
{
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

} // namespace

TEST_CASE("Composición fuera de pantalla con colores y ajuste de aspecto exactos", "[render]")
{
    constexpr int W = 320, H = 180; // lienzo 16:9
    render::OffscreenRenderer renderer;
    QString error;
    REQUIRE(renderer.init(W, H, &error));
    std::printf("[render] %s\n", qPrintable(renderer.backendName()));

    engine::FrameBundle bundle;
    bundle.canvasWidth = W;
    bundle.canvasHeight = H;
    std::vector<uint8_t> rgba;

    SECTION("lienzo vacío: negro")
    {
        REQUIRE(renderer.render(bundle, rgba));
        CHECK(near(pixel(rgba, W, W / 2, H / 2), {0, 0, 0}));
    }
    SECTION("una capa a pantalla completa con la conversión BT.709")
    {
        bundle.layers.push_back({1, 0, kRed(64, 36)});
        REQUIRE(renderer.render(bundle, rgba));
        CHECK(near(pixel(rgba, W, 5, 5), {255, 0, 0}));
        CHECK(near(pixel(rgba, W, W - 5, H - 5), {255, 0, 0}));
    }
    SECTION("capa 4:3 sobre lienzo 16:9: bandas laterales negras")
    {
        bundle.layers.push_back({1, 0, kBlue(48, 36)});
        REQUIRE(renderer.render(bundle, rgba));
        CHECK(near(pixel(rgba, W, W / 2, H / 2), {0, 0, 255}));
        CHECK(near(pixel(rgba, W, 10, H / 2), {0, 0, 0}));          // banda izquierda
        CHECK(near(pixel(rgba, W, W - 10, H / 2), {0, 0, 0}));      // banda derecha
        // 4:3 en 180 px de alto = 240 px de ancho, de x = 40 a x = 280.
        CHECK(near(pixel(rgba, W, 42, H / 2), {0, 0, 255}));
        CHECK(near(pixel(rgba, W, 37, H / 2), {0, 0, 0}));
    }
    SECTION("la capa superior tapa a la inferior y sus bandas la dejan ver")
    {
        bundle.layers.push_back({1, 0, kRed(64, 36)});
        bundle.layers.push_back({2, 1, kBlue(48, 36)});
        REQUIRE(renderer.render(bundle, rgba));
        CHECK(near(pixel(rgba, W, W / 2, H / 2), {0, 0, 255}));
        CHECK(near(pixel(rgba, W, 10, H / 2), {255, 0, 0}));
    }
    SECTION("la orientación vertical se conserva")
    {
        auto f = solidFrame(64, 36, 235, 128, 128); // blanco
        auto* frame = const_cast<media::VideoFrame*>(f.get());
        std::fill(frame->y.begin(), frame->y.begin() + 64 * 18, uint8_t(16)); // mitad superior negra
        bundle.layers.push_back({1, 0, f});
        REQUIRE(renderer.render(bundle, rgba));
        CHECK(near(pixel(rgba, W, W / 2, 10), {0, 0, 0}));
        CHECK(near(pixel(rgba, W, W / 2, H - 10), {255, 255, 255}));
    }
}
