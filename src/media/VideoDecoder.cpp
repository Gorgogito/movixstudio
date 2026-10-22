#include "media/VideoDecoder.h"

#include "media/ColorMatrix.h"
#include "media/GpuFrames.h"

#include <cstring>

#ifdef _WIN32
#include <d3d11.h> // antes que las cabeceras de FFmpeg (que la incluirían dentro de extern "C")
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace movix::media {

namespace {

// Dispositivo D3D11VA propio o, para la copia cero, el del compositor (compartido con él).
bool createHwDevice(AVBufferRef** out, void* external)
{
    if (!external)
        return av_hwdevice_ctx_create(out, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) == 0;
#ifdef _WIN32
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref)
        return false;
    auto* hw = reinterpret_cast<AVHWDeviceContext*>(ref->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(hw->hwctx);
    d3d->device = static_cast<ID3D11Device*>(external);
    d3d->device->AddRef(); // FFmpeg lo libera al cerrar
    if (av_hwdevice_ctx_init(ref) < 0) {
        av_buffer_unref(&ref);
        return false;
    }
    *out = ref;
    return true;
#else
    return false;
#endif
}

// Elige el formato hardware si el decodificador lo ofrece; si no (perfil no soportado,
// fallo al inicializar la aceleración...), el primer formato de software disponible.
AVPixelFormat chooseFormat(AVCodecContext* ctx, const AVPixelFormat* formats)
{
    const auto wanted = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == wanted)
            return *p;
    }
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *p;
    }
    return AV_PIX_FMT_NONE;
}

ColorSpace mapColorSpace(AVColorSpace cs, int width, int height)
{
    switch (cs) {
    case AVCOL_SPC_BT709:
        return ColorSpace::BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
        return ColorSpace::BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return ColorSpace::BT2020;
    default:
        return guessColorSpace(width, height);
    }
}

bool isFullRangeFormat(int format)
{
    return format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P
        || format == AV_PIX_FMT_YUVJ440P || format == AV_PIX_FMT_YUVJ411P;
}

} // namespace

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder()
{
    close();
}

void VideoDecoder::close()
{
    m_gpuPool.reset();
    m_codec.reset();
    m_format.reset();
    m_packet.reset();
    m_frame.reset();
    m_swFrame.reset();
    m_pending.reset();
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    av_buffer_unref(&m_hwDevice);
    m_hwPixFmt = -1;
    m_streamIndex = -1;
    m_lastPts = kNoTime;
    m_flushing = false;
    m_hasPending = false;
    m_lastFrameWasHw = false;
}

bool VideoDecoder::open(const std::string& path, const VideoDecoderOptions& options, std::string* error)
{
    close();
    m_options = options;
    auto fail = [&](const std::string& msg) {
        if (error)
            *error = msg;
        close();
        return false;
    };

    m_format = openInput(path, error);
    if (!m_format)
        return false;

    m_streamIndex = findVideoStream(m_format.get());
    if (m_streamIndex < 0)
        return fail("El archivo no contiene un stream de video.");

    AVStream* st = m_format->streams[m_streamIndex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
        return fail(std::string("No hay decodificador para el códec ") + avcodec_get_name(st->codecpar->codec_id));

    m_codec.reset(avcodec_alloc_context3(codec));
    if (!m_codec || avcodec_parameters_to_context(m_codec.get(), st->codecpar) < 0)
        return fail("No se pudo preparar el decodificador de video.");
    m_codec->pkt_timebase = st->time_base;

    if (options.hardware) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                if (createHwDevice(&m_hwDevice, options.d3d11Device)) {
                    m_hwPixFmt = cfg->pix_fmt;
                    m_codec->hw_device_ctx = av_buffer_ref(m_hwDevice);
                    m_codec->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(m_hwPixFmt));
                    m_codec->get_format = chooseFormat;
                }
                break;
            }
        }
    }
    // Con aceleración por hardware, un único hilo; en CPU, hilos automáticos (por fotograma y por slice).
    m_codec->thread_count = m_hwPixFmt >= 0 ? 1 : 0;
    m_codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (int ret = avcodec_open2(m_codec.get(), codec, nullptr); ret < 0)
        return fail("No se pudo abrir el decodificador de video: " + ffmpegError(ret));

    m_timeBase = st->time_base;
    m_origin = containerStart(m_format.get());
    AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    m_frameRate = (fr.num && fr.den) ? av_q2d(fr) : 25.0;
    m_frameDuration = static_cast<TimeUs>(kUsPerSecond / m_frameRate + 0.5);
    if (st->duration != AV_NOPTS_VALUE)
        m_duration = av_rescale_q(st->duration, st->time_base, AVRational{1, 1'000'000});
    else
        m_duration = m_format->duration == AV_NOPTS_VALUE ? 0 : m_format->duration;
    AVRational sar = av_guess_sample_aspect_ratio(m_format.get(), st, nullptr);
    m_sampleAspect = (sar.num && sar.den) ? av_q2d(sar) : 1.0;

    m_packet.reset(av_packet_alloc());
    m_frame.reset(av_frame_alloc());
    m_swFrame.reset(av_frame_alloc());
    m_pending.reset(av_frame_alloc());
    return true;
}

VideoDecoder::Receive VideoDecoder::receiveRaw(AVFrame* out)
{
    for (;;) {
        int ret = avcodec_receive_frame(m_codec.get(), out);
        if (ret == 0)
            return Receive::Frame;
        if (ret == AVERROR_EOF)
            return Receive::Eof;
        if (ret != AVERROR(EAGAIN) || m_flushing)
            return Receive::Error;

        // El decodificador necesita más datos: leer el siguiente paquete de nuestro stream.
        for (;;) {
            ret = av_read_frame(m_format.get(), m_packet.get());
            if (ret < 0) {
                avcodec_send_packet(m_codec.get(), nullptr); // vaciar los fotogramas retenidos
                m_flushing = true;
                break;
            }
            if (m_packet->stream_index != m_streamIndex) {
                av_packet_unref(m_packet.get());
                continue;
            }
            ret = avcodec_send_packet(m_codec.get(), m_packet.get());
            av_packet_unref(m_packet.get());
            if (ret < 0 && ret != AVERROR(EAGAIN))
                continue; // paquete corrupto: se descarta y se sigue leyendo
            break;
        }
    }
}

TimeUs VideoDecoder::framePts(const AVFrame* frame)
{
    int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    TimeUs t = streamToUs(ts, m_timeBase, m_origin);
    if (t == kNoTime)
        t = m_lastPts == kNoTime ? 0 : m_lastPts + m_frameDuration;
    m_lastPts = t;
    return t;
}

TimeUs VideoDecoder::frameDur(const AVFrame* frame) const
{
    if (frame->duration > 0)
        return av_rescale_q(frame->duration, m_timeBase, AVRational{1, 1'000'000});
    return m_frameDuration;
}

std::shared_ptr<VideoFrame> VideoDecoder::decodeNext()
{
    if (!m_codec)
        return nullptr;
    if (m_hasPending) {
        m_hasPending = false;
        auto out = convert(m_pending.get(), m_pendingPts);
        av_frame_unref(m_pending.get());
        return out;
    }
    if (receiveRaw(m_frame.get()) != Receive::Frame)
        return nullptr;
    auto out = convert(m_frame.get(), framePts(m_frame.get()));
    av_frame_unref(m_frame.get());
    return out;
}

bool VideoDecoder::seek(TimeUs t)
{
    if (!m_codec)
        return false;
    if (t < 0)
        t = 0;

    const int64_t ts = usToStream(t, m_timeBase, m_origin);
    if (av_seek_frame(m_format.get(), m_streamIndex, ts, AVSEEK_FLAG_BACKWARD) < 0
        && avformat_seek_file(m_format.get(), m_streamIndex, INT64_MIN, ts, ts, 0) < 0)
        return false;

    avcodec_flush_buffers(m_codec.get());
    m_flushing = false;
    m_lastPts = kNoTime;
    av_frame_unref(m_pending.get());
    m_hasPending = false;

    // Decodificar desde el keyframe anterior hasta el fotograma que cubre `t`, sin convertirlos.
    // Si se llega al final, queda pendiente el último fotograma del archivo.
    while (receiveRaw(m_frame.get()) == Receive::Frame) {
        const TimeUs pts = framePts(m_frame.get());
        const TimeUs dur = frameDur(m_frame.get());
        av_frame_unref(m_pending.get());
        av_frame_move_ref(m_pending.get(), m_frame.get());
        m_pendingPts = pts;
        m_hasPending = true;
        if (pts + dur > t)
            break;
    }
    return m_hasPending;
}

std::shared_ptr<VideoFrame> VideoDecoder::convert(AVFrame* frame, TimeUs pts)
{
    AVFrame* src = frame;
    m_lastFrameWasHw = frame->format == m_hwPixFmt;
    if (m_lastFrameWasHw && m_options.d3d11Device && frame->hw_frames_ctx) {
        // Copia cero: el fotograma se copia dentro de la GPU a una textura que lee el compositor.
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
        const AVPixFmtDescriptor* swDesc = av_pix_fmt_desc_get(frames->sw_format);
        const bool deep = swDesc && swDesc->comp[0].depth > 8;
        if ((!deep || m_options.highBitDepth) && frame->width % 2 == 0 && frame->height % 2 == 0) {
            if (!m_gpuPool || !m_gpuPool->matches(frame->width, frame->height, deep))
                m_gpuPool = std::make_unique<GpuTexturePool>(m_options.d3d11Device, frame->width, frame->height, deep);
            std::shared_ptr<void> hold;
            if (void* tex = m_gpuPool->copyFrom(frame->data[0], int(reinterpret_cast<intptr_t>(frame->data[1])), hold)) {
                auto out = std::make_shared<VideoFrame>();
                out->width = frame->width;
                out->height = frame->height;
                out->sampleAspect = m_sampleAspect;
                out->pts = pts;
                out->duration = frameDur(frame);
                out->colorSpace = mapColorSpace(frame->colorspace, frame->width, frame->height);
                out->fullRange = frame->color_range == AVCOL_RANGE_JPEG;
                out->transfer = frame->color_trc == AVCOL_TRC_SMPTE2084     ? Transfer::PQ
                                : frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ? Transfer::HLG
                                                                             : Transfer::SDR;
                out->format = deep ? PixelFormat::P016 : PixelFormat::NV12;
                out->gpuTexture = tex;
                out->gpuHold = std::move(hold);
                return out;
            }
        }
    }
    if (m_lastFrameWasHw) {
        av_frame_unref(m_swFrame.get());
        if (av_hwframe_transfer_data(m_swFrame.get(), frame, 0) < 0)
            return nullptr;
        src = m_swFrame.get();
    }

    auto out = std::make_shared<VideoFrame>();
    out->width = src->width;
    out->height = src->height;
    out->sampleAspect = m_sampleAspect;
    out->pts = pts;
    out->duration = frameDur(frame);
    out->colorSpace = mapColorSpace(frame->colorspace, src->width, src->height);
    out->fullRange = frame->color_range == AVCOL_RANGE_JPEG || isFullRangeFormat(src->format);
    out->transfer = frame->color_trc == AVCOL_TRC_SMPTE2084     ? Transfer::PQ
                    : frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ? Transfer::HLG
                                                                 : Transfer::SDR;

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(src->format));
    const bool deep = m_options.highBitDepth && desc && desc->comp[0].depth > 8;
    out->format = deep ? PixelFormat::P016 : PixelFormat::NV12;
    const int bytes = deep ? 2 : 1;
    const int w = out->width, h = out->height;
    const int yRow = w * bytes, uvRow = out->chromaWidth() * 2 * bytes, ch = out->chromaHeight();
    out->y.resize(static_cast<size_t>(yRow) * h);
    out->uv.resize(static_cast<size_t>(uvRow) * ch);
    const AVPixelFormat direct = deep ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    const bool sameLayout = src->format == direct || (deep && src->format == AV_PIX_FMT_P016LE);

    if (sameLayout) {
        for (int row = 0; row < h; ++row)
            std::memcpy(out->y.data() + static_cast<size_t>(row) * yRow, src->data[0] + static_cast<ptrdiff_t>(row) * src->linesize[0], yRow);
        for (int row = 0; row < ch; ++row)
            std::memcpy(out->uv.data() + static_cast<size_t>(row) * uvRow, src->data[1] + static_cast<ptrdiff_t>(row) * src->linesize[1], uvRow);
    } else {
        m_sws = sws_getCachedContext(m_sws, w, h, static_cast<AVPixelFormat>(src->format), w, h, direct,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws)
            return nullptr;
        // Conservar el rango original (no comprimir rango completo a limitado).
        const int full = out->fullRange ? 1 : 0;
        const int* coeffs = sws_getCoefficients(SWS_CS_DEFAULT);
        sws_setColorspaceDetails(m_sws, coeffs, full, coeffs, full, 0, 1 << 16, 1 << 16);
        uint8_t* dst[4] = {out->y.data(), out->uv.data(), nullptr, nullptr};
        int dstStride[4] = {yRow, uvRow, 0, 0};
        sws_scale(m_sws, src->data, src->linesize, 0, h, dst, dstStride);
    }
    return out;
}

bool VideoDecoder::isHardwareAccelerated() const
{
    return m_hwPixFmt >= 0 && m_lastFrameWasHw;
}

std::string VideoDecoder::decoderDescription() const
{
    if (!m_codec)
        return {};
    std::string desc = avcodec_get_name(m_codec->codec_id);
    if (isHardwareAccelerated())
        return desc + " · D3D11VA (GPU)";
    return desc + " · CPU (" + std::to_string(m_codec->thread_count) + " hilos)";
}

int VideoDecoder::width() const
{
    return m_codec ? m_codec->width : 0;
}

int VideoDecoder::height() const
{
    return m_codec ? m_codec->height : 0;
}

} // namespace movix::media
