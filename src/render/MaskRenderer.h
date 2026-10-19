#pragma once

#include "engine/Sources.h"

namespace movix::render {

// Rasteriza y combina las máscaras de una capa con QPainter. El borde suave se obtiene con un
// desenfoque de caja del ancho del difuminado: en bordes rectos da la misma rampa que el cálculo
// analítico del shader.
class QtMaskRenderer : public engine::MaskRenderer {
public:
    std::shared_ptr<const engine::MaskImage> render(const std::vector<core::LayerMask>& masks, int width, int height) override;
};

} // namespace movix::render
