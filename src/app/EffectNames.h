#pragma once

#include "core/Effects.h"

#include <QColor>
#include <QString>

#include <cstdint>
#include <iterator>
#include <optional>

// Nombres estables (los que usa QML) de los enumerados de efectos y conversiones de color ARGB.
namespace movix::app {

inline constexpr const char* kBlendModeNames[] = {"normal",    "multiply",  "screen",    "overlay",    "darken",
                                                  "lighten",   "colorDodge", "colorBurn", "hardLight", "softLight",
                                                  "difference", "exclusion", "add",       "subtract"};
static_assert(std::size(kBlendModeNames) == core::kBlendModeCount);
inline constexpr const char* kMaskShapeNames[] = {"none", "rectangle", "ellipse", "linear", "path"};
inline constexpr const char* kMaskModeNames[] = {"add", "subtract", "intersect"};

template <typename E, size_t N>
std::optional<E> enumFromName(const QString& name, const char* const (&names)[N])
{
    for (size_t i = 0; i < N; ++i)
        if (name == QLatin1String(names[i]))
            return E(i);
    return std::nullopt;
}

inline QString colorName(std::uint32_t argb)
{
    return QColor::fromRgba(QRgb(argb)).name(QColor::HexArgb);
}

inline std::uint32_t colorValue(const QVariant& v, std::uint32_t fallback)
{
    const QColor c(v.toString());
    return c.isValid() ? std::uint32_t(c.rgba()) : fallback;
}

} // namespace movix::app
