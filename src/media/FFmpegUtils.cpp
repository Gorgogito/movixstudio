#include "media/FFmpegUtils.h"

namespace movix::media {

std::string ffmpegError(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

FormatContextPtr openInput(const std::string& path, std::string* error)
{
    AVFormatContext* raw = nullptr;
    int ret = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (error)
            *error = "No se pudo abrir el archivo: " + ffmpegError(ret);
        return nullptr;
    }
    FormatContextPtr fmt(raw);
    ret = avformat_find_stream_info(fmt.get(), nullptr);
    if (ret < 0) {
        if (error)
            *error = "No se pudo leer la información del archivo: " + ffmpegError(ret);
        return nullptr;
    }
    return fmt;
}

int findVideoStream(AVFormatContext* fmt)
{
    int best = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (best >= 0 && !(fmt->streams[best]->disposition & AV_DISPOSITION_ATTACHED_PIC))
        return best;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
            return static_cast<int>(i);
    }
    return -1;
}

int findAudioStream(AVFormatContext* fmt)
{
    int best = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    return best >= 0 ? best : -1;
}

TimeUs containerStart(const AVFormatContext* fmt)
{
    return fmt->start_time == AV_NOPTS_VALUE ? 0 : fmt->start_time; // AV_TIME_BASE == 1e6
}

TimeUs streamToUs(int64_t ts, AVRational timeBase, TimeUs origin)
{
    if (ts == AV_NOPTS_VALUE)
        return kNoTime;
    return av_rescale_q(ts, timeBase, AVRational{1, 1'000'000}) - origin;
}

int64_t usToStream(TimeUs t, AVRational timeBase, TimeUs origin)
{
    return av_rescale_q(t + origin, AVRational{1, 1'000'000}, timeBase);
}

} // namespace movix::media
