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
    UpdateController.h UpdateController.cpp
    ${PROJECT_SOURCE_DIR}/cmake/utf8.manifest
)
if(WIN32)
    set(MOVIX_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/icons/movixstudio.ico")
    configure_file(movixstudio.rc.in movixstudio.rc @ONLY)
    target_sources(MovixStudio PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/movixstudio.rc)
endif()
# Versión publicada más reciente (API de GitHub); vacía = sin búsqueda de actualizaciones.
set(MOVIX_UPDATE_URL "https://api.github.com/repos/Gorgogito/movixstudio/releases/latest"
    CACHE STRING "Dirección de la versión más reciente (formato de la API de versiones de GitHub)")
target_compile_definitions(MovixStudio PRIVATE MOVIX_VERSION="${PROJECT_VERSION}" MOVIX_UPDATE_URL="${MOVIX_UPDATE_URL}")
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
        qml/UpdateBanner.qml
        qml/MulticamOverlay.qml
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

# Instalación autocontenida (cmake --install): ejecutable, Qt y plugins QML en una sola carpeta
# plana, como es habitual en Windows; el resto de las DLL y las licencias, en cmake/Packaging.cmake.
install(TARGETS MovixStudio RUNTIME DESTINATION .)
# Sin la biblioteca de ejecución (va junto al programa), sin compiladores de sombreadores de
# Direct3D (D3D11 usa d3dcompiler_47.dll, que ya trae Windows; dxil.dll es propietaria y solo
# serviría para D3D12), sin OpenGL por software y sin los complementos de depuración de QML.
qt_generate_deploy_qml_app_script(TARGET MovixStudio OUTPUT_SCRIPT deploy_script NO_TRANSLATIONS
    DEPLOY_TOOL_OPTIONS --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
                        --skip-plugin-types qmltooling)
install(SCRIPT ${deploy_script})
