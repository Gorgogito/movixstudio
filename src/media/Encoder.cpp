#include "media/Encoder.h"

#include "media/FFmpegUtils.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavutil/mastering_display_metadata.h>
#include <libswscale/swscale.h>
}

namespace movix::media {

namespace {

bool isHardware(const std::string& codec)
{
    return codec.find("_nvenc") != std::string::npos || codec.find("_amf") != std::string::npos
        || codec.find("_qsv") != std::string::npos;
}

bool startsWith(const std::string& s, const char* prefix)
{
    return s.rfind(prefix, 0) == 0;
}

template <typename T>
std::vector<T> supported(const AVCodecContext* ctx, const AVCodec* codec, AVCodecConfig config, T terminator)
{
    const void* list = nullptr;
    int count = 0;
    std::vector<T> out;
    if (avcodec_get_supported_config(ctx, codec, config, 0, &list, &count) >= 0 && list) {
        const T* items = static_cast<const T*>(list);
        for (int i = 0; i < count && items[i] != terminator; ++i)
            out.push_back(items[i]);
    }
    return out; // vacío = el códec no restringe
}

AVPixelFormat choosePixelFormat(const AVCodecContext* ctx, const AVCodec* codec, bool tenBit)
{
    const auto formats = supported<AVPixelFormat>(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, AV_PIX_FMT_NONE);
    if (tenBit) {
        for (AVPixelFormat preferred : {AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV422P10LE})
            if (std::find(formats.begin(), formats.end(), preferred) != formats.end())
                return preferred;
        return AV_PIX_FMT_NONE; // el códec no admite 10 bits
    }
    if (formats.empty())
        return AV_PIX_FMT_YUV420P;
    for (AVPixelFormat preferred : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12})
        if (std::find(formats.begin(), formats.end(), preferred) != formats.end())
            return preferred;
    return formats.front(); // p. ej. yuv422p10le para ProRes
}

AVSampleFormat chooseSampleFormat(const AVCodecContext* ctx, const AVCodec* codec)
{
    const auto formats = supported<AVSampleFormat>(ctx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, AV_SAMPLE_FMT_NONE);
    if (formats.empty())
        return AV_SAMPLE_FMT_FLTP;
    for (AVSampleFormat preferred : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_S16})
        if (std::find(formats.begin(), formats.end(), preferred) != formats.end())
            return preferred;
    return formats.front();
}

// Opciones de calidad y velocidad de cada familia de codificadores.
void configureVideo(AVCodecContext* ctx, const VideoEncoderSettings& s, AVDictionary** opts)
{
    const std::string& c = s.codec;
    const std::string q = std::to_string(s.quality);
    auto set = [&](const char* key, const std::string& value) { av_dict_set(opts, key, value.c_str(), 0); };
    auto preset = [&](const char* fallback) { set("preset", s.preset.empty() ? fallback : s.preset); };

    if (s.bitRate > 0) {
        ctx->bit_rate = s.bitRate;
        ctx->rc_max_rate = s.bitRate * 3 / 2;
        ctx->rc_buffer_size = int(std::min<int64_t>(s.bitRate * 2, INT32_MAX));
    }

    if (c == "libx264" || c == "libx265") {
        preset("medium");
        if (s.bitRate <= 0)
            set("crf", q);
        if (c == "libx265")
            set("x265-params", "log-level=error");
    } else if (c == "libsvtav1") {
        preset("8");
        if (s.bitRate <= 0)
            set("crf", q);
    } else if (startsWith(c, "libvpx")) {
        set("deadline", "good");
        set("cpu-used", "4");
        set("row-mt", "1");
        if (s.bitRate <= 0) {
            set("crf", q);
            ctx->bit_rate = 0;
        }
    } else if (c.find("_nvenc") != std::string::npos) {
        preset("p5");
        set("rc", "vbr");
        if (s.bitRate <= 0) {
            set("cq", q);
            ctx->bit_rate = 0;
        }
    } else if (c.find("_amf") != std::string::npos) {
        set("quality", s.preset.empty() ? "balanced" : s.preset);
        if (s.bitRate <= 0) {
            set("rc", "cqp");
            set("qp_i", q);
            set("qp_p", q);
            set("qp_b", q);
        } else {
            set("rc", "vbr_peak");
        }
    } else if (c.find("_qsv") != std::string::npos) {
        preset("medium");
        if (s.bitRate <= 0)
            ctx->global_quality = s.quality;
    } else if (c == "prores_ks") {
        set("profile", "hq");
    }
}

} // namespace

struct Encoder::Impl {
    EncoderSettings settings;
    std::string error;
    AVFormatContext* fmt = nullptr; // contexto de salida (se libera en close())
    bool headerWritten = false;
    PacketPtr packet{av_packet_alloc()};

    CodecContextPtr vctx;
    AVStream* vstream = nullptr;
    FramePtr vframe;
    SwsContext* sws = nullptr;
    int64_t vpts = 0;

    CodecContextPtr actx;
    AVStream* astream = nullptr;
    FramePtr aframe;
    SwrContext* swr = nullptr;
    int aframeSize = 1024;
    bool aSmallLast = false;
    std::vector<float> afifo;
    int64_t apts = 0;

    ~Impl() { close(); }

    void close()
    {
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        swr_free(&swr);
        vctx.reset();
        actx.reset();
        vframe.reset();
        aframe.reset();
        if (fmt) {
            if (!(fmt->oformat->flags & AVFMT_NOFILE))
                avio_closep(&fmt->pb);
            avformat_free_context(fmt);
            fmt = nullptr;
        }
        headerWritten = false;
    }

    bool fail(const std::string& msg)
    {
        error = msg;
        return false;
    }

    bool drain(AVCodecContext* ctx, AVStream* st)
    {
        for (;;) {
            int ret = avcodec_receive_packet(ctx, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return true;
            if (ret < 0)
                return fail("Error del codificador: " + ffmpegError(ret));
            av_packet_rescale_ts(packet.get(), ctx->time_base, st->time_base);
            packet->stream_index = st->index;
            ret = av_interleaved_write_frame(fmt, packet.get());
            if (ret < 0)
                return fail("No se pudo escribir en el archivo: " + ffmpegError(ret));
        }
    }

    bool openVideo()
    {
        const VideoEncoderSettings& s = settings.video;
        const AVCodec* codec = avcodec_find_encoder_by_name(s.codec.c_str());
        if (!codec)
            return fail("Codificador de video no disponible: " + s.codec);
        vctx.reset(avcodec_alloc_context3(codec));
        vctx->width = s.width;
        vctx->height = s.height;
        vctx->time_base = AVRational{s.fpsDen, s.fpsNum};
        vctx->framerate = AVRational{s.fpsNum, s.fpsDen};
        vctx->sample_aspect_ratio = AVRational{1, 1};
        vctx->pix_fmt = choosePixelFormat(vctx.get(), codec, s.tenBit);
        if (vctx->pix_fmt == AV_PIX_FMT_NONE)
            return fail("El códec " + s.codec + " no admite 10 bits; para HDR usa HEVC, AV1, VP9 o ProRes.");
        vctx->gop_size = s.gop > 0 ? s.gop : std::max(1, 2 * s.fpsNum / std::max(1, s.fpsDen)); // keyframe cada ~2 s
        const bool hdr = s.signal != VideoEncoderSettings::Signal::SDR;
        const AVColorPrimaries primaries = hdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
        const AVColorTransferCharacteristic trc = s.signal == VideoEncoderSettings::Signal::PQ  ? AVCOL_TRC_SMPTE2084
                                                  : s.signal == VideoEncoderSettings::Signal::HLG ? AVCOL_TRC_ARIB_STD_B67
                                                                                                  : AVCOL_TRC_BT709;
        const AVColorSpace matrix = hdr ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
        vctx->color_range = AVCOL_RANGE_MPEG;
        vctx->color_primaries = primaries;
        vctx->color_trc = trc;
        vctx->colorspace = matrix;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
            vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVDictionary* opts = nullptr;
        configureVideo(vctx.get(), s, &opts);
        int ret = avcodec_open2(vctx.get(), codec, &opts);
        av_dict_free(&opts);
        if (ret < 0)
            return fail("No se pudo iniciar el codificador " + s.codec + ": " + ffmpegError(ret));

        vstream = avformat_new_stream(fmt, nullptr);
        avcodec_parameters_from_context(vstream->codecpar, vctx.get());
        if (s.signal == VideoEncoderSettings::Signal::PQ) {
            // Metadatos HDR10 del contenedor: pantalla de referencia P3-D65 de 1000 nits.
            AVCodecParameters* par = vstream->codecpar;
            if (AVPacketSideData* sd = av_packet_side_data_new(&par->coded_side_data, &par->nb_coded_side_data,
                                                               AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
                                                               sizeof(AVMasteringDisplayMetadata), 0)) {
                auto* m = reinterpret_cast<AVMasteringDisplayMetadata*>(sd->data);
                *m = {};
                const int primariesP3[3][2] = {{34000, 16000}, {13250, 34500}, {7500, 3000}}; // R, G, B en 1/50000
                for (int i = 0; i < 3; ++i) {
                    m->display_primaries[i][0] = av_make_q(primariesP3[i][0], 50000);
                    m->display_primaries[i][1] = av_make_q(primariesP3[i][1], 50000);
                }
                m->white_point[0] = av_make_q(15635, 50000);
                m->white_point[1] = av_make_q(16450, 50000);
                m->max_luminance = av_make_q(1000, 1);
                m->min_luminance = av_make_q(1, 10000);
                m->has_primaries = m->has_luminance = 1;
            }
            if (AVPacketSideData* sd = av_packet_side_data_new(&par->coded_side_data, &par->nb_coded_side_data,
                                                               AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sizeof(AVContentLightMetadata), 0)) {
                auto* cll = reinterpret_cast<AVContentLightMetadata*>(sd->data);
                cll->MaxCLL = 1000;
                cll->MaxFALL = 400;
            }
        }
        vstream->time_base = vctx->time_base;
        vstream->avg_frame_rate = vctx->framerate;

        vframe.reset(av_frame_alloc());
        vframe->format = vctx->pix_fmt;
        vframe->width = s.width;
        vframe->height = s.height;
        vframe->color_range = AVCOL_RANGE_MPEG;
        vframe->colorspace = matrix;
        vframe->color_primaries = primaries;
        vframe->color_trc = trc;
        if (av_frame_get_buffer(vframe.get(), 0) < 0)
            return fail("Sin memoria para el fotograma de salida.");

        sws = sws_getContext(s.width, s.height, s.input16 ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA, s.width, s.height, vctx->pix_fmt,
                             SWS_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
        if (!sws)
            return fail("No se pudo preparar la conversión de color de salida.");
        // RGB de rango completo → YUV de rango limitado (matriz BT.709, o BT.2020 en HDR).
        const int* coeffs = sws_getCoefficients(hdr ? SWS_CS_BT2020 : SWS_CS_ITU709);
        sws_setColorspaceDetails(sws, coeffs, 1, coeffs, 0, 0, 1 << 16, 1 << 16);
        return true;
    }

    bool openAudio()
    {
        const AudioEncoderSettings& s = settings.audio;
        const AVCodec* codec = avcodec_find_encoder_by_name(s.codec.c_str());
        if (!codec)
            return fail("Codificador de audio no disponible: " + s.codec);
        actx.reset(avcodec_alloc_context3(codec));
        actx->sample_fmt = chooseSampleFormat(actx.get(), codec);
        actx->sample_rate = s.sampleRate;
        av_channel_layout_default(&actx->ch_layout, s.channels);
        actx->bit_rate = s.bitRate;
        actx->time_base = AVRational{1, s.sampleRate};
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
            actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        int ret = avcodec_open2(actx.get(), codec, nullptr);
        if (ret < 0)
            return fail("No se pudo iniciar el codificador " + s.codec + ": " + ffmpegError(ret));

        astream = avformat_new_stream(fmt, nullptr);
        avcodec_parameters_from_context(astream->codecpar, actx.get());
        astream->time_base = actx->time_base;

        aframeSize = actx->frame_size > 0 ? actx->frame_size : 1024;
        aSmallLast = (codec->capabilities & (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) != 0;

        AVChannelLayout layout{};
        av_channel_layout_default(&layout, s.channels);
        ret = swr_alloc_set_opts2(&swr, &actx->ch_layout, actx->sample_fmt, s.sampleRate, &layout, AV_SAMPLE_FMT_FLT,
                                  s.sampleRate, 0, nullptr);
        av_channel_layout_uninit(&layout);
        if (ret < 0 || swr_init(swr) < 0)
            return fail("No se pudo preparar la conversión de audio de salida.");
        aframe.reset(av_frame_alloc());
        return true;
    }

    bool encodeAudioFrame(const float* data, int frames)
    {
        av_frame_unref(aframe.get());
        aframe->format = actx->sample_fmt;
        aframe->sample_rate = actx->sample_rate;
        aframe->nb_samples = frames;
        av_channel_layout_copy(&aframe->ch_layout, &actx->ch_layout);
        if (av_frame_get_buffer(aframe.get(), 0) < 0)
            return fail("Sin memoria para el audio de salida.");
        const auto* in = reinterpret_cast<const uint8_t*>(data);
        if (swr_convert(swr, aframe->data, frames, &in, frames) < 0)
            return fail("Error al convertir el audio de salida.");
        aframe->pts = apts;
        apts += frames;
        int ret = avcodec_send_frame(actx.get(), aframe.get());
        if (ret < 0)
            return fail("Error del codificador de audio: " + ffmpegError(ret));
        return drain(actx.get(), astream);
    }
};

Encoder::Encoder() : d(std::make_unique<Impl>()) {}

Encoder::~Encoder() = default;

bool Encoder::open(const EncoderSettings& settings, std::string* error)
{
    d->close();
    d->settings = settings;
    d->vpts = d->apts = 0;
    d->afifo.clear();

    auto fail = [&]() {
        if (error)
            *error = d->error;
        abort();
        return false;
    };

    int ret = avformat_alloc_output_context2(&d->fmt, nullptr, settings.container.empty() ? nullptr : settings.container.c_str(),
                                             settings.path.c_str());
    if (ret < 0 || !d->fmt) {
        d->error = "Formato de salida no reconocido.";
        return fail();
    }
    if ((settings.hasVideo && !d->openVideo()) || (settings.hasAudio && !d->openAudio()))
        return fail();

    if (!(d->fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&d->fmt->pb, settings.path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            d->error = "No se pudo crear el archivo: " + ffmpegError(ret);
            return fail();
        }
    }
    AVDictionary* muxOpts = nullptr;
    const std::string name = d->fmt->oformat->name;
    if (name.find("mp4") != std::string::npos || name.find("mov") != std::string::npos)
        av_dict_set(&muxOpts, "movflags", "+faststart", 0); // reproducible antes de descargarse completo
    ret = avformat_write_header(d->fmt, &muxOpts);
    av_dict_free(&muxOpts);
    if (ret < 0) {
        d->error = "El contenedor no admite esta combinación de códecs: " + ffmpegError(ret);
        return fail();
    }
    d->headerWritten = true;
    return true;
}

bool Encoder::writeVideo(const uint8_t* rgba, int stride)
{
    if (!d->vctx)
        return false;
    if (av_frame_make_writable(d->vframe.get()) < 0)
        return d->fail("Sin memoria para el fotograma de salida.");
    const uint8_t* src[1] = {rgba};
    const int srcStride[1] = {stride};
    sws_scale(d->sws, src, srcStride, 0, d->settings.video.height, d->vframe->data, d->vframe->linesize);
    d->vframe->pts = d->vpts++;
    int ret = avcodec_send_frame(d->vctx.get(), d->vframe.get());
    if (ret < 0)
        return d->fail("Error del codificador de video: " + ffmpegError(ret));
    return d->drain(d->vctx.get(), d->vstream);
}

bool Encoder::acceptsNv12() const
{
    return d->vctx && !d->settings.video.input16 && !d->settings.video.tenBit
        && (d->vctx->pix_fmt == AV_PIX_FMT_NV12 || d->vctx->pix_fmt == AV_PIX_FMT_YUV420P)
        && d->settings.video.signal == VideoEncoderSettings::Signal::SDR;
}

bool Encoder::writeVideoNv12(const uint8_t* nv12)
{
    if (!d->vctx || !acceptsNv12())
        return false;
    if (av_frame_make_writable(d->vframe.get()) < 0)
        return d->fail("Sin memoria para el fotograma de salida.");
    const int w = d->settings.video.width, h = d->settings.video.height;
    AVFrame* f = d->vframe.get();
    for (int y = 0; y < h; ++y)
        std::memcpy(f->data[0] + ptrdiff_t(y) * f->linesize[0], nv12 + size_t(y) * size_t(w), size_t(w));
    const uint8_t* uv = nv12 + size_t(w) * size_t(h);
    if (d->vctx->pix_fmt == AV_PIX_FMT_NV12) {
        for (int y = 0; y < h / 2; ++y)
            std::memcpy(f->data[1] + ptrdiff_t(y) * f->linesize[1], uv + size_t(y) * size_t(w), size_t(w));
    } else { // YUV420P: se separan U y V
        for (int y = 0; y < h / 2; ++y) {
            const uint8_t* row = uv + size_t(y) * size_t(w);
            uint8_t* u = f->data[1] + ptrdiff_t(y) * f->linesize[1];
            uint8_t* v = f->data[2] + ptrdiff_t(y) * f->linesize[2];
            for (int x = 0; x < w / 2; ++x) {
                u[x] = row[2 * x];
                v[x] = row[2 * x + 1];
            }
        }
    }
    f->pts = d->vpts++;
    const int ret = avcodec_send_frame(d->vctx.get(), f);
    if (ret < 0)
        return d->fail("Error del codificador de video: " + ffmpegError(ret));
    return d->drain(d->vctx.get(), d->vstream);
}

bool Encoder::writeAudio(const float* samples, int frames)
{
    if (!d->actx)
        return false;
    const int ch = d->settings.audio.channels;
    d->afifo.insert(d->afifo.end(), samples, samples + size_t(frames) * size_t(ch));
    const size_t block = size_t(d->aframeSize) * size_t(ch);
    size_t offset = 0;
    while (d->afifo.size() - offset >= block) {
        if (!d->encodeAudioFrame(d->afifo.data() + offset, d->aframeSize))
            return false;
        offset += block;
    }
    d->afifo.erase(d->afifo.begin(), d->afifo.begin() + std::ptrdiff_t(offset));
    return true;
}

bool Encoder::finish(std::string* error)
{
    auto fail = [&]() {
        if (error)
            *error = d->error;
        return false;
    };
    if (!d->headerWritten)
        return fail();

    if (d->actx) {
        const int ch = d->settings.audio.channels;
        int remaining = int(d->afifo.size() / size_t(ch));
        if (remaining > 0) {
            if (!d->aSmallLast) { // completar el último bloque con silencio
                d->afifo.resize(size_t(d->aframeSize) * size_t(ch), 0.0f);
                remaining = d->aframeSize;
            }
            if (!d->encodeAudioFrame(d->afifo.data(), remaining))
                return fail();
        }
        d->afifo.clear();
        avcodec_send_frame(d->actx.get(), nullptr);
        if (!d->drain(d->actx.get(), d->astream))
            return fail();
    }
    if (d->vctx) {
        avcodec_send_frame(d->vctx.get(), nullptr);
        if (!d->drain(d->vctx.get(), d->vstream))
            return fail();
    }
    int ret = av_write_trailer(d->fmt);
    d->close();
    if (ret < 0) {
        d->error = "No se pudo cerrar el archivo: " + ffmpegError(ret);
        return fail();
    }
    return true;
}

void Encoder::abort()
{
    const bool hadFile = d->fmt != nullptr;
    d->close();
    if (hadFile && !d->settings.path.empty()) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(std::u8string(d->settings.path.begin(), d->settings.path.end())), ec);
    }
}

int64_t Encoder::videoFramesWritten() const
{
    return d->vpts;
}

std::string Encoder::lastError() const
{
    return d->error;
}

const std::vector<EncoderInfo>& Encoder::availableVideoEncoders()
{
    static std::vector<EncoderInfo> result;
    static std::once_flag once;
    std::call_once(once, [] {
        const int previousLevel = av_log_get_level();
        av_log_set_level(AV_LOG_QUIET); // las pruebas fallidas son normales (sin GPU de ese fabricante)
        for (const char* name : {"libx264", "h264_nvenc", "h264_amf", "h264_qsv", "libx265", "hevc_nvenc", "hevc_amf", "hevc_qsv",
                                 "libsvtav1", "av1_nvenc", "av1_amf", "av1_qsv", "libvpx-vp9", "prores_ks"}) {
            const AVCodec* codec = avcodec_find_encoder_by_name(name);
            if (!codec)
                continue;
            CodecContextPtr ctx(avcodec_alloc_context3(codec));
            ctx->width = 640;
            ctx->height = 360;
            ctx->time_base = AVRational{1, 30};
            ctx->framerate = AVRational{30, 1};
            ctx->pix_fmt = choosePixelFormat(ctx.get(), codec, false);
            if (avcodec_open2(ctx.get(), codec, nullptr) == 0)
                result.push_back({name, codec->long_name ? codec->long_name : name, isHardware(name)});
        }
        av_log_set_level(previousLevel);
    });
    return result;
}

} // namespace movix::media
