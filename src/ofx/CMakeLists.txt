# movix_ofx: anfitrión de efectos OpenFX (plugins .ofx de terceros). Solo cabeceras de OpenFX
# (BSD-3-Clause, vcpkg) y Qt Core para cargar las bibliotecas.
find_package(unofficial-openfx CONFIG REQUIRED)

add_library(movix_ofx STATIC
    OfxHost.h OfxHost.cpp
)

target_include_directories(movix_ofx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(movix_ofx PUBLIC Qt6::Core PRIVATE unofficial::openfx::OpenFx)
target_compile_definitions(movix_ofx PRIVATE MOVIX_VERSION="${PROJECT_VERSION}" MOVIX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MOVIX_VERSION_MINOR=${PROJECT_VERSION_MINOR} MOVIX_VERSION_PATCH=${PROJECT_VERSION_PATCH})
