#pragma once

#include "media/Time.h"

#include <optional>
#include <string>

namespace movix::media {

struct VideoStreamInfo {
    std::string codec;        // nombre corto del códec (h264, hevc, vp9...)
    std::string pixelFormat;  // yuv420p, yuv420p10le...
    int width = 0;
    int height = 0;
    double frameRate = 0.0;   // fotogramas por segundo (promedio)
    double sampleAspect = 1.0;
    int64_t bitRate = 0;
};

struct AudioStreamInfo {
    std::string codec;
    std::string sampleFormat;
    int sampleRate = 0;
    int channels = 0;
    int64_t bitRate = 0;
};

struct MediaInfo {
    std::string path;
    std::string container;    // nombre largo del formato
    TimeUs duration = 0;
    int64_t bitRate = 0;
    std::optional<VideoStreamInfo> video;
    std::optional<AudioStreamInfo> audio;
};

// Sondea un archivo y describe su mejor stream de video y de audio.
std::optional<MediaInfo> probe(const std::string& path, std::string* error = nullptr);

} // namespace movix::media
