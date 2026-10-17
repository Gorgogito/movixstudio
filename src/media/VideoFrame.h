#pragma once

#include "media/Time.h"

#include <cstdint>
#include <vector>

namespace movix::media {

enum class ColorSpace { BT601, BT709, BT2020 };

// Fotograma decodificado en NV12 de 8 bits con planos compactos (stride == ancho en bytes):
//   y:  width × height bytes
//   uv: chromaWidth()*2 × chromaHeight() bytes (Cb/Cr intercalados)
// Es independiente de FFmpeg, así que puede viajar entre hilos y subirse a la GPU directamente.
struct VideoFrame {
    int width = 0;
    int height = 0;
    double sampleAspect = 1.0;
    TimeUs pts = 0;
    TimeUs duration = 0;
    ColorSpace colorSpace = ColorSpace::BT709;
    bool fullRange = false;
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;

    int chromaWidth() const { return (width + 1) / 2; }
    int chromaHeight() const { return (height + 1) / 2; }
    double displayAspect() const { return height ? width * sampleAspect / height : 1.0; }
};

} // namespace movix::media
