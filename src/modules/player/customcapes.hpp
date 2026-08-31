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
// edge colors so the 1-voxel-thick cape mesh does not look flat. The design
// is also mapped onto the tapered Elytra UV area used by both wings. Exact
// 64x32 inputs keep manually-authored Elytra pixels; if that area is empty,
// the wing fallback is generated from the cape face. The result is written
// into the local player's SerializedSkinImpl::mCapeImage each tick, together
// with the image metadata the engine needs to upload it: the RGBA8Unorm
// format (mce::ImageFormat value 3 — the enum tops out at 3; writing the
// naive "4 channels" value 4 hands the texture factory an out-of-range
// format and it silently refuses to build the cape texture), width,
// height, a depth of 1 (it is a 2D texture) and an image usage. A player
// who owns no cape at all carries a default-constructed, all-zero
// mCapeImage, so those fields have to be filled in instead of left alone
// -- an image claiming 64x32 RGBA pixels at depth 0 has a computed size of
// zero bytes and the engine drops it without ever drawing a cape.
// Modern game versions
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
//     cleanup is needed on our side. If the game rebuilds the same skin object
//     in-place, the module detects that its blob was detached and takes a fresh
//     backup instead of restoring/freeing stale state.
//   * the cape offsets are version-specific and derived from the verified
//     skin offsets (see include/bedrocktools/sdk/offsets/Skin.hpp); sanity
//     checks on the live skin and cape images abort the patch if the layout shifts.
//   * persona skins go through the persona pipeline (no classic cape image),
//     so the module leaves them untouched.
//   * on Leave World the engine detaches the player from its level
//     (Actor::mLevel goes null) before the player and skin are freed; the
//     tick hook bails on a null player or a null level link and detaches
//     its engine references WITHOUT touching the skin — the engine frees
//     the injected blob through its deleter tag, so a teardown tick can
//     never write freed memory or double-free the blob.
//
// KNOWN LIMITATION (the bug this module used to have) and its fix:
//
//   Patching mCapeImage/mCapeId every tick writes the right pixels into the
//   right place, but the engine builds the cape mesh — and decides whether a
//   cape exists at all — ONCE from the skin when the player's ActorRendererData
//   is created at world entry, uploading the cape texture to its cache then.
//   A later memory patch never rebuilds that mesh, so a player who owned no
//   cape at world entry never gets a cape mesh and nothing shows.
//
//   The fix runs the same patch from ClientInstanceUpdateEvent, which fires
//   once per frame BEFORE the level render pass. On the first frame after the
//   local player is created, the patch therefore lands in SerializedSkinImpl
//   before ActorRendererData is built from the skin during that frame's
//   render — the engine then creates the cape mesh and uploads our texture
//   through its own pipeline (real cape, with the game's waving animation).
//
//   As a second, independent path (RenderMode Overlay/Both, config
//   "m_capeRenderMode"), the module can draw the cape itself in the
//   RenderLevel hook as a textured rectangle on the player's back — a GLES
//   texture built from the same 64x32 pixels, tessellated through the engine
//   like the Wings module. This works even when the mesh was already built
//   (e.g. the module was enabled mid-world), at the cost of not being the
//   engine's own animated cape.
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

    // Called from the ClientInstanceUpdateEvent subscription. ClientInstance
    // updates once per frame BEFORE the level render pass, so on the very
    // first frame after the local player is created this hook patches the
    // cape into SerializedSkinImpl before ActorRendererData is built from the
    // skin during that frame's render — the engine then creates a real cape
    // mesh and uploads our texture to the cape cache (the thing the per-tick
    // patch alone can never trigger for a player who owned no cape at world
    // entry; see the KNOWN LIMITATION note in the class comment below).
    // Falls back to the exact same guarded tick logic as onLocalPlayerTick.
    void onClientInstanceUpdate(void* clientInstance);

    // World-exit teardown: drops every engine reference (patched skin,
    // injected blob, backup) without writing to the skin or freeing the
    // blob — the engine owns both while it destroys the world. Idempotent;
    // the loaded cape file stays so the cape re-applies on rejoin.
    void onWorldExit();

    // How the cape is shown once the module is enabled:
    //   0 = Engine mesh — patch SerializedSkinImpl only; the game builds the
    //       cape mesh from the patched skin (requires the patch to land
    //       before ActorRendererData is built — the ClientInstanceUpdate
    //       early hook above does that on the first post-join frame).
    //   1 = Overlay     — self-rendered textured cape in the RenderLevel
    //       hook (GL texture + tessellator); the skin is left untouched.
    //   2 = Both        — patch the skin AND draw the overlay (diagnostic:
    //       two capes means both paths work, one means that path fails).
    enum RenderMode : int {
        RenderModeEngine = 0,
        RenderModeOverlay = 1,
        RenderModeBoth = 2,
    };
    static constexpr const char* kRenderModeRadioLabels[3] = {"Engine mesh", "Overlay", "Both"};
    static constexpr int kRenderModeRadioCount = 3;

    // Directory the module watches; exposed for the menu description.
    const std::string& capesDirectory() const { return m_capesDir; }

    // Current render mode (see RenderMode enum); read by the render hook.
    int renderMode() const { return m_renderMode; }

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
    void backupOriginalCape(std::uintptr_t capeImage, std::uintptr_t capeIdAddr);
    void clearPatchState();

    // Overlay renderer (RenderMode Overlay/Both). The hook itself lives in
    // the .cpp translation unit together with its shared render state; the
    // module only publishes the config, the cape pixels and the player pose.
    void syncOverlayHook();  // installs/removes the RenderLevel hook on mode change
    void publishOverlayCape();  // copies the resampled cape for the render thread

    std::string m_capesDir;
    std::vector<std::string> m_files; // refreshed by saveConfig (menu build / save)
    int m_selectedIndex = 0;          // 0 = None, i>=1 -> m_files[i-1]

    // Decoded, already resampled cape pixels (module-owned), w*h = 64*32.
    std::vector<std::uint8_t> m_pixels;
    bool m_capeLoaded = false;
    bool m_loadFailed = false;
    int m_retryTicks = 0;

    // Synthetic cape id that changes on every switch so the engine's texture
    // cache (keyed by mCapeId) is invalidated and the new pixels show up
    // without leaving/rejoining the world.
    uint32_t m_capeIdSerial = 0;
    std::string m_activeCapeId = "bedrocktoolsplus";

    // Render mode for the fix (see the RenderMode enum above): 0 = engine
    // mesh only (default), 1 = self-rendered overlay only, 2 = both.
    int m_renderMode = RenderModeEngine;

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
