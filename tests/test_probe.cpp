#include "TestMedia.h"
#include "media/MediaInfo.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace movix;
using Catch::Matchers::WithinAbs;

TEST_CASE("Sondeo de un MP4 con video y audio", "[probe]")
{
    std::string err;
    auto info = media::probe(test::mediaPath("sync_720p30.mp4"), &err);
    REQUIRE(info);
    REQUIRE(info->video);
    REQUIRE(info->audio);
    CHECK(info->video->codec == "h264");
    CHECK(info->video->width == 1280);
    CHECK(info->video->height == 720);
    CHECK_THAT(info->video->frameRate, WithinAbs(30.0, 0.01));
    CHECK(info->audio->codec == "aac");
    CHECK(info->audio->sampleRate == 48000);
    CHECK(info->audio->channels == 2);
    CHECK_THAT(toSeconds(info->duration), WithinAbs(5.0, 0.05));
}

TEST_CASE("Sondeo de HEVC 10 bits y de audio sin video", "[probe]")
{
    auto hevc = media::probe(test::mediaPath("hevc10_360p.mkv"));
    REQUIRE(hevc);
    REQUIRE(hevc->video);
    CHECK(hevc->video->codec == "hevc");
    CHECK(hevc->video->pixelFormat == "yuv420p10le");
    REQUIRE(hevc->audio);
    CHECK(hevc->audio->sampleRate == 44100);

    auto wav = media::probe(test::mediaPath("tone_44k_mono.wav"));
    REQUIRE(wav);
    CHECK_FALSE(wav->video);
    REQUIRE(wav->audio);
    CHECK(wav->audio->channels == 1);
}

TEST_CASE("Sondeo de un archivo inexistente informa el error", "[probe]")
{
    std::string err;
    auto info = media::probe(test::mediaPath("no_existe.mp4"), &err);
    CHECK_FALSE(info);
    CHECK_FALSE(err.empty());
}
