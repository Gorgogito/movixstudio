#include "engine/MediaImport.h"

#include "media/MediaInfo.h"

#include <filesystem>

namespace movix::engine {

std::optional<core::MediaItem> probeMediaItem(const std::string& path, std::string* error)
{
    auto info = media::probe(path, error);
    if (!info)
        return std::nullopt;
    core::MediaItem m;
    m.path = path;
    const std::u8string name = std::filesystem::path(std::u8string(path.begin(), path.end())).filename().u8string();
    m.name.assign(name.begin(), name.end());
    m.duration = info->duration;
    m.hasVideo = info->video.has_value();
    m.hasAudio = info->audio.has_value();
    if (info->video) {
        m.width = info->video->width;
        m.height = info->video->height;
        m.frameRate = info->video->frameRate;
    }
    if (info->isStill && m.hasVideo) {
        m.isStill = true;
        m.duration = 0;
        m.frameRate = 0;
        m.hasAudio = false;
    }
    return m;
}

} // namespace movix::engine
