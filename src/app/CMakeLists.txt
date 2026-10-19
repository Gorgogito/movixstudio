qt_add_executable(MovixStudio WIN32
    main.cpp
    LanguageManager.h LanguageManager.cpp
    PlayerForeign.h
    VideoView.h VideoView.cpp
    ScopeFeed.h
    ScopeView.h ScopeView.cpp
    EffectNames.h
    ColorWheelProvider.h
    ThumbnailProvider.h ThumbnailProvider.cpp
    Waveform.h Waveform.cpp
    TimelineModels.h TimelineModels.cpp
    ProjectController.h ProjectController.cpp ProjectControllerColor.cpp
    ExportController.h ExportController.cpp
    AiController.h AiController.cpp
    ${PROJECT_SOURCE_DIR}/cmake/utf8.manifest
)
target_compile_definitions(MovixStudio PRIVATE MOVIX_VERSION="${PROJECT_VERSION}")
if(MSVC)
    # C4702 (código inalcanzable) proviene de plantillas de Qt instanciadas en Debug.
    target_compile_options(MovixStudio PRIVATE /wd4702)
endif()

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

set(MOVIX_ICONS
    logo play pause step-back step-forward skip-start skip-end volume volume-mute folder-open
    scissors trash undo redo magnet plus eye eye-off lock lock-open zoom-in zoom-out fit film music
    export import settings save)
list(TRANSFORM MOVIX_ICONS PREPEND "icons/")
list(TRANSFORM MOVIX_ICONS APPEND ".svg")

qt_add_qml_module(MovixStudio
    URI MovixStudio
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Format.js
        qml/IconButton.qml
        qml/MxSlider.qml
        qml/InfoRow.qml
        qml/Banner.qml
        qml/TransportBar.qml
        qml/ViewerPanel.qml
        qml/MediaBin.qml
        qml/Inspector.qml
        qml/Timeline.qml
        qml/TimeRuler.qml
        qml/TrackHeader.qml
        qml/TimelineClip.qml
        qml/ExportDialog.qml
        qml/ProjectSettingsDialog.qml
        qml/PropertyRow.qml
        qml/LibraryPanel.qml
        qml/TransformOverlay.qml
        qml/ScopesPanel.qml
        qml/ColorPanel.qml
        qml/ColorWheel.qml
        qml/CurveEditor.qml
        qml/EffectsPanel.qml
        qml/SliderRow.qml
        qml/SubtitlesDialog.qml
        qml/ModelsDialog.qml
        qml/AiStatus.qml
        qml/TranscriptPanel.qml
        qml/SilenceDialog.qml
        qml/TtsDialog.qml
        qml/TranslateDialog.qml
    RESOURCES
        ${MOVIX_ICONS}
)

target_link_libraries(MovixStudio PRIVATE
    movix_core movix_engine movix_render movix_export movix_playback movix_ai
    Qt6::Gui Qt6::GuiPrivate Qt6::Quick Qt6::QuickControls2)

# Español = idioma fuente (cadenas del código); inglés = traducción.
qt_add_translations(MovixStudio
    TS_FILES ${PROJECT_SOURCE_DIR}/i18n/movixstudio_en.ts
    SOURCE_TARGETS MovixStudio movix_playback movix_core movix_color movix_export movix_render movix_ai
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none
)

# Para ejecutar desde el directorio de compilación: DLL de FFmpeg junto al ejecutable.
add_custom_command(TARGET MovixStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} $<TARGET_FILE_DIR:MovixStudio>)

# Instalación autocontenida (cmake --install): ejecutable, Qt, plugins QML y FFmpeg en una
# sola carpeta plana, como es habitual en Windows (CMAKE_INSTALL_BINDIR = "." en el nivel superior).
install(TARGETS MovixStudio RUNTIME DESTINATION .)
install(FILES ${FFMPEG_RUNTIME_DLLS} DESTINATION .)
# DLL de las bibliotecas de vcpkg (OpenColorIO y sus dependencias).
install(FILES $<TARGET_RUNTIME_DLLS:MovixStudio> DESTINATION .)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION .)
qt_generate_deploy_qml_app_script(TARGET MovixStudio OUTPUT_SCRIPT deploy_script NO_TRANSLATIONS)
install(SCRIPT ${deploy_script})
