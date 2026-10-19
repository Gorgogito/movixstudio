# movix-ai-worker: proceso aparte que ejecuta los trabajos de IA (ver ai/WorkerProtocol.h).
add_executable(movix-ai-worker main.cpp ${PROJECT_SOURCE_DIR}/cmake/utf8.manifest)
target_link_libraries(movix-ai-worker PRIVATE movix_ai movix_core movix_media)

# Las variantes del backend de CPU de ggml (ggml-cpu-haswell.dll...) se cargan en tiempo de
# ejecución: el toolchain de vcpkg no las copia porque nadie las enlaza.
set(MOVIX_VCPKG_BIN "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MOVIX_VCPKG_BIN "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
endif()
file(GLOB MOVIX_GGML_BACKENDS "${MOVIX_VCPKG_BIN}/ggml-cpu-*.dll")

# DLL de FFmpeg, de ONNX Runtime y backends de ggml junto al ejecutable (whisper, ggml, eSpeak y
# SentencePiece los copia el toolchain de vcpkg), y los datos de eSpeak NG.
add_custom_command(TARGET movix-ai-worker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} ${MOVIX_GGML_BACKENDS} ${ONNXRUNTIME_RUNTIME_DLLS}
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/espeak-ng.dll" # solo hay versión Release
            $<TARGET_FILE_DIR:movix-ai-worker>
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${MOVIX_ESPEAK_DATA} $<TARGET_FILE_DIR:movix-ai-worker>/espeak-ng-data)

install(TARGETS movix-ai-worker RUNTIME DESTINATION .)
file(GLOB MOVIX_WHISPER_DLLS "${MOVIX_VCPKG_BIN}/whisper.dll" "${MOVIX_VCPKG_BIN}/ggml*.dll"
                             "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/espeak-ng.dll")
install(FILES ${MOVIX_WHISPER_DLLS} ${ONNXRUNTIME_RUNTIME_DLLS} DESTINATION .)
install(DIRECTORY ${MOVIX_ESPEAK_DATA} DESTINATION .)
install(FILES ${ONNXRUNTIME_LICENSE_FILES} DESTINATION licenses/onnxruntime)
