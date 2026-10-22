#include "ProjectController.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"
#include "render/TitleRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace movix;
using namespace movix::core;
using Catch::Approx;

namespace {

// Un título de 4 s (120 fotogramas a 30 fps) seleccionado en un proyecto nuevo.
qint64 addTitle(ProjectController& project)
{
    project.newProject();
    REQUIRE(project.addTitle(0, -1, 0));
    return project.selection()[0].toLongLong();
}

} // namespace

TEST_CASE("Interpolación de los keyframes desde la aplicación", "[app][keyframes]")
{
    ProjectController project;
    const qint64 clip = addTitle(project);
    auto* player = project.player();

    // Escala 100 % en 0 y 200 % en 60.
    player->seekFrame(0);
    project.toggleKeyframe(clip, "scale");
    player->seekFrame(60);
    project.setClipProperty(clip, "scale", 2.0);
    project.endInteraction();
    player->seekFrame(30);
    CHECK(project.propertyState(clip, "scale")["value"].toDouble() == Approx(1.5)); // lineal
    CHECK(project.propertyState(clip, "scale")["interp"].toString().isEmpty()); // sin keyframe aquí

    player->seekFrame(0);
    CHECK(project.propertyState(clip, "scale")["interp"].toString() == "linear");
    project.setKeyInterpolation(clip, "scale", "hold");
    CHECK(project.propertyState(clip, "scale")["interp"].toString() == "hold");
    player->seekFrame(30);
    CHECK(project.propertyState(clip, "scale")["value"].toDouble() == Approx(1.0)); // se mantiene hasta 60
    player->seekFrame(0);
    project.setKeyInterpolation(clip, "scale", "smooth");
    player->seekFrame(15);
    const double smooth = project.propertyState(clip, "scale")["value"].toDouble();
    CHECK(smooth > 1.0);
    CHECK(smooth < 1.25); // más lento al principio que la lineal
    project.undo();
    player->seekFrame(30);
    CHECK(project.propertyState(clip, "scale")["value"].toDouble() == Approx(1.0)); // vuelve a «mantener»

    // Keyframes de color.
    player->seekFrame(0);
    project.toggleColorKeyframe(clip);
    CHECK(project.colorGrade(clip)["interp"].toString() == "linear");
    project.setKeyInterpolation(clip, "color", "smooth");
    CHECK(project.colorGrade(clip)["interp"].toString() == "smooth");
}

TEST_CASE("Animaciones de texto: entrada y salida", "[core][titles]")
{
    Project p = Project::createDefault(); // 30 fps
    TitleStyle style;
    style.text = "Hola mundo"; // 10 caracteres
    style.animIn = TextAnimation::Typewriter;
    style.animOut = TextAnimation::SlideUp;
    style.animSeconds = 1.0;
    const Id clip = edit::placeTitle(p, p.videoTracks[1].id, 0, 120, style);
    REQUIRE(clip != kNoId);
    auto layer = [&](FramePos f) {
        const auto layers = videoLayersAt(p, f);
        REQUIRE(layers.size() == 1);
        return layers[0];
    };
    // Máquina de escribir: 10 caracteres en 30 fotogramas.
    CHECK(layer(0).titleChars == 0);
    CHECK(layer(14).titleChars == 5);
    CHECK(layer(29).titleChars == -1); // ya completo
    CHECK(layer(60).titleChars == -1);
    CHECK(layer(60).transform.y == Approx(0.0));
    CHECK(layer(60).opacity == Approx(1.0));
    // Salida deslizando hacia arriba: en el último fotograma, casi abajo y casi transparente.
    const auto last = layer(119);
    CHECK(last.transform.y > 0.1);
    CHECK(last.opacity < 0.15);
    CHECK(layer(100).transform.y > 0.0);
    CHECK(layer(100).transform.y < last.transform.y);

    // En el archivo de proyecto.
    const auto loaded = deserializeProject(serializeProject(p, "C:/x/p.mxproj"), "C:/x/p.mxproj");
    REQUIRE(loaded.project);
    CHECK(*loaded.project->findClip(clip)->title == style);
}

TEST_CASE("Máquina de escribir: los caracteres aparecen en su sitio", "[render][titles]")
{
    render::QtTitleRenderer renderer;
    TitleStyle style;
    style.text = "AAAA BBBB";
    style.shadow = false;
    constexpr int W = 320, H = 180;
    const auto full = renderer.render(style, W, H);
    const auto half = renderer.render(style, W, H, 4); // solo «AAAA»
    const auto none = renderer.render(style, W, H, 0);
    REQUIRE((full && half && none));
    auto alphaIn = [&](const media::VideoFrame& f, int x0, int x1) {
        long sum = 0;
        for (int y = 0; y < H; ++y)
            for (int x = x0; x < x1; ++x)
                sum += f.rgba[(size_t(y) * W + size_t(x)) * 4 + 3];
        return sum;
    };
    CHECK(alphaIn(*none, 0, W) == 0);
    // Hasta donde acaba «AAAA», idéntico al texto completo (no se movió); después, vacío.
    int end = 0;
    for (int x = 0; x < W; ++x)
        if (alphaIn(*half, x, x + 1) > 0)
            end = x + 1;
    REQUIRE(end > 0);
    CHECK(end < W * 2 / 3);
    for (int x = 0; x < end; ++x)
        REQUIRE(alphaIn(*half, x, x + 1) == alphaIn(*full, x, x + 1));
    CHECK(alphaIn(*full, end + 1, W) > 0); // «BBBB» solo en el completo
}
