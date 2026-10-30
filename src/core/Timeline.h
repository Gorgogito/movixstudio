#pragma once

#include "core/Project.h"

#include <algorithm>
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

// Máscara evaluada en un fotograma (ver core::Mask).
struct LayerMask {
    MaskShape shape = MaskShape::None;
    bool invert = false;
    double x = 0.5, y = 0.5, width = 0.6, height = 0.6, rotation = 0.0, feather = 0.02;
    MaskMode mode = MaskMode::Add;
    std::vector<PathPoint> points; // solo en las de forma libre
    bool operator==(const LayerMask&) const = default;
};

struct VideoLayer {
    Id clipId = kNoId;
    // Identifica la aparición concreta del clip (dentro de un compuesto, la del compuesto y la
    // del clip): un lector de medios por aparición.
    std::uint64_t instance = 0;
    Id mediaId = kNoId;
    const MediaItem* media = nullptr;  // nullptr en títulos y generadores
    const TitleStyle* title = nullptr; // solo en títulos
    const OfxEffect* generator = nullptr; // solo en generadores OpenFX
    int titleChars = -1;               // máquina de escribir: caracteres visibles (−1 = todos)
    int trackIndex = 0;                // 0 = V1 (capa inferior)
    TimeUs sourceTime = 0;             // instante del medio que corresponde al fotograma
    LayerTransform transform;
    double opacity = 1.0;
    CropRect crop;
    // Corrección de color: `grade` (nullptr si no hay) mezclada con `gradeTo` en la proporción
    // `gradeMix` entre dos keyframes de color.
    const ColorGrade* grade = nullptr;
    const ColorGrade* gradeTo = nullptr;
    double gradeMix = 0.0;
    ChromaKey chromaKey;
    std::vector<LayerMask> masks;
    const AiMatte* matte = nullptr; // mate de IA (el instante es el de `sourceTime`)
    // Efectos OpenFX del clip (nullptr si no tiene) y el fotograma dentro del clip que se evalúa.
    const std::vector<OfxEffect>* ofx = nullptr;
    FramePos clipFrame = 0;
    BlendMode blend = BlendMode::Normal;
    // Clip compuesto: capas de su secuencia en el fotograma que corresponde (de abajo arriba).
    bool compound = false;
    std::vector<VideoLayer> nested;
    // Transición OpenFX: en `nested`, la capa saliente y la entrante tal cual (sin transición). El
    // plugin las mezcla con el progreso `transitionProgress` (0..1) en el fotograma
    // `transitionFrame` de la transición.
    const OfxEffect* transition = nullptr;
    double transitionProgress = 0.0;
    FramePos transitionFrame = 0;
};

inline constexpr int kMaxNesting = 8; // profundidad máxima de clips compuestos que se evalúa

// Capas visibles en el fotograma `frame`, de abajo arriba (dentro de una transición, la entrante
// va encima de la saliente). Omite pistas ocultas y clips desactivados.
std::vector<VideoLayer> videoLayersAt(const Project& p, FramePos frame);

struct AudioSegment {
    Id clipId = kNoId;
    std::uint64_t instance = 0; // ver VideoLayer::instance
    const MediaItem* media = nullptr;
    TimeUs timelineStart = 0; // intersección del clip con el intervalo pedido
    TimeUs timelineEnd = 0;
    TimeUs sourceStart = 0;   // instante del medio en `timelineStart` (negativo = antes del medio)
    // Velocidad variable o al revés: instante del medio en `timelineEnd`; el tramo se recorre
    // linealmente de sourceStart a sourceEnd (puede ir hacia atrás) sin conservar el tono.
    TimeUs sourceEnd = kNoTime;
    double speed = 1.0;
    float gainStart = 1.0f;   // ganancia total al principio y al final del tramo (rampa lineal)
    float gainEnd = 1.0f;
    double denoise = 0.0;     // reducción de ruido del clip (0..1)

    bool varispeed() const { return sourceEnd != kNoTime; }
    // Instante de la línea de tiempo en que suena el instante `source` del medio.
    TimeUs timelineFor(TimeUs source) const;
};

// Tramos de audio que suenan en [t0, t1): clips de pistas de audio y el audio de los clips de
// video (salvo pistas silenciadas o audio separado). Incluye volumen de pista y de clip,
// fundidos y el fundido cruzado de las transiciones.
std::vector<AudioSegment> audioSegmentsIn(const Project& p, TimeUs t0, TimeUs t1);

// Cuadrícula de la vista multicámara (Project::multicamGrid): ángulo a ángulo de izquierda a derecha
// y de arriba abajo. Las medidas son fracciones del lienzo.
struct MulticamGrid {
    int cols = 1;
    int rows = 1;
    struct Rect {
        double x = 0, y = 0, width = 1, height = 1;
    };
    double scale() const { return 1.0 / double(std::max(cols, rows)); } // escala de cada ángulo
    Rect cell(int angle) const;  // celda del ángulo
    Rect image(int angle) const; // parte de la celda que ocupa su imagen (centrada)
    int angleAt(double x, double y, int angles) const; // −1 fuera de las imágenes
};
MulticamGrid multicamGridFor(int angles);

// Partes de una transición en un corte: [corte - before, corte + after).
struct TransitionSpan {
    FramePos before = 0;
    FramePos after = 0;
};
TransitionSpan transitionSpan(const TransitionIn& t);
// Primer fotograma de la transición de entrada del clip (el 0 de los keyframes de una OpenFX).
FramePos transitionStart(const Clip& c);

} // namespace movix::core
