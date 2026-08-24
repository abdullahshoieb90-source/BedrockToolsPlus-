#pragma once

#include "modules/Module.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Wings
//
// Wears a custom "4D" skin on the local player: a Bedrock geometry file plus
// the RGBA texture it samples. When the module is enabled it reads
// `<configDir>/wings/wings_geometry.json` and `<configDir>/wings/wings.png`
// (the same directory that holds config.json — typically
// `/sdcard/games/BedrockTools/wings` on Android) and injects both into the
// local player's SerializedSkinImpl:
//
//   * mSkinImage            -> the decoded wings.png pixels (the engine's
//                              model renders this texture through the custom
//                              geometry's UVs).
//   * mGeometryData         -> the raw contents of wings_geometry.json.
//   * mDefaultGeometryName  -> the geometry identifier parsed from the JSON
//                              (e.g. "geometry.wings"), which makes the game
//                              pick the custom model instead of the default
//                              humanoid one.
//
// The module is tick driven exactly like CustomCapes: the local-player tick
// re-resolves a fresh skin object every frame, so a skin that the engine
// rebuilds afterwards is patched again on the next tick, and the injected
// pixel blob is handed to the engine with free() as its deleter. The original
// skin image, geometry data and default-geometry name are backed up and
// restored when the module is disabled or the assets are removed.
//
// Expected geometry format: a Bedrock skin pack geometry JSON, i.e. a
// `format_version` object containing either `"minecraft:geometry"` or
// `"geometry"` as an array whose first entry carries
// `description.identifier`, `description.texture_width` and
// `description.texture_height`. The JSON must define the standard player bones
// (head/body/rightArm/leftArm/rightLeg/leftLeg) for the body to keep
// rendering, plus any extra bones for the wings. The texture should be the
// full skin canvas (e.g. 64x64 or 128x128) with the wing artwork baked into
// the UV region referenced by those bones.
//
// Memory safety follows the CustomCapes module: every patch happens inside the
// local-player tick so only a freshly resolved, live skin object is touched;
// if the engine replaces the skin object it destroys our previous blob through
// the deleter we installed, so no dangling pointers are left behind. Persona
// skins render through the persona/animated-image pipeline and are left
// untouched.
class WingsModule : public Module {
public:
    WingsModule();
    ~WingsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the LocalPlayerTickEvent subscription.
    void onLocalPlayerTick(void* player);

    // Directory the module watches; exposed for the menu description.
    const std::string& wingsDirectory() const { return m_wingsDir; }

private:
    void loadWingsAssets();
    void releaseWingsAssets();

    bool applyWings(void* skin);
    void restoreOriginalSkin(void* skin);
    void clearPatchState();

    std::string m_wingsDir;
    bool m_assetsLoaded = false;
    bool m_loadFailed = false;
    int m_retryTicks = 0;

    // The loaded assets (module-owned).
    std::string m_geometryData;
    std::string m_defaultGeometryName;
    std::vector<std::uint8_t> m_texturePixels; // RGBA8
    int m_textureWidth = 0;
    int m_textureHeight = 0;

    // State of the in-game skin patch.
    void* m_patchedSkin = nullptr;  // SerializedSkinImpl* currently patched
    void* m_injectedBlob = nullptr; // pixel buffer currently handed to the game
    bool m_needsApply = true;

    // Backup of the skin fields we overwrite so disabling can restore the
    // vanilla skin exactly.
    struct SkinBackup {
        std::uint32_t format = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t depth = 0;
        std::uint32_t usage = 0;
        void* blob = nullptr;      // original pixel pointer (detached, kept alive)
        void* deleter = nullptr;   // original mce::Blob deleter
        std::size_t size = 0;
        std::string geometryData;
        std::string defaultGeometryName;
    };
    SkinBackup m_backup;
    bool m_hasBackup = false;
};
