#pragma once

#include "core/Animated.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Efectos de imagen de un clip de video: corrección de color, croma, máscara y modo de fusión.
// Solo datos: la matemática del color está en movix_color y el dibujo en el compositor.
namespace movix::core {

// Rueda de color (sombras/medios/luces): desplazamiento por canal y uno común a los tres.
struct ColorWheel {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double master = 0.0;
    bool isNeutral() const { return r == 0.0 && g == 0.0 && b == 0.0 && master == 0.0; }
    bool operator==(const ColorWheel&) const = default;
};

// Punto de una curva (entrada → salida, ambos en 0..1).
struct CurvePoint {
    double x = 0.0;
    double y = 0.0;
    bool operator==(const CurvePoint&) const = default;
};

// Ajuste de una gama de tonos (HSL secundario).
struct HslBand {
    double hue = 0.0;        // desplazamiento de tono en grados (-30..30)
    double saturation = 0.0; // -1..1
    double luminance = 0.0;  // -1..1
    bool isNeutral() const { return hue == 0.0 && saturation == 0.0 && luminance == 0.0; }
    bool operator==(const HslBand&) const = default;
};

// Centros de las 8 gamas HSL en grados: rojo, naranja, amarillo, verde, aguamarina, azul, morado, magenta.
inline constexpr std::array<double, 8> kHslBandHues{0.0, 30.0, 60.0, 120.0, 180.0, 240.0, 270.0, 300.0};

// Corrección de color de un clip. Todos los valores neutros = sin cambio (y sin coste al dibujar).
struct ColorGrade {
    // Básica
    double exposure = 0.0;    // pasos (-4..4)
    double contrast = 0.0;    // -1..1
    double saturation = 0.0;  // -1..1 (−1 = blanco y negro)
    double vibrance = 0.0;    // -1..1 (afecta más a los colores poco saturados)
    double temperature = 0.0; // -1..1 (frío ↔ cálido)
    double tint = 0.0;        // -1..1 (verde ↔ magenta)
    double highlights = 0.0;  // -1..1
    double shadows = 0.0;     // -1..1
    double whites = 0.0;      // -1..1 (punto blanco)
    double blacks = 0.0;      // -1..1 (punto negro)
    // Ruedas
    ColorWheel lift;  // sombras
    ColorWheel gamma; // medios tonos
    ColorWheel gain;  // luces
    // Curvas: vacías = identidad. Puntos ordenados por x.
    std::vector<CurvePoint> curveMaster, curveRed, curveGreen, curveBlue;
    // HSL por gamas
    std::array<HslBand, 8> hsl{};
    // LUT .cube (ruta absoluta UTF-8) y su intensidad
    std::string lutPath;
    double lutIntensity = 1.0;
    // Espacio de color de cámara de OpenColorIO (vacío = la señal del clip ya es la del proyecto).
    std::string inputSpace;

    bool isIdentity() const;
    bool operator==(const ColorGrade&) const = default;
};

// Corrección animable: keyframes con la corrección completa. Entre dos keyframes la GPU mezcla las
// dos LUT, así que se animan también curvas, HSL y LUT. `frame` es relativo al inicio del clip.
struct GradeKey {
    std::int64_t frame = 0;
    ColorGrade grade;
    Interpolation interp = Interpolation::Linear; // cómo se llega al keyframe siguiente
    bool operator==(const GradeKey&) const = default;
};

struct AnimatedGrade {
    ColorGrade value; // sin keyframes
    std::vector<GradeKey> keys;

    bool isAnimated() const { return !keys.empty(); }
    bool isIdentity() const;
    // Las dos correcciones que se mezclan en `rel` (to = nullptr si no hay mezcla) y su proporción.
    struct Sample {
        const ColorGrade* from = nullptr;
        const ColorGrade* to = nullptr;
        double t = 0.0;
    };
    Sample at(std::int64_t rel) const;
    // Corrección numérica en `rel` (interpola los valores; la usa la interfaz para mostrar y editar).
    ColorGrade valueAt(std::int64_t rel) const;
    // Como Animated::set: con keyframes crea o actualiza el de `rel`; sin ellos cambia `value`.
    void set(std::int64_t rel, const ColorGrade& g);
    void setKey(std::int64_t rel, const ColorGrade& g, Interpolation interp = Interpolation::Linear);
    bool removeKey(std::int64_t rel); // al quitar el último, su corrección queda como `value`
    const GradeKey* keyAt(std::int64_t rel) const;
    bool operator==(const AnimatedGrade&) const = default;
};

// Interpolación numérica de dos correcciones (las curvas solo si tienen los mismos puntos).
ColorGrade lerpGrade(const ColorGrade& a, const ColorGrade& b, double t);

// Clave de croma: vuelve transparente un color (fondo verde/azul).
struct ChromaKey {
    bool enabled = false;
    std::uint32_t color = 0xFF00FF00; // ARGB del color clave
    double tolerance = 0.25;          // 0..1: distancia de croma que se elimina por completo
    double softness = 0.1;            // 0..1: ancho del borde suave
    double spill = 0.5;               // 0..1: cuánto se quita del tinte del color clave
    bool operator==(const ChromaKey&) const = default;
};

enum class MaskShape { None, Rectangle, Ellipse, Linear, Path };
// Cómo se combina una máscara con las anteriores del clip.
enum class MaskMode { Add, Subtract, Intersect };
inline constexpr size_t kMaxMasks = 8;

// Punto de una máscara de forma libre: posición y tangentes de Bézier (relativas al punto), en el
// sistema propio de la máscara (−0,5..0,5 a lo ancho y alto de su caja, antes de girarla).
struct PathPoint {
    double x = 0.0;
    double y = 0.0;
    double inX = 0.0, inY = 0.0;   // tangente de entrada
    double outX = 0.0, outY = 0.0; // tangente de salida
    bool operator==(const PathPoint&) const = default;
};

// Máscara en coordenadas de la propia capa (se mueve con ella). x/y es el centro en fracciones del
// ancho/alto de la capa; width/height, el tamaño de su caja en esas fracciones; feather, el ancho
// del borde suave en fracción del alto de la capa. En la lineal, la línea pasa por x/y con el
// ángulo `rotation` y se ve el lado de abajo (antes de girar). En la de forma libre, el contorno
// cerrado `points` se escala con width/height y gira con rotation.
struct Mask {
    MaskShape shape = MaskShape::None;
    MaskMode mode = MaskMode::Add;
    Animated x{0.5};
    Animated y{0.5};
    Animated width{0.6};
    Animated height{0.6};
    Animated rotation{0.0}; // grados, sentido horario
    Animated feather{0.02};
    bool invert = false;
    std::vector<PathPoint> points;
    bool active() const { return shape != MaskShape::None; }
    bool operator==(const Mask&) const = default;
};

// Pentágono inicial de una máscara de forma libre.
std::vector<PathPoint> defaultMaskPath();

// Modos de fusión (fórmulas de W3C Compositing and Blending).
enum class BlendMode {
    Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn,
    HardLight, SoftLight, Difference, Exclusion, Add, Subtract,
};
inline constexpr int kBlendModeCount = 14;

} // namespace movix::core
