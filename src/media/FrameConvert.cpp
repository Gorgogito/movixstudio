#include "media/FrameConvert.h"

extern "C" {
#include <libswscale/swscale.h>
}

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

} // namespace movix::media
