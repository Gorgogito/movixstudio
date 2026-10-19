# eSpeak NG compilado con su CMake: biblioteca compartida sin audio (solo fonetización y síntesis a
# memoria) y los datos de las voces (espeak-ng-data), que se generan con el propio ejecutable.
set(VCPKG_BUILD_TYPE release)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO espeak-ng/espeak-ng
    REF "${VERSION}"
    SHA512 28793503f2be00505c3fe1dada8197a4e2d3af0d143e31ef883130fe2685d048ae9dee8efab637cdbe972461d95e210d2277b8e8fed4ae580d0cca8b505b0662
    HEAD_REF master
)

# Sin libsonic no hace falta descargarlo (vcpkg no permite descargas durante la compilación).
vcpkg_replace_string("${SOURCE_PATH}/cmake/deps.cmake" "if (SONIC_LIB AND SONIC_INC)" "if (NOT USE_LIBSONIC)
elseif (SONIC_LIB AND SONIC_INC)")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED_LIBS=ON
        -DUSE_ASYNC=OFF
        -DUSE_MBROLA=OFF
        -DUSE_LIBSONIC=OFF
        -DUSE_LIBPCAUDIO=OFF
        -DUSE_KLATT=OFF
        -DUSE_SPEECHPLAYER=OFF
)
vcpkg_cmake_install()

# Los datos van en share/espeak-ng/espeak-ng-data (convención de vcpkg: share/<puerto>).
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(RENAME "${CURRENT_PACKAGES_DIR}/share/espeak-ng-data" "${CURRENT_PACKAGES_DIR}/share/espeak-ng/espeak-ng-data")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/vim")
# El ejecutable de línea de órdenes no hace falta.
file(REMOVE "${CURRENT_PACKAGES_DIR}/bin/espeak-ng.exe")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/tools")

file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" "espeak-ng: find_path(ESPEAKNG_INCLUDE_DIR espeak-ng/speak_lib.h) y find_library(ESPEAKNG_LIBRARY espeak-ng)\n")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
