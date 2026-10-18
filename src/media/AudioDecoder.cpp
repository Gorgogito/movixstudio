#include "media/AudioDecoder.h"

#include <algorithm>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace movix::media {

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder()
{
    close();
}

void AudioDecoder::close()
{
    m_codec.reset();
    m_format.reset();
    m_packet.reset();
    m_frame.reset();
    swr_free(&m_swr);
    m_streamIndex = -1;
    m_basePts = kNoTime;
    m_samplesOut = 0;
    m_trimUntil = kNoTime;
    m_flushing = false;
    m_drained = false;
}

bool AudioDecoder::open(const std::string& path, int outSampleRate, int outChannels, std::string* error)
{
    close();
    auto fail = [&](const std::string& msg) {
        if (error)
            *error = msg;
        close();
        return false;
    };

    m_format = openInput(path, error);
    if (!m_format)
        return false;
    m_streamIndex = findAudioStream(m_format.get());
    if (m_streamIndex < 0)
        return fail("El archivo no contiene un stream de audio.");

    AVStream* st = m_format->streams[m_streamIndex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
        return fail(std::string("No hay decodificador para el códec ") + avcodec_get_name(st->codecpar->codec_id));
    m_codec.reset(avcodec_alloc_context3(codec));
    if (!m_codec || avcodec_parameters_to_context(m_codec.get(), st->codecpar) < 0)
        return fail("No se pudo preparar el decodificador de audio.");
    m_codec->pkt_timebase = st->time_base;
    if (int ret = avcodec_open2(m_codec.get(), codec, nullptr); ret < 0)
        return fail("No se pudo abrir el decodificador de audio: " + ffmpegError(ret));

    m_timeBase = st->time_base;
    m_origin = containerStart(m_format.get());
    if (st->duration != AV_NOPTS_VALUE)
        m_duration = av_rescale_q(st->duration, st->time_base, AVRational{1, 1'000'000});
    else
        m_duration = m_format->duration == AV_NOPTS_VALUE ? 0 : m_format->duration;
    m_outRate = outSampleRate;
    m_outChannels = outChannels;
    m_path = path;
    m_packet.reset(av_packet_alloc());
    m_frame.reset(av_frame_alloc());
    return true;
}

bool AudioDecoder::resetResampler(const AVFrame* frame)
{
    swr_free(&m_swr);
    AVChannelLayout inLayout{};
    if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || frame->ch_layout.nb_channels == 0)
        av_channel_layout_default(&inLayout, std::max(1, frame->ch_layout.nb_channels));
    else
        av_channel_layout_copy(&inLayout, &frame->ch_layout);
    AVChannelLayout outLayout{};
    av_channel_layout_default(&outLayout, m_outChannels);

    int ret = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_FLT, m_outRate, &inLayout,
                                  static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
    // Un clip mono debe sonar al mismo nivel en todos los canales. FFmpeg aplica 1/√2 fijo
    // en el caso mono → estéreo, así que se fija la matriz de mezcla explícitamente.
    if (ret >= 0 && inLayout.nb_channels == 1) {
        std::vector<double> matrix(static_cast<size_t>(m_outChannels), 1.0);
        swr_set_matrix(m_swr, matrix.data(), 1);
    }
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    if (ret < 0 || swr_init(m_swr) < 0) {
        swr_free(&m_swr);
        return false;
    }
    m_swrInRate = frame->sample_rate;
    m_swrInFormat = frame->format;
    m_swrInChannels = frame->ch_layout.nb_channels;
    return true;
}

bool AudioDecoder::decodeFrame()
{
    for (;;) {
        int ret = avcodec_receive_frame(m_codec.get(), m_frame.get());
        if (ret == 0)
            return true;
        if (ret != AVERROR(EAGAIN) || m_flushing)
            return false;
        for (;;) {
            ret = av_read_frame(m_format.get(), m_packet.get());
            if (ret < 0) {
                avcodec_send_packet(m_codec.get(), nullptr);
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
                continue;
            break;
        }
    }
}

void AudioDecoder::appendOutput(AudioChunk& chunk, const float* data, int frames)
{
    const TimeUs pts = m_basePts + m_samplesOut * kUsPerSecond / m_outRate;
    m_samplesOut += frames;

    int skip = 0;
    if (m_trimUntil != kNoTime) {
        const int64_t before = (m_trimUntil - pts) * m_outRate / kUsPerSecond;
        skip = static_cast<int>(std::clamp<int64_t>(before, 0, frames));
        if (skip < frames)
            m_trimUntil = kNoTime;
    }
    const int kept = frames - skip;
    if (kept <= 0)
        return;
    if (chunk.frames == 0)
        chunk.pts = pts + static_cast<TimeUs>(skip) * kUsPerSecond / m_outRate;
    chunk.samples.insert(chunk.samples.end(), data + static_cast<size_t>(skip) * m_outChannels,
                         data + static_cast<size_t>(frames) * m_outChannels);
    chunk.frames += kept;
}

bool AudioDecoder::read(AudioChunk& chunk)
{
    chunk.samples.clear();
    chunk.frames = 0;
    if (!m_codec)
        return false;

    while (chunk.frames == 0 && !m_drained) {
        if (!decodeFrame()) {
            // Fin del stream: vaciar lo que retenga el remuestreador.
            if (m_swr) {
                const int cap = swr_get_out_samples(m_swr, 0);
                if (cap > 0) {
                    m_scratch.resize(static_cast<size_t>(cap) * m_outChannels);
                    auto* out = reinterpret_cast<uint8_t*>(m_scratch.data());
                    const int n = swr_convert(m_swr, &out, cap, nullptr, 0);
                    if (n > 0)
                        appendOutput(chunk, m_scratch.data(), n);
                }
            }
            m_drained = true;
            break;
        }

        AVFrame* f = m_frame.get();
        if (!m_swr || f->sample_rate != m_swrInRate || f->format != m_swrInFormat
            || f->ch_layout.nb_channels != m_swrInChannels) {
            if (!resetResampler(f)) {
                av_frame_unref(f);
                return false;
            }
        }
        if (m_basePts == kNoTime) {
            const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            const TimeUs pts = streamToUs(ts, m_timeBase, m_origin);
            m_basePts = pts == kNoTime ? 0 : pts;
            m_samplesOut = 0;
        }

        const int cap = swr_get_out_samples(m_swr, f->nb_samples);
        m_scratch.resize(static_cast<size_t>(std::max(cap, 0)) * m_outChannels);
        auto* out = reinterpret_cast<uint8_t*>(m_scratch.data());
        const int n = swr_convert(m_swr, &out, cap, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        av_frame_unref(f);
        if (n > 0)
            appendOutput(chunk, m_scratch.data(), n);
    }
    return chunk.frames > 0;
}

bool AudioDecoder::seek(TimeUs t)
{
    if (!m_codec)
        return false;
    if (t < 0)
        t = 0;
    // Pre-roll: se empieza a decodificar un poco antes y se descarta hasta `t`, para que los
    // códecs con solapamiento (AAC, Opus) y el remuestreador lleguen a `t` con su historial completo.
    constexpr TimeUs kPreroll = 100'000;
    if (t < kPreroll) {
        // Cerca del inicio no hay margen para el pre-roll, y tras vaciar el decodificador se
        // perdería el tratamiento del "priming" inicial (las primeras muestras de un AAC).
        // Reabrir equivale exactamente a leer desde el principio.
        const std::string path = m_path;
        if (!open(path, m_outRate, m_outChannels))
            return false;
        m_trimUntil = t;
        return true;
    }
    const int64_t ts = usToStream(std::max<TimeUs>(0, t - kPreroll), m_timeBase, m_origin);
    if (av_seek_frame(m_format.get(), m_streamIndex, ts, AVSEEK_FLAG_BACKWARD) < 0
        && avformat_seek_file(m_format.get(), m_streamIndex, INT64_MIN, ts, ts, 0) < 0)
        return false;
    avcodec_flush_buffers(m_codec.get());
    swr_free(&m_swr);
    m_flushing = false;
    m_drained = false;
    m_basePts = kNoTime;
    m_samplesOut = 0;
    m_trimUntil = t;
    return true;
}

std::string AudioDecoder::decoderDescription() const
{
    if (!m_codec)
        return {};
    return std::string(avcodec_get_name(m_codec->codec_id)) + " · " + std::to_string(m_codec->sample_rate) + " Hz → "
        + std::to_string(m_outRate) + " Hz";
}

} // namespace movix::media
