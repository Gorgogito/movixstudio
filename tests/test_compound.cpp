#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"
#include "export/Exporter.h"
#include "media/VideoDecoder.h"
#include "render/OffscreenRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

#include <cmath>

using namespace movix;
using namespace movix::core;
using Catch::Approx;

namespace {

// Proyecto con un medio de video con audio (sin archivo: solo se evalúa la línea de tiempo).
struct Fixture {
    Project p = Project::createDefault();
    Id media = kNoId;
    Id a = kNoId, b = kNoId, c = kNoId; // V1, V2 y A1

    Fixture()
    {
        MediaItem m;
        m.path = "inexistente.mp4";
        m.name = "clip";
        m.duration = 20 * kUsPerSecond;
        m.hasVideo = m.hasAudio = true;
        m.width = 1920;
        m.height = 1080;
        m.frameRate = 30;
        media = edit::addMedia(p, m);
        a = edit::placeClip(p, p.videoTracks[0].id, media, 30, 0, 60);
        b = edit::placeClip(p, p.videoTracks[1].id, media, 60, 5 * kUsPerSecond, 60);
        c = edit::placeClip(p, p.audioTracks[0].id, media, 45, 2 * kUsPerSecond, 30);
        REQUIRE((a && b && c));
    }
};

std::shared_ptr<const media::VideoFrame> rgbaFrame(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    auto f = std::make_shared<media::VideoFrame>();
    f->width = w;
    f->height = h;
    f->format = media::PixelFormat::RGBA;
    f->rgba.resize(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < f->rgba.size(); i += 4) {
        f->rgba[i] = r;
        f->rgba[i + 1] = g;
        f->rgba[i + 2] = b;
        f->rgba[i + 3] = 255;
    }
    return f;
}

} // namespace

TEST_CASE("Crear un clip compuesto conserva posiciones, pistas y contenido", "[core][compound]")
{
    Fixture f;
    Project& p = f.p;
    const auto before = videoLayersAt(p, 70); // a y b visibles
    REQUIRE(before.size() == 2);
    const Id comp = edit::createCompound(p, {f.a, f.b, f.c}, "Compuesto 1");
    REQUIRE(comp != kNoId);
    INFO([&] { std::string why; p.validate(&why); return why; }());
    REQUIRE(p.validate());

    const Clip* cc = p.findClip(comp);
    REQUIRE(cc);
    CHECK(cc->start == 30);
    CHECK(cc->duration == 90); // de 30 a 120
    CHECK(p.trackOfClip(comp) == &p.videoTracks[0]);
    CHECK_FALSE(p.findClip(f.a));
    REQUIRE(p.sequences.size() == 1);
    const Sequence& seq = p.sequences[0];
    CHECK(seq.name == "Compuesto 1");
    REQUIRE(seq.videoTracks.size() == 2);
    REQUIRE(seq.audioTracks.size() == 1);
    CHECK(seq.videoTracks[0].clips[0].start == 0);
    CHECK(seq.videoTracks[1].clips[0].start == 30);
    CHECK(seq.audioTracks[0].clips[0].start == 15);
    CHECK(p.sequenceDuration(seq.id) == 90);

    SECTION("la imagen es la misma, ahora anidada")
    {
        const auto after = videoLayersAt(p, 70);
        REQUIRE(after.size() == 1);
        CHECK(after[0].compound);
        REQUIRE(after[0].nested.size() == 2);
        for (size_t i = 0; i < 2; ++i)
            CHECK(after[0].nested[i].sourceTime == before[i].sourceTime);
        CHECK(after[0].nested[0].instance != after[0].nested[0].clipId); // lector propio por aparición
    }
    SECTION("el audio de dentro suena en su sitio y con el volumen del compuesto")
    {
        REQUIRE(edit::setProperty(p, comp, edit::Property::Volume, 0, 0.5));
        const TimeUs t0 = p.settings.frameToTime(50), t1 = p.settings.frameToTime(52);
        const auto segs = audioSegmentsIn(p, t0, t1);
        // Dentro: el audio de a (V1), el del clip de audio (A1); b aún no empieza.
        REQUIRE(segs.size() == 2);
        for (const auto& s : segs) {
            CHECK(s.timelineStart == t0);
            CHECK(s.timelineEnd == t1);
            CHECK(s.gainStart == Approx(0.5f));
        }
        const auto& audio = *std::find_if(segs.begin(), segs.end(), [&](const AudioSegment& s) { return s.clipId == f.c; });
        CHECK(audio.sourceStart == 2 * kUsPerSecond + p.settings.frameToTime(50 - 45));
    }
    SECTION("a doble velocidad, el contenido avanza el doble")
    {
        REQUIRE(edit::setClipSpeed(p, comp, 2.0));
        CHECK(p.findClip(comp)->duration == 45);
        const auto layers = videoLayersAt(p, 40); // 10 fotogramas → 20 dentro
        REQUIRE(layers.size() == 1);
        REQUIRE(layers[0].nested.size() == 1);
        CHECK(layers[0].nested[0].sourceTime == p.settings.frameToTime(20));
        const auto segs = audioSegmentsIn(p, p.settings.frameToTime(40), p.settings.frameToTime(41));
        for (const auto& s : segs)
            CHECK(s.speed == Approx(2.0));
    }
    SECTION("recortar el inicio desplaza la vista de la secuencia")
    {
        REQUIRE(edit::trimClip(p, comp, edit::Edge::Start, 40));
        CHECK(p.findClip(comp)->sourceIn == p.settings.frameToTime(10));
        CHECK_FALSE(edit::trimClip(p, comp, edit::Edge::End, 500) == std::optional<FramePos>(500)); // limitado a la secuencia
        CHECK(p.findClip(comp)->end() == 120);
    }
}

TEST_CASE("Deshacer un clip compuesto devuelve los clips a su sitio", "[core][compound]")
{
    Fixture f;
    Project& p = f.p;
    const Project original = p;
    const Id comp = edit::createCompound(p, {f.a, f.b, f.c}, "C");
    const auto ids = edit::breakCompound(p, comp);
    CHECK(ids.size() == 3);
    REQUIRE(p.validate());
    CHECK(p.sequences.empty()); // la secuencia ya no se usa
    // Mismas posiciones e instantes del medio (con ids nuevos).
    for (size_t k = 0; k < 2; ++k) {
        const auto& kindA = k == 0 ? original.videoTracks : original.audioTracks;
        const auto& kindB = k == 0 ? p.videoTracks : p.audioTracks;
        for (size_t t = 0; t < kindA.size(); ++t) {
            REQUIRE(kindA[t].clips.size() == kindB[t].clips.size());
            for (size_t i = 0; i < kindA[t].clips.size(); ++i) {
                CHECK(kindA[t].clips[i].start == kindB[t].clips[i].start);
                CHECK(kindA[t].clips[i].duration == kindB[t].clips[i].duration);
                CHECK(kindA[t].clips[i].sourceIn == kindB[t].clips[i].sourceIn);
            }
        }
    }
}

TEST_CASE("Abrir y editar la secuencia de un compuesto", "[core][compound]")
{
    Fixture f;
    Project& p = f.p;
    const Id main = p.activeSequence;
    const Id comp = edit::createCompound(p, {f.a, f.b}, "Dentro");
    const Id seq = p.findClip(comp)->sequenceId;
    REQUIRE(edit::openSequence(p, seq));
    CHECK(p.activeSequence == seq);
    CHECK(p.activeName == "Dentro");
    CHECK(p.sequencePath == std::vector<Id>{main});
    REQUIRE(p.validate());
    CHECK(p.videoTracks[0].clips.size() == 1);

    SECTION("acortar la secuencia acorta el compuesto al volver")
    {
        const Id inner = p.videoTracks[1].clips[0].id;
        REQUIRE(edit::deleteClips(p, {inner}));
        CHECK(p.duration() == 60);
        REQUIRE(edit::closeSequence(p));
        CHECK(p.activeSequence == main);
        CHECK(p.sequencePath.empty());
        CHECK(p.findClip(comp)->duration == 60);
        REQUIRE(p.validate());
    }
    SECTION("no se puede meter un compuesto dentro de sí mismo")
    {
        // Se copia el compuesto en la principal y se intenta pegar dentro de su propia secuencia.
        REQUIRE(edit::closeSequence(p));
        const auto clipboard = edit::copyClips(p, {comp});
        REQUIRE(edit::openSequence(p, seq));
        CHECK(edit::pasteClips(p, clipboard, 200).empty());
        REQUIRE(p.validate());
    }
    SECTION("el proyecto se guarda y se abre con la secuencia anidada activa")
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("anidado.mxproj");
        REQUIRE(saveProjectFile(p, file));
        auto loaded = loadProjectFile(file);
        INFO(loaded.error.toStdString());
        REQUIRE(loaded.project);
        CHECK(*loaded.project == p);
    }
}

TEST_CASE("GPU: un compuesto se dibuja como una capa con fondo transparente", "[render][compound]")
{
    constexpr int W = 320, H = 180;
    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    auto nested = std::make_shared<engine::FrameBundle>();
    nested->canvasWidth = W;
    nested->canvasHeight = H;
    engine::LayerFrame red;
    red.frame = rgbaFrame(32, 18, 255, 0, 0);
    red.transform.scale = 0.5; // solo el centro: alrededor, transparente
    nested->layers.push_back(red);

    engine::FrameBundle b;
    b.canvasWidth = W;
    b.canvasHeight = H;
    engine::LayerFrame blue;
    blue.frame = rgbaFrame(32, 18, 0, 0, 255);
    b.layers.push_back(blue);
    engine::LayerFrame comp;
    comp.nested = nested;
    comp.opacity = 0.5;
    b.layers.push_back(comp);

    std::vector<uint8_t> px;
    REQUIRE(r.render(b, px));
    auto at = [&](int x, int y) {
        const size_t i = (size_t(y) * W + size_t(x)) * 4;
        return std::array<int, 3>{px[i], px[i + 1], px[i + 2]};
    };
    const auto center = at(W / 2, H / 2);
    CHECK(std::abs(center[0] - 128) <= 3); // rojo al 50 % sobre azul
    CHECK(std::abs(center[2] - 127) <= 3);
    const auto corner = at(10, 10);
    CHECK(corner[0] <= 2); // fuera del rojo: se ve el azul de abajo, sin oscurecer
    CHECK(corner[2] >= 252);
}

TEST_CASE("Exportar un proyecto con un título dentro de un compuesto", "[export][compound]")
{
    QTemporaryDir dir;
    Project p = Project::createDefault();
    p.settings.width = 320;
    p.settings.height = 180;
    TitleStyle style;
    style.text = "M"; // estrecho: cabe en la mitad derecha al desplazarlo
    style.size = 0.4;
    style.shadow = false;
    const Id title = edit::placeTitle(p, p.videoTracks[0].id, 0, 10, style);
    const Id comp = edit::createCompound(p, {title}, "Título anidado");
    REQUIRE(comp != kNoId);
    REQUIRE(edit::setProperty(p, comp, edit::Property::PositionX, 0, 0.25)); // el compuesto se mueve a la derecha
    // Se exporta la principal aunque se esté editando la secuencia anidada.
    REQUIRE(edit::openSequence(p, p.findClip(comp)->sequenceId));
    exporting::ExportSettings s;
    s.path = dir.filePath("compuesto.mp4");
    s.width = 320;
    s.height = 180;
    s.frameRate = p.settings.frameRate;
    exporting::Exporter exporter;
    std::atomic<bool> cancel{false};
    QString error;
    REQUIRE(exporter.run(p, s, cancel, {}, &error));
    media::VideoDecoder dec;
    REQUIRE(dec.open(s.path.toStdString(), media::VideoDecoderOptions{false}));
    auto frame = dec.decodeNext();
    REQUIRE(frame);
    int left = 0, right = 0;
    for (int y = 0; y < frame->height; ++y)
        for (int x = 0; x < frame->width; ++x)
            (x < frame->width / 2 ? left : right) = std::max(x < frame->width / 2 ? left : right,
                                                            int(frame->y[size_t(y) * size_t(frame->width) + size_t(x)]));
    CHECK(right > 200); // el texto, desplazado a la derecha
    CHECK(left < 60);
}
