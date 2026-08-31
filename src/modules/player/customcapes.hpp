#pragma once

#include "../Module.hpp"
#include "customcapes_ui.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Custom Capes
//
// Lets the player wear any PNG as a classic cape. The module owns a "capes"
// directory next to config.json (`<configDir>/capes`, created on first
// launch together with a sample cape); every .png file in it shows up as an
// option of the module's radio picker in the launcher mod menu.
//
// Instead of the launcher's text radio picker (which could only show file
// names like "cape13.png"), the module renders its own in-game picker:
// CustomCapesUi draws the "None" option plus every cape as a thumbnail card
// in a centered grid, with a bright gold border around the selected card.
// Each thumbnail is the UV-cropped outer cape face (x=1..11, y=1..17 of the
// 64x32 canvas), bilinearly upscaled and registered with the overlay's
// texture loader (pl::modmenu::registerImage) — the overlay's Image draw
// command stretches the whole bitmap, so cropping at texture-load time is
// what keeps the preview from showing the complete atlas. The radio config
// value ("m_cape") is still (de)serialized for persistence; the launcher
// menu simply does not register it as a Radio entry anymore (see
// src/launcher/ModuleMenu.cpp).
//
// The selected file is decoded with stb_image and resampled onto the
// classic-cape layout of the 64x32 canvas Minecraft uses: the image is
// painted onto the outer back face only (x=1..11, y=1..17), the inner
// front face is filled with a flat lining color instead of a repeat of the
// image, and the top/bottom/side edge strips are continued from the image's
// edge colors so the 1-voxel-thick cape mesh does not look flat. The design
// is also mapped onto the tapered Elytra UV area used by both wings. Exact
// 64x32 inputs keep manually-authored Elytra pixels; if that area is empty,
// the wing fallback is generated from the cape face. The result is written
// into the local player's SerializedSkinImpl::mCapeImage each tick. Modern game versions
// only render the classic cape when SerializedSkinImpl::mCapeId is
// non-empty, so a synthetic short-string id is written alongside the image
// and restored together with it. The blob handed to the
// game is malloc'd and tagged with free() as its deleter, so whatever the
// engine does with the image afterwards (move, destroy, skin rebuild) is
// memory-safe. The original cape is restored when the module is disabled or
// "None" is picked.
//
// Memory safety notes:
//   * all patching happens inside the local-player tick, so the only skin
//     object ever dereferenced is one freshly resolved from the live player
//     pointer — a previous skin object can never dangle.
//   * when the skin object is replaced by the game, the old object owns the
//     blob we injected (freed by the engine through our deleter), so no
//     cleanup is needed on our side.
//   * the cape offsets are version-specific and derived from the verified
//     skin offsets (see include/bedrocktools/sdk/offsets/Skin.hpp); a sanity
//     check on the live skin image aborts the patch if the layout shifts.
//   * persona skins go through the persona pipeline (no classic cape image),
//     so the module leaves them untouched.
//   * on Leave World the engine detaches the player from its level
//     (Actor::mLevel goes null) before the player and skin are freed; the
//     tick hook bails on a null player or a null level link and detaches
//     its engine references WITHOUT touching the skin — the engine frees
//     the injected blob through its deleter tag, so a teardown tick can
//     never write freed memory or double-free the blob.
class CustomCapesModule : public Module {
public:
    CustomCapesModule();
    ~CustomCapesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    bool onTouchEvent(float x, float y, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the LocalPlayerTickEvent subscription.
    void onLocalPlayerTick(void* player);

    // World-exit teardown: drops every engine reference (patched skin,
    // injected blob, backup) without writing to the skin or freeing the
    // blob — the engine owns both while it destroys the world. Idempotent;
    // the loaded cape file stays so the cape re-applies on rejoin.
    void onWorldExit();

    // Directory the module watches; exposed for the menu description.
    const std::string& capesDirectory() const { return m_capesDir; }

private:
    // True when the player pointer and its level link are both usable.
    // During Leave World the engine nulls Actor::mLevel before the player
    // and skin objects are freed, so this is the safe early-out for the
    // tick hook: a null level means the skin must not be touched.
    static bool playerHasLiveLevel(const void* player);

    void ensureCapesDirectory();
    void writeSamplePng(const std::string& path) const;
    void loadSelectedCape();
    void releaseLoadedCape();
    void selectCapeIndex(int index);
    void rebuildPreviewGrid();
    void refreshGridIfNeeded();

    bool applyCustomCape(void* skin);
    void restoreOriginalCape(void* skin);
    void clearPatchState();

    std::string m_capesDir;
    std::vector<std::string> m_files; // refreshed by saveConfig (menu build / save)
    int m_selectedIndex = 0;          // 0 = None, i>=1 -> m_files[i-1]

    // In-game preview grid ("Cape" picker) — replaces the text radio list.
    CustomCapesUi m_ui;
    std::vector<std::string> m_gridFiles; // file list the grid was built from
    bool m_showPicker = true;             // menu toggle; opens the grid

    // Decoded, already resampled cape pixels (module-owned), w*h = 64*32.
    std::vector<std::uint8_t> m_pixels;
    bool m_capeLoaded = false;
    bool m_loadFailed = false;
    int m_retryTicks = 0;

    // Synthetic cape id that changes on every switch so the engine's texture
    // cache (keyed by mCapeId) is invalidated and the new pixels show up
    // without leaving/rejoining the world.
    uint32_t m_capeIdSerial = 0;
    std::string m_activeCapeId = "bedrocktools";

    // State of the in-game skin patch.
    void* m_patchedSkin = nullptr;  // SerializedSkinImpl* currently patched
    void* m_injectedBlob = nullptr; // pixel buffer currently handed to the game
    bool m_needsApply = true;

    // Backup of the cape image we overwrote so "None"/disable can restore it.
    struct CapeBackup {
        uint32_t format = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
        uint32_t usage = 0;
        void* blob = nullptr;       // original pixel pointer (detached, kept alive)
        void* deleter = nullptr;    // original mce::Blob deleter
        size_t size = 0;
        std::uint8_t capeIdBytes[24] = {};  // raw copy of the original mCapeId std::string
    };
    CapeBackup m_backup;
    bool m_hasBackup = false;
};

extern CustomCapesModule* g_customCapes;
