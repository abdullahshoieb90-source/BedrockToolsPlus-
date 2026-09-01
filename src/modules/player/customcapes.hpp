#pragma once

#include "../Module.hpp"
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
// The selected file is decoded with stb_image and resampled onto the
// classic-cape layout of the 64x32 canvas Minecraft uses: the image is
// painted onto the outer back face only (x=1..11, y=1..17), the inner
// front face is filled with a flat lining color instead of a repeat of the
// image, and the top/bottom/side edge strips are continued from the image's
// edge colors so the 1-voxel-thick cape mesh does not look flat. Any source
// resolution is accepted — 22x23 and every other non-standard size included;
// the module's "Cape Fit" setting decides how the artwork is mapped: Fit
// (default) uses the whole image with its aspect ratio intact and continues
// the image's own edge colors into the leftover bands, Fill stretches the
// whole image over the face, and Crop center-crops it. The design
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

    // Cape Physics coordination: while the Cape Physics module hides the
    // game's classic cape mesh (it clears SerializedSkinImpl::mCapeId), this
    // module must keep patching the cape PIXELS (the physics cape renders
    // them) but must not write its synthetic cape id back into the skin —
    // otherwise the two modules would fight over mCapeId every tick and the
    // id would flicker between empty and "bedrocktools-N". When suppression
    // is lifted the next tick notices the id is no longer intact and
    // re-applies it, so lifting the suppression needs no extra handling
    // here. Runtime-only state, never serialized.
    void setCapeMeshSuppressed(bool suppressed) { m_capeMeshSuppressed = suppressed; }
    bool capeMeshSuppressed() const { return m_capeMeshSuppressed; }

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

    bool applyCustomCape(void* skin);
    void restoreOriginalCape(void* skin);
    void clearPatchState();

    std::string m_capesDir;
    std::vector<std::string> m_files; // refreshed by saveConfig (menu build / save)
    int m_selectedIndex = 0;          // 0 = None, i>=1 -> m_files[i-1]

    // How an image that is not 64x32 is mapped onto the cape face: index into
    // customcapes::kCapeFitLabels (0 = Fit, 1 = Fill, 2 = Crop). Exposed to
    // the mod menu as the "Cape Fit" radio.
    int m_capeFit = 0;

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

    // Set by the Cape Physics module while it hides the game's cape mesh
    // (see setCapeMeshSuppressed).
    bool m_capeMeshSuppressed = false;
};

extern CustomCapesModule* g_customCapes;
