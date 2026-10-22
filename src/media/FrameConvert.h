#pragma once

#include "media/VideoFrame.h"

#include <cstdint>
#include <vector>

namespace movix::media {

// Convierte un fotograma (NV12, P016 o RGBA) a RGBA de 8 bits escalado a outWidth×outHeight
// (miniaturas, efectos OpenFX). Los de más de 8 bits pierden precisión.
bool frameToRgba(const VideoFrame& frame, int outWidth, int outHeight, std::vector<uint8_t>& rgba);

} // namespace movix::media
