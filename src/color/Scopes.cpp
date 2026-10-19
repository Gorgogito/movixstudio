#include "color/Scopes.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace movix::color {

namespace {

int luma8(int r, int g, int b)
{
    return std::clamp(int(std::lround(0.2126 * r + 0.7152 * g + 0.0722 * b)), 0, 255);
}

// Color premultiplicado con intensidad `v` (0..1) de un tinte RGB.
std::uint32_t shade(double v, std::array<int, 3> tint)
{
    v = std::clamp(v, 0.0, 1.0);
    const auto a = std::uint32_t(std::lround(255 * v));
    auto ch = [&](int c) { return std::uint32_t(std::lround(c * v)); };
    return (a << 24) | (ch(tint[0]) << 16) | (ch(tint[1]) << 8) | ch(tint[2]);
}

// Convierte cuentas en intensidad: raíz cuadrada (realza lo poco frecuente) saturando en `full`.
double density(std::uint32_t count, double full)
{
    return count == 0 ? 0.0 : std::min(1.0, std::sqrt(double(count) / full));
}

template <typename F>
void forEachPixel(const std::uint8_t* rgba, int width, int height, int stride, F f)
{
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = rgba + size_t(y) * size_t(stride);
        for (int x = 0; x < width; ++x)
            f(x, y, row[x * 4], row[x * 4 + 1], row[x * 4 + 2]);
    }
}

constexpr std::array<int, 3> kWaveTint{170, 255, 170};
constexpr std::array<std::array<int, 3>, 3> kChannelTint{{{255, 70, 70}, {70, 255, 70}, {90, 140, 255}}};

} // namespace

ScopePoint vectorscopePosition(int r, int g, int b)
{
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double cb = (b - y) / 1.8556 / 255.0; // −0,5..0,5
    const double cr = (r - y) / 1.5748 / 255.0;
    return {0.5 + cb * 0.9, 0.5 - cr * 0.9};
}

ScopeImage computeScope(ScopeType type, const std::uint8_t* rgba, int width, int height, int stride, int outW, int outH)
{
    ScopeImage img;
    img.width = outW;
    img.height = outH;
    img.pixels.assign(size_t(outW) * size_t(outH), 0u);
    if (!rgba || width <= 0 || height <= 0 || outW <= 0 || outH <= 0)
        return img;
    const double total = double(width) * double(height);

    switch (type) {
    case ScopeType::Waveform:
    case ScopeType::Parade: {
        const bool parade = type == ScopeType::Parade;
        const int panes = parade ? 3 : 1;
        const int paneW = std::max(1, outW / panes);
        std::vector<std::uint32_t> counts(size_t(paneW) * size_t(outH) * size_t(panes), 0u);
        auto row = [&](int v) { return (255 - v) * (outH - 1) / 255; };
        forEachPixel(rgba, width, height, stride, [&](int x, int, int r, int g, int b) {
            const int col = std::min(paneW - 1, x * paneW / width);
            if (parade) {
                const int v[3] = {r, g, b};
                for (int p = 0; p < 3; ++p)
                    ++counts[(size_t(p) * size_t(outH) + size_t(row(v[p]))) * size_t(paneW) + size_t(col)];
            } else {
                ++counts[size_t(row(luma8(r, g, b))) * size_t(paneW) + size_t(col)];
            }
        });
        // Una columna de salida reúne width/paneW columnas de la imagen; si se repartieran por
        // igual en toda la altura, cada celda recibiría `spread` muestras.
        const double perColumn = total / paneW;
        const double full = std::max(1.0, 4.0 * perColumn / outH);
        for (int p = 0; p < panes; ++p)
            for (int y = 0; y < outH; ++y)
                for (int x = 0; x < paneW; ++x) {
                    const std::uint32_t n = counts[(size_t(p) * size_t(outH) + size_t(y)) * size_t(paneW) + size_t(x)];
                    if (n)
                        img.pixels[size_t(y) * size_t(outW) + size_t(p * paneW + x)] =
                            shade(density(n, full), parade ? kChannelTint[size_t(p)] : kWaveTint);
                }
        break;
    }
    case ScopeType::Vectorscope: {
        const int side = std::min(outW, outH);
        const int ox = (outW - side) / 2, oy = (outH - side) / 2;
        std::vector<std::uint32_t> counts(size_t(side) * size_t(side), 0u);
        forEachPixel(rgba, width, height, stride, [&](int, int, int r, int g, int b) {
            const ScopePoint pt = vectorscopePosition(r, g, b);
            const int x = std::clamp(int(pt.x * side), 0, side - 1);
            const int y = std::clamp(int(pt.y * side), 0, side - 1);
            ++counts[size_t(y) * size_t(side) + size_t(x)];
        });
        const double full = std::max(1.0, 16.0 * total / (double(side) * side));
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
                if (const std::uint32_t n = counts[size_t(y) * size_t(side) + size_t(x)])
                    img.pixels[size_t(y + oy) * size_t(outW) + size_t(x + ox)] = shade(density(n, full), kWaveTint);
        break;
    }
    case ScopeType::Histogram: {
        std::array<std::array<std::uint32_t, 256>, 3> bins{};
        forEachPixel(rgba, width, height, stride, [&](int, int, int r, int g, int b) {
            ++bins[0][size_t(r)];
            ++bins[1][size_t(g)];
            ++bins[2][size_t(b)];
        });
        std::uint32_t peak = 1;
        for (const auto& ch : bins)
            peak = std::max(peak, *std::max_element(ch.begin(), ch.end()));
        // Escala de raíz cuadrada: los picos (p. ej. negro puro) no aplastan el resto.
        for (int x = 0; x < outW; ++x) {
            const int bin = outW > 1 ? x * 255 / (outW - 1) : 0;
            int heights[3];
            for (int c = 0; c < 3; ++c)
                heights[c] = int(std::lround(std::sqrt(double(bins[size_t(c)][size_t(bin)]) / peak) * outH));
            for (int y = 0; y < outH; ++y) {
                const int fromBottom = outH - 1 - y;
                int r = 0, g = 0, b = 0, layers = 0;
                for (int c = 0; c < 3; ++c) {
                    if (fromBottom < heights[c]) {
                        r += kChannelTint[size_t(c)][0];
                        g += kChannelTint[size_t(c)][1];
                        b += kChannelTint[size_t(c)][2];
                        ++layers;
                    }
                }
                if (layers)
                    img.pixels[size_t(y) * size_t(outW) + size_t(x)] =
                        shade(0.85, {std::min(255, r), std::min(255, g), std::min(255, b)});
            }
        }
        break;
    }
    }
    return img;
}

} // namespace movix::color
