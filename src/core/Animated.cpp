#include "core/Animated.h"

#include <algorithm>

namespace movix::core {

double Animated::at(std::int64_t rel) const
{
    if (keys.empty())
        return value;
    if (rel <= keys.front().frame)
        return keys.front().value;
    if (rel >= keys.back().frame)
        return keys.back().value;
    // Primer keyframe posterior a `rel`.
    auto next = std::upper_bound(keys.begin(), keys.end(), rel, [](std::int64_t f, const Keyframe& k) { return f < k.frame; });
    const Keyframe& b = *next;
    const Keyframe& a = *(next - 1);
    const double t = double(rel - a.frame) / double(b.frame - a.frame);
    switch (a.interp) {
    case Interpolation::Hold:
        return a.value;
    case Interpolation::Smooth: {
        const double s = t * t * (3.0 - 2.0 * t); // suavizado al entrar y salir
        return a.value + (b.value - a.value) * s;
    }
    case Interpolation::Linear:
    default:
        return a.value + (b.value - a.value) * t;
    }
}

void Animated::setKey(std::int64_t rel, double v, Interpolation interp)
{
    auto it = std::lower_bound(keys.begin(), keys.end(), rel, [](const Keyframe& k, std::int64_t f) { return k.frame < f; });
    if (it != keys.end() && it->frame == rel) {
        it->value = v;
        it->interp = interp;
        return;
    }
    keys.insert(it, Keyframe{rel, v, interp});
}

bool Animated::removeKey(std::int64_t rel)
{
    auto it = std::find_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.frame == rel; });
    if (it == keys.end())
        return false;
    // Al quitar el último keyframe, el valor constante pasa a ser el suyo (la propiedad no salta).
    if (keys.size() == 1)
        value = it->value;
    keys.erase(it);
    return true;
}

const Keyframe* Animated::keyAt(std::int64_t rel) const
{
    for (const Keyframe& k : keys)
        if (k.frame == rel)
            return &k;
    return nullptr;
}

void Animated::set(std::int64_t rel, double v)
{
    if (keys.empty()) {
        value = v;
        return;
    }
    const Keyframe* existing = keyAt(rel);
    setKey(rel, v, existing ? existing->interp : Interpolation::Linear);
}

} // namespace movix::core
