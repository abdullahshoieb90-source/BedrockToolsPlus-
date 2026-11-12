#pragma once

#include "modules/Module.hpp"
#include "customcapes_files.hpp"
#include <bedrocktools/sdk/offsets/Skin.hpp>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

// Custom Capes injects a PNG into the local player's SerializedSkin cape
// image. It uses Bedrock's own cape model/animation rather than drawing a
// second world-space plane, and never sends the image or cape id to a server.
class CustomCapesModule : public Module {
public:
    CustomCapesModule();
    ~CustomCapesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void onLocalPlayerTick(void* player);
    const std::string& capesDirectory() const { return m_dir; }

private:
    struct CapePixels {
        int width = 0;
        int height = 0;
        std::vector<unsigned char> rgba;
        bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
    };

    void ensureCapesDirectory();
    void refreshFiles();
    void select(const customcapes::Selection& selection);
    std::string selectedPath() const;
    bool loadSelectedCape();
    void applyToPlayer(void* player);
    void applyImage(void* skinImpl);
    void restoreAppliedCape();
    void clearTrackedCape();

    std::string m_dir;
    std::vector<std::string> m_files;
    int m_selectedIndex = 0;
    std::string m_selectedId = customcapes::kDefaultId;
    bool m_dirty = true;

    std::string m_loadedId;
    CapePixels m_loadedPixels;

    void* m_appliedSkinImpl = nullptr;
    std::array<unsigned char, bedrocktools::sdk::offsets::Image::Size> m_originalImage{};
    std::string m_originalCapeId;
    void* m_installedBlob = nullptr;
};
