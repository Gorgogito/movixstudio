# Distribución en Windows: instalación autocontenida (cmake --install) y paquetes con CPack
# (cpack -C RelWithDebInfo en build/release): instalador MSI (WiX 4 o posterior) y ZIP portable.
#
# El instalador MSI necesita la herramienta de WiX y su extensión de interfaz:
#   dotnet tool install --global wix
#   wix extension add --global WixToolset.UI.wixext/<versión de wix>

if(NOT WIN32)
    return()
endif()

# --- Dependencias en tiempo de ejecución ----------------------------------------------------------
# Las DLL que enlazan los ejecutables (y las que enlazan esas, como las de OpenColorIO) se resuelven
# al instalar; Qt la despliega windeployqt (ver src/app/CMakeLists.txt) y la biblioteca de ejecución
# de MSVC, InstallRequiredSystemLibraries. Los complementos que se cargan en tiempo de ejecución
# (backends de ggml, proveedores de ONNX Runtime) se instalan aparte y se analizan como bibliotecas.
set(_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
endif()
list(GET FFMPEG_RUNTIME_DLLS 0 _ffmpeg_dll)
cmake_path(GET _ffmpeg_dll PARENT_PATH _ffmpeg_bin)
file(GLOB _runtime_plugins "${_vcpkg_bin}/ggml-cpu-*.dll" "${_vcpkg_bin}/ggml-vulkan.dll")
list(APPEND _runtime_plugins ${ONNXRUNTIME_RUNTIME_DLLS})
set(_search_dirs "${_vcpkg_bin}" "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin" "${_ffmpeg_bin}")

install(CODE "
    if(POLICY CMP0207)
        cmake_policy(SET CMP0207 NEW) # normalizar las rutas antes de aplicar las exclusiones
    endif()
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES \"$<TARGET_FILE:MovixStudio>\" \"$<TARGET_FILE:movix-ai-worker>\"
        LIBRARIES ${_runtime_plugins}
        DIRECTORIES ${_search_dirs}
        PRE_EXCLUDE_REGEXES \"^[Aa][Pp][Ii]-[Mm][Ss]-\" \"^[Ee][Xx][Tt]-[Mm][Ss]-\" \"^[Qq][Tt]6\"
        POST_EXCLUDE_REGEXES \"[/\\\\\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\\\\\]\"
        RESOLVED_DEPENDENCIES_VAR _movix_deps
        UNRESOLVED_DEPENDENCIES_VAR _movix_missing
        CONFLICTING_DEPENDENCIES_PREFIX _movix_conflict)
    if(_movix_missing)
        message(FATAL_ERROR \"DLL sin resolver: \${_movix_missing}\")
    endif()
    # La misma DLL junto al ejecutable compilado y en su carpeta de origen: son copias, vale la primera.
    foreach(_name IN LISTS _movix_conflict_FILENAMES)
        list(GET _movix_conflict_\${_name} 0 _first)
        list(APPEND _movix_deps \"\${_first}\")
    endforeach()
    file(INSTALL \${_movix_deps} DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")
")
install(FILES ${_runtime_plugins} DESTINATION .)

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE) # la UCRT forma parte de Windows 10 y posteriores
include(InstallRequiredSystemLibraries)

# --- Licencias -----------------------------------------------------------------------------------
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/packaging/AVISOS_DE_TERCEROS.txt" DESTINATION .)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/licenses/LGPL-3.0.txt" DESTINATION licenses/qt)
install(FILES "${_ffmpeg_bin}/../LICENSE.txt" DESTINATION licenses/ffmpeg)
install(FILES ${ONNXRUNTIME_LICENSE_FILES} DESTINATION licenses/onnxruntime)
install(FILES ${ONNXRUNTIME_WEBGPU_LICENSE_FILES} DESTINATION licenses/onnxruntime-webgpu)
foreach(_port abseil bzip2 espeak-ng expat ggml imath liblzma miniaudio minizip-ng opencolorio protobuf
              pystring sentencepiece vulkan-loader whisper-cpp yaml-cpp zlib zstd)
    set(_copyright "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${_port}/copyright")
    if(NOT EXISTS "${_copyright}")
        message(FATAL_ERROR "Falta la licencia de ${_port}: ${_copyright}")
    endif()
    install(FILES "${_copyright}" DESTINATION licenses/${_port})
endforeach()

# --- Paquetes ------------------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "MovixStudio")
set(CPACK_PACKAGE_VENDOR "Colaboradores de MovixStudio")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "MovixStudio")
set(CPACK_PACKAGE_FILE_NAME "MovixStudio-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_EXECUTABLES "MovixStudio" "MovixStudio")
set(CPACK_CREATE_DESKTOP_LINKS "MovixStudio")
# WiX solo acepta la licencia como .txt o .rtf.
configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_BINARY_DIR}/LICENSE.txt")
set(CPACK_GENERATOR WIX ZIP)

set(CPACK_WIX_VERSION 4)
# Identifica el producto entre versiones (actualización mayor): no debe cambiar nunca.
set(CPACK_WIX_UPGRADE_GUID "79140C40-00C2-4EA5-9FAA-7D254802DCB1")
set(CPACK_WIX_ARCHITECTURE x64)
set(CPACK_WIX_INSTALL_SCOPE perMachine)
set(CPACK_WIX_CULTURES es-ES)
set(CPACK_WIX_PRODUCT_ICON "${PROJECT_SOURCE_DIR}/src/app/icons/movixstudio.ico")
# Imágenes del asistente (generadas a partir de src/app/icons/logo.svg).
set(CPACK_WIX_UI_BANNER "${PROJECT_SOURCE_DIR}/packaging/wix-banner.bmp")
set(CPACK_WIX_UI_DIALOG "${PROJECT_SOURCE_DIR}/packaging/wix-dialog.bmp")
# Acceso directo directamente en el menú Inicio, sin carpeta ni acceso para desinstalar (como es
# habitual en Windows 11: se desinstala desde Configuración).
set(CPACK_WIX_PROGRAM_MENU_FOLDER ".")
set(CPACK_WIX_PATCH_FILE "${PROJECT_SOURCE_DIR}/packaging/wix-patch.xml")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "${PROJECT_HOMEPAGE_URL}")
set(CPACK_WIX_PROPERTY_ARPHELPLINK "${PROJECT_HOMEPAGE_URL}")

include(CPack)
