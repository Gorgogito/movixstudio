#pragma once

#include "ai/Speech.h"
#include "media/Time.h"

#include <string>
#include <vector>

// Seguimiento de movimiento y localización del sujeto, sobre los fotogramas de un medio.
namespace movix::ai {

// Región en fracciones del fotograma (centro y tamaño, 0..1) y su giro inicial (grados, horario).
struct Region {
    double cx = 0.5, cy = 0.5, w = 0.2, h = 0.2;
    double angle = 0;
};

struct TrackPoint {
    TimeUs time = 0; // instante del medio
    double cx = 0.5;
    double cy = 0.5;
    float confidence = 1.0f; // correlación del ajuste (0..1)
    double scale = 1.0;      // tamaño respecto del primer fotograma
    double angle = 0.0;      // giro respecto del primer fotograma (grados, horario)
};

// Sigue la región desde `from` hasta `to` (instantes del medio) por correlación cruzada normalizada
// sobre luma y croma reducidas, buscando alrededor de la última posición y adaptando poco a poco la
// plantilla. Con `scaleRotation` también estima cuánto crece y gira la región. Devuelve un punto por
// fotograma.
bool trackRegion(const std::string& mediaPath, TimeUs from, TimeUs to, const Region& start, bool scaleRotation,
                 std::vector<TrackPoint>& out, std::string* error = nullptr, const ProgressFn& progress = {},
                 const CancelFn& cancel = {});

// Centro horizontal del sujeto a lo largo del tramo, cada `step`: el de las personas (MODNet, si se
// da su modelo y las encuentra) o, si no, el de la zona que más destaca del color medio del fondo.
// Suavizado para que el reencuadre no tiemble.
bool subjectCenters(const std::string& mediaPath, TimeUs from, TimeUs to, TimeUs step, const std::string& portraitModel,
                    std::vector<TrackPoint>& out, std::string* error = nullptr, const ProgressFn& progress = {},
                    const CancelFn& cancel = {});

} // namespace movix::ai
