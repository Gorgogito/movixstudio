#include "media/FrameConvert.h"

#include "media/ColorMatrix.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <QFloat16>

#include <algorithm>
#include <cstring>

namespace movix::media {

bool frameToRgba(const VideoFrame& frame, int outWidth, int outHeight, std::vector<uint8_t>& rgba)
{
    if (frame.width <= 0 || frame.height <= 0 || outWidth <= 0 || outHeight <= 0)
        return false;
    const bool rgbaIn = frame.format == PixelFormat::RGBA;
    const bool deep = frame.format == PixelFormat::P016;
    const AVPixelFormat inFormat = rgbaIn ? AV_PIX_FMT_RGBA : deep ? AV_PIX_FMT_P016LE : AV_PIX_FMT_NV12;
    SwsContext* sws = sws_getContext(frame.width, frame.height, inFormat, outWidth, outHeight, AV_PIX_FMT_RGBA, SWS_AREA,
                                     nullptr, nullptr, nullptr);
    if (!sws)
        return false;
    if (!rgbaIn) {
        const int cs = frame.colorSpace == ColorSpace::BT601 ? SWS_CS_ITU601
                     : frame.colorSpace == ColorSpace::BT2020 ? SWS_CS_BT2020
                                                              : SWS_CS_ITU709;
        const int* coeffs = sws_getCoefficients(cs);
        sws_setColorspaceDetails(sws, coeffs, frame.fullRange ? 1 : 0, coeffs, 1, 0, 1 << 16, 1 << 16);
    }

    rgba.resize(size_t(outWidth) * size_t(outHeight) * 4);
    const int sample = deep ? 2 : 1;
    const uint8_t* src[4] = {rgbaIn ? frame.rgba.data() : frame.y.data(), rgbaIn ? nullptr : frame.uv.data(), nullptr, nullptr};
    const int srcStride[4] = {rgbaIn ? frame.width * 4 : frame.width * sample, rgbaIn ? 0 : frame.chromaWidth() * 2 * sample, 0, 0};
    uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
    const int dstStride[4] = {outWidth * 4, 0, 0, 0};
    sws_scale(sws, src, srcStride, 0, frame.height, dst, dstStride);
    sws_freeContext(sws);
    return true;
}

bool frameToRgbaF(const VideoFrame& frame, std::vector<float>& rgba)
{
    const int w = frame.width, h = frame.height;
    if (w <= 0 || h <= 0 || frame.onGpu())
        return false;
    const size_t values = size_t(w) * size_t(h) * 4;
    rgba.resize(values);
    if (frame.format == PixelFormat::RGBA16F) {
        for (size_t i = 0; i < values; ++i)
            rgba[i] = halfAt(frame.rgba, i);
        return true;
    }
    if (frame.format == PixelFormat::RGBA) {
        for (size_t i = 0; i < values; ++i)
            rgba[i] = frame.rgba[i] / 255.0f;
        return true;
    }
    // Con la misma matriz que el compositor en la GPU: un efecto que no cambia nada deja la imagen
    // igual que sin él. Croma del píxel 2×2 que le corresponde.
    const bool deep = frame.format == PixelFormat::P016;
    const auto m = yuvToRgbMatrix(frame.colorSpace, frame.fullRange, deep);
    const int cw = frame.chromaWidth();
    auto sample = [&](const std::vector<uint8_t>& plane, size_t i) {
        return deep ? float(plane[i * 2] | (plane[i * 2 + 1] << 8)) / 65535.0f : float(plane[i]) / 255.0f;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float yy = sample(frame.y, size_t(y) * size_t(w) + size_t(x));
            const size_t ci = (size_t(y / 2) * size_t(cw) + size_t(x / 2)) * 2;
            const float cb = sample(frame.uv, ci), cr = sample(frame.uv, ci + 1);
            float* out = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            for (size_t c = 0; c < 3; ++c)
                out[c] = std::clamp(m[c * 4] * yy + m[c * 4 + 1] * cb + m[c * 4 + 2] * cr + m[c * 4 + 3], 0.0f, 1.0f);
            out[3] = 1.0f;
        }
    }
    return true;
}

void floatToHalf(const std::vector<float>& rgba, std::vector<uint8_t>& half)
{
    half.resize(rgba.size() * sizeof(qfloat16));
    qFloatToFloat16(reinterpret_cast<qfloat16*>(half.data()), rgba.data(), qsizetype(rgba.size()));
}

float halfAt(const std::vector<uint8_t>& half, size_t index)
{
    qfloat16 v;
    std::memcpy(&v, half.data() + index * sizeof(qfloat16), sizeof v);
    return float(v);
}

} // namespace movix::media
