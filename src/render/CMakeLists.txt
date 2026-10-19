# movix_render: composición en la GPU con Qt RHI (visor y exportación comparten el compositor).
add_library(movix_render STATIC
    Compositor.h Compositor.cpp
    OffscreenRenderer.h OffscreenRenderer.cpp
    TitleRenderer.h TitleRenderer.cpp
    MaskRenderer.h MaskRenderer.cpp
)

target_include_directories(movix_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# QRhi (rhi/qrhi.h) se distribuye con los headers privados de QtGui en Qt 6.8.
target_link_libraries(movix_render PUBLIC movix_engine Qt6::Gui Qt6::GuiPrivate)

qt_add_shaders(movix_render "movix_shaders"
    PREFIX "/movix"
    FILES
        shaders/layer.vert
        shaders/layer.frag
        shaders/solid.frag
        shaders/canvas.frag
)
