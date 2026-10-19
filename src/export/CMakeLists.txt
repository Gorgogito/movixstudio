# movix_export: exportación de la línea de tiempo a archivo (composición GPU + mezcla + codificación).
add_library(movix_export STATIC
    BoundedQueue.h
    Exporter.h Exporter.cpp
)

target_include_directories(movix_export PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(movix_export PUBLIC movix_render movix_engine movix_media)
