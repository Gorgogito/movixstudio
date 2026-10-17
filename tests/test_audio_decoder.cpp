#include "TestMedia.h"
#include "media/AudioDecoder.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace movix;
using media::AudioChunk;
using media::AudioDecoder;

TEST_CASE("Remuestreo de 44,1 kHz mono a 48 kHz estéreo conserva la duración", "[audio]")
{
    AudioDecoder dec;
    std::string err;
    REQUIRE(dec.open(test::mediaPath("tone_44k_mono.wav"), 48000, 2, &err));

    AudioChunk chunk;
    int64_t total = 0;
    TimeUs expectedPts = 0;
    float peak = 0.0f;
    while (dec.read(chunk)) {
        // Los bloques forman una línea de tiempo continua.
        CHECK(std::llabs(chunk.pts - expectedPts) <= 21); // ≤ 1 muestra
        REQUIRE(chunk.samples.size() == size_t(chunk.frames) * 2);
        for (float s : chunk.samples)
            peak = std::max(peak, std::fabs(s));
        total += chunk.frames;
        expectedPts = chunk.pts + chunk.frames * kUsPerSecond / 48000;
    }
    CHECK(std::llabs(total - 3 * 48000) <= 48); // ±1 ms
    // El generador "sine" de FFmpeg tiene amplitud 1/8; el mono se copia sin atenuar a ambos canales.
    CHECK(std::fabs(peak - 0.125f) < 0.005f);
}

TEST_CASE("Búsqueda de audio exacta a la muestra", "[audio][seek]")
{
    AudioDecoder dec;
    REQUIRE(dec.open(test::mediaPath("tone_44k_mono.wav"), 48000, 2));

    for (double target : {1.0, 2.25, 0.0, 0.5}) {
        INFO("objetivo " << target << " s");
        REQUIRE(dec.seek(fromSeconds(target)));
        AudioChunk chunk;
        REQUIRE(dec.read(chunk));
        CHECK(std::llabs(chunk.pts - fromSeconds(target)) <= 21);
    }

    // Las muestras tras la búsqueda son las mismas que en lectura secuencial.
    AudioDecoder seq;
    REQUIRE(seq.open(test::mediaPath("tone_44k_mono.wav"), 48000, 2));
    std::vector<float> all;
    AudioChunk c;
    while (seq.read(c))
        all.insert(all.end(), c.samples.begin(), c.samples.end());

    REQUIRE(dec.seek(fromSeconds(1.5)));
    REQUIRE(dec.read(c));
    const size_t offset = size_t(1.5 * 48000) * 2;
    double maxDiff = 0.0;
    for (int i = 0; i < 200; ++i)
        maxDiff = std::max(maxDiff, double(std::fabs(c.samples[i] - all[offset + i])));
    CHECK(maxDiff < 0.02); // el remuestreador arranca en frío: diferencia mínima
}

TEST_CASE("Audio de Opus, FLAC y AAC", "[audio]")
{
    for (const char* name : {"vp9_opus.webm", "hevc10_360p.mkv", "sync_720p30.mp4"}) {
        INFO(name);
        AudioDecoder dec;
        REQUIRE(dec.open(test::mediaPath(name), 48000, 2));
        AudioChunk chunk;
        int64_t total = 0;
        while (dec.read(chunk))
            total += chunk.frames;
        CHECK(std::llabs(total - int64_t(toSeconds(dec.duration()) * 48000)) < 4800); // ±100 ms
    }
}
