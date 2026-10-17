#pragma once

#include <cstdint>

namespace movix {

// Instante o duración en microsegundos. Todo el motor trabaja en esta unidad;
// el origen (0) es el inicio del medio (start_time del contenedor).
using TimeUs = std::int64_t;

inline constexpr TimeUs kNoTime = INT64_MIN;
inline constexpr TimeUs kUsPerSecond = 1'000'000;

constexpr double toSeconds(TimeUs t) { return static_cast<double>(t) / kUsPerSecond; }
constexpr TimeUs fromSeconds(double s) { return static_cast<TimeUs>(s * kUsPerSecond + (s >= 0 ? 0.5 : -0.5)); }

} // namespace movix
