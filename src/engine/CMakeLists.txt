# movix_engine: motor de la línea de tiempo (lectores por clip, composición de fotogramas,
# mezcla de audio). Lo comparten la vista previa y la exportación.
add_library(movix_engine STATIC
    VideoReader.h VideoReader.cpp
    AudioReader.h AudioReader.cpp
    Sources.h Sources.cpp
    MediaImport.h MediaImport.cpp
)

target_include_directories(movix_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(movix_engine PUBLIC movix_core movix_color movix_media)
