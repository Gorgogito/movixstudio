#pragma once

#include "core/Project.h"

#include <vector>

// Evaluación de la línea de tiempo: qué se ve y qué se oye en cada instante. La usan por igual
// la vista previa y la exportación, de modo que ambas producen exactamente lo mismo.
namespace movix::core {

struct VideoLayer {
    Id clipId = kNoId;
    Id mediaId = kNoId;
    const MediaItem* media = nullptr;
    int trackIndex = 0;       // 0 = V1 (capa inferior)
    TimeUs sourceTime = 0;    // instante del medio que corresponde al fotograma
};

// Capas visibles en el fotograma `frame`, de abajo arriba. Omite pistas ocultas y clips desactivados.
std::vector<VideoLayer> videoLayersAt(const Project& p, FramePos frame);

struct AudioSegment {
    Id clipId = kNoId;
    const MediaItem* media = nullptr;
    TimeUs timelineStart = 0; // intersección del clip con el intervalo pedido
    TimeUs timelineEnd = 0;
    TimeUs sourceStart = 0;   // instante del medio en `timelineStart`
    float gain = 1.0f;
};

// Tramos de audio que suenan en [t0, t1): clips de pistas de audio y el audio de los clips de
// video (salvo pistas silenciadas). El tiempo de la línea es exacto al microsegundo.
std::vector<AudioSegment> audioSegmentsIn(const Project& p, TimeUs t0, TimeUs t1);

} // namespace movix::core
