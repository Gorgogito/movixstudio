#pragma once

#include <filesystem>
#include <string>

namespace movix::test {

// Ruta UTF-8 a un medio generado por GenerateTestMedia.cmake.
inline std::string mediaPath(const char* name)
{
    return (std::filesystem::path(MOVIX_TEST_MEDIA_DIR) / name).string();
}

} // namespace movix::test
