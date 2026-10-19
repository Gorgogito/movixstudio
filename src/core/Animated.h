#pragma once

#include <cstdint>
#include <vector>

namespace movix::core {

enum class Interpolation { Linear, Hold, Smooth };

// Keyframe de una propiedad. `frame` es relativo al inicio del clip, así que los keyframes se
// mueven con él al desplazarlo por la línea de tiempo.
struct Keyframe {
    std::int64_t frame = 0;
    double value = 0.0;
    Interpolation interp = Interpolation::Linear; // cómo se llega al keyframe siguiente
    bool operator==(const Keyframe&) const = default;
};

// Valor animable: constante (`value`) o definido por keyframes ordenados.
struct Animated {
    double value = 0.0;
    std::vector<Keyframe> keys;

    Animated() = default;
    Animated(double v) : value(v) {} // NOLINT: conversión implícita deseada (Animated x = 1.0)

    bool isAnimated() const { return !keys.empty(); }
    // Valor en el fotograma `rel` (relativo al clip). Antes del primer keyframe y después del
    // último se mantiene su valor.
    double at(std::int64_t rel) const;

    // Edición: crea o actualiza el keyframe de `rel`; quitarlo; consultar si existe.
    void setKey(std::int64_t rel, double v, Interpolation interp = Interpolation::Linear);
    bool removeKey(std::int64_t rel);
    const Keyframe* keyAt(std::int64_t rel) const;
    // Fija el valor en `rel`: si está animado actualiza/crea el keyframe; si no, el valor base.
    void set(std::int64_t rel, double v);

    bool operator==(const Animated&) const = default;
};

} // namespace movix::core
