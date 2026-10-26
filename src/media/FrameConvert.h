#pragma once

#include "media/VideoFrame.h"

#include <cstdint>
#include <vector>

namespace movix::media {

// Convierte un fotograma (NV12, P016 o RGBA) a RGBA de 8 bits escalado a outWidth×outHeight
// (miniaturas, efectos OpenFX). Los de más de 8 bits pierden precisión.
bool frameToRgba(const VideoFrame& frame, int outWidth, int outHeight, std::vector<uint8_t>& rgba);
// Igual en coma flotante (0..1, en la señal del fotograma), sin perder los bits de más; mismo tamaño
// y la misma matriz YUV → RGB que el compositor.
bool frameToRgbaF(const VideoFrame& frame, std::vector<float>& rgba);
// RGBA en coma flotante ↔ los qfloat16 de un fotograma RGBA16F.
void floatToHalf(const std::vector<float>& rgba, std::vector<uint8_t>& half);
float halfAt(const std::vector<uint8_t>& half, size_t index);

} // namespace movix::media
