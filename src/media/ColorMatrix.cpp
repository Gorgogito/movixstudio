#include "media/ColorMatrix.h"

namespace movix::media {

std::array<float, 16> yuvToRgbMatrix(ColorSpace space, bool fullRange, bool deep)
{
    double kr = 0.2126, kb = 0.0722; // BT.709
    if (space == ColorSpace::BT601) {
        kr = 0.299;
        kb = 0.114;
    } else if (space == ColorSpace::BT2020) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const double kg = 1.0 - kr - kb;

    // Expansión de rango: Y' = (Y - yOff) * yScale ; C' = (C - 128/255) * cScale
    // En 8 bits, los niveles son 16..235 (luma) y 16..240 (croma) sobre 255; en P016, los de 10
    // bits (64..940, 64..960) desplazados 6 bits, sobre 65535.
    const double maxCode = deep ? 65535.0 : 255.0;
    const double unit = deep ? 64.0 * 4.0 : 1.0; // un nivel de 8 bits en códigos del contenedor
    const double yOff = fullRange ? 0.0 : 16.0 * unit / maxCode;
    const double yScale = fullRange ? 1.0 : maxCode / (219.0 * unit);
    const double cScale = fullRange ? 1.0 : maxCode / (224.0 * unit);
    const double cOff = (deep ? 512.0 * 64.0 : 128.0) / maxCode;

    // Coeficientes de R'G'B' a partir de Y' y Cb/Cr centrados en cero.
    const double rCr = 2.0 * (1.0 - kr);
    const double gCb = -2.0 * kb * (1.0 - kb) / kg;
    const double gCr = -2.0 * kr * (1.0 - kr) / kg;
    const double bCb = 2.0 * (1.0 - kb);

    auto row = [&](double cy, double cb, double cr) {
        const double sy = cy * yScale, sb = cb * cScale, sr = cr * cScale;
        const double offset = -(sy * yOff + sb * cOff + sr * cOff);
        return std::array<double, 4>{sy, sb, sr, offset};
    };
    const auto r = row(1.0, 0.0, rCr);
    const auto g = row(1.0, gCb, gCr);
    const auto b = row(1.0, bCb, 0.0);

    std::array<float, 16> m{};
    for (int i = 0; i < 4; ++i) {
        m[0 + i] = static_cast<float>(r[i]);
        m[4 + i] = static_cast<float>(g[i]);
        m[8 + i] = static_cast<float>(b[i]);
    }
    m[15] = 1.0f;
    return m;
}

ColorSpace guessColorSpace(int width, int height)
{
    return (width >= 1280 || height > 576) ? ColorSpace::BT709 : ColorSpace::BT601;
}

} // namespace movix::media
