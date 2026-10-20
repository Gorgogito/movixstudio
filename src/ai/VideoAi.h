#pragma once

#include "ai/Speech.h"
#include "media/Time.h"

#include <cstdint>
#include <string>
#include <vector>

// IA que genera video nuevo a partir de un tramo de un medio: escalado (Real-ESRGAN) y fotogramas
// intermedios (RIFE). El resultado es un archivo H.264 de alta calidad sin audio.
namespace movix::ai {

struct VideoResult {
    std::string output;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int frames = 0;
};

// Escala un fotograma RGBA (×4 con el modelo; ×2 reduciendo a la mitad su salida). Procesa en
// teselas de tamaño fijo con solapamiento para acotar la memoria y no dejar costuras.
bool upscaleFrame(const std::string& modelPath, const uint8_t* rgba, int width, int height, int scale,
                  std::vector<uint8_t>& out, std::string* error = nullptr);

bool upscaleVideo(const std::string& modelPath, const std::string& mediaPath, TimeUs from, TimeUs to, int scale,
                  const std::string& output, VideoResult& result, std::string* error = nullptr,
                  const ProgressFn& progress = {}, const CancelFn& cancel = {});

// Fotograma intermedio entre dos (RGBA del mismo tamaño) en la fase t ∈ (0, 1).
bool interpolateFrame(const std::string& modelPath, const uint8_t* a, const uint8_t* b, int width, int height, double t,
                      std::vector<uint8_t>& out, std::string* error = nullptr);

// Multiplica por `factor` (2..8) los fotogramas del tramo: entre cada par de originales se generan
// factor − 1 intermedios. La duración no cambia; el ritmo sube a fps × factor.
bool interpolateVideo(const std::string& modelPath, const std::string& mediaPath, TimeUs from, TimeUs to, int factor,
                      const std::string& output, VideoResult& result, std::string* error = nullptr,
                      const ProgressFn& progress = {}, const CancelFn& cancel = {});

} // namespace movix::ai
