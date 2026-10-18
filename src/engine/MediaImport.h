#pragma once

#include "core/Project.h"

#include <optional>
#include <string>

namespace movix::engine {

// Sondea un archivo y lo describe como elemento de la biblioteca de medios (sin id asignado).
std::optional<core::MediaItem> probeMediaItem(const std::string& path, std::string* error = nullptr);

} // namespace movix::engine
