cmake_minimum_required(VERSION 3.25)

project(MovixStudio
    VERSION 0.1.0
    DESCRIPTION "Editor de video de escritorio libre y de código abierto"
    HOMEPAGE_URL "https://github.com/Gorgogito/movixstudio"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(MOVIX_BUILD_TESTS "Compilar las pruebas automáticas" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# Instalación plana en Windows: ejecutable y DLL en la raíz de la carpeta de la aplicación.
if(WIN32)
    set(CMAKE_INSTALL_BINDIR ".")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Quick QuickControls2 ShaderTools LinguistTools Test)
qt_standard_project_setup(REQUIRES 6.8 I18N_SOURCE_LANGUAGE es I18N_TRANSLATED_LANGUAGES en)

find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale swresample avfilter)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
include(OnnxRuntime)

add_subdirectory(src)
include(Packaging)

if(MOVIX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
