find_package(Catch2 3 CONFIG REQUIRED)
find_program(FFMPEG_EXE ffmpeg HINTS "${FFMPEG_ROOT}/bin" REQUIRED)

set(MOVIX_TEST_MEDIA_DIR "${CMAKE_CURRENT_BINARY_DIR}/media")

add_executable(movix_tests
    main.cpp
    TestMedia.h
    test_color_matrix.cpp
    test_probe.cpp
    test_video_decoder.cpp
    test_audio_decoder.cpp
    test_av_sync.cpp
    test_core_edits.cpp
    test_core_history.cpp
    test_core_file.cpp
    test_core_f2.cpp
    test_f2_media.cpp
    test_f3.cpp
    test_f3_more.cpp
    test_compound.cpp
    test_hdr.cpp
    test_engine.cpp
    test_render.cpp
    test_export.cpp
    test_timeline_player.cpp
    test_models.cpp
    test_ai.cpp
    test_ai_audio.cpp
    test_ai_media.cpp
    test_ai_onnx.cpp
    test_ai_vision.cpp
    test_updates.cpp
    test_otio.cpp
    test_multicam.cpp
    test_templates.cpp
    test_scripts.cpp
    test_ofx.cpp
    test_speed.cpp
    ${PROJECT_SOURCE_DIR}/src/app/TimelineModels.h ${PROJECT_SOURCE_DIR}/src/app/TimelineModels.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ThumbnailProvider.h ${PROJECT_SOURCE_DIR}/src/app/ThumbnailProvider.cpp
    # Controladores reales para las pruebas de integración de la IA (sin interfaz).
    ${PROJECT_SOURCE_DIR}/src/app/ProjectController.h ${PROJECT_SOURCE_DIR}/src/app/ProjectController.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ProjectControllerColor.cpp ${PROJECT_SOURCE_DIR}/src/app/ProjectControllerOfx.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ExportController.h ${PROJECT_SOURCE_DIR}/src/app/ExportController.cpp
    ${PROJECT_SOURCE_DIR}/src/app/AiController.h ${PROJECT_SOURCE_DIR}/src/app/AiController.cpp
    ${PROJECT_SOURCE_DIR}/src/app/UpdateController.h ${PROJECT_SOURCE_DIR}/src/app/UpdateController.cpp
    ${PROJECT_SOURCE_DIR}/src/app/TemplateStore.h ${PROJECT_SOURCE_DIR}/src/app/TemplateStore.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ScriptHost.h ${PROJECT_SOURCE_DIR}/src/app/ScriptHost.cpp
    test_ai_app.cpp
    ${PROJECT_SOURCE_DIR}/cmake/utf8.manifest
)
target_link_libraries(movix_tests PRIVATE movix_media movix_core movix_color movix_engine movix_render movix_export movix_playback movix_ai Qt6::Test Catch2::Catch2 $<$<PLATFORM_ID:Windows>:winmm>)
target_include_directories(movix_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(movix_tests PRIVATE Qt6::Quick)
# Los scripts incluidos, como en la aplicación (":/scripts/*.js").
qt_add_resources(movix_tests movix_test_scripts PREFIX / BASE ${PROJECT_SOURCE_DIR}/src/app
    FILES ${PROJECT_SOURCE_DIR}/src/app/scripts/cortar-en-marcadores.js ${PROJECT_SOURCE_DIR}/src/app/scripts/quitar-huecos.js
          ${PROJECT_SOURCE_DIR}/src/app/scripts/marcadores-cada-10-s.js ${PROJECT_SOURCE_DIR}/src/app/scripts/titulos-en-marcadores.js
          ${PROJECT_SOURCE_DIR}/src/app/scripts/musica-de-fondo.js)
# Modelos de IA de las pruebas: compartidos entre configuraciones (se descargan una vez).
set(MOVIX_TEST_MODELS_DIR "${PROJECT_SOURCE_DIR}/build/test-models" CACHE PATH "Modelos de IA para las pruebas")
target_compile_definitions(movix_tests PRIVATE
    MOVIX_TEST_MEDIA_DIR="${MOVIX_TEST_MEDIA_DIR}"
    MOVIX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    MOVIX_TEST_MODELS_DIR="${MOVIX_TEST_MODELS_DIR}"
    MOVIX_AI_WORKER="$<TARGET_FILE:movix-ai-worker>")
add_dependencies(movix_tests movix-ai-worker)

# Plugin OpenFX de prueba, empaquetado como los de terceros (Nombre.ofx.bundle/Contents/Win64/Nombre.ofx).
find_package(unofficial-openfx CONFIG REQUIRED)
add_library(MovixTestGain MODULE ofx/MovixTestGain.cpp)
target_link_libraries(MovixTestGain PRIVATE unofficial::openfx::OfxSupport)
target_compile_definitions(MovixTestGain PRIVATE _HAS_AUTO_PTR_ETC=1) # la biblioteca de apoyo de OFX usa std::auto_ptr
set_target_properties(MovixTestGain PROPERTIES PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ofx/MovixTestGain.ofx.bundle/Contents/Win64")
target_compile_definitions(movix_tests PRIVATE MOVIX_TEST_OFX_DIR="${CMAKE_CURRENT_BINARY_DIR}/ofx")
add_dependencies(movix_tests MovixTestGain)

# Las DLL de FFmpeg junto al ejecutable de pruebas.
add_custom_command(TARGET movix_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} $<TARGET_FILE_DIR:movix_tests>)

add_test(NAME generate_test_media
    COMMAND ${CMAKE_COMMAND} -DFFMPEG_EXE=${FFMPEG_EXE} -DOUT_DIR=${MOVIX_TEST_MEDIA_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/GenerateTestMedia.cmake)
set_tests_properties(generate_test_media PROPERTIES FIXTURES_SETUP test_data)
add_test(NAME download_test_models
    COMMAND ${CMAKE_COMMAND} -DOUT_DIR=${MOVIX_TEST_MODELS_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/DownloadTestModels.cmake)
set_tests_properties(download_test_models PROPERTIES FIXTURES_SETUP test_data TIMEOUT 1800)

include(Catch)
catch_discover_tests(movix_tests
    DISCOVERY_MODE PRE_TEST
    DL_PATHS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
    # Un solo valor: una lista aquí se desarma en pares propiedad/valor (Catch2 ya añade
    # SKIP_RETURN_CODE para las pruebas que se saltan).
    PROPERTIES FIXTURES_REQUIRED test_data)

