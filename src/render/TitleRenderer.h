#pragma once

#include "engine/Sources.h"

namespace movix::render {

// Genera la imagen de un título con QPainter: texto (varias líneas) centrado en el lienzo, con
// contorno, sombra y recuadro de fondo opcionales. Requiere una QGuiApplication (fuentes).
class QtTitleRenderer : public engine::TitleRenderer {
public:
    std::shared_ptr<const media::VideoFrame> render(const core::TitleStyle& style, int canvasWidth, int canvasHeight,
                                                    int visibleChars = -1) override;
};

} // namespace movix::render
