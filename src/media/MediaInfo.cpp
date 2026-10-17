#include "media/MediaInfo.h"

#include "media/FFmpegUtils.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace movix::media {

std::optional<MediaInfo> probe(const std::string& path, std::string* error)
{
    FormatContextPtr fmt = openInput(path, error);
    if (!fmt)
        return std::nullopt;

    MediaInfo info;
    info.path = path;
    info.container = fmt->iformat->long_name ? fmt->iformat->long_name : fmt->iformat->name;
    info.duration = fmt->duration == AV_NOPTS_VALUE ? 0 : fmt->duration;
    info.bitRate = fmt->bit_rate;

    if (int vi = findVideoStream(fmt.get()); vi >= 0) {
        const AVStream* st = fmt->streams[vi];
        const AVCodecParameters* par = st->codecpar;
        VideoStreamInfo v;
        v.codec = avcodec_get_name(par->codec_id);
        if (const char* pf = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format)))
            v.pixelFormat = pf;
        v.width = par->width;
        v.height = par->height;
        AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
        v.frameRate = fr.den ? av_q2d(fr) : 0.0;
        AVRational sar = av_guess_sample_aspect_ratio(fmt.get(), const_cast<AVStream*>(st), nullptr);
        v.sampleAspect = sar.num && sar.den ? av_q2d(sar) : 1.0;
        v.bitRate = par->bit_rate;
        info.video = v;
    }

    if (int ai = findAudioStream(fmt.get()); ai >= 0) {
        const AVCodecParameters* par = fmt->streams[ai]->codecpar;
        AudioStreamInfo a;
        a.codec = avcodec_get_name(par->codec_id);
        if (const char* sf = av_get_sample_fmt_name(static_cast<AVSampleFormat>(par->format)))
            a.sampleFormat = sf;
        a.sampleRate = par->sample_rate;
        a.channels = par->ch_layout.nb_channels;
        a.bitRate = par->bit_rate;
        info.audio = a;
    }

    if (!info.video && !info.audio) {
        if (error)
            *error = "El archivo no contiene video ni audio reproducible.";
        return std::nullopt;
    }
    return info;
}

} // namespace movix::media
