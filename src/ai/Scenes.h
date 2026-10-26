#pragma once

#include "ai/Speech.h"
#include "media/Time.h"
#include "media/VideoFrame.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Detección de cambios de escena en un video. Cortes: compara cada fotograma con el anterior por la
// distribución de luz y color y por la imagen reducida, con un umbral adaptativo (el salto debe
// destacar sobre la variación de su entorno, así el movimiento rápido no cuenta como corte).
// Transiciones graduales (disolvencias, fundidos a negro y desde negro): tramos en los que la imagen
// reducida pasa en línea recta de un plano al otro, como hace una mezcla (el movimiento no).
namespace movix::ai {

struct SceneFeatures {
    std::array<float, 16 * 9> grid{}; // luma media por celda (0..1)
    std::array<float, 128> hist{};    // histograma luma(8) × Cb(4) × Cr(4), normalizado
};

SceneFeatures sceneFeatures(const media::VideoFrame& frame);
// Diferencia entre dos fotogramas (0 = iguales, ~1 = completamente distintos).
float sceneDistance(const SceneFeatures& a, const SceneFeatures& b);

struct SceneOptions {
    float threshold = 0.18f;   // diferencia mínima absoluta para un corte
    float contrast = 3.0f;     // cuántas veces debe superar la media de su entorno
    TimeUs minScene = 500'000; // duración mínima de una escena
    bool gradual = true;       // buscar también disolvencias y fundidos
    float fadeThreshold = 0.08f; // cambio medio mínimo de la luma reducida en una transición gradual
};

// Cortes a partir de las diferencias entre fotogramas consecutivos: `times[i]` es el instante del
// fotograma i y `distances[i]` su diferencia con el anterior (distances[0] se ignora).
std::vector<TimeUs> findCuts(const std::vector<TimeUs>& times, const std::vector<float>& distances, const SceneOptions& options);

// Luma reducida (16 × 9) de un fotograma, en bytes.
using SceneGrid = std::array<uint8_t, 16 * 9>;
SceneGrid sceneGrid(const SceneFeatures& features);

// Añade a los cortes (`hardCuts`, de findCuts) los cambios de escena por transiciones graduales;
// `grids[i]` es la luma reducida del fotograma `times[i]`. Una disolvencia cambia de escena a mitad
// de la mezcla; un fundido a negro (o a otro color liso) seguido de una aparición, en el centro del
// tramo liso. Un fundido al final o desde el principio no separa escenas, y los cortes que caen
// dentro de una transición se descartan (son parte de ella). Devuelve la lista completa, ordenada.
std::vector<TimeUs> withGradualCuts(const std::vector<TimeUs>& times, const std::vector<SceneGrid>& grids,
                                    const std::vector<TimeUs>& hardCuts, const SceneOptions& options);

// Decodifica el video en [from, to) y devuelve los instantes (del medio) donde empieza cada escena
// nueva (sin incluir `from`).
bool detectScenes(const std::string& path, TimeUs from, TimeUs to, const SceneOptions& options, std::vector<TimeUs>& cuts,
                  std::string* error = nullptr, const ProgressFn& progress = {}, const CancelFn& cancel = {});

} // namespace movix::ai
