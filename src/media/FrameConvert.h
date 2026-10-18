#pragma once

#include "media/VideoFrame.h"

#include <cstdint>
#include <vector>

namespace movix::media {

// Convierte un fotograma NV12 a RGBA de 8 bits escalado a outWidth×outHeight (miniaturas).
bool frameToRgba(const VideoFrame& frame, int outWidth, int outHeight, std::vector<uint8_t>& rgba);

} // namespace movix::media
