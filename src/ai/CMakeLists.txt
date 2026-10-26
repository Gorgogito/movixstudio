# movix_ai: IA local. Catálogo y descarga de modelos, cliente del proceso de IA y los algoritmos
# que ese proceso ejecuta: whisper.cpp (transcripción, detección de voz), ONNX Runtime (texto a voz,
# traducción, separación de voces), eSpeak NG (fonemas), SentencePiece y DSP propio (ritmo, escenas,
# sincronización por audio).
find_package(whisper CONFIG REQUIRED)
# SentencePiece (vcpkg) es estática y sin configuración CMake: depende de protobuf-lite y abseil.
find_package(protobuf CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_library(SENTENCEPIECE_LIBRARY sentencepiece REQUIRED)
find_path(ESPEAKNG_INCLUDE_DIR espeak-ng/speak_lib.h REQUIRED)
find_library(ESPEAKNG_LIBRARY espeak-ng REQUIRED)

add_library(movix_ai STATIC
    ModelCatalog.h ModelCatalog.cpp
    ModelDownloader.h ModelDownloader.cpp
    WorkerProtocol.h
    AiWorkerClient.h AiWorkerClient.cpp
    Speech.h Speech.cpp
    Scenes.h Scenes.cpp
    Beats.h Beats.cpp
    AudioSync.h AudioSync.cpp
    Onnx.h Onnx.cpp
    OnnxRewrite.h OnnxRewrite.cpp
    Tts.h Tts.cpp
    Translate.h Translate.cpp
    Separation.h Separation.cpp
    Vision.h Vision.cpp
    VideoAi.h VideoAi.cpp
    Tracking.h Tracking.cpp
)

target_include_directories(movix_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(movix_ai PRIVATE ${ESPEAKNG_INCLUDE_DIR})
target_link_libraries(movix_ai
    PUBLIC movix_core movix_media Qt6::Core Qt6::Network onnxruntime::onnxruntime
    PRIVATE whisper ${ESPEAKNG_LIBRARY})
target_link_libraries(movix_ai PRIVATE ${SENTENCEPIECE_LIBRARY} protobuf::libprotobuf-lite absl::flags absl::flags_marshalling absl::flags_reflection)

# Datos de eSpeak NG (fonemas y diccionarios de todos los idiomas): van junto al ejecutable.
set(MOVIX_ESPEAK_DATA "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/espeak-ng/espeak-ng-data" CACHE INTERNAL "")
