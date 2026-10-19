#include "TestMedia.h"
#include "color/Hdr.h"
#include "color/Ocio.h"
#include "core/Edits.h"
#include "engine/Sources.h"
#include "export/Exporter.h"
#include "media/ColorMatrix.h"
#include "media/Encoder.h"
#include "media/MediaInfo.h"
#include "media/VideoDecoder.h"
#include "render/OffscreenRenderer.h"
#include "render/TitleRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <cstdio>

using namespace movix;
using namespace movix::core;
using Catch::Approx;

namespace {

uint16_t sample16(const std::vector<uint8_t>& bytes, size_t index)
{
    return uint16_t(bytes[index * 2] | (bytes[index * 2 + 1] << 8));
}

std::shared_ptr<const media::VideoFrame> white(int w, int h)
{
    auto f = std::make_shared<media::VideoFrame>();
    f->width = w;
    f->height = h;
    f->format = media::PixelFormat::RGBA;
    f->rgba.assign(size_t(w) * size_t(h) * 4, 255);
    return f;
}

} // namespace

TEST_CASE("Señales HDR: PQ, HLG y mapeo de tonos", "[color][hdr]")
{
    CHECK(color::nitsToPq(100) == Approx(0.508).margin(0.002));
    CHECK(color::nitsToPq(1000) == Approx(0.752).margin(0.002));
    for (double n : {0.01, 1.0, 50.0, 203.0, 1000.0, 4000.0})
        CHECK(color::pqToNits(color::nitsToPq(n)) == Approx(n).epsilon(1e-6));

    SECTION("el blanco SDR va a 203 nits (BT.2408)")
    {
        const auto pq = color::convertSignal({1, 1, 1}, color::Signal::SDR, false, ColorMode::PQ);
        CHECK(pq.r == Approx(color::nitsToPq(203)).margin(1e-4));
        CHECK(pq.g == Approx(pq.r).margin(1e-4));
        const auto hlg = color::convertSignal({1, 1, 1}, color::Signal::SDR, false, ColorMode::HLG);
        CHECK(hlg.r == Approx(0.75).margin(0.005)); // HLG al 75 %
    }
    SECTION("SDR → HDR → SDR conserva los tonos medios")
    {
        const color::Rgb mid{0.5f, 0.4f, 0.3f};
        const auto pq = color::convertSignal(mid, color::Signal::SDR, false, ColorMode::PQ);
        const auto back = color::convertSignal(pq, color::Signal::PQ, true, ColorMode::SDR);
        CHECK(back.r == Approx(mid.r).margin(0.01));
        CHECK(back.g == Approx(mid.g).margin(0.01));
        CHECK(back.b == Approx(mid.b).margin(0.01));
    }
    SECTION("el mapeo de tonos es continuo, creciente y lleva el pico al máximo")
    {
        CHECK(color::toneMap(0.5) == 0.5);
        CHECK(color::toneMap(1000.0 / 203.0) == Approx(1.0));
        double prev = 0;
        for (int i = 0; i <= 200; ++i) {
            const double v = color::toneMap(i * 0.025);
            CHECK(v >= prev);
            prev = v;
        }
        CHECK(color::toneMap(0.7001) == Approx(0.7001).margin(1e-4));
    }
    CHECK_FALSE(color::needsConversion(color::Signal::SDR, false, ColorMode::SDR));
    CHECK_FALSE(color::needsConversion(color::Signal::PQ, true, ColorMode::PQ));
    CHECK(color::needsConversion(color::Signal::PQ, true, ColorMode::SDR));
}

TEST_CASE("Decodificación de 10 bits y de la señal HDR", "[media][hdr]")
{
    const auto info = media::probe(test::mediaPath("hdr_pq.mkv"));
    REQUIRE(info);
    media::VideoDecoder deep;
    REQUIRE(deep.open(test::mediaPath("hdr_pq.mkv"), media::VideoDecoderOptions{false, true}));
    const auto f = deep.decodeNext();
    REQUIRE(f);
    CHECK(f->format == media::PixelFormat::P016);
    CHECK(f->transfer == media::Transfer::PQ);
    CHECK(f->colorSpace == media::ColorSpace::BT2020);
    CHECK(f->y.size() == size_t(f->width) * size_t(f->height) * 2);
    // Gris 0x9C de 8 bits = 156 → en 10 bits de rango limitado 64 + 876·156/255 ≈ 600.
    const int code = sample16(f->y, size_t(f->width) * 90 + 160) >> 6;
    CHECK(std::abs(code - 600) <= 2);

    media::VideoDecoder shallow; // por defecto, NV12 de 8 bits (miniaturas, pruebas)
    REQUIRE(shallow.open(test::mediaPath("hdr_pq.mkv"), media::VideoDecoderOptions{false, false}));
    const auto g = shallow.decodeNext();
    REQUIRE(g);
    CHECK(g->format == media::PixelFormat::NV12);
    CHECK(g->transfer == media::Transfer::PQ);
}

TEST_CASE("GPU: conversión de la señal de cada capa a la del proyecto", "[render][hdr]")
{
    constexpr int W = 160, H = 90;
    SECTION("un título SDR en un proyecto HDR10 queda a 203 nits")
    {
        render::OffscreenRenderer r;
        REQUIRE(r.init(W, H, nullptr, true));
        engine::FrameBundle b;
        b.canvasWidth = W;
        b.canvasHeight = H;
        b.colorMode = ColorMode::PQ;
        engine::LayerFrame l;
        l.frame = white(16, 9);
        b.layers.push_back(l);
        std::vector<uint8_t> px;
        REQUIRE(r.render(b, px));
        REQUIRE(px.size() == size_t(W) * H * 8);
        const double v = sample16(px, (size_t(H / 2) * W + W / 2) * 4) / 65535.0;
        std::printf("[hdr] blanco SDR en PQ: %.4f (esperado %.4f)\n", v, color::nitsToPq(203));
        CHECK(v == Approx(color::nitsToPq(203)).margin(0.003));
    }
    SECTION("la clave de croma se calcula sobre la señal de la fuente, también en HDR")
    {
        render::OffscreenRenderer r;
        REQUIRE(r.init(W, H, nullptr, true));
        engine::FrameBundle b;
        b.canvasWidth = W;
        b.canvasHeight = H;
        b.colorMode = ColorMode::PQ;
        auto solid = [](uint8_t red, uint8_t green, uint8_t blue) {
            auto f = std::make_shared<media::VideoFrame>();
            f->width = 16;
            f->height = 9;
            f->format = media::PixelFormat::RGBA;
            f->rgba.resize(16 * 9 * 4);
            for (size_t i = 0; i < f->rgba.size(); i += 4) {
                f->rgba[i] = red;
                f->rgba[i + 1] = green;
                f->rgba[i + 2] = blue;
                f->rgba[i + 3] = 255;
            }
            return f;
        };
        engine::LayerFrame back;
        back.frame = solid(255, 0, 0);
        engine::LayerFrame screen;
        screen.frame = solid(0, 0xB1, 0x40);
        screen.chromaKey = {true, 0xFF00B140, 0.25, 0.1, 0.5};
        b.layers = {back, screen};
        std::vector<uint8_t> px;
        REQUIRE(r.render(b, px));
        const size_t o = (size_t(H / 2) * W + W / 2) * 4;
        const auto red = color::convertSignal({1, 0, 0}, color::Signal::SDR, false, ColorMode::PQ);
        CHECK(sample16(px, o) / 65535.0 == Approx(red.r).margin(0.003)); // se ve el fondo rojo
        CHECK(sample16(px, o + 1) / 65535.0 == Approx(red.g).margin(0.003));
    }
    SECTION("un clip PQ en un proyecto SDR se ve como la referencia en la CPU")
    {
        media::VideoDecoder dec;
        REQUIRE(dec.open(test::mediaPath("hdr_pq.mkv"), media::VideoDecoderOptions{false, true}));
        const auto f = dec.decodeNext();
        REQUIRE(f);
        // Referencia: YUV de 16 bits → R'G'B' PQ (BT.2020) → señal SDR.
        const auto m = media::yuvToRgbMatrix(f->colorSpace, f->fullRange, true);
        const size_t i = size_t(f->width) * 90 + 160;
        const size_t ci = (size_t(45) * size_t(f->chromaWidth()) + 80) * 2;
        const double y = sample16(f->y, i) / 65535.0, cb = sample16(f->uv, ci) / 65535.0, cr = sample16(f->uv, ci + 1) / 65535.0;
        double rgb[3];
        for (size_t c = 0; c < 3; ++c)
            rgb[c] = std::clamp(m[c * 4] * y + m[c * 4 + 1] * cb + m[c * 4 + 2] * cr + m[c * 4 + 3], 0.0, 1.0);
        const auto ref = color::convertSignal({float(rgb[0]), float(rgb[1]), float(rgb[2])}, color::Signal::PQ, true, ColorMode::SDR);

        render::OffscreenRenderer r;
        REQUIRE(r.init(W, H));
        engine::FrameBundle b;
        b.canvasWidth = W;
        b.canvasHeight = H;
        engine::LayerFrame l;
        l.frame = f;
        b.layers.push_back(l);
        std::vector<uint8_t> px;
        REQUIRE(r.render(b, px));
        const size_t o = (size_t(H / 2) * W + W / 2) * 4;
        std::printf("[hdr] PQ → SDR: GPU %d, referencia %.1f\n", px[o], ref.r * 255);
        CHECK(std::abs(px[o] - ref.r * 255) <= 3);
        CHECK(std::abs(px[o + 2] - ref.b * 255) <= 3);
    }
}

TEST_CASE("Exportar un proyecto HDR10 a HEVC de 10 bits", "[export][hdr]")
{
    const auto& encoders = media::Encoder::availableVideoEncoders();
    if (std::none_of(encoders.begin(), encoders.end(), [](const auto& e) { return e.name == "libx265"; }))
        SKIP("Sin libx265");
    QTemporaryDir dir;
    Project p = Project::createDefault();
    p.settings.width = 320;
    p.settings.height = 180;
    REQUIRE(edit::setColorMode(p, ColorMode::PQ));
    TitleStyle style;
    style.background = 0xFFFFFFFF; // recuadro blanco: mucha superficie de blanco SDR
    style.text = "M";
    style.size = 0.5;
    edit::placeTitle(p, p.videoTracks[0].id, 0, 5, style);
    exporting::ExportSettings s;
    s.path = dir.filePath("hdr10.mkv");
    s.width = 320;
    s.height = 180;
    s.frameRate = p.settings.frameRate;
    s.videoCodec = "libx265";
    exporting::Exporter exporter;
    std::atomic<bool> cancel{false};
    QString error;
    REQUIRE(exporter.run(p, s, cancel, {}, &error));

    media::VideoDecoder dec;
    REQUIRE(dec.open(s.path.toStdString(), media::VideoDecoderOptions{false, true}));
    const auto f = dec.decodeNext();
    REQUIRE(f);
    CHECK(f->format == media::PixelFormat::P016);
    CHECK(f->transfer == media::Transfer::PQ);
    CHECK(f->colorSpace == media::ColorSpace::BT2020);
    // El blanco SDR del recuadro: PQ(203 nits) = 0,58 → código de 10 bits 64 + 876·0,58 ≈ 573.
    // Se toma el valor más frecuente entre los claros (el máximo incluye el sobreimpulso de la
    // compresión en los bordes).
    std::array<int, 1024> hist{};
    for (size_t i = 0; i < f->y.size() / 2; ++i)
        ++hist[size_t(sample16(f->y, i) >> 6)];
    const int mode = int(std::max_element(hist.begin() + 300, hist.end()) - hist.begin());
    std::printf("[hdr] luma del blanco exportado: %d (esperado ~573)\n", mode);
    CHECK(std::abs(mode - 573) <= 4);

    SECTION("un códec de 8 bits se rechaza con un mensaje claro")
    {
        s.videoCodec = "libx264";
        s.path = dir.filePath("hdr8.mp4");
        exporting::Exporter again;
        QString err;
        const bool ok = again.run(p, s, cancel, {}, &err);
        if (!ok)
            CHECK(err.contains("10 bits"));
        // (Algunas compilaciones de x264 admiten 10 bits: entonces la exportación es válida.)
    }
}

TEST_CASE("OpenColorIO: espacios de cámara en la LUT del clip", "[color][ocio]")
{
    std::printf("[ocio] %s\n", color::ocioDescription().c_str());
    const auto& spaces = color::ocioInputSpaces();
    REQUIRE_FALSE(spaces.empty());
    const auto slog3 = std::find_if(spaces.begin(), spaces.end(), [](const color::InputSpace& s) { return s.name.find("S-Log3") != std::string::npos; });
    REQUIRE(slog3 != spaces.end());
    std::printf("[ocio] %zu espacios de entrada; p. ej. «%s» (%s)\n", spaces.size(), slog3->name.c_str(), slog3->family.c_str());

    std::string error;
    const auto toSdr = color::ocioInputTransform(slog3->name, ColorMode::SDR, &error);
    INFO(error);
    REQUIRE(toSdr);
    // Gris del 18 % en S-Log3 (código 0,41): queda en los medios tonos de la salida SDR.
    const auto gray = toSdr({0.41f, 0.41f, 0.41f});
    std::printf("[ocio] gris del 18 %% S-Log3 → SDR %.3f\n", gray.g);
    CHECK(gray.g > 0.3f);
    CHECK(gray.g < 0.6f);
    CHECK(std::abs(gray.r - gray.b) < 0.02f); // neutro
    const auto black = toSdr({0.09f, 0.09f, 0.09f});
    CHECK(black.g < gray.g);
    CHECK_FALSE(color::ocioInputTransform("No existe", ColorMode::SDR, &error));

    // En el motor: la corrección con espacio de entrada genera una LUT que convierte el material
    // y la capa no se vuelve a convertir en el shader.
    Project p = Project::createDefault();
    p.settings.width = 64;
    p.settings.height = 36;
    const Id clip = edit::placeTitle(p, p.videoTracks[0].id, 0, 5, TitleStyle{});
    ColorGrade g;
    g.inputSpace = slog3->name;
    REQUIRE(edit::setColorGrade(p, clip, g));
    engine::VideoSources sources;
    sources.setTitleRenderer(std::make_shared<render::QtTitleRenderer>());
    const auto b = sources.build(p, 0);
    REQUIRE(b.layers.size() == 1);
    CHECK(b.layers[0].rawSignal);
    REQUIRE(b.layers[0].lut);
    CHECK(b.layers[0].lut->sample({0.41f, 0.41f, 0.41f}).g == Approx(gray.g).margin(0.01));
}
