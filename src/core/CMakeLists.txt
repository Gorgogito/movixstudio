# movix_core: modelo del proyecto, ediciones, deshacer/rehacer, archivo de proyecto y evaluación
# de la línea de tiempo. Sin interfaz ni dependencias multimedia (solo Qt Core para JSON).
add_library(movix_core STATIC
    Animated.h Animated.cpp
    Project.h Project.cpp
    Edits.h Edits.cpp
    History.h History.cpp
    ProjectFile.h ProjectFile.cpp
    Timeline.h Timeline.cpp
)

target_include_directories(movix_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(movix_core PUBLIC Qt6::Core)
