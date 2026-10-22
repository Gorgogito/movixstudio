#include "ProjectController.h"
#include "TestMedia.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "engine/MediaImport.h"
#include "engine/Sources.h"
#include "ofx/OfxHost.h"

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

    const auto values = ofx::withDefaults(*info, {{"gain", {2.0}}});
    CHECK(values.at("gain") == std::vector<double>{2.0});
    CHECK(values.at("tint") == std::vector<double>{1.0, 1.0, 1.0});
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
    CHECK(p.findClip(clip)->ofx[1].params.at("gain") == std::vector<double>{0.5});
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

    // Solo en clips con imagen que no son compuestos.
    CHECK(edit::addOfxEffect(p, p.audioTracks[0].id, kGain) == -1);
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
        project.setOfxEnabled(clip, 0, false);
        CHECK_FALSE(project.ofxEffects(clip)[0].toMap()["enabled"].toBool());
        project.removeOfxEffect(clip, 0);
        CHECK(project.ofxEffects(clip).isEmpty());
    }
    qunsetenv("MOVIX_OFX_DIR");
}
