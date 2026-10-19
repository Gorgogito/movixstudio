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
    test_engine.cpp
    test_render.cpp
    test_export.cpp
    test_timeline_player.cpp
    test_models.cpp
    ${PROJECT_SOURCE_DIR}/src/app/TimelineModels.h ${PROJECT_SOURCE_DIR}/src/app/TimelineModels.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ThumbnailProvider.h ${PROJECT_SOURCE_DIR}/src/app/ThumbnailProvider.cpp
    ${PROJECT_SOURCE_DIR}/cmake/utf8.manifest
)
target_link_libraries(movix_tests PRIVATE movix_media movix_core movix_engine movix_render movix_export movix_playback Qt6::Test Catch2::Catch2 $<$<PLATFORM_ID:Windows>:winmm>)
target_include_directories(movix_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(movix_tests PRIVATE Qt6::Quick)
target_compile_definitions(movix_tests PRIVATE MOVIX_TEST_MEDIA_DIR="${MOVIX_TEST_MEDIA_DIR}")

# Las DLL de FFmpeg junto al ejecutable de pruebas.
add_custom_command(TARGET movix_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} $<TARGET_FILE_DIR:movix_tests>)

add_test(NAME generate_test_media
    COMMAND ${CMAKE_COMMAND} -DFFMPEG_EXE=${FFMPEG_EXE} -DOUT_DIR=${MOVIX_TEST_MEDIA_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/GenerateTestMedia.cmake)
set_tests_properties(generate_test_media PROPERTIES FIXTURES_SETUP test_media)

include(Catch)
catch_discover_tests(movix_tests
    DISCOVERY_MODE PRE_TEST
    DL_PATHS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
    PROPERTIES FIXTURES_REQUIRED test_media)
