#pragma once

#include "core/Project.h"

#include <vector>

// Evaluación de la línea de tiempo: qué se ve y qué se oye en cada instante. La usan por igual
// la vista previa y la exportación, de modo que ambas producen exactamente lo mismo.
namespace movix::core {

// Transformación final de una capa: desplazamiento del centro (fracciones del lienzo), escala
// sobre el ajuste al lienzo y rotación en grados.
struct LayerTransform {
    double x = 0.0;
    double y = 0.0;
    double scale = 1.0;
    double rotation = 0.0;
};

// Parte visible de la capa en coordenadas de la propia capa (0..1); la usan los barridos.
struct CropRect {
    double left = 0.0;
    double top = 0.0;
    double right = 1.0;
    double bottom = 1.0;
};

struct VideoLayer {
    Id clipId = kNoId;
    Id mediaId = kNoId;
    const MediaItem* media = nullptr;  // nullptr en títulos
    const TitleStyle* title = nullptr; // solo en títulos
    int trackIndex = 0;                // 0 = V1 (capa inferior)
    TimeUs sourceTime = 0;             // instante del medio que corresponde al fotograma
    LayerTransform transform;
    double opacity = 1.0;
    CropRect crop;
};

// Capas visibles en el fotograma `frame`, de abajo arriba (dentro de una transición, la entrante
// va encima de la saliente). Omite pistas ocultas y clips desactivados.
std::vector<VideoLayer> videoLayersAt(const Project& p, FramePos frame);

struct AudioSegment {
    Id clipId = kNoId;
    const MediaItem* media = nullptr;
    TimeUs timelineStart = 0; // intersección del clip con el intervalo pedido
    TimeUs timelineEnd = 0;
    TimeUs sourceStart = 0;   // instante del medio en `timelineStart` (negativo = antes del medio)
    double speed = 1.0;
    float gainStart = 1.0f;   // ganancia total al principio y al final del tramo (rampa lineal)
    float gainEnd = 1.0f;
};

// Tramos de audio que suenan en [t0, t1): clips de pistas de audio y el audio de los clips de
// video (salvo pistas silenciadas o audio separado). Incluye volumen de pista y de clip,
// fundidos y el fundido cruzado de las transiciones.
std::vector<AudioSegment> audioSegmentsIn(const Project& p, TimeUs t0, TimeUs t1);

// Partes de una transición en un corte: [corte - before, corte + after).
struct TransitionSpan {
    FramePos before = 0;
    FramePos after = 0;
};
TransitionSpan transitionSpan(const TransitionIn& t);

} // namespace movix::core
