#pragma once

#include <cstdint>
#include <vector>

// Scopes de vídeo: forma de onda, desfile RGB, vectorscopio e histograma, calculados sobre una
// imagen RGBA de 8 bits (normalmente una copia reducida del lienzo del visor).
namespace movix::color {

enum class ScopeType { Waveform = 0, Parade, Vectorscope, Histogram };

// Imagen del scope: densidad ya coloreada, en ARGB premultiplicado (0xAARRGGBB), sin retícula.
struct ScopeImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
    std::uint32_t at(int x, int y) const { return pixels[size_t(y) * size_t(width) + size_t(x)]; }
};

// `stride` en bytes por fila. La salida es de outWidth × outHeight (el vectorscopio usa un
// cuadrado centrado del lado menor).
ScopeImage computeScope(ScopeType type, const std::uint8_t* rgba, int width, int height, int stride, int outWidth, int outHeight);

// Posición en el vectorscopio (fracciones del lado, centro = 0,5) de un color RGB de 8 bits.
struct ScopePoint {
    double x = 0.5;
    double y = 0.5;
};
ScopePoint vectorscopePosition(int r, int g, int b);

} // namespace movix::color
