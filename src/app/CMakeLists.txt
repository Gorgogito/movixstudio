qt_add_executable(MovixStudio WIN32
    main.cpp
    LanguageManager.h LanguageManager.cpp
    PlayerForeign.h
    VideoView.h VideoView.cpp
    ${PROJECT_SOURCE_DIR}/cmake/utf8.manifest
)
target_compile_definitions(MovixStudio PRIVATE MOVIX_VERSION="${PROJECT_VERSION}")
if(MSVC)
    # C4702 (código inalcanzable) proviene de plantillas de Qt instanciadas en Debug.
    target_compile_options(MovixStudio PRIVATE /wd4702)
endif()

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(MovixStudio
    URI MovixStudio
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/TransportBar.qml
        qml/InfoPanel.qml
        qml/IconButton.qml
        qml/InfoRow.qml
        qml/MxSlider.qml
        qml/Format.js
    RESOURCES
        icons/logo.svg
        icons/play.svg
        icons/pause.svg
        icons/step-back.svg
        icons/step-forward.svg
        icons/skip-start.svg
        icons/skip-end.svg
        icons/volume.svg
        icons/volume-mute.svg
        icons/folder-open.svg
)

qt_add_shaders(MovixStudio "video_shaders"
    PREFIX "/"
    FILES
        shaders/video.vert
        shaders/video_nv12.frag
)

target_link_libraries(MovixStudio PRIVATE movix_playback Qt6::Gui Qt6::GuiPrivate Qt6::Quick Qt6::QuickControls2)

# Español = idioma fuente (cadenas del código); inglés = traducción.
qt_add_translations(MovixStudio
    TS_FILES ${PROJECT_SOURCE_DIR}/i18n/movixstudio_en.ts
    SOURCE_TARGETS MovixStudio movix_playback
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none
)

# Para ejecutar desde el directorio de compilación: DLL de FFmpeg junto al ejecutable.
add_custom_command(TARGET MovixStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:FFmpeg::avformat> $<TARGET_FILE:FFmpeg::avcodec> $<TARGET_FILE:FFmpeg::avutil>
        $<TARGET_FILE:FFmpeg::swscale> $<TARGET_FILE:FFmpeg::swresample>
        $<TARGET_FILE_DIR:MovixStudio>)

# Instalación autocontenida (cmake --install): ejecutable, Qt, plugins QML y FFmpeg en una
# sola carpeta plana, como es habitual en Windows (CMAKE_INSTALL_BINDIR = "." en el nivel superior).
install(TARGETS MovixStudio RUNTIME DESTINATION .)
install(FILES
    $<TARGET_FILE:FFmpeg::avformat> $<TARGET_FILE:FFmpeg::avcodec> $<TARGET_FILE:FFmpeg::avutil>
    $<TARGET_FILE:FFmpeg::swscale> $<TARGET_FILE:FFmpeg::swresample>
    DESTINATION .)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION .)
qt_generate_deploy_qml_app_script(TARGET MovixStudio OUTPUT_SCRIPT deploy_script NO_TRANSLATIONS)
install(SCRIPT ${deploy_script})
