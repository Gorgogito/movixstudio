#include "color/ColorMath.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"
#include "engine/Sources.h"
#include "render/MaskRenderer.h"
#include "render/OffscreenRenderer.h"
#include "render/TitleRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace movix;
using namespace movix::core;
using Catch::Approx;

namespace {

constexpr int W = 320, H = 180;

std::shared_ptr<const media::VideoFrame> rgbaFrame(int w, int h, color::Rgb c)
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
        f->rgba[i + 3] = 255;
    }
    return f;
}

engine::FrameBundle bundleWith(engine::LayerFrame l)
{
    engine::FrameBundle b;
    b.canvasWidth = W;
    b.canvasHeight = H;
    b.layers.push_back(std::move(l));
    return b;
}

int lum(const std::vector<uint8_t>& px, int x, int y)
{
    return px[(size_t(y) * W + size_t(x)) * 4];
}

} // namespace

TEST_CASE("Corrección de color con keyframes", "[core][color][f3]")
{
    Project p = Project::createDefault();
    const Id clip = edit::placeTitle(p, p.videoTracks[0].id, 0, 60, TitleStyle{});
    ColorGrade gray;
    gray.saturation = -1;
    REQUIRE(edit::setColorGrade(p, clip, gray)); // sin keyframes: todo el clip
    CHECK_FALSE(p.findClip(clip)->color.isAnimated());

    REQUIRE(edit::toggleColorKeyframe(p, clip, 0));  // keyframe con la corrección actual
    ColorGrade vivid;
    vivid.saturation = 0.5;
    vivid.exposure = 1;
    REQUIRE(edit::setColorGrade(p, clip, vivid, 40)); // crea el segundo keyframe
    const AnimatedGrade& a = p.findClip(clip)->color;
    REQUIRE(a.keys.size() == 2);

    SECTION("a mitad de camino se mezclan los dos")
    {
        const auto s = a.at(20);
        REQUIRE(s.to);
        CHECK(s.from->saturation == -1);
        CHECK(s.to->saturation == 0.5);
        CHECK(s.t == Approx(0.5));
        CHECK(a.valueAt(20).saturation == Approx(-0.25));
        CHECK(a.valueAt(20).exposure == Approx(0.5));
        const auto layers = videoLayersAt(p, 20);
        REQUIRE(layers[0].grade);
        REQUIRE(layers[0].gradeTo);
        CHECK(layers[0].gradeMix == Approx(0.5));
        CHECK_FALSE(videoLayersAt(p, 50)[0].gradeTo); // después del último: constante
    }
    SECTION("mantener: sin mezcla hasta el keyframe siguiente")
    {
        REQUIRE(edit::setColorKeyInterpolation(p, clip, 0, Interpolation::Hold));
        CHECK_FALSE(p.findClip(clip)->color.at(30).to);
        CHECK(p.findClip(clip)->color.at(30).from->saturation == -1);
    }
    SECTION("los keyframes de color se mueven al dividir y al cambiar la velocidad")
    {
        const Id right = edit::splitClip(p, clip, 10);
        REQUIRE(right != kNoId);
        CHECK(p.findClip(right)->color.keys[0].frame == -10);
        CHECK(p.findClip(right)->color.keys[1].frame == 30);
    }
    SECTION("quitar el último keyframe deja su corrección para todo el clip")
    {
        REQUIRE(edit::toggleColorKeyframe(p, clip, 0));
        REQUIRE(edit::toggleColorKeyframe(p, clip, 40));
        CHECK_FALSE(p.findClip(clip)->color.isAnimated());
        CHECK(p.findClip(clip)->color.value.exposure == 1);
    }
    CHECK(p.validate());
}

TEST_CASE("El motor mezcla las LUT de dos keyframes de color", "[engine][color][f3]")
{
    Project p = Project::createDefault();
    p.settings.width = 64;
    p.settings.height = 36;
    const Id clip = edit::placeTitle(p, p.videoTracks[0].id, 0, 30, TitleStyle{});
    ColorGrade a;
    a.saturation = -1;
    ColorGrade b;
    b.gain = {0.0, -1.0, -1.0, 0.0}; // solo queda el rojo
    REQUIRE(edit::setColorGrade(p, clip, a));
    REQUIRE(edit::toggleColorKeyframe(p, clip, 0));
    REQUIRE(edit::setColorGrade(p, clip, b, 20));
    engine::VideoSources sources;
    sources.setTitleRenderer(std::make_shared<render::QtTitleRenderer>());
    const auto mid = sources.build(p, 10);
    REQUIRE(mid.layers[0].lut);
    const color::Rgb in{0.2f, 0.8f, 0.4f};
    const auto lutA = color::bakeLut(color::GradeEvaluator(a, nullptr));
    const auto lutB = color::bakeLut(color::GradeEvaluator(b, nullptr));
    const auto ra = lutA->sample(in), rb = lutB->sample(in), rm = mid.layers[0].lut->sample(in);
    CHECK(rm.r == Approx((ra.r + rb.r) / 2).margin(1e-4));
    CHECK(rm.g == Approx((ra.g + rb.g) / 2).margin(1e-4));
}

TEST_CASE("Máscaras rasterizadas: igual que el cálculo del shader", "[render][f3]")
{
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    render::QtMaskRenderer raster;
    std::vector<uint8_t> analytic, rasterized;
    const LayerMask ellipse{MaskShape::Ellipse, false, 0.4, 0.55, 0.5, 0.6, 30.0, 0.08, MaskMode::Add, {}};

    engine::LayerFrame l;
    l.frame = rgbaFrame(64, 36, {1, 1, 1});
    l.masks = {ellipse};
    REQUIRE(r.render(bundleWith(l), analytic));
    l.maskImage = raster.render(l.masks, 320, 180);
    REQUIRE(r.render(bundleWith(l), rasterized));

    double total = 0;
    int worst = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int d = std::abs(lum(analytic, x, y) - lum(rasterized, x, y));
            total += d;
            worst = std::max(worst, d);
        }
    std::printf("[mask] máscara rasterizada frente a la analítica: media %.2f, máximo %d niveles\n", total / (W * H), worst);
    CHECK(total / (W * H) < 1.5);
    CHECK(worst <= 40); // solo en el borde (antialias distinto)
}

TEST_CASE("Varias máscaras se combinan y la de forma libre sigue su contorno", "[render][f3]")
{
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    render::QtMaskRenderer raster;
    std::vector<uint8_t> px;
    auto renderMasks = [&](std::vector<LayerMask> masks) {
        engine::LayerFrame l;
        l.frame = rgbaFrame(64, 36, {1, 1, 1});
        l.masks = masks;
        l.maskImage = raster.render(masks, 320, 180);
        REQUIRE(r.render(bundleWith(l), px));
    };
    const LayerMask left{MaskShape::Rectangle, false, 0.25, 0.5, 0.3, 0.5, 0, 0, MaskMode::Add, {}};
    const LayerMask right{MaskShape::Ellipse, false, 0.75, 0.5, 0.3, 0.5, 0, 0, MaskMode::Add, {}};

    SECTION("sumar: se ven las dos")
    {
        renderMasks({left, right});
        CHECK(lum(px, 80, 90) > 250);
        CHECK(lum(px, 240, 90) > 250);
        CHECK(lum(px, 160, 90) < 5);
    }
    SECTION("restar: un agujero dentro del rectángulo")
    {
        LayerMask big{MaskShape::Rectangle, false, 0.5, 0.5, 0.8, 0.8, 0, 0, MaskMode::Add, {}};
        LayerMask hole{MaskShape::Ellipse, false, 0.5, 0.5, 0.2, 0.2, 0, 0, MaskMode::Subtract, {}};
        renderMasks({big, hole});
        CHECK(lum(px, 160, 90) < 5);  // agujero
        CHECK(lum(px, 60, 40) > 250); // dentro del rectángulo
        CHECK(lum(px, 5, 5) < 5);     // fuera
    }
    SECTION("intersección: solo lo común")
    {
        LayerMask a{MaskShape::Rectangle, false, 0.4, 0.5, 0.4, 0.8, 0, 0, MaskMode::Add, {}};
        LayerMask b{MaskShape::Rectangle, false, 0.6, 0.5, 0.4, 0.8, 0, 0, MaskMode::Intersect, {}};
        renderMasks({a, b});
        CHECK(lum(px, 160, 90) > 250); // x = 0,5: en las dos
        CHECK(lum(px, 85, 90) < 5);    // solo en la primera
        CHECK(lum(px, 235, 90) < 5);   // solo en la segunda
    }
    SECTION("forma libre: triángulo girado")
    {
        LayerMask tri{MaskShape::Path, false, 0.5, 0.5, 0.5, 0.8, 0, 0, MaskMode::Add,
                      {{0.0, -0.5}, {0.5, 0.5}, {-0.5, 0.5}}};
        renderMasks({tri});
        CHECK(lum(px, 160, 110) > 250); // cerca de la base, centro
        CHECK(lum(px, 100, 30) < 5);    // fuera, arriba a la izquierda
        CHECK(lum(px, 160, 25) > 200);  // cerca del vértice superior (y = 0,1 → 18 px)
        CHECK(lum(px, 100, 150) > 250); // ancho cerca de la base
        tri.rotation = 180;             // girado: la base arriba y el vértice abajo
        renderMasks({tri});
        CHECK(lum(px, 100, 30) > 250);
        CHECK(lum(px, 100, 150) < 5);
        CHECK(lum(px, 160, 155) > 200);
    }
    SECTION("invertir la forma libre")
    {
        LayerMask tri{MaskShape::Path, true, 0.5, 0.5, 0.5, 0.8, 0, 0, MaskMode::Add,
                      {{0.0, -0.5}, {0.5, 0.5}, {-0.5, 0.5}}};
        renderMasks({tri});
        CHECK(lum(px, 160, 110) < 5);
        CHECK(lum(px, 5, 5) > 250);
    }
}

TEST_CASE("Edición de máscaras múltiples", "[core][f3]")
{
    Project p = Project::createDefault();
    const Id clip = edit::placeTitle(p, p.videoTracks[0].id, 0, 30, TitleStyle{});
    for (size_t i = 0; i < kMaxMasks; ++i)
        REQUIRE(edit::addMask(p, clip, MaskShape::Ellipse) == int(i));
    CHECK(edit::addMask(p, clip, MaskShape::Ellipse) == -1); // límite
    REQUIRE(edit::setMaskShape(p, clip, 2, MaskShape::Path));
    CHECK(p.findClip(clip)->masks[2].points.size() == 5); // pentágono inicial
    CHECK_FALSE(edit::setMaskPoints(p, clip, 2, {{0, 0}, {1, 1}})); // menos de 3 puntos
    REQUIRE(edit::setMaskPoints(p, clip, 2, {{0, -0.5}, {0.5, 0.5}, {-0.5, 0.5}}));
    REQUIRE(edit::setProperty(p, clip, edit::Property::MaskWidth, 0, 0.3, 4));
    CHECK(p.findClip(clip)->masks[4].width.value == Approx(0.3));
    CHECK(p.findClip(clip)->masks[3].width.value == Approx(0.6));
    REQUIRE(edit::removeMask(p, clip, 0));
    CHECK(p.findClip(clip)->masks.size() == kMaxMasks - 1);
    CHECK(p.findClip(clip)->masks[1].shape == MaskShape::Path); // los siguientes suben una posición
    const auto layers = videoLayersAt(p, 0);
    CHECK(layers[0].masks.size() == kMaxMasks - 1);
    CHECK(engine::masksNeedRaster(layers[0].masks));
    CHECK(p.validate());
}

TEST_CASE("Forma de máscara animada: keyframes de todos los puntos", "[core][f3][mask]")
{
    Project p = Project::createDefault();
    const Id clip = edit::placeTitle(p, p.videoTracks[0].id, 0, 100, TitleStyle{});
    REQUIRE(edit::addMask(p, clip, MaskShape::Path) == 0);
    const std::vector<PathPoint> square{{-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
    REQUIRE(edit::setMaskPoints(p, clip, 0, square));
    const Mask& m = p.findClip(clip)->masks[0];

    // Sin keyframes, editar cambia la forma fija.
    CHECK(m.pathAt(50) == square);
    // Keyframe en 10 y otra forma en 30 (crea el segundo keyframe).
    REQUIRE(edit::toggleMaskPathKey(p, clip, 0, 10));
    std::vector<PathPoint> moved = square;
    for (PathPoint& pt : moved)
        pt.x += 0.2;
    REQUIRE(edit::setMaskPoints(p, clip, 0, moved, 30));
    REQUIRE(m.pathKeys.size() == 2);
    CHECK(m.pathAt(0)[0].x == Approx(-0.5));  // antes del primero, el primero
    CHECK(m.pathAt(20)[0].x == Approx(-0.4)); // a mitad, a mitad
    CHECK(m.pathAt(90)[0].x == Approx(-0.3));
    CHECK(videoLayersAt(p, 20)[0].masks[0].points[2].x == Approx(0.6));

    // Interpolación: mantener salta en el siguiente.
    REQUIRE(edit::setMaskPathKeyInterpolation(p, clip, 0, 10, Interpolation::Hold));
    CHECK(m.pathAt(29)[0].x == Approx(-0.5));
    CHECK(m.pathAt(30)[0].x == Approx(-0.3));

    // Añadir un punto (entre el 1 y el 2) en un keyframe lo añade en todos.
    std::vector<PathPoint> five = m.pathAt(30);
    five.insert(five.begin() + 2, PathPoint{0.9, 0.0});
    REQUIRE(edit::setMaskPoints(p, clip, 0, five, 30));
    REQUIRE(m.pathKeys[0].points.size() == 5);
    CHECK(m.pathKeys[0].points[2].x == Approx(0.5)); // punto medio del lado en el otro keyframe
    CHECK(m.pathKeys[0].points[2].y == Approx(0.0));
    CHECK(m.pathKeys[1].points[2].x == Approx(0.9));
    // Y quitarlo, también.
    std::vector<PathPoint> four = m.pathAt(10);
    four.erase(four.begin() + 2);
    REQUIRE(edit::setMaskPoints(p, clip, 0, four, 10));
    CHECK(m.pathKeys[0].points.size() == 4);
    CHECK(m.pathKeys[1].points.size() == 4);
    CHECK(m.pathKeys[1].points[0].x == Approx(-0.3));

    // Se guarda y se carga.
    const auto loaded = deserializeProject(serializeProject(p, QStringLiteral("C:/x/p.mxproj")), QStringLiteral("C:/x/p.mxproj"));
    REQUIRE(loaded.project);
    CHECK(loaded.project->findClip(clip)->masks[0] == m);

    // Recortar el inicio desplaza los keyframes para que sigan en el mismo instante.
    REQUIRE(edit::trimClip(p, clip, edit::Edge::Start, 5));
    CHECK(p.findClip(clip)->masks[0].pathKeys[0].frame == 5);

    // Quitar el último keyframe deja su forma fija.
    Mask& mm = p.findClip(clip)->masks[0];
    REQUIRE(edit::toggleMaskPathKey(p, clip, 0, mm.pathKeys[1].frame));
    const auto last = mm.pathKeys[0].points;
    REQUIRE(edit::toggleMaskPathKey(p, clip, 0, mm.pathKeys[0].frame));
    CHECK(mm.pathKeys.empty());
    CHECK(mm.points == last);
    CHECK(p.validate());
}

TEST_CASE("Composición en luz lineal: opacidad, fusión y secuencias anidadas", "[render][f3][linear]")
{
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    std::vector<uint8_t> px;
    auto center = [&] { return lum(px, W / 2, H / 2); };
    auto layer = [](color::Rgb c, double opacity, BlendMode blend = BlendMode::Normal) {
        engine::LayerFrame l;
        l.frame = rgbaFrame(64, 36, c);
        l.opacity = opacity;
        l.blend = blend;
        return l;
    };
    auto render = [&](engine::FrameBundle b, bool linear) {
        b.linearLight = linear;
        REQUIRE(r.render(b, px));
        return center();
    };

    // Blanco al 50 % sobre negro: mitad de señal o mitad de luz (0,5^(1/2,4) = 0,749).
    CHECK(std::abs(render(bundleWith(layer({1, 1, 1}, 0.5)), false) - 128) <= 1);
    CHECK(std::abs(render(bundleWith(layer({1, 1, 1}, 0.5)), true) - 191) <= 1);

    // Sin mezclas, ida y vuelta exacta (también en las sombras).
    for (float v : {0.05f, 0.2f, 0.5f, 0.9f}) {
        INFO("gris " << v);
        CHECK(std::abs(render(bundleWith(layer({v, v, v}, 1.0)), true) - int(std::lround(v * 255))) <= 1);
    }

    // Sumar dos grises medios: satura en la señal; en luz, 2·0,5^2,4 → 0,668.
    engine::FrameBundle add = bundleWith(layer({0.5, 0.5, 0.5}, 1.0));
    add.layers.push_back(layer({0.5, 0.5, 0.5}, 1.0, BlendMode::Add));
    CHECK(render(add, false) == 255);
    CHECK(std::abs(render(add, true) - 170) <= 1);

    // Una secuencia anidada (fondo transparente, color premultiplicado) da lo mismo que sin anidar.
    auto nested = std::make_shared<engine::FrameBundle>(bundleWith(layer({1, 1, 1}, 0.5)));
    nested->linearLight = true;
    engine::LayerFrame compound;
    compound.nested = nested;
    CHECK(std::abs(render(bundleWith(compound), true) - 191) <= 1);
}
