#pragma once

// Filesystem and launcher-radio helpers for Custom Capes. Kept free of game
// types so discovery and config migration can be tested on the host.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace customcapes {

inline constexpr const char* kDefaultLabel = "Default Cape";
inline constexpr const char* kNoneLabel = "No Cape";
inline constexpr const char* kDefaultId = "default";
inline constexpr const char* kNoneId = "none";

inline bool hasPngExtension(const std::string& name) {
    if (name.size() < 5) return false;
    std::string extension = name.substr(name.size() - 4);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".png";
}

inline bool isUsableCapeFileName(const std::string& name) {
    // The launcher radio uses commas as separators. Also exclude path
    // separators so a config value can never escape the capes directory.
    return hasPngExtension(name) && name.find(',') == std::string::npos &&
           name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
           name != "." && name != "..";
}

inline std::vector<std::string> scanCapeFiles(const std::string& directory) {
    std::vector<std::string> files;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) return files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (isUsableCapeFileName(name)) files.push_back(name);
    }
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(), [](unsigned char ac, unsigned char bc) {
                return std::tolower(ac) < std::tolower(bc);
            });
    });
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

inline std::string makeRadioValue(int selectedIndex, const std::vector<std::string>& files) {
    const int optionCount = static_cast<int>(files.size()) + 2;
    selectedIndex = std::clamp(selectedIndex, 0, optionCount - 1);
    std::string value = std::to_string(selectedIndex) + "," + kDefaultLabel + "," + kNoneLabel;
    for (const auto& file : files) value += "," + file;
    return value;
}

struct Selection {
    int index = 0; // 0 = vanilla, 1 = hidden, >=2 = files[index - 2]
    std::string id = kDefaultId;
};

inline Selection selectionForId(const std::string& id, const std::vector<std::string>& files) {
    if (id == kNoneId) return {1, kNoneId};
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i] == id) return {static_cast<int>(i) + 2, id};
    }
    return {};
}

// Accepts all values the launcher and old/hand-edited configs can produce:
// a full radio string, a bare numeric option index, or a bare file/id name.
inline Selection parseSelection(const std::string& value, const std::vector<std::string>& files) {
    if (value.empty()) return {};

    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = value.find(',', start);
        tokens.push_back(value.substr(start, comma == std::string::npos ? comma : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    const bool numeric = !tokens[0].empty() && std::all_of(tokens[0].begin(), tokens[0].end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
    if (!numeric) return selectionForId(tokens[0], files);

    int index = 0;
    try {
        index = std::stoi(tokens[0]);
    } catch (...) {
        return {};
    }
    if (index <= 0) return {};
    if (index == 1) return {1, kNoneId};

    // Prefer the name embedded in a full radio value. This preserves the same
    // cape when adding a file changes its alphabetical position.
    if (tokens.size() > 2 && index < static_cast<int>(tokens.size()) - 1) {
        const Selection named = selectionForId(tokens[index + 1], files);
        if (named.index >= 2) return named;
    }
    const int fileIndex = index - 2;
    if (fileIndex >= 0 && fileIndex < static_cast<int>(files.size())) {
        return {index, files[static_cast<std::size_t>(fileIndex)]};
    }
    return {};
}

} // namespace customcapes
