#include "TestMedia.h"
#include "core/Edits.h"
#include "engine/MediaImport.h"
#include "engine/Sources.h"
#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace movix;
using namespace movix::core;

namespace {

Id importMedia(Project& p, const char* name)
{
    auto item = engine::probeMediaItem(test::mediaPath(name));
    REQUIRE(item);
    return edit::addMedia(p, *item);
}

// Todos los fotogramas de un archivo, decodificados en orden (referencia).
std::vector<std::shared_ptr<media::VideoFrame>> allFrames(const char* name)
{
    media::VideoDecoder dec;
    REQUIRE(dec.open(test::mediaPath(name), media::VideoDecoderOptions{false}));
    std::vector<std::shared_ptr<media::VideoFrame>> frames;
    while (auto f = dec.decodeNext())
        frames.push_back(f);
    return frames;
}

std::vector<float> allSamples(const char* name)
{
    media::AudioDecoder dec;
    REQUIRE(dec.open(test::mediaPath(name), 48000, 2));
    std::vector<float> samples;
    media::AudioChunk c;
    while (dec.read(c))
        samples.insert(samples.end(), c.samples.begin(), c.samples.end());
    return samples;
}

} // namespace

TEST_CASE("El lector de video entrega el fotograma correcto y decodifica en orden", "[engine][video]")
{
    const auto ref = allFrames("count_1080p60.mp4"); // 180 fotogramas a 60 fps
    engine::VideoReader reader;
    REQUIRE(reader.open(test::mediaPath("count_1080p60.mp4"), false));

    // Muestreo a 30 fps: cada petición avanza dos fotogramas del medio.
    for (int k = 0; k < 90; ++k) {
        auto f = reader.frameAt(k * kUsPerSecond / 30);
        REQUIRE(f);
        CHECK(f->y == ref[size_t(2 * k)]->y);
    }
    CHECK(reader.seekCount() == 1); // solo la búsqueda inicial

    // Acceso aleatorio hacia atrás y muy adelante.
    for (int idx : {10, 170, 3, 120}) {
        auto f = reader.frameAt(ref[size_t(idx)]->pts + 4000); // a mitad de fotograma
        REQUIRE(f);
        CHECK(f->y == ref[size_t(idx)]->y);
    }
    // Más allá del final: el último fotograma.
    CHECK(reader.frameAt(100 * kUsPerSecond)->y == ref.back()->y);
}

TEST_CASE("Composición: capas por pista con el instante de fuente correcto", "[engine][video]")
{
    Project p = Project::createDefault();
    p.settings.frameRate = {60, 1};
    const Id media = importMedia(p, "count_1080p60.mp4");
    edit::placeClip(p, p.videoTracks[0].id, media, 0);                         // V1: todo el clip
    const Id top = edit::placeClip(p, p.videoTracks[1].id, media, 30, kUsPerSecond, 60); // V2 en 30-90, desde 1 s
    const auto ref = allFrames("count_1080p60.mp4");

    engine::VideoSources sources(false);
    const engine::FrameBundle b = sources.build(p, 45);
    REQUIRE(b.layers.size() == 2);
    CHECK(b.layers[0].frame->y == ref[45]->y);          // V1: fotograma 45 del medio
    CHECK(b.layers[1].clipId == top);
    CHECK(b.layers[1].frame->y == ref[60 + 15]->y);    // V2: 1 s + 15 fotogramas
    CHECK(sources.openCount() == 2);                   // un lector por clip

    CHECK(sources.build(p, 100).layers.size() == 1);
    CHECK(sources.build(p, 1000).layers.empty());
}

TEST_CASE("Mezcla de audio exacta a la muestra", "[engine][audio]")
{
    Project p = Project::createDefault(); // 30 fps
    const Id tone = importMedia(p, "tone_44k_mono.wav");    // 3 s
    const Id clip = edit::placeClip(p, p.audioTracks[0].id, tone, 30); // empieza en 1 s
    const auto ref = allSamples("tone_44k_mono.wav");

    engine::AudioMixer mixer(48000, 2);

    SECTION("bloques consecutivos reproducen el archivo sin huecos ni búsquedas")
    {
        std::vector<float> rendered;
        std::vector<float> block(1000 * 2);
        for (int64_t pos = 0; pos < 5 * 48000; pos += 1000) {
            mixer.mix(p, pos, 1000, block.data());
            rendered.insert(rendered.end(), block.begin(), block.end());
        }
        // Silencio antes de 1 s y después de 4 s; el tono, idéntico, en medio.
        double before = 0, after = 0, diff = 0;
        for (size_t i = 0; i < 48000 * 2; ++i)
            before = std::max(before, double(std::fabs(rendered[i])));
        for (size_t i = 0; i < ref.size(); ++i)
            diff = std::max(diff, double(std::fabs(rendered[48000 * 2 + i] - ref[i])));
        for (size_t i = 48000 * 2 + ref.size(); i < rendered.size(); ++i)
            after = std::max(after, double(std::fabs(rendered[i])));
        CHECK(before == 0.0);
        CHECK(after == 0.0);
        CHECK(diff < 1e-6);
        CHECK(mixer.readerFor(clip)->seekCount() == 1);
    }
    SECTION("volumen, suma de clips y pista silenciada")
    {
        edit::placeClip(p, p.audioTracks[1].id, tone, 30); // el mismo tono en A2
        edit::setProperty(p, clip, edit::Property::Volume, 0, 0.5f);
        std::vector<float> block(480 * 2);
        // Empezar a mitad del medio implica una búsqueda; con el pre-roll el resultado coincide
        // con la decodificación secuencial salvo la fase del remuestreo 44,1→48 kHz (< 1e-3).
        mixer.mix(p, 48000 + 4800, 480, block.data()); // 1,1 s
        const size_t offset = 4800 * 2;
        for (size_t i = 0; i < block.size(); ++i)
            CHECK(std::fabs(block[i] - 1.5f * ref[offset + i]) < 1e-3); // 0,5 + 1,0

        // Volver a mezclar la misma posición obliga a buscar hacia atrás: el remuestreador
        // arranca en frío y difiere en ~1e-5 (inaudible).
        edit::setTrackMuted(p, p.audioTracks[1].id, true);
        mixer.mix(p, 48000 + 4800, 480, block.data());
        for (size_t i = 0; i < block.size(); ++i)
            CHECK(std::fabs(block[i] - 0.5f * ref[offset + i]) < 1e-3);
    }
    SECTION("un salto en la línea de tiempo busca exacto")
    {
        std::vector<float> block(480 * 2);
        mixer.mix(p, 48000 * 3, 480, block.data()); // 3 s → 2 s del medio
        const size_t offset = 48000 * 2 * 2;
        double diff = 0;
        for (size_t i = 0; i < block.size(); ++i)
            diff = std::max(diff, double(std::fabs(block[i] - ref[offset + i])));
        CHECK(diff < 1e-3); // con pre-roll no hay transitorio de arranque
    }
}

TEST_CASE("El audio de un clip de video suena desde la pista de video", "[engine][audio]")
{
    Project p = Project::createDefault();
    const Id media = importMedia(p, "sync_720p30.mp4"); // pitidos en cada segundo
    edit::placeClip(p, p.videoTracks[0].id, media, 0);
    engine::AudioMixer mixer(48000, 2);
    std::vector<float> block(4800 * 2);
    mixer.mix(p, 2 * 48000, 4800, block.data()); // 2,0-2,1 s: pitido
    float peak = 0;
    for (float s : block)
        peak = std::max(peak, std::fabs(s));
    CHECK(peak > 0.5f);

    edit::setTrackMuted(p, p.videoTracks[0].id, true);
    mixer.mix(p, 2 * 48000, 4800, block.data());
    peak = 0;
    for (float s : block)
        peak = std::max(peak, std::fabs(s));
    CHECK(peak == 0.0f);
}
