#include "color/ColorMath.h"
#include "color/Scopes.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"
#include "engine/Sources.h"
#include "render/OffscreenRenderer.h"
#include "render/TitleRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <random>

using namespace movix;
using namespace movix::core;
using Catch::Approx;

namespace {

constexpr int W = 320, H = 180;

// Capa RGBA de un color uniforme (8 bits por canal).
std::shared_ptr<const media::VideoFrame> rgbaFrame(int w, int h, color::Rgb c, float alpha = 1.0f)
{
    auto f = std::make_shared<media::VideoFrame>();
    f->width = w;
    f->height = h;
    f->format = media::PixelFormat::RGBA;
    f->rgba.resize(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < f->rgba.size(); i += 4) {
        f->rgba[i] = uint8_t(std::lround(c.r * 255));
        f->rgba[i + 1] = uint8_t(std::lround(c.g * 255));
        f->rgba[i + 2] = uint8_t(std::lround(c.b * 255));
        f->rgba[i + 3] = uint8_t(std::lround(alpha * 255));
    }
    return f;
}

engine::LayerFrame layer(std::shared_ptr<const media::VideoFrame> f)
{
    engine::LayerFrame l;
    l.frame = std::move(f);
    return l;
}

color::Rgb at(const std::vector<uint8_t>& px, int x, int y)
{
    const size_t i = (size_t(y) * W + size_t(x)) * 4;
    return {px[i] / 255.0f, px[i + 1] / 255.0f, px[i + 2] / 255.0f};
}

// Diferencia máxima entre canales en niveles de 8 bits.
int diff(color::Rgb a, color::Rgb b)
{
    return int(std::lround(255 * std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)})));
}

color::Rgb quantize(color::Rgb c)
{
    return {std::round(c.r * 255) / 255, std::round(c.g * 255) / 255, std::round(c.b * 255) / 255};
}

engine::FrameBundle bundle()
{
    engine::FrameBundle b;
    b.canvasWidth = W;
    b.canvasHeight = H;
    return b;
}

std::string str(color::Rgb c)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "(%.0f, %.0f, %.0f)", c.r * 255, c.g * 255, c.b * 255);
    return buf;
}

} // namespace

TEST_CASE("Lectura de LUT .cube", "[color][f3]")
{
    SECTION("3D de tamaño 2 que invierte los colores")
    {
        const char* text = "# comentario\nTITLE \"Negativo\"\nLUT_3D_SIZE 2\n"
                           "1 1 1\n0 1 1\n1 0 1\n0 0 1\n1 1 0\n0 1 0\n1 0 0\n0 0 0\n";
        std::string error;
        auto lut = color::parseCubeLut(text, &error);
        INFO(error);
        REQUIRE(lut);
        CHECK(lut->title == "Negativo");
        CHECK(lut->size == 2);
        const auto c = lut->apply({0.25f, 0.5f, 1.0f});
        CHECK(c.r == Approx(0.75f));
        CHECK(c.g == Approx(0.5f));
        CHECK(c.b == Approx(0.0f).margin(1e-6));
    }
    SECTION("1D con dominio propio")
    {
        auto lut = color::parseCubeLut("LUT_1D_SIZE 3\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n0 0 0\n0.25 0.25 0.25\n1 1 1\n");
        REQUIRE(lut);
        CHECK_FALSE(lut->is3D);
        CHECK(lut->apply({1.0f, 0.5f, 2.0f}).r == Approx(0.25f)); // 1 es la mitad del dominio
        CHECK(lut->apply({1.0f, 0.5f, 2.0f}).g == Approx(0.125f));
        CHECK(lut->apply({1.0f, 0.5f, 2.0f}).b == Approx(1.0f));
    }
    SECTION("errores")
    {
        std::string error;
        CHECK_FALSE(color::parseCubeLut("1 2 3\n", &error));
        CHECK(error.find("LUT_3D_SIZE") != std::string::npos);
        CHECK_FALSE(color::parseCubeLut("LUT_3D_SIZE 2\n0 0 0\n", &error));
        CHECK(error.find("entradas") != std::string::npos);
        CHECK_FALSE(color::parseCubeLut("LUT_3D_SIZE 2\n0 0 x\n", &error));
    }
    SECTION("archivo con acentos en la ruta")
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("señal ñ.cube");
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
        f.close();
        auto lut = color::loadCubeLut(path.toStdString());
        REQUIRE(lut);
        CHECK(lut->apply({0.3f, 0.6f, 0.9f}).g == Approx(0.6f));
    }
}

TEST_CASE("Curvas monótonas", "[color][f3]")
{
    CHECK(color::Curve({}).isIdentity());
    const color::Curve s({{0.0, 0.0}, {0.25, 0.15}, {0.75, 0.85}, {1.0, 1.0}});
    CHECK(s(0.25f) == Approx(0.15f).margin(0.002));
    CHECK(s(0.75f) == Approx(0.85f).margin(0.002));
    CHECK(s(0.5f) == Approx(0.5f).margin(0.01));
    float prev = -1;
    for (int i = 0; i <= 100; ++i) { // sin oscilaciones
        const float y = s(float(i) / 100);
        CHECK(y >= prev);
        prev = y;
    }
    // Sin puntos en los extremos se añaden (0,0) y (1,1).
    const color::Curve lift({{0.5, 0.7}});
    CHECK(lift(0.0f) == Approx(0.0f).margin(1e-6));
    CHECK(lift(0.5f) == Approx(0.7f).margin(0.002));
    CHECK(lift(1.0f) == Approx(1.0f));
}

TEST_CASE("Corrección de color en la CPU", "[color][f3]")
{
    auto apply = [](const ColorGrade& g, color::Rgb c) { return color::GradeEvaluator(g, nullptr).apply(c); };
    const color::Rgb gray{0.5f, 0.5f, 0.5f};

    SECTION("neutra: identidad")
    {
        ColorGrade g;
        CHECK(g.isIdentity());
        const auto c = apply(g, {0.2f, 0.6f, 0.9f});
        CHECK(diff(c, {0.2f, 0.6f, 0.9f}) == 0);
    }
    SECTION("exposición +1: el doble de luz lineal")
    {
        ColorGrade g;
        g.exposure = 1;
        const auto c = apply(g, gray);
        const float expected = std::pow(2.0f * std::pow(0.5f, 2.2f), 1 / 2.2f);
        CHECK(c.r == Approx(expected).margin(1e-4));
        CHECK(c.g == Approx(expected).margin(1e-4));
    }
    SECTION("saturación −1: blanco y negro con la luma BT.709")
    {
        ColorGrade g;
        g.saturation = -1;
        const auto c = apply(g, {1.0f, 0.0f, 0.0f});
        CHECK(c.r == Approx(0.2126f).margin(1e-4));
        CHECK(c.g == Approx(0.2126f).margin(1e-4));
        CHECK(c.b == Approx(0.2126f).margin(1e-4));
    }
    SECTION("temperatura cálida sobre gris: más rojo, menos azul")
    {
        ColorGrade g;
        g.temperature = 0.5;
        const auto c = apply(g, gray);
        CHECK(c.r > 0.54f);
        CHECK(c.b < 0.44f);
    }
    SECTION("contraste: aleja del gris medio")
    {
        ColorGrade g;
        g.contrast = 0.5;
        CHECK(apply(g, {0.25f, 0.25f, 0.25f}).r == Approx(0.125f).margin(1e-4));
        CHECK(apply(g, gray).r == Approx(0.5f).margin(1e-4));
    }
    SECTION("ruedas: lift levanta el negro y gain baja el blanco")
    {
        ColorGrade g;
        g.lift.master = 0.2;
        CHECK(apply(g, {0, 0, 0}).r == Approx(0.1f).margin(1e-4));
        CHECK(apply(g, {1, 1, 1}).r == Approx(1.0f).margin(1e-4));
        ColorGrade h;
        h.gain.b = -0.5;
        const auto w = apply(h, {1, 1, 1});
        CHECK(w.b == Approx(0.5f).margin(1e-4));
        CHECK(w.r == Approx(1.0f).margin(1e-4));
    }
    SECTION("HSL: el tono de la gama roja gira hacia el amarillo y el azul no cambia")
    {
        ColorGrade g;
        g.hsl[0].hue = 30;
        const auto red = apply(g, {0.9f, 0.1f, 0.1f});
        CHECK(red.g > 0.3f);
        CHECK(red.r > red.g);
        const color::Rgb blue{0.1f, 0.1f, 0.9f};
        CHECK(diff(apply(g, blue), blue) == 0);
        CHECK(diff(apply(g, gray), gray) == 0); // los grises no tienen tono
    }
    SECTION("curva general")
    {
        ColorGrade g;
        g.curveMaster = {{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}};
        CHECK(apply(g, gray).r == Approx(0.75f).margin(0.003));
    }
    SECTION("LUT con intensidad parcial")
    {
        ColorGrade g;
        g.lutPath = "x.cube";
        g.lutIntensity = 0.5;
        auto negative = color::parseCubeLut("LUT_3D_SIZE 2\n1 1 1\n0 1 1\n1 0 1\n0 0 1\n1 1 0\n0 1 0\n1 0 0\n0 0 0\n");
        const auto c = color::GradeEvaluator(g, negative).apply({0.2f, 0.2f, 0.2f});
        CHECK(c.r == Approx(0.5f).margin(1e-4)); // mitad entre 0,2 y 0,8
    }
}

TEST_CASE("La LUT 3D reproduce la corrección completa", "[color][f3]")
{
    ColorGrade g;
    g.exposure = 0.4;
    g.contrast = 0.2;
    g.saturation = 0.3;
    g.temperature = -0.2;
    g.shadows = 0.3;
    g.highlights = -0.2;
    g.lift.b = 0.1;
    g.gamma.r = 0.15;
    g.hsl[3].saturation = -0.5;
    g.curveGreen = {{0.0, 0.05}, {0.5, 0.55}, {1.0, 1.0}};
    const color::GradeEvaluator eval(g, nullptr);
    const auto lut = color::bakeLut(eval);
    REQUIRE(lut->size == 33);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    int worst = 0;
    double total = 0;
    constexpr int kSamples = 2000;
    for (int i = 0; i < kSamples; ++i) {
        const color::Rgb c{d(rng), d(rng), d(rng)};
        const color::Rgb a = lut->sample(c), b = eval.apply(c);
        worst = std::max(worst, diff(a, b));
        total += 255.0 * (std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b)) / 3;
    }
    std::printf("[color] LUT 33³ frente al cálculo directo: error medio %.2f, máximo %d niveles\n", total / kSamples, worst);
    // El error se concentra donde un canal se recorta dentro de una celda de la rejilla.
    CHECK(total / kSamples < 0.5);
    CHECK(worst <= 4);
}

TEST_CASE("GPU: corrección de color igual que la referencia", "[render][color][f3]")
{
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    ColorGrade g;
    g.exposure = 0.5;
    g.saturation = 0.4;
    g.contrast = 0.15;
    g.gain.r = 0.1;
    g.hsl[6].hue = -20;
    const color::GradeEvaluator eval(g, nullptr);
    const auto lut = color::bakeLut(eval);
    std::vector<uint8_t> px;
    int worst = 0;
    for (const color::Rgb c : {color::Rgb{0.8f, 0.2f, 0.1f}, color::Rgb{0.1f, 0.5f, 0.3f}, color::Rgb{0.5f, 0.5f, 0.5f},
                               color::Rgb{0.4f, 0.2f, 0.7f}, color::Rgb{0.95f, 0.9f, 0.85f}}) {
        auto b = bundle();
        auto l = layer(rgbaFrame(32, 18, c));
        l.lut = lut;
        b.layers.push_back(l);
        REQUIRE(r.render(b, px));
        const auto gpu = at(px, W / 2, H / 2);
        const auto ref = eval.apply(quantize(c));
        INFO("entrada " << str(c) << " GPU " << str(gpu) << " referencia " << str(ref));
        worst = std::max(worst, diff(gpu, ref));
        CHECK(diff(gpu, ref) <= 3);
    }
    std::printf("[color] GPU frente a la referencia: %d niveles como máximo\n", worst);
}

TEST_CASE("GPU: clave de croma", "[render][f3]")
{
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    std::vector<uint8_t> px;
    ChromaKey key;
    key.enabled = true;
    key.color = 0xFF00C000;
    key.tolerance = 0.3;
    key.softness = 0.2;
    key.spill = 0.0;

    auto keyed = [&](color::Rgb fg) {
        auto b = bundle();
        b.layers.push_back(layer(rgbaFrame(32, 18, {1.0f, 0.0f, 0.0f}))); // fondo rojo
        auto l = layer(rgbaFrame(32, 18, fg));
        l.chromaKey = key;
        b.layers.push_back(l);
        REQUIRE(r.render(b, px));
        return at(px, W / 2, H / 2);
    };
    SECTION("el verde desaparece y deja ver el fondo")
    {
        CHECK(diff(keyed({0.0f, 0.75f, 0.0f}), {1.0f, 0.0f, 0.0f}) <= 2);
        CHECK(r.lastPassCount() == 1); // sin modos de fusión: directo al destino
    }
    SECTION("un color lejano queda intacto")
    {
        CHECK(diff(keyed({0.9f, 0.8f, 0.7f}), {0.9f, 0.8f, 0.7f}) <= 2);
    }
    SECTION("en el borde suave el alfa coincide con la referencia")
    {
        const color::Rgb fg{0.2f, 0.6f, 0.2f};
        const auto ref = color::applyChromaKey(quantize(fg), key);
        INFO("alfa de referencia " << ref.alpha);
        REQUIRE(ref.alpha > 0.1f);
        REQUIRE(ref.alpha < 0.9f);
        const color::Rgb expected{ref.color.r * ref.alpha + (1 - ref.alpha), ref.color.g * ref.alpha, ref.color.b * ref.alpha};
        CHECK(diff(keyed(fg), expected) <= 3);
    }
    SECTION("supresión del derrame: un gris verdoso pierde el tinte")
    {
        key.tolerance = 0.0;
        key.softness = 0.0;
        key.spill = 1.0;
        const auto out = keyed({0.5f, 0.6f, 0.5f});
        CHECK(out.g < 0.58f);
        CHECK(diff(out, color::applyChromaKey(quantize({0.5f, 0.6f, 0.5f}), key).color) <= 3);
    }
}

TEST_CASE("GPU: modos de fusión con las fórmulas de W3C", "[render][f3]")
{
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    std::vector<uint8_t> px;
    const color::Rgb back{0.2f, 0.5f, 0.8f};
    const color::Rgb top{0.7f, 0.4f, 0.1f};
    for (int m = 0; m < kBlendModeCount; ++m) {
        const auto mode = BlendMode(m);
        for (float opacity : {1.0f, 0.5f}) {
            auto b = bundle();
            b.layers.push_back(layer(rgbaFrame(32, 18, back)));
            auto l = layer(rgbaFrame(32, 18, top));
            l.blend = mode;
            l.opacity = opacity;
            b.layers.push_back(l);
            REQUIRE(r.render(b, px));
            const auto blended = color::blend(mode, quantize(back), quantize(top));
            const color::Rgb expected{back.r + (blended.r - back.r) * opacity, back.g + (blended.g - back.g) * opacity,
                                      back.b + (blended.b - back.b) * opacity};
            const auto gpu = at(px, W / 2, H / 2);
            INFO("modo " << m << " opacidad " << opacity << " GPU " << str(gpu) << " esperado " << str(expected));
            CHECK(diff(gpu, expected) <= 2);
        }
    }
}

TEST_CASE("GPU: varias capas con fusión se apilan en orden", "[render][f3]")
{
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    std::vector<uint8_t> px;
    auto b = bundle();
    b.layers.push_back(layer(rgbaFrame(32, 18, {0.5f, 0.5f, 0.5f})));
    auto mul = layer(rgbaFrame(32, 18, {0.5f, 1.0f, 1.0f}));
    mul.blend = BlendMode::Multiply;
    b.layers.push_back(mul);
    auto normal = layer(rgbaFrame(32, 18, {0.0f, 0.0f, 1.0f}));
    normal.transform.scale = 0.5; // solo el centro
    b.layers.push_back(normal);
    auto add = layer(rgbaFrame(32, 18, {0.2f, 0.2f, 0.0f}));
    add.blend = BlendMode::Add;
    b.layers.push_back(add);
    REQUIRE(r.render(b, px));
    CHECK(r.lastPassCount() == 4); // lienzo inicial, una por cada capa con fusión y la copia final
    CHECK(diff(at(px, 10, 10), {0.45f, 0.7f, 0.5f}) <= 2);    // (0,25 0,5 0,5) + (0,2 0,2 0)
    CHECK(diff(at(px, W / 2, H / 2), {0.2f, 0.2f, 1.0f}) <= 2);
}

TEST_CASE("GPU: máscaras de forma", "[render][f3]")
{
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    std::vector<uint8_t> px;
    const color::Rgb white{1, 1, 1};
    const color::Rgb black{0, 0, 0};
    auto masked = [&](LayerMask m) {
        auto b = bundle();
        auto l = layer(rgbaFrame(64, 36, white));
        l.masks = {m};
        b.layers.push_back(l);
        REQUIRE(r.render(b, px));
    };
    SECTION("elipse centrada: dentro sí, esquinas no")
    {
        masked({MaskShape::Ellipse, false, 0.5, 0.5, 0.5, 0.5, 0.0, 0.0});
        CHECK(diff(at(px, W / 2, H / 2), white) <= 1);
        CHECK(diff(at(px, W / 2 + 70, H / 2), white) <= 1);  // semieje horizontal 80 px
        CHECK(diff(at(px, W / 2 + 90, H / 2), black) <= 1);
        CHECK(diff(at(px, W / 2 + 60, H / 2 + 40), black) <= 1); // fuera, en diagonal
        CHECK(diff(at(px, 5, 5), black) <= 1);
    }
    SECTION("rectángulo invertido")
    {
        masked({MaskShape::Rectangle, true, 0.5, 0.5, 0.5, 0.5, 0.0, 0.0});
        CHECK(diff(at(px, W / 2, H / 2), black) <= 1);
        CHECK(diff(at(px, 5, 5), white) <= 1);
    }
    SECTION("rectángulo girado 45°: la esquina del eje queda fuera")
    {
        masked({MaskShape::Rectangle, false, 0.5, 0.5, 0.25, 0.25 * 16 / 9.0, 45.0, 0.0}); // cuadrado de 80 px
        CHECK(diff(at(px, W / 2 + 50, H / 2), white) <= 1); // la diagonal llega a 56 px
        CHECK(diff(at(px, W / 2 + 36, H / 2 + 36), black) <= 1);
    }
    SECTION("lineal: se ve la mitad inferior, con borde suave")
    {
        masked({MaskShape::Linear, false, 0.5, 0.5, 0.0, 0.0, 0.0, 0.2});
        CHECK(diff(at(px, W / 2, 10), black) <= 1);
        CHECK(diff(at(px, W / 2, H - 10), white) <= 1);
        const auto mid = at(px, W / 2, H / 2);
        CHECK(mid.r > 0.3f);
        CHECK(mid.r < 0.7f);
    }
}

TEST_CASE("Recorte, máscara y efectos en el proyecto", "[core][f3]")
{
    Project p = Project::createDefault();
    const Id title = edit::placeTitle(p, p.videoTracks[0].id, 0, 60, TitleStyle{});
    REQUIRE(title != kNoId);

    SECTION("el recorte se anima y se combina con el barrido")
    {
        REQUIRE(edit::setProperty(p, title, edit::Property::CropLeft, 0, 0.1));
        REQUIRE(edit::toggleKeyframe(p, title, edit::Property::CropRight, 0));
        REQUIRE(edit::setProperty(p, title, edit::Property::CropRight, 30, 0.4));
        CHECK(edit::setProperty(p, title, edit::Property::CropTop, 0, 2.0)); // se limita
        CHECK(p.findClip(title)->transform.cropTop.value == Approx(0.99));
        const auto layers = videoLayersAt(p, 15);
        REQUIRE(layers.size() == 1);
        CHECK(layers[0].crop.left == Approx(0.1));
        CHECK(layers[0].crop.right == Approx(1.0 - 0.2));
    }
    SECTION("los keyframes de la máscara se mueven al dividir")
    {
        REQUIRE(edit::addMask(p, title, MaskShape::Ellipse) == 0);
        REQUIRE(edit::toggleKeyframe(p, title, edit::Property::MaskX, 0));
        REQUIRE(edit::setProperty(p, title, edit::Property::MaskX, 40, 0.9));
        const Id right = edit::splitClip(p, title, 20);
        REQUIRE(right != kNoId);
        const auto layers = videoLayersAt(p, 30); // fotograma 10 de la parte derecha
        REQUIRE(layers.size() == 1);
        REQUIRE(layers[0].masks.size() == 1);
        CHECK(layers[0].masks[0].shape == MaskShape::Ellipse);
        CHECK(layers[0].masks[0].x == Approx(0.5 + 0.4 * 30 / 40.0));
        REQUIRE(edit::removeMask(p, right, 0));
        CHECK(p.findClip(right)->masks.empty());
        CHECK_FALSE(edit::setProperty(p, right, edit::Property::MaskX, 0, 0.2)); // ya no existe
    }
    SECTION("la corrección se limpia y solo se aplica a clips de video")
    {
        ColorGrade g;
        g.exposure = 9;
        g.curveRed = {{1.0, 1.0}, {0.0, 0.0}}; // diagonal desordenada → sin curva
        g.curveBlue = {{0.7, 0.2}, {0.2, 0.4}};
        REQUIRE(edit::setColorGrade(p, title, g));
        const ColorGrade& stored = p.findClip(title)->color.value;
        CHECK(stored.exposure == 4);
        CHECK(stored.curveRed.empty());
        REQUIRE(stored.curveBlue.size() == 2);
        CHECK(stored.curveBlue[0].x == Approx(0.2));
        CHECK_FALSE(edit::setColorGrade(p, title, g)); // sin cambio
        const auto layers = videoLayersAt(p, 0);
        REQUIRE(layers[0].grade);
        CHECK(layers[0].grade->exposure == 4);
        CHECK(edit::setBlendMode(p, title, BlendMode::Screen));
        CHECK_FALSE(edit::setBlendMode(p, title, BlendMode(99)));
        CHECK(videoLayersAt(p, 0)[0].blend == BlendMode::Screen);
    }
    CHECK(p.validate());
}

TEST_CASE("El formato v3 guarda los efectos (LUT con ruta relativa)", "[core][f3]")
{
    QTemporaryDir dir;
    const QString lutPath = QDir::toNativeSeparators(dir.filePath("luts/cálido.cube"));
    QDir(dir.path()).mkpath("luts");
    {
        QFile f(lutPath);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    }
    Project p = Project::createDefault();
    const Id clip = edit::placeTitle(p, p.videoTracks[0].id, 0, 30, TitleStyle{});
    ColorGrade g;
    g.contrast = 0.3;
    g.lift = {0.1, -0.05, 0.0, 0.02};
    g.curveMaster = {{0.0, 0.1}, {1.0, 0.9}};
    g.hsl[2] = {10, -0.5, 0.25};
    g.lutPath = lutPath.toStdString();
    g.lutIntensity = 0.7;
    REQUIRE(edit::setColorGrade(p, clip, g));
    REQUIRE(edit::setChromaKey(p, clip, ChromaKey{true, 0xFF0000FF, 0.2, 0.05, 0.8}));
    REQUIRE(edit::addMask(p, clip, MaskShape::Rectangle) == 0);
    REQUIRE(edit::setMaskOptions(p, clip, 0, MaskMode::Add, true));
    REQUIRE(edit::toggleKeyframe(p, clip, edit::Property::MaskFeather, 5));
    REQUIRE(edit::addMask(p, clip, MaskShape::Path) == 1);
    REQUIRE(edit::setMaskOptions(p, clip, 1, MaskMode::Subtract, false));
    REQUIRE(edit::toggleKeyframe(p, clip, edit::Property::MaskWidth, 3, 1));
    REQUIRE(edit::toggleColorKeyframe(p, clip, 0));
    ColorGrade later = g;
    later.exposure = 1.5;
    REQUIRE(edit::setColorGrade(p, clip, later, 20));
    REQUIRE(edit::setProperty(p, clip, edit::Property::CropBottom, 0, 0.25));
    REQUIRE(edit::setBlendMode(p, clip, BlendMode::SoftLight));

    const QString file = dir.filePath("proyecto.mxproj");
    REQUIRE(saveProjectFile(p, file));
    auto loaded = loadProjectFile(file);
    INFO(loaded.error.toStdString());
    REQUIRE(loaded.project);
    CHECK(*loaded.project == p);
    CHECK(loaded.missingMedia.isEmpty());

    // Se mueve la carpeta entera: la LUT se encuentra por su ruta relativa.
    QTemporaryDir moved;
    QDir(moved.path()).mkpath("luts");
    REQUIRE(QFile::copy(file, moved.filePath("proyecto.mxproj")));
    REQUIRE(QFile::copy(lutPath, moved.filePath("luts/cálido.cube")));
    QFile::remove(lutPath);
    auto relocated = loadProjectFile(moved.filePath("proyecto.mxproj"));
    REQUIRE(relocated.project);
    CHECK(QString::fromStdString(relocated.project->findClip(clip)->color.keys.at(1).grade.lutPath)
          == QDir::toNativeSeparators(QDir::cleanPath(moved.filePath("luts/cálido.cube"))));
    CHECK(relocated.missingMedia.isEmpty());
}

TEST_CASE("Scopes sobre imágenes conocidas", "[color][f3]")
{
    using color::ScopeType;
    // Mitad izquierda negra, mitad derecha blanca.
    constexpr int w = 64, h = 32;
    std::vector<uint8_t> img(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w / 2; ++x)
            for (int c = 0; c < 3; ++c)
                img[(size_t(y) * w + size_t(x)) * 4 + size_t(c)] = 0;
    auto lit = [](const color::ScopeImage& s, int x, int y) { return (s.at(x, y) >> 24) > 0; };

    SECTION("forma de onda: negro abajo a la izquierda, blanco arriba a la derecha")
    {
        const auto s = color::computeScope(ScopeType::Waveform, img.data(), w, h, w * 4, 32, 50);
        CHECK(lit(s, 5, 49));
        CHECK_FALSE(lit(s, 5, 0));
        CHECK(lit(s, 25, 0));
        CHECK_FALSE(lit(s, 25, 49));
        CHECK_FALSE(lit(s, 25, 25));
        CHECK((s.at(5, 49) >> 24) == 255); // toda la columna en una celda: intensidad máxima
    }
    SECTION("desfile RGB de un rojo puro")
    {
        std::vector<uint8_t> red(size_t(w) * h * 4, 0);
        for (size_t i = 0; i < red.size(); i += 4)
            red[i] = 255;
        const auto s = color::computeScope(ScopeType::Parade, red.data(), w, h, w * 4, 90, 40);
        CHECK(lit(s, 15, 0));        // R arriba
        CHECK(lit(s, 45, 39));       // G abajo
        CHECK(lit(s, 75, 39));       // B abajo
        CHECK_FALSE(lit(s, 45, 0));
    }
    SECTION("vectorscopio: el rojo cae en su posición")
    {
        std::vector<uint8_t> red(size_t(w) * h * 4, 0);
        for (size_t i = 0; i < red.size(); i += 4)
            red[i] = 255;
        const auto s = color::computeScope(ScopeType::Vectorscope, red.data(), w, h, w * 4, 100, 100);
        const auto pt = color::vectorscopePosition(255, 0, 0);
        CHECK(pt.x < 0.5); // Cb negativo
        CHECK(pt.y == Approx(0.5 - 0.5 * 0.9).margin(0.01)); // Cr = 0,5: arriba del todo
        CHECK(lit(s, int(pt.x * 100), int(pt.y * 100)));
        CHECK_FALSE(lit(s, 50, 50)); // sin grises
    }
    SECTION("histograma de blanco y negro")
    {
        const auto s = color::computeScope(ScopeType::Histogram, img.data(), w, h, w * 4, 128, 40);
        CHECK(lit(s, 0, 0));     // barra del 0 a toda altura
        CHECK(lit(s, 127, 0));   // barra del 255
        CHECK_FALSE(lit(s, 64, 39));
    }
}

TEST_CASE("El motor genera la LUT de la corrección y la reutiliza", "[engine][f3]")
{
    Project p = Project::createDefault();
    p.settings.width = 64;
    p.settings.height = 36;
    const Id clip = edit::placeTitle(p, p.videoTracks[0].id, 0, 30, TitleStyle{});
    ColorGrade g;
    g.saturation = -1;
    g.lutPath = "D:/no/existe.cube"; // ilegible: se ignora y se aplica el resto
    REQUIRE(edit::setColorGrade(p, clip, g));
    engine::VideoSources sources;
    sources.setTitleRenderer(std::make_shared<render::QtTitleRenderer>());
    const auto a = sources.build(p, 0);
    const auto b = sources.build(p, 1);
    REQUIRE(a.layers.size() == 1);
    REQUIRE(a.layers[0].lut);
    CHECK(a.layers[0].lut == b.layers[0].lut);
    const auto c = a.layers[0].lut->sample({1.0f, 0.0f, 0.0f});
    CHECK(c.g == Approx(0.2126f).margin(0.002));
    ColorGrade none;
    REQUIRE(edit::setColorGrade(p, clip, none));
    CHECK_FALSE(sources.build(p, 0).layers[0].lut);
}
