#pragma once

#include "core/Effects.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Matemática del color de MovixStudio. La GPU aplica la corrección de cada clip como una LUT 3D
// generada aquí (así visor y exportación coinciden por construcción); la clave de croma y los
// modos de fusión se calculan en los shaders con las mismas fórmulas que las funciones de
// referencia de este archivo, que usan las pruebas para comprobar la GPU.
namespace movix::color {

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// LUT de un archivo .cube (1D o 3D) tal como se leyó.
struct CubeLut {
    std::string title;
    int size = 0;
    bool is3D = true;
    std::array<float, 3> domainMin{0.0f, 0.0f, 0.0f};
    std::array<float, 3> domainMax{1.0f, 1.0f, 1.0f};
    std::vector<float> data; // RGB; en 3D el índice de rojo varía más rápido

    // Aplica la LUT con interpolación (trilineal en 3D, lineal en 1D).
    Rgb apply(Rgb in) const;
};

// Lee un .cube (formato de Adobe/Resolve). nullptr y `error` si no es válido.
std::shared_ptr<const CubeLut> loadCubeLut(const std::string& path, std::string* error = nullptr);
std::shared_ptr<const CubeLut> parseCubeLut(std::string_view text, std::string* error = nullptr);

// Curva monótona (Hermite cúbica de Fritsch–Carlson) por los puntos dados. Si no hay puntos en
// los extremos se añaden (0,0) y (1,1). Se evalúa con una tabla precalculada.
class Curve {
public:
    Curve() = default;
    explicit Curve(const std::vector<core::CurvePoint>& points);
    bool isIdentity() const { return m_table.empty(); }
    float operator()(float x) const;

private:
    std::vector<float> m_table;
};

// Evalúa la corrección de color completa de un clip sobre RGB no lineal en 0..1. Orden:
// transformación de entrada (OCIO) → balance de blancos y exposición (en luz lineal) → puntos negro/blanco → sombras/luces →
// contraste → ruedas (lift/gamma/gain) → saturación/intensidad → HSL → curvas → LUT.
class GradeEvaluator {
public:
    // `input`: transformación de entrada (OCIO) que se aplica antes que todo lo demás.
    GradeEvaluator(const core::ColorGrade& grade, std::shared_ptr<const CubeLut> lut, std::function<Rgb(Rgb)> input = {});
    Rgb apply(Rgb in) const;

private:
    std::function<Rgb(Rgb)> m_input;
    core::ColorGrade m_g;
    std::shared_ptr<const CubeLut> m_lut;
    Curve m_master, m_red, m_green, m_blue;
    std::array<float, 3> m_balance{1.0f, 1.0f, 1.0f};
    bool m_linearStage = false;
    bool m_hsl = false;
};

// LUT 3D lista para la GPU: size³ entradas RGBA en float (el rojo varía más rápido).
struct Lut3D {
    int size = 0;
    std::vector<float> rgba;
    Rgb sample(Rgb in) const; // trilineal, como la GPU (para pruebas)
};

inline constexpr int kDefaultLutSize = 33;
std::shared_ptr<const Lut3D> bakeLut(const GradeEvaluator& grade, int size = kDefaultLutSize);
// Mezcla lineal de dos LUT del mismo tamaño (keyframes de color).
std::shared_ptr<const Lut3D> mixLuts(const Lut3D& a, const Lut3D& b, double t);

// --- Referencias de lo que hace la GPU ---

// Croma BT.709 (Cb, Cr) de un RGB no lineal.
std::array<float, 2> chroma(Rgb c);
// Alfa de la clave de croma y color con el tinte del fondo reducido.
struct KeyResult {
    Rgb color;
    float alpha = 1.0f;
};
KeyResult applyChromaKey(Rgb c, const core::ChromaKey& key);
Rgb argbToRgb(std::uint32_t argb);

// Modo de fusión de un canal (b = fondo, s = capa), ambos en 0..1.
float blendChannel(core::BlendMode mode, float b, float s);
Rgb blend(core::BlendMode mode, Rgb backdrop, Rgb source);

} // namespace movix::color
