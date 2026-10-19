#include "color/Hdr.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace movix::color {

namespace {

using Vec = std::array<double, 3>;
using Mat = std::array<double, 9>;

// Primarias lineales (BT.2087).
constexpr Mat k709To2020{0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956};
constexpr Mat k2020To709{1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187};
constexpr Vec kLuma709{0.2126, 0.7152, 0.0722};
constexpr Vec kLuma2020{0.2627, 0.6780, 0.0593};

constexpr double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
constexpr double hA = 0.17883277, hB = 0.28466892, hC = 0.55991073;

Vec mul(const Mat& m, const Vec& v)
{
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

double dot(const Vec& a, const Vec& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double hlgToScene(double e)
{
    return e <= 0.5 ? e * e / 3.0 : (std::exp((e - hC) / hA) + hB) / 12.0;
}

double sceneToHlg(double s)
{
    s = std::max(s, 0.0);
    return s <= 1.0 / 12.0 ? std::sqrt(3.0 * s) : hA * std::log(12.0 * s - hB) + hC;
}

// Señal → luz de pantalla en nits (en las primarias de la señal).
Vec toNits(const Vec& e, Signal sig)
{
    Vec out{};
    switch (sig) {
    case Signal::SDR:
        for (size_t i = 0; i < 3; ++i)
            out[i] = std::pow(std::clamp(e[i], 0.0, 1.0), 2.4) * kSdrWhiteNits;
        break;
    case Signal::PQ:
        for (size_t i = 0; i < 3; ++i)
            out[i] = pqToNits(e[i]);
        break;
    case Signal::HLG: { // OOTF de una pantalla de 1000 nits
        Vec scene{};
        for (size_t i = 0; i < 3; ++i)
            scene[i] = hlgToScene(std::clamp(e[i], 0.0, 1.0));
        const double ys = std::max(dot(scene, kLuma2020), 1e-9);
        for (size_t i = 0; i < 3; ++i)
            out[i] = kHdrPeakNits * std::pow(ys, 0.2) * scene[i];
        break;
    }
    }
    return out;
}

// Luz de pantalla en nits → señal (las primarias ya deben ser las de la señal).
Vec fromNits(const Vec& nits, Signal sig)
{
    Vec out{};
    switch (sig) {
    case Signal::SDR: {
        // Mapeo de tonos sobre la luminancia (conserva el tono) y codificación BT.1886.
        Vec x{nits[0] / kSdrWhiteNits, nits[1] / kSdrWhiteNits, nits[2] / kSdrWhiteNits};
        const double y = std::max(dot(x, kLuma709), 0.0);
        if (y > kToneKnee) {
            const double ym = toneMap(y);
            for (double& v : x)
                v *= ym / y;
        }
        for (size_t i = 0; i < 3; ++i)
            out[i] = std::pow(std::clamp(x[i], 0.0, 1.0), 1.0 / 2.4);
        break;
    }
    case Signal::PQ:
        for (size_t i = 0; i < 3; ++i)
            out[i] = nitsToPq(nits[i]);
        break;
    case Signal::HLG: { // inversa de la OOTF y OETF de HLG
        Vec d{nits[0] / kHdrPeakNits, nits[1] / kHdrPeakNits, nits[2] / kHdrPeakNits};
        const double yd = std::max(dot(d, kLuma2020), 1e-9);
        const double k = std::pow(yd, -0.2 / 1.2);
        for (size_t i = 0; i < 3; ++i)
            out[i] = sceneToHlg(d[i] * k);
        break;
    }
    }
    return out;
}

} // namespace

double toneMap(double y)
{
    // Lineal hasta la rodilla; por encima, Reinhard extendido sobre el exceso, que lleva el pico
    // HDR (1000 nits) al máximo de SDR con pendiente continua.
    if (y <= kToneKnee)
        return y;
    const double peak = kHdrPeakNits / kSdrWhiteNits;
    const double range = (peak - kToneKnee) / (1.0 - kToneKnee);
    const double e = (y - kToneKnee) / (1.0 - kToneKnee);
    return kToneKnee + (1.0 - kToneKnee) * e * (1.0 + e / (range * range)) / (1.0 + e);
}

double pqToNits(double e)
{
    const double p = std::pow(std::clamp(e, 0.0, 1.0), 1.0 / m2);
    return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

double nitsToPq(double nits)
{
    const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
    return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

Signal signalOf(core::ColorMode mode)
{
    return mode == core::ColorMode::PQ ? Signal::PQ : mode == core::ColorMode::HLG ? Signal::HLG : Signal::SDR;
}

bool needsConversion(Signal from, bool from2020, core::ColorMode to)
{
    const Signal target = signalOf(to);
    const bool target2020 = to != core::ColorMode::SDR;
    return from != target || from2020 != target2020;
}

Rgb convertSignal(Rgb in, Signal from, bool from2020, core::ColorMode to)
{
    if (!needsConversion(from, from2020, to))
        return in;
    const bool to2020 = to != core::ColorMode::SDR;
    Vec nits = toNits({in.r, in.g, in.b}, from);
    if (from2020 && !to2020)
        nits = mul(k2020To709, nits);
    else if (!from2020 && to2020)
        nits = mul(k709To2020, nits);
    for (double& v : nits)
        v = std::max(v, 0.0); // fuera de la gama de destino
    const Vec out = fromNits(nits, signalOf(to));
    return {float(out[0]), float(out[1]), float(out[2])};
}

} // namespace movix::color
