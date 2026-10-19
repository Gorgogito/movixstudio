#pragma once

#include "media/VideoFrame.h"

#include <array>

namespace movix::media {

// Matriz 4×4 (orden por filas) que transforma (Y, Cb, Cr, 1) normalizados en [0,1]
// a RGB no lineal en [0,1], según el espacio de color y el rango (limitado o completo).
// `deep`: muestras de 10 bits en contenedor de 16 (P016, normalizadas a 65535).
std::array<float, 16> yuvToRgbMatrix(ColorSpace space, bool fullRange, bool deep = false);

// Heurística habitual cuando el archivo no declara la matriz: HD o mayor → BT.709.
ColorSpace guessColorSpace(int width, int height);

} // namespace movix::media
