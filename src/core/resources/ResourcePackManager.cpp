#include "ResourcePackManager.hpp"
#include <filesystem>
#include <system_error>

namespace bedrocktools::resources {

std::vector<std::string> scanResourcePacks(const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    std::filesystem::path path(dir);
    if (!std::filesystem::is_directory(path, ec)) return files;

    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
        if (entry.is_regular_file()) {
            const auto& ext = entry.path().extension().string();
            // الـ Launcher يدعم ملفات الـ Resource Pack بصيغة .zip أو .mcpack أو .mcaddon
            if (ext == ".zip" || ext == ".mcpack" || ext == ".mcaddon") {
                files.push_back(entry.path().filename().string());
            }
        }
    }
    return files;
}

} // namespace bedrocktools::resources
