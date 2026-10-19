# movix_color: matemática del color (corrección, curvas, HSL, LUT .cube, croma, fusión) y scopes.
# Sin GPU ni interfaz: el compositor aplica en la GPU lo que se calcula aquí.
add_library(movix_color STATIC
    ColorMath.h ColorMath.cpp
    Hdr.h Hdr.cpp
    Ocio.h Ocio.cpp
    Scopes.h Scopes.cpp
)

target_include_directories(movix_color PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(OpenColorIO CONFIG REQUIRED)
target_link_libraries(movix_color PUBLIC movix_core PRIVATE OpenColorIO::OpenColorIO)
