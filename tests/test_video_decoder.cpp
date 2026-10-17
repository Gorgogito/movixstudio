#include "TestMedia.h"
#include "media/VideoDecoder.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace movix;
using media::VideoDecoder;
using media::VideoDecoderOptions;

namespace {

const VideoDecoderOptions kSoftware{false};
const VideoDecoderOptions kHardware{true};

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    REQUIRE(a.size() == b.size());
    double mse = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = double(a[i]) - double(b[i]);
        mse += d * d;
    }
    mse /= double(a.size());
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

TimeUs expectedPts(int index, double fps)
{
    return fromSeconds(index / fps);
}

} // namespace

TEST_CASE("Decodificación secuencial: 180 fotogramas con timestamps exactos", "[video]")
{
    VideoDecoder dec;
    std::string err;
    REQUIRE(dec.open(test::mediaPath("count_1080p60.mp4"), kSoftware, &err));
    CHECK(dec.width() == 1920);
    CHECK(dec.height() == 1080);

    int count = 0;
    while (auto f = dec.decodeNext()) {
        INFO("fotograma " << count);
        CHECK(std::llabs(f->pts - expectedPts(count, 60.0)) <= 1000); // ±1 ms
        CHECK(f->y.size() == size_t(1920) * 1080);
        CHECK(f->uv.size() == size_t(1920) * 540);
        ++count;
    }
    CHECK(count == 180);
}

TEST_CASE("Búsqueda precisa al fotograma", "[video][seek]")
{
    VideoDecoder seq;
    REQUIRE(seq.open(test::mediaPath("count_1080p60.mp4"), kSoftware));
    std::vector<std::shared_ptr<media::VideoFrame>> frames;
    while (auto f = seq.decodeNext())
        frames.push_back(f);
    REQUIRE(frames.size() == 180);

    VideoDecoder dec;
    REQUIRE(dec.open(test::mediaPath("count_1080p60.mp4"), kSoftware));

    SECTION("a mitad de un GOP (1,5 s → fotograma 90)")
    {
        REQUIRE(dec.seek(fromSeconds(1.5)));
        auto f = dec.decodeNext();
        REQUIRE(f);
        CHECK(std::llabs(f->pts - frames[90]->pts) <= 1000);
        CHECK(f->y == frames[90]->y);
        // Tras la búsqueda la decodificación continúa en orden.
        auto next = dec.decodeNext();
        REQUIRE(next);
        CHECK(next->y == frames[91]->y);
    }
    SECTION("entre dos fotogramas devuelve el que se está mostrando")
    {
        REQUIRE(dec.seek(expectedPts(100, 60.0) + 5000)); // 5 ms después del fotograma 100
        auto f = dec.decodeNext();
        REQUIRE(f);
        CHECK(f->y == frames[100]->y);
    }
    SECTION("hacia atrás después de llegar al final")
    {
        while (dec.decodeNext()) {
        }
        REQUIRE(dec.seek(fromSeconds(0.5)));
        auto f = dec.decodeNext();
        REQUIRE(f);
        CHECK(f->y == frames[30]->y);
    }
    SECTION("más allá del final devuelve el último fotograma")
    {
        REQUIRE(dec.seek(fromSeconds(10.0)));
        auto f = dec.decodeNext();
        REQUIRE(f);
        CHECK(f->y == frames.back()->y);
        CHECK_FALSE(dec.decodeNext());
    }
}

TEST_CASE("La decodificación por GPU coincide con la de CPU", "[video][hw]")
{
    VideoDecoder hw;
    REQUIRE(hw.open(test::mediaPath("count_1080p60.mp4"), kHardware));
    auto first = hw.decodeNext();
    REQUIRE(first);
    if (!hw.isHardwareAccelerated())
        SKIP("D3D11VA no disponible en este equipo");

    VideoDecoder sw;
    REQUIRE(sw.open(test::mediaPath("count_1080p60.mp4"), kSoftware));
    REQUIRE(hw.seek(fromSeconds(1.0)));
    REQUIRE(sw.seek(fromSeconds(1.0)));
    auto a = hw.decodeNext();
    auto b = sw.decodeNext();
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->pts == b->pts);
    CHECK(psnr(a->y, b->y) > 45.0);
    CHECK(psnr(a->uv, b->uv) > 45.0);
}

TEST_CASE("HEVC de 10 bits y VP9 se convierten a NV12", "[video]")
{
    for (const char* name : {"hevc10_360p.mkv", "vp9_opus.webm"}) {
        INFO(name);
        for (const auto& opts : {kSoftware, kHardware}) {
            VideoDecoder dec;
            REQUIRE(dec.open(test::mediaPath(name), opts));
            int count = 0;
            while (auto f = dec.decodeNext()) {
                CHECK(f->width == 640);
                CHECK(f->height == 360);
                ++count;
            }
            CHECK(count >= 48); // 2 s a 25/30 fps
        }
    }
}

TEST_CASE("Rendimiento: 1080p60 se decodifica más rápido que el tiempo real", "[video][perf]")
{
    for (const auto& opts : {kSoftware, kHardware}) {
        VideoDecoder dec;
        REQUIRE(dec.open(test::mediaPath("count_1080p60.mp4"), opts));
        const auto t0 = std::chrono::steady_clock::now();
        int count = 0;
        while (dec.decodeNext())
            ++count;
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double fps = count / secs;
        std::printf("[perf] %-28s %6.0f fps\n", dec.decoderDescription().c_str(), fps);
        CHECK(fps > 60.0);
    }
}
