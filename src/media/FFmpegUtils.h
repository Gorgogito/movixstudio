#pragma once

#include "media/Time.h"

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
}

namespace movix::media {

std::string ffmpegError(int err);

struct FormatContextDeleter { void operator()(AVFormatContext* c) const { avformat_close_input(&c); } };
struct CodecContextDeleter { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

// Abre un archivo (ruta UTF-8) y lee la información de sus streams.
FormatContextPtr openInput(const std::string& path, std::string* error);

// Índice del mejor stream de video "real" (ignora carátulas incrustadas), o -1.
int findVideoStream(AVFormatContext* fmt);
int findAudioStream(AVFormatContext* fmt);

// Origen de tiempos del contenedor en µs (start_time), 0 si no está definido.
TimeUs containerStart(const AVFormatContext* fmt);

// Convierte un timestamp del stream a µs relativos al inicio del contenedor.
TimeUs streamToUs(int64_t ts, AVRational timeBase, TimeUs origin);
int64_t usToStream(TimeUs t, AVRational timeBase, TimeUs origin);

} // namespace movix::media
