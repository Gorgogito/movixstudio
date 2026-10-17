# movix_playback: reproducción en tiempo real (hilos de decodificación, salida de audio, reloj A/V).
add_library(movix_playback STATIC
    AudioOutput.h AudioOutput.cpp
    Player.h Player.cpp
)

target_include_directories(movix_playback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(movix_playback PRIVATE ${MINIAUDIO_INCLUDE_DIR})
target_link_libraries(movix_playback PUBLIC movix_media Qt6::Core)
