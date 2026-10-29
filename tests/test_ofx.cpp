#include "ProjectController.h"
#include "TestMedia.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "engine/MediaImport.h"
#include "engine/Sources.h"
#include "media/FrameConvert.h"
#include "ofx/OfxHost.h"
#include "playback/TimelinePlayer.h"
#include "render/OffscreenRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

using namespace movix;
using namespace movix::core;

namespace {

const std::string kGain = "io.github.gorgogito.movixstudio.test.Gain";

void useTestPlugins()
{
    ofx::Registry::instance().setExtraDirs({MOVIX_TEST_OFX_DIR});
    ofx::Registry::instance().rescan();
}

// Degradado RGBA de w×h con un píxel distinto arriba a la izquierda (para la orientación).
std::vector<uint8_t> testImage(int w, int h)
{
    std::vector<uint8_t> img(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &img[(size_t(y) * size_t(w) + size_t(x)) * 4];
            p[0] = uint8_t(x * 255 / (w - 1));
            p[1] = uint8_t(y * 255 / (h - 1));
            p[2] = 200;
            p[3] = 255;
        }
    img[0] = 10, img[1] = 220, img[2] = 30;
    return img;
}

} // namespace

TEST_CASE("OpenFX: el anfitrión encuentra y describe un plugin", "[ofx]")
{
    useTestPlugins();
    const auto info = ofx::Registry::instance().find(kGain);
    INFO([] {
        std::string all;
        for (const auto& e : ofx::Registry::instance().errors())
            all += e + "\n";
        return all;
    }());
    REQUIRE(info);
    CHECK(info->label == "Ganancia de prueba");
    CHECK(info->group == "MovixStudio/Pruebas");
    CHECK(info->versionMajor == 1);
    CHECK(info->versionMinor == 2);
    CHECK(info->context == "Filter");
    CHECK(info->floatImages);
    auto param = [&](const char* name) {
        auto it = std::find_if(info->params.begin(), info->params.end(), [&](const ofx::ParamDef& p) { return p.name == name; });
        REQUIRE(it != info->params.end());
        return *it;
    };
    const auto gain = param("gain");
    CHECK(gain.type == ofx::ParamType::Double);
    CHECK(gain.label == "Ganancia");
    CHECK(gain.defaults == std::vector<double>{1.0});
    CHECK(gain.min == 0.0);
    CHECK(gain.max == 4.0);
    CHECK(param("invert").type == ofx::ParamType::Bool);
    const auto channel = param("channel");
    CHECK(channel.type == ofx::ParamType::Choice);
    CHECK(channel.options == std::vector<std::string>{"Todos", "Rojo", "Verde", "Azul"});
    const auto tint = param("tint");
    CHECK(tint.type == ofx::ParamType::Rgb);
    CHECK(tint.defaults == std::vector<double>{1.0, 1.0, 1.0});
    CHECK(param("Controles").type == ofx::ParamType::Page);

    CHECK(gain.animates);
    CHECK(param("channel").animates); // OFX: también las listas y casillas (saltan de un valor a otro)

    const auto values = ofx::withDefaults(*info, {{"gain", {2.0}}});
    CHECK(values.at("gain") == std::vector<Animated>{2.0});
    CHECK(values.at("tint") == std::vector<Animated>{1.0, 1.0, 1.0});
    CHECK_FALSE(values.contains("Controles"));
}

TEST_CASE("OpenFX: una instancia renderiza con sus parámetros", "[ofx]")
{
    useTestPlugins();
    constexpr int W = 64, H = 32;
    std::string error;
    auto effect = ofx::Effect::create(kGain, W, H, 25.0, &error);
    INFO(error);
    REQUIRE(effect);
    const auto in = testImage(W, H);
    std::vector<uint8_t> out(in.size());
    auto px = [&](int x, int y) { return &out[(size_t(y) * W + size_t(x)) * 4]; };

    // Ganancia 0,5: cada canal de color a la mitad; el alfa intacto; la orientación se conserva.
    REQUIRE(effect->render({{"gain", {0.5}}}, 0, in.data(), out.data(), &error));
    CHECK(int(px(0, 0)[0]) == 5);
    CHECK(int(px(0, 0)[1]) == 110);
    CHECK(int(px(W - 1, H - 1)[0]) == 128);
    CHECK(int(px(W - 1, H - 1)[1]) == 128);
    CHECK(int(px(10, 10)[2]) == 100);
    CHECK(int(px(10, 10)[3]) == 255);

    // Solo el rojo, invertido; los valores que faltan toman el último puesto (no los por defecto):
    // por eso el que llama pasa todos (ver withDefaults).
    const auto info = ofx::Registry::instance().find(kGain);
    REQUIRE(effect->render(ofx::withDefaults(*info, {{"channel", {1}}, {"invert", {1}}}), 5, in.data(), out.data(), &error));
    CHECK(int(px(0, 0)[0]) == 245);
    CHECK(int(px(0, 0)[1]) == 220);
    CHECK(int(px(W - 1, 0)[0]) == 0);

    // El tinte (color) llega en sus tres componentes.
    REQUIRE(effect->render(ofx::withDefaults(*info, {{"tint", {1.0, 0.0, 0.5}}}), 0, in.data(), out.data(), &error));
    CHECK(int(px(W - 1, 0)[0]) == 255);
    CHECK(int(px(W - 1, 0)[1]) == 0);
    CHECK(int(px(W - 1, 0)[2]) == 100);
}

TEST_CASE("OpenFX: parámetros con keyframes", "[ofx]")
{
    useTestPlugins();
    constexpr int W = 64, H = 32;
    std::string error;
    auto effect = ofx::Effect::create(kGain, W, H, 25.0, &error);
    INFO(error);
    REQUIRE(effect);
    const auto info = ofx::Registry::instance().find(kGain);
    const auto in = testImage(W, H);
    std::vector<uint8_t> out(in.size());
    const size_t corner = (size_t(H - 1) * W + size_t(W - 1)) * 4; // rojo 255

    // Ganancia de 0,5 en el fotograma 0 a 1 en el 10; el plugin la lee en el instante del render.
    Animated gain(1.0);
    gain.setKey(0, 0.5);
    gain.setKey(10, 1.0);
    const auto values = ofx::withDefaults(*info, {{"gain", {gain}}});
    auto redAt = [&](double time) {
        REQUIRE(effect->render(values, time, in.data(), out.data(), &error));
        return int(out[corner]);
    };
    CHECK(redAt(0) == 128);
    CHECK(redAt(5) == 191);
    CHECK(redAt(2.5) == 159); // entre dos fotogramas (desenfoque de movimiento): interpolado
    CHECK(redAt(10) == 255);
    CHECK(redAt(40) == 255); // después del último keyframe se mantiene

    // Mantener: salta al llegar al keyframe siguiente.
    gain.setKey(0, 0.5, Interpolation::Hold);
    const auto held = ofx::withDefaults(*info, {{"gain", {gain}}});
    REQUIRE(effect->render(held, 9, in.data(), out.data(), &error));
    CHECK(int(out[corner]) == 128);

    // En coma flotante (imágenes de más de 8 bits): sin cuantizar a 1/255.
    std::vector<float> inF(in.size()), outF(in.size());
    for (size_t i = 0; i < in.size(); ++i)
        inF[i] = in[i] / 255.0f;
    inF[corner] = 0.6004f;
    REQUIRE(effect->render(values, 5, inF.data(), outF.data(), &error));
    CHECK(std::abs(outF[corner] - 0.6004f * 0.75f) < 1e-5f);
}

TEST_CASE("OpenFX: un efecto que no está instalado", "[ofx]")
{
    useTestPlugins();
    std::string error;
    CHECK_FALSE(ofx::Effect::create("org.inexistente.Efecto", 16, 16, 25.0, &error));
    CHECK(error.find("no está instalado") != std::string::npos);
    CHECK_FALSE(ofx::Registry::instance().find("org.inexistente.Efecto"));
}

TEST_CASE("OpenFX: plugins de terceros de una carpeta", "[ofx][externos]")
{
    // Fuera de la batería: MOVIX_OFX_EXTERNAL=<carpeta con paquetes .ofx.bundle>.
    const std::string dir = qEnvironmentVariable("MOVIX_OFX_EXTERNAL").toStdString();
    if (dir.empty())
        SKIP("Sin MOVIX_OFX_EXTERNAL");
    ofx::Registry::instance().setExtraDirs({dir});
    ofx::Registry::instance().rescan();
    for (const auto& e : ofx::Registry::instance().errors())
        WARN("No se cargó: " << e);
    constexpr int W = 64, H = 32;
    const auto in = testImage(W, H);
    int rendered = 0;
    for (const auto& p : ofx::Registry::instance().plugins()) {
        if (p.binary.find(dir.substr(dir.find_last_of("/\\") + 1)) == std::string::npos)
            continue; // solo los de esa carpeta
        std::string error;
        auto effect = ofx::Effect::create(p.id, W, H, 25.0, &error);
        INFO(p.id << ": " << error);
        REQUIRE(effect);
        std::vector<uint8_t> out(in.size(), 0);
        REQUIRE(effect->render(ofx::withDefaults(p, {}), 0, in.data(), out.data(), &error));
        size_t changed = 0;
        for (size_t i = 0; i < out.size(); ++i)
            changed += out[i] != in[i] ? 1 : 0;
        WARN(p.label << " (" << p.id << ", " << p.params.size() << " parámetros, " << (p.floatImages ? "float" : "8 bits")
                     << "): " << changed << " valores distintos de la entrada; píxel (0,0) = " << int(out[0]) << "," << int(out[1]) << ","
                     << int(out[2]) << "," << int(out[3]));
        ++rendered;
    }
    CHECK(rendered > 0);
}

TEST_CASE("OpenFX: efectos en un clip, en el archivo y en el motor", "[ofx][engine]")
{
    useTestPlugins();
    Project p = Project::createDefault();
    auto item = engine::probeMediaItem(test::mediaPath("sync_720p30.mp4"));
    REQUIRE(item);
    const Id media = edit::addMedia(p, *item);
    p.settings.width = 1280;
    p.settings.height = 720;
    const Id clip = edit::placeClip(p, p.videoTracks[0].id, media, 0, 0, 60);
    REQUIRE(clip != kNoId);

    // Ediciones: añadir, parámetros, desactivar, reordenar y quitar.
    REQUIRE(edit::addOfxEffect(p, clip, kGain) == 0);
    REQUIRE(edit::addOfxEffect(p, clip, kGain) == 1);
    REQUIRE(edit::setOfxParam(p, clip, 0, "gain", {0.5}));
    CHECK_FALSE(edit::setOfxParam(p, clip, 0, "gain", {0.5})); // sin cambio
    REQUIRE(edit::setOfxParam(p, clip, 1, "invert", {1}));
    REQUIRE(edit::moveOfxEffect(p, clip, 1, 0));
    CHECK(p.findClip(clip)->ofx[1].params.at("gain") == std::vector<Animated>{0.5});
    REQUIRE(edit::setOfxEffectEnabled(p, clip, 0, false)); // el de invertir, desactivado
    CHECK(p.validate());

    // En el archivo de proyecto.
    const auto loaded = deserializeProject(serializeProject(p, "C:/x/p.mxproj"), "C:/x/p.mxproj");
    REQUIRE(loaded.project);
    CHECK(loaded.project->findClip(clip)->ofx == p.findClip(clip)->ofx);

    // En el motor: el primer fotograma (blanco) sale a la mitad.
    engine::VideoSources sources(false);
    const auto bundle = sources.build(p, 0);
    REQUIRE(bundle.layers.size() == 1);
    const auto& f = bundle.layers[0].frame;
    REQUIRE(f);
    REQUIRE(f->format == media::PixelFormat::RGBA);
    const size_t center = (size_t(f->height / 2) * size_t(f->width) + size_t(f->width / 2)) * 4;
    CHECK(std::abs(int(f->rgba[center]) - 128) <= 3);
    CHECK(int(f->rgba[center + 3]) == 255);
    CHECK(sources.takeOfxError().empty());

    // Un plugin que no está instalado no rompe el render: el clip se ve sin él y se informa.
    REQUIRE(edit::removeOfxEffect(p, clip, 1));
    REQUIRE(edit::setOfxEffectEnabled(p, clip, 0, true));
    p.findClip(clip)->ofx[0].pluginId = "org.inexistente.Efecto";
    const auto plain = sources.build(p, 0);
    REQUIRE(plain.layers.size() == 1);
    CHECK(plain.layers[0].frame->format != media::PixelFormat::RGBA);
    CHECK(sources.takeOfxError().find("no está instalado") != std::string::npos);

    // Solo en clips con imagen.
    CHECK(edit::addOfxEffect(p, p.audioTracks[0].id, kGain) == -1);
}

TEST_CASE("OpenFX en un clip compuesto: el plugin recibe su secuencia compuesta, con su transparencia", "[ofx][engine][compound]")
{
    useTestPlugins();
    Project p = Project::createDefault();
    p.settings.width = 320;
    p.settings.height = 180;
    // Dentro: un video reducido a la mitad y, encima, una imagen con la mitad derecha transparente.
    auto video = engine::probeMediaItem(test::mediaPath("scenes_720p30.mp4"));
    auto image = engine::probeMediaItem(test::mediaPath("half_alpha.png"));
    REQUIRE(video);
    REQUIRE(image);
    const Id a = edit::placeClip(p, p.videoTracks[0].id, edit::addMedia(p, *video), 0, 0, 30);
    p.findClip(a)->transform.scale = 0.5;
    edit::addTrack(p, TrackKind::Video);
    const Id b = edit::placeClip(p, p.videoTracks[1].id, edit::addMedia(p, *image), 0, 0, 30);
    const Id comp = edit::createCompound(p, {a, b}, "C");
    REQUIRE(comp != kNoId);

    engine::VideoSources sources(false);
    sources.setBundleRenderer(std::make_shared<render::QtBundleRenderer>());
    render::OffscreenRenderer r;
    REQUIRE(r.init(320, 180));
    auto picture = [&]() {
        std::vector<uint8_t> px;
        REQUIRE(r.render(sources.build(p, 0), px));
        return px;
    };
    auto at = [](const std::vector<uint8_t>& px, int x, int y) { return &px[(size_t(y) * 320 + size_t(x)) * 4]; };
    const auto plain = picture();
    CHECK(at(plain, 200, 90)[0] + at(plain, 200, 90)[1] + at(plain, 200, 90)[2] > 150); // video
    CHECK(at(plain, 300, 10)[0] + at(plain, 300, 10)[1] + at(plain, 300, 10)[2] == 0);   // nada: fondo negro

    // Efecto neutro: el compuesto se ve igual (la secuencia pasa por memoria y vuelve).
    REQUIRE(edit::addOfxEffect(p, comp, kGain) == 0);
    const auto bundle = sources.build(p, 0);
    REQUIRE(bundle.layers.size() == 1);
    REQUIRE(bundle.layers[0].frame);
    CHECK_FALSE(bundle.layers[0].nested);
    CHECK(bundle.layers[0].frame->format == media::PixelFormat::RGBA);
    CHECK(int(bundle.layers[0].frame->rgba[(size_t(10) * 320 + 300) * 4 + 3]) == 0); // transparente al plugin
    CHECK(sources.takeOfxError().empty());
    const auto neutral = picture();
    int worst = 0;
    for (size_t i = 0; i < plain.size(); ++i)
        worst = std::max(worst, std::abs(int(plain[i]) - int(neutral[i])));
    std::printf("[ofx] compuesto con un efecto neutro: diferencia máxima %d/255\n", worst);
    CHECK(worst <= 2);

    // Ganancia 0,5 sobre el compuesto: el video baja a la mitad y lo transparente sigue sin verse.
    REQUIRE(edit::setOfxParam(p, comp, 0, "gain", {0.5}));
    const auto half = picture();
    for (int c = 0; c < 3; ++c)
        CHECK(std::abs(int(at(half, 200, 90)[c]) - int(at(plain, 200, 90)[c]) / 2) <= 2);
    // Invertir: lo opaco se invierte; lo transparente, aunque el plugin cambie su color, no aparece.
    REQUIRE(edit::setOfxParam(p, comp, 0, "gain", {1.0}));
    REQUIRE(edit::setOfxParam(p, comp, 0, "invert", {1}));
    const auto inverted = picture();
    for (int c = 0; c < 3; ++c) {
        CHECK(std::abs(int(at(inverted, 200, 90)[c]) - (255 - int(at(plain, 200, 90)[c]))) <= 2);
        CHECK(int(at(inverted, 300, 10)[c]) == 0);
    }

    // Sin renderizador de secuencias el compuesto se ve sin el efecto (y no se rompe nada).
    engine::VideoSources bare(false);
    const auto nested = bare.build(p, 0);
    REQUIRE(nested.layers.size() == 1);
    CHECK(nested.layers[0].nested);

    // En un proyecto HDR10 la secuencia pasa en coma flotante y en su señal: un efecto neutro no la cambia.
    {
        Project hp = Project::createDefault();
        hp.settings.width = 320;
        hp.settings.height = 180;
        hp.settings.colorMode = ColorMode::PQ;
        auto hdr = engine::probeMediaItem(test::mediaPath("hdr_pq.mkv"));
        REQUIRE(hdr);
        const Id h = edit::placeClip(hp, hp.videoTracks[0].id, edit::addMedia(hp, *hdr), 0, 0, 30);
        const Id hc = edit::createCompound(hp, {h}, "HDR");
        REQUIRE(hc != kNoId);
        render::OffscreenRenderer deep;
        REQUIRE(deep.init(320, 180, nullptr, true));
        auto centerPq = [&]() {
            std::vector<uint8_t> px;
            REQUIRE(deep.render(sources.build(hp, 0), px));
            const size_t o = (size_t(90) * 320 + 160) * 4 * 2;
            return (px[o] | (px[o + 1] << 8)) / 65535.0;
        };
        const double before = centerPq();
        REQUIRE(edit::addOfxEffect(hp, hc, kGain) == 0);
        const auto hb = sources.build(hp, 0);
        REQUIRE(hb.layers.size() == 1);
        REQUIRE(hb.layers[0].frame);
        CHECK(hb.layers[0].frame->format == media::PixelFormat::RGBA16F);
        const double after = centerPq();
        std::printf("[ofx] compuesto HDR10 sin y con el efecto: %.4f / %.4f\n", before, after);
        CHECK(std::abs(before - after) < 0.002);
    }

    // El efecto del compuesto viaja en el archivo.
    const auto loaded = deserializeProject(serializeProject(p, "C:/x/p.mxproj"), "C:/x/p.mxproj");
    REQUIRE(loaded.project);
    CHECK(loaded.project->findClip(comp)->ofx == p.findClip(comp)->ofx);
}

TEST_CASE("OpenFX: keyframes en el clip, en el archivo y en el motor", "[ofx][engine]")
{
    useTestPlugins();
    Project p = Project::createDefault();
    auto item = engine::probeMediaItem(test::mediaPath("sync_720p30.mp4"));
    REQUIRE(item);
    const Id media = edit::addMedia(p, *item);
    p.settings.width = 1280;
    p.settings.height = 720;
    const Id clip = edit::placeClip(p, p.videoTracks[0].id, media, 0, 0, 60);
    REQUIRE(edit::addOfxEffect(p, clip, kGain) == 0);
    auto gainOf = [&]() -> const std::vector<Animated>& { return p.findClip(clip)->ofx[0].params.at("gain"); };

    // Keyframes: 0 → ganancia 0 y 30 → 1 (cambiar el valor en un keyframe lo actualiza; fuera de los
    // keyframes, crea otro).
    REQUIRE(edit::toggleOfxKey(p, clip, 0, "gain", 0, {1.0}));
    REQUIRE(edit::setOfxParam(p, clip, 0, "gain", {0.0}, 0));
    REQUIRE(edit::toggleOfxKey(p, clip, 0, "gain", 30, {0.0}));
    REQUIRE(edit::setOfxParam(p, clip, 0, "gain", {1.0}, 30));
    REQUIRE(gainOf()[0].keys.size() == 2);
    CHECK(gainOf()[0].at(15) == 0.5);
    REQUIRE(edit::setOfxKeyInterpolation(p, clip, 0, "gain", 0, Interpolation::Smooth));
    CHECK_FALSE(edit::setOfxKeyInterpolation(p, clip, 0, "gain", 7, Interpolation::Hold)); // sin keyframe ahí
    REQUIRE(edit::setOfxKeyInterpolation(p, clip, 0, "gain", 0, Interpolation::Linear));
    // Los de varios componentes llevan los keyframes en los mismos fotogramas.
    REQUIRE(edit::toggleOfxKey(p, clip, 0, "tint", 10, {1.0, 0.5, 0.25}));
    const auto& tint = p.findClip(clip)->ofx[0].params.at("tint");
    REQUIRE(tint.size() == 3);
    CHECK(tint[1].keyAt(10));
    CHECK(tint[2].at(10) == 0.25);
    REQUIRE(edit::toggleOfxKey(p, clip, 0, "tint", 10, {1.0, 0.5, 0.25})); // quitar el único: fijo en su valor
    CHECK_FALSE(p.findClip(clip)->ofx[0].params.at("tint")[1].isAnimated());
    CHECK(p.findClip(clip)->ofx[0].params.at("tint")[1].value == 0.5);

    // En el archivo (los parámetros fijos siguen siendo números, como en la versión 8).
    const QByteArray json = serializeProject(p, "C:/x/p.mxproj");
    const auto loaded = deserializeProject(json, "C:/x/p.mxproj");
    REQUIRE(loaded.project);
    CHECK(loaded.project->findClip(clip)->ofx == p.findClip(clip)->ofx);
    CHECK(json.contains(R"("tint": [)"));

    // En el motor: el primer fotograma (blanco) va de negro a blanco.
    engine::VideoSources sources(false);
    auto centerRed = [&](FramePos frame) {
        const auto bundle = sources.build(p, frame);
        REQUIRE(bundle.layers.size() == 1);
        const auto& f = bundle.layers[0].frame;
        REQUIRE(f->format == media::PixelFormat::RGBA);
        return int(f->rgba[(size_t(f->height / 2) * size_t(f->width) + size_t(f->width / 2)) * 4]);
    };
    const int start = centerRed(0), end = centerRed(30); // los dos fotogramas son blancos
    // A mitad de camino entre dos keyframes (el fotograma 0, entre −30 y 30), la mitad.
    Animated half;
    half.setKey(-30, 0.0);
    half.setKey(30, 1.0);
    const auto animated = p.findClip(clip)->ofx[0].params;
    p.findClip(clip)->ofx[0].params["gain"] = {half};
    const int middle = centerRed(0);
    p.findClip(clip)->ofx[0].params = animated;
    std::printf("[ofx] rojo con la ganancia animada: %d, %d, %d\n", start, middle, end);
    CHECK(start <= 3);
    CHECK(end >= 230);
    CHECK(std::abs(middle - end / 2) <= 3);

    // Recortar el principio del clip mantiene los keyframes en el mismo instante de la línea de tiempo.
    REQUIRE(edit::trimClip(p, clip, edit::Edge::Start, 10) == FramePos(10));
    CHECK(gainOf()[0].keys.front().frame == -10);
    CHECK(gainOf()[0].keys.back().frame == 20);
}

TEST_CASE("OpenFX en video de 10 bits y HDR: sin pasar por 8 bits ni perder la señal", "[ofx][engine][hdr]")
{
    useTestPlugins();
    SECTION("10 bits: la imagen llega con su precisión")
    {
        Project p = Project::createDefault();
        auto item = engine::probeMediaItem(test::mediaPath("hevc10_360p.mkv"));
        REQUIRE(item);
        p.settings.width = 640;
        p.settings.height = 360;
        const Id clip = edit::placeClip(p, p.videoTracks[0].id, edit::addMedia(p, *item), 0, 0, 25);
        REQUIRE(edit::addOfxEffect(p, clip, kGain) == 0); // ganancia 1: la imagen tal cual
        engine::VideoSources sources(false);
        const auto bundle = sources.build(p, 0);
        REQUIRE(bundle.layers.size() == 1);
        const auto& f = *bundle.layers[0].frame;
        REQUIRE(f.format == media::PixelFormat::RGBA16F);
        CHECK(sources.takeOfxError().empty());
        // Valores que no caen en la rejilla de 8 bits: la entrada del plugin no se redujo a 1/255.
        // (Entre los que no están saturados: con 8 bits serían el 0 %; con 10, más de la mitad.)
        size_t offGrid = 0, middle = 0, total = size_t(f.width) * size_t(f.height) * 4;
        for (size_t i = 0; i < total; i += 4) {
            const float v = media::halfAt(f.rgba, i) * 255.0f;
            if (v < 2.0f || v > 253.0f)
                continue;
            ++middle;
            offGrid += std::abs(v - std::round(v)) > 0.2f ? 1 : 0;
        }
        std::printf("[ofx] 10 bits: %.1f %% de los valores fuera de la rejilla de 8 bits\n", 100.0 * double(offGrid) / double(middle));
        REQUIRE(middle > total / 4 / 10);
        CHECK(offGrid > middle / 2);
    }
    SECTION("HDR10: el clip con el efecto se ve igual que sin él")
    {
        Project p = Project::createDefault();
        auto item = engine::probeMediaItem(test::mediaPath("hdr_pq.mkv"));
        REQUIRE(item);
        p.settings.width = 320;
        p.settings.height = 180;
        p.settings.colorMode = ColorMode::PQ;
        const Id clip = edit::placeClip(p, p.videoTracks[0].id, edit::addMedia(p, *item), 0, 0, 30);
        engine::VideoSources sources(false);
        render::OffscreenRenderer r;
        REQUIRE(r.init(320, 180, nullptr, true));
        auto centerPq = [&]() {
            std::vector<uint8_t> px;
            REQUIRE(r.render(sources.build(p, 0), px));
            const size_t o = (size_t(90) * 320 + 160) * 4 * 2;
            return std::array<double, 3>{(px[o] | (px[o + 1] << 8)) / 65535.0, (px[o + 2] | (px[o + 3] << 8)) / 65535.0,
                                         (px[o + 4] | (px[o + 5] << 8)) / 65535.0};
        };
        const auto plain = centerPq();
        REQUIRE(edit::addOfxEffect(p, clip, kGain) == 0);
        const auto bundle = sources.build(p, 0);
        REQUIRE(bundle.layers.size() == 1);
        CHECK(bundle.layers[0].frame->format == media::PixelFormat::RGBA16F);
        CHECK(bundle.layers[0].frame->transfer == media::Transfer::PQ);
        CHECK(bundle.layers[0].frame->colorSpace == media::ColorSpace::BT2020);
        const auto effect = centerPq();
        std::printf("[ofx] HDR10 sin y con el efecto: %.4f %.4f %.4f / %.4f %.4f %.4f\n", plain[0], plain[1], plain[2], effect[0], effect[1],
                    effect[2]);
        for (int c = 0; c < 3; ++c)
            CHECK(std::abs(plain[size_t(c)] - effect[size_t(c)]) < 0.001);
        // Con ganancia 0,5 la señal PQ baja (el efecto se aplica de verdad).
        REQUIRE(edit::setOfxParam(p, clip, 0, "gain", {0.5}));
        CHECK(centerPq()[0] < plain[0] * 0.6);
    }
}

TEST_CASE("OpenFX desde la aplicación", "[ofx][app]")
{
    QTemporaryDir dir;
    qputenv("MOVIX_OFX_DIR", MOVIX_TEST_OFX_DIR);
    {
        ProjectController project;
        project.rescanOfx(); // el registro es global: con la carpeta de las pruebas
        const QVariantList plugins = project.ofxPlugins();
        REQUIRE(std::any_of(plugins.begin(), plugins.end(), [](const QVariant& v) { return v.toMap()["id"].toString() == QString::fromStdString(kGain); }));
        project.newProject();
        REQUIRE(project.addTitle(0, -1, 0));
        const qint64 clip = project.selection()[0].toLongLong();
        project.addOfxEffect(clip, QString::fromStdString(kGain));
        QVariantList effects = project.ofxEffects(clip);
        REQUIRE(effects.size() == 1);
        QVariantMap fx = effects[0].toMap();
        CHECK(fx["installed"].toBool());
        CHECK(fx["label"].toString() == "Ganancia de prueba");
        const QVariantList params = fx["params"].toList();
        REQUIRE(params.size() == 4); // la página no es un control
        const QVariantMap gain = params[0].toMap();
        CHECK(gain["type"].toString() == "double");
        CHECK(gain["value"].toList() == QVariantList{1.0});
        CHECK(gain["max"].toDouble() == 4.0);
        CHECK(params[2].toMap()["options"].toList().size() == 4);

        // Arrastrar el deslizador: un solo paso de deshacer.
        project.setOfxParam(clip, 0, "gain", {0.8});
        project.setOfxParam(clip, 0, "gain", {0.6});
        project.endInteraction();
        CHECK(project.ofxEffects(clip)[0].toMap()["params"].toList()[0].toMap()["value"].toList() == QVariantList{0.6});
        project.undo();
        CHECK(project.ofxEffects(clip)[0].toMap()["params"].toList()[0].toMap()["value"].toList() == QVariantList{1.0});

        // Keyframes desde la interfaz: en el cabezal, con el estado de cada parámetro.
        auto gainState = [&] { return project.ofxEffects(clip)[0].toMap()["params"].toList()[0].toMap(); };
        CHECK(gainState()["animates"].toBool());
        CHECK_FALSE(gainState()["animated"].toBool());
        project.player()->seekFrame(0);
        project.toggleOfxKeyframe(clip, 0, "gain");
        CHECK(gainState()["keyHere"].toBool());
        CHECK(gainState()["interp"].toString() == "linear");
        project.player()->seekFrame(20);
        project.setOfxParam(clip, 0, "gain", {2.0}); // animado: crea el keyframe de este instante
        project.endInteraction();
        CHECK(gainState()["keyHere"].toBool());
        CHECK(gainState()["hasPrev"].toBool());
        project.player()->seekFrame(10);
        CHECK(gainState()["value"].toList() == QVariantList{1.5});
        CHECK_FALSE(gainState()["keyHere"].toBool());
        CHECK(project.keyframeFrames(clip).size() == 2);
        project.jumpToKeyframe(clip, true);
        CHECK(project.player()->positionFrame() == 20);
        project.setOfxKeyInterpolation(clip, 0, "gain", "hold");
        CHECK(gainState()["interp"].toString() == "hold");
        // Las listas saltan de una opción a otra.
        project.toggleOfxKeyframe(clip, 0, "channel");
        CHECK(project.ofxEffects(clip)[0].toMap()["params"].toList()[2].toMap()["interp"].toString() == "hold");
        project.toggleOfxKeyframe(clip, 0, "gain"); // quitar el de 20
        CHECK_FALSE(gainState()["keyHere"].toBool());
        project.undo();
        CHECK(gainState()["keyHere"].toBool());

        project.setOfxEnabled(clip, 0, false);
        CHECK_FALSE(project.ofxEffects(clip)[0].toMap()["enabled"].toBool());
        project.removeOfxEffect(clip, 0);
        CHECK(project.ofxEffects(clip).isEmpty());
    }
    qunsetenv("MOVIX_OFX_DIR");
}
