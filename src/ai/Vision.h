#pragma once

#include "ai/Speech.h"
#include "media/Time.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// IA visual sobre fotogramas de video: recorte de fondo (mates), y utilidades comunes para los
// modelos de imagen (conversión a tensores, redimensionado).
namespace movix::ai {

enum class MatteModel {
    Portrait, // MODNet: personas, rápido (lado corto a 512, múltiplos de 32)
    General,  // BiRefNet-lite: cualquier sujeto, más calidad (1024 × 1024)
};

// Opacidad (0..255) del sujeto de un fotograma RGBA de `width` × `height`, a esa misma resolución.
bool matteFrame(const std::string& modelPath, MatteModel model, const uint8_t* rgba, int width, int height,
                std::vector<uint8_t>& alpha, std::string* error = nullptr);

struct MatteResult {
    std::string output;      // video del mate escrito
    TimeUs start = 0;        // instante del medio del primer fotograma del mate
    int frames = 0;
};

// Mate de todo el tramo [from, to) del medio (to = 0: hasta el final), como video en escala de
// grises (H.264, la luma es la opacidad) al mismo ritmo que la fuente.
bool removeBackground(const std::string& modelPath, MatteModel model, const std::string& mediaPath, TimeUs from, TimeUs to,
                      const std::string& output, MatteResult& result, std::string* error = nullptr,
                      const ProgressFn& progress = {}, const CancelFn& cancel = {});

// Mate por fotograma: opacidad (0..255, w × h) del fotograma RGBA número `index` del tramo.
using MatteFn = std::function<bool(const uint8_t* rgba, int width, int height, int index, std::vector<uint8_t>& alpha,
                                   std::string* error)>;
// Decodifica [from, to) y escribe el mate de cada fotograma como video en escala de grises.
bool writeMatte(const std::string& mediaPath, TimeUs from, TimeUs to, const std::string& output, const MatteFn& matte,
                MatteResult& result, std::string* error = nullptr, const ProgressFn& progress = {}, const CancelFn& cancel = {});

// SAM 2.1 (hiera tiny): segmenta el objeto señalado por un punto (y, si la hay, una caja), en
// fracciones del fotograma. `score`: confianza estimada de la máscara elegida.
struct SegmentPrompt {
    double x = 0.5, y = 0.5;
    bool useBox = false;
    std::array<double, 4> box{0, 0, 1, 1}; // x1, y1, x2, y2
};
bool segmentFrame(const std::string& modelFolder, const uint8_t* rgba, int width, int height, const SegmentPrompt& prompt,
                  std::vector<uint8_t>& alpha, float* score = nullptr, std::string* error = nullptr);

// Mate del objeto que hay en (x, y) del primer fotograma de [from, to), seguido por el resto del
// tramo (cada fotograma se guía por la caja y el centro de la máscara anterior).
bool segmentObject(const std::string& modelFolder, const std::string& mediaPath, TimeUs from, TimeUs to, double x, double y,
                   const std::string& output, MatteResult& result, std::string* error = nullptr,
                   const ProgressFn& progress = {}, const CancelFn& cancel = {});

// Redimensiona un plano de floats (bilineal) a bytes 0..255.
void resizePlane(const float* src, int srcWidth, int srcHeight, std::vector<uint8_t>& dst, int dstWidth, int dstHeight);

} // namespace movix::ai
