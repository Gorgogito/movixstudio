#pragma once

#include "color/ColorMath.h"
#include "core/Project.h"

#include <functional>
#include <string>
#include <vector>

// Gestión de color con OpenColorIO: el material de cámara (logarítmico, gamas amplias) se lleva a
// la señal del proyecto con el config de estudio de ACES que trae OCIO y la vista estándar de la
// pantalla del proyecto (Rec.709, Rec.2100 PQ o HLG). La transformación se incorpora a la LUT 3D
// del clip, antes de la corrección de color.
namespace movix::color {

struct InputSpace {
    std::string name;   // nombre del espacio en el config (p. ej. "S-Log3 S-Gamut3")
    std::string family; // p. ej. "Input/Sony"
};

// Espacios de entrada de cámara disponibles (vacío si OCIO no pudo cargar su config).
const std::vector<InputSpace>& ocioInputSpaces();
std::string ocioDescription(); // versión de OCIO y config usado

using RgbTransform = std::function<Rgb(Rgb)>;
// Del espacio de cámara `space` a la señal de `mode`. Vacío (y `error`) si no existe.
RgbTransform ocioInputTransform(const std::string& space, core::ColorMode mode, std::string* error = nullptr);

} // namespace movix::color
