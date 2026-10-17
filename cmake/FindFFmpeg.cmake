# Localiza las bibliotecas compartidas de FFmpeg y crea targets importados FFmpeg::<componente>.
#
# Variables de entrada:
#   FFMPEG_ROOT  Directorio con include/, lib/ y bin/ (p. ej. un build "shared" de BtbN).
#
# Targets creados: FFmpeg::avformat, FFmpeg::avcodec, FFmpeg::avutil, ...

set(_ffmpeg_hints "${FFMPEG_ROOT}" "$ENV{FFMPEG_ROOT}")

find_path(FFMPEG_INCLUDE_DIR libavutil/avutil.h HINTS ${_ffmpeg_hints} PATH_SUFFIXES include)

foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
    find_library(FFMPEG_${_comp}_LIBRARY NAMES ${_comp} HINTS ${_ffmpeg_hints} PATH_SUFFIXES lib)
    file(GLOB _dll "${FFMPEG_INCLUDE_DIR}/../bin/${_comp}-*.dll")
    if(_dll)
        list(GET _dll 0 FFMPEG_${_comp}_DLL)
    endif()

    if(FFMPEG_${_comp}_LIBRARY AND FFMPEG_${_comp}_DLL)
        set(FFmpeg_${_comp}_FOUND TRUE)
        if(NOT TARGET FFmpeg::${_comp})
            add_library(FFmpeg::${_comp} SHARED IMPORTED)
            set_target_properties(FFmpeg::${_comp} PROPERTIES
                IMPORTED_IMPLIB "${FFMPEG_${_comp}_LIBRARY}"
                IMPORTED_LOCATION "${FFMPEG_${_comp}_DLL}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
        endif()
    endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg REQUIRED_VARS FFMPEG_INCLUDE_DIR HANDLE_COMPONENTS)
