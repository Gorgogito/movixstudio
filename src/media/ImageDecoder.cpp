#include "media/ImageDecoder.h"

#include "media/FFmpegUtils.h"

extern "C" {
#include <libswscale/swscale.h>
}

namespace movix::media {

std::shared_ptr<VideoFrame> decodeImageRgba(const std::string& path, std::string* error)
{
    auto fail = [&](const std::string& msg) -> std::shared_ptr<VideoFrame> {
        if (error)
            *error = msg;
        return nullptr;
    };
    FormatContextPtr fmt = openInput(path, error);
    if (!fmt)
        return nullptr;
    const int index = findVideoStream(fmt.get());
    if (index < 0)
        return fail("El archivo no contiene una imagen.");
    const AVCodecParameters* par = fmt->streams[index]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
        return fail("Formato de imagen no compatible.");
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), par);
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
        return fail("No se pudo abrir el decodificador de imagen.");

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    bool got = false;
    while (!got && av_read_frame(fmt.get(), packet.get()) >= 0) {
        if (packet->stream_index == index && avcodec_send_packet(ctx.get(), packet.get()) >= 0)
            got = avcodec_receive_frame(ctx.get(), frame.get()) == 0;
        av_packet_unref(packet.get());
    }
    if (!got) {
        avcodec_send_packet(ctx.get(), nullptr);
        got = avcodec_receive_frame(ctx.get(), frame.get()) == 0;
    }
    if (!got)
        return fail("No se pudo decodificar la imagen.");

    auto out = std::make_shared<VideoFrame>();
    out->format = PixelFormat::RGBA;
    out->width = frame->width;
    out->height = frame->height;
    const AVRational sar = frame->sample_aspect_ratio;
    out->sampleAspect = sar.num && sar.den ? av_q2d(sar) : 1.0;
    out->rgba.resize(size_t(out->width) * size_t(out->height) * 4);
    SwsContext* sws = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), frame->width, frame->height,
                                     AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws)
        return fail("No se pudo convertir la imagen.");
    uint8_t* dst[4] = {out->rgba.data(), nullptr, nullptr, nullptr};
    const int dstStride[4] = {out->width * 4, 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
    sws_freeContext(sws);
    return out;
}

} // namespace movix::media
