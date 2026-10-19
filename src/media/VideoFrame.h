#pragma once

#include "media/Time.h"

#include <cstdint>
#include <vector>

namespace movix::media {

enum class ColorSpace { BT601, BT709, BT2020 };

// Función de transferencia de la señal: SDR (BT.709/BT.1886) o HDR (PQ de SMPTE ST 2084, HLG).
enum class Transfer { SDR, PQ, HLG };

enum class PixelFormat {
    NV12, // video: luma + croma intercalado (8 bits)
    RGBA, // imágenes y títulos: RGBA de 8 bits sin premultiplicar, con transparencia
    P016, // video de 10 bits o más: como NV12 con muestras de 16 bits (little endian, bits altos)
};

// Fotograma decodificado con planos compactos (stride == ancho en bytes):
//   NV12: y = width × height; uv = chromaWidth()*2 × chromaHeight() (Cb/Cr intercalados)
//   P016: igual que NV12 con 2 bytes por muestra
//   RGBA: rgba = width × height × 4
// Es independiente de FFmpeg, así que puede viajar entre hilos y subirse a la GPU directamente.
struct VideoFrame {
    PixelFormat format = PixelFormat::NV12;
    int width = 0;
    int height = 0;
    double sampleAspect = 1.0;
    TimeUs pts = 0;
    TimeUs duration = 0;
    ColorSpace colorSpace = ColorSpace::BT709;
    Transfer transfer = Transfer::SDR;
    bool fullRange = false;
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;
    std::vector<uint8_t> rgba;

    int chromaWidth() const { return (width + 1) / 2; }
    int chromaHeight() const { return (height + 1) / 2; }
    double displayAspect() const { return height ? width * sampleAspect / height : 1.0; }
};

} // namespace movix::media
