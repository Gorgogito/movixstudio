#include "TestMedia.h"
#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace movix;

// El archivo sync_720p30.mp4 tiene un fotograma blanco y el inicio de un pitido en cada segundo exacto.
// Esta prueba comprueba que los timestamps que producen ambos decodificadores están alineados.
TEST_CASE("Destellos de video y pitidos de audio coinciden en el tiempo", "[sync]")
{
    const std::string path = test::mediaPath("sync_720p30.mp4");

    std::vector<TimeUs> flashes;
    media::VideoDecoder video;
    REQUIRE(video.open(path, media::VideoDecoderOptions{false}));
    while (auto f = video.decodeNext()) {
        const double mean = std::accumulate(f->y.begin(), f->y.end(), 0.0) / double(f->y.size());
        if (mean > 128.0)
            flashes.push_back(f->pts);
    }

    std::vector<TimeUs> beeps;
    media::AudioDecoder audio;
    REQUIRE(audio.open(path, 48000, 2));
    media::AudioChunk chunk;
    TimeUs lastLoud = kNoTime;
    while (audio.read(chunk)) {
        for (int i = 0; i < chunk.frames; ++i) {
            if (std::fabs(chunk.samples[size_t(i) * 2]) < 0.2f)
                continue;
            const TimeUs t = chunk.pts + TimeUs(i) * kUsPerSecond / 48000;
            if (lastLoud == kNoTime || t - lastLoud > 300'000) // nuevo pitido tras ≥300 ms de silencio
                beeps.push_back(t);
            lastLoud = t;
        }
    }

    REQUIRE(flashes.size() == 5);
    REQUIRE(beeps.size() == 5);
    for (size_t i = 0; i < 5; ++i) {
        const TimeUs offset = beeps[i] - flashes[i];
        std::printf("[sync] segundo %zu: video %.3f s, audio %.3f s, desfase %+.1f ms\n", i, toSeconds(flashes[i]),
                    toSeconds(beeps[i]), offset / 1000.0);
        CHECK(std::llabs(flashes[i] - fromSeconds(double(i))) <= 1000);
        CHECK(std::llabs(offset) < 5000); // < 5 ms, muy por debajo de un fotograma (33 ms)
    }
}
