#pragma once

#include "media/VideoFrame.h"

#include <memory>
#include <string>

namespace movix::media {

// Decodifica una imagen fija (PNG, JPEG, WebP, BMP, TIFF...) a RGBA conservando la transparencia.
std::shared_ptr<VideoFrame> decodeImageRgba(const std::string& path, std::string* error = nullptr);

} // namespace movix::media
