#include "media/ColorMatrix.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>

using namespace movix::media;
using Catch::Matchers::WithinAbs;

namespace {

std::array<float, 3> apply(const std::array<float, 16>& m, int y, int cb, int cr)
{
    const float v[4] = {y / 255.0f, cb / 255.0f, cr / 255.0f, 1.0f};
    std::array<float, 3> out{};
    for (int r = 0; r < 3; ++r)
        out[r] = m[r * 4 + 0] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * v[3];
    return out;
}

} // namespace

TEST_CASE("Rango limitado: negro y blanco de referencia", "[color]")
{
    for (ColorSpace cs : {ColorSpace::BT601, ColorSpace::BT709, ColorSpace::BT2020}) {
        const auto m = yuvToRgbMatrix(cs, false);
        const auto black = apply(m, 16, 128, 128);
        const auto white = apply(m, 235, 128, 128);
        for (int c = 0; c < 3; ++c) {
            CHECK_THAT(black[c], WithinAbs(0.0, 0.003));
            CHECK_THAT(white[c], WithinAbs(1.0, 0.003));
        }
    }
}

TEST_CASE("Rango completo: extremos 0 y 255", "[color]")
{
    const auto m = yuvToRgbMatrix(ColorSpace::BT709, true);
    const auto black = apply(m, 0, 128, 128);
    const auto white = apply(m, 255, 128, 128);
    for (int c = 0; c < 3; ++c) {
        CHECK_THAT(black[c], WithinAbs(0.0, 0.003));
        CHECK_THAT(white[c], WithinAbs(1.0, 0.003));
    }
}

TEST_CASE("BT.709 limitado: rojo puro de las barras de color", "[color]")
{
    // Rojo 100 % en BT.709 de 8 bits y rango limitado: Y=63, Cb=102, Cr=240.
    const auto rgb = apply(yuvToRgbMatrix(ColorSpace::BT709, false), 63, 102, 240);
    CHECK_THAT(rgb[0], WithinAbs(1.0, 0.01));
    CHECK_THAT(rgb[1], WithinAbs(0.0, 0.01));
    CHECK_THAT(rgb[2], WithinAbs(0.0, 0.01));
}

TEST_CASE("BT.601 limitado: azul puro", "[color]")
{
    // Azul 100 % en BT.601: Y=41, Cb=240, Cr=110.
    const auto rgb = apply(yuvToRgbMatrix(ColorSpace::BT601, false), 41, 240, 110);
    CHECK_THAT(rgb[0], WithinAbs(0.0, 0.01));
    CHECK_THAT(rgb[1], WithinAbs(0.0, 0.01));
    CHECK_THAT(rgb[2], WithinAbs(1.0, 0.01));
}
