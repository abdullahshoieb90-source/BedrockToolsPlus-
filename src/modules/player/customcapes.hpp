#pragma once

#include "../Module.hpp"
#include "customcapes_files.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Custom Capes
//
// Lets the player wear any PNG as a classic cape. The module owns a "capes"
// directory next to config.json (`<configDir>/capes`, created on first launch
// together with a sample cape); every usable .png file in it shows up as an
// option of the module's Cape picker in the launcher mod menu.
//
// What the module does with an image:
//   * the file is validated (PNG signature + IHDR) before it is decoded, so a
//     renamed JPEG, a truncated download or an absurdly large image can never
//     reach the decoder;
//   * it is decoded with stb_image and resampled with an area-average filter
//     onto the outer back face (x=1..11, y=1..17) of the 64x32 cape canvas;
//   * the inner front face is filled with a flat lining colour, the edge
//     strips continue the image's edge colours so the 1-voxel-thick cuboid
//     does not look flat, and the design is mapped onto the tapered Elytra UV
//     area used by both wings;
//   * exact 64x32 inputs keep manually authored Elytra pixels.
//
// Settings (all persisted in config.json):
//   * m_cape       - the picker (None + one option per cape file)
//   * m_capeFit    - how an arbitrary image is fitted on the cape face
//                    (Cover / Contain / Stretch)
//   * m_reloadButton     - re-reads the selected file from disk and re-applies
//                          it (picks up an edited/replaced PNG without a
//                          restart; the picker's option list itself is fixed
//                          at launch, so a brand new file needs a menu reload)
//   * m_copyPathButton   - copies the capes folder path to the clipboard
//   * m_folderPath       - the folder path, shown as a text row
//
// Config callbacks and the in-game tick run on different threads (the launcher
// menu versus the game's client tick), so everything they share — the file
// list, the resampled pixels, the selection and the loaded-source cache — is
// protected by m_mutex.
//
// The result is written into the local player's
// SerializedSkinImpl::mCapeImage each tick. Modern game versions only render
// the classic cape when SerializedSkinImpl::mCapeId is non-empty, so a
// synthetic short-string id is written alongside the image and restored
// together with it. The blob handed to the game is malloc'd and tagged with
// free() as its deleter, so whatever the engine does with the image afterwards
// (move, destroy, skin rebuild) is memory-safe. The original cape is restored
// when the module is disabled or "None" is picked.
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
//   * the injected blob is never freed while the skin may still reference it:
//     the skin is repointed first and only the previous blob is released.
class CustomCapesModule : public Module {
public:
    // Why the cape is not (or no longer) applied. The tick path is silent by
    // design — it runs every tick and cannot log — so the last decision is
    // recorded here and rendered in the menu status row. Without this, "the
    // cape does not change" is indistinguishable between "no file selected",
    // "the PNG failed to decode", "the player wears a persona skin" and "the
    // engine did not re-upload the texture".
    enum class ApplyStatus {
        Idle,             // module disabled or "None" selected
        Applied,          // pixels are in the live skin right now
        NoPlayer,         // no live local player this tick
        NoSkin,           // could not resolve SerializedSkinImpl
        PersonaSkin,      // persona (Character Creator) skin: no classic cape image
        LayoutMismatch,   // skin image offsets do not match this game build
        EmptyPixels,      // nothing decoded yet
        LoadFailed,       // the selected PNG was rejected
    };

    static const char* applyStatusLabel(ApplyStatus status);

    // Actor::normalTick fires for every actor in the level, so the tick handler
    // has to prove it is looking at the local player before touching a skin.
    // The production predicate resolves the local player through ClientInstance
    // and compares pointers; the host tests have no client instance, so they
    // install their own predicate that accepts the fake actor they drive.
    using LocalPlayerPredicate = bool (*)(const void* actor);
    static void setLocalPlayerPredicateForTests(LocalPlayerPredicate predicate);
    ApplyStatus applyStatus() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_applyStatus;
    }
    CustomCapesModule();
    ~CustomCapesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;

    // Called from the LocalPlayerTickEvent subscription.
    void onLocalPlayerTick(void* player);

    // World-exit teardown: drops every engine reference (patched skin,
    // injected blob, backup) without writing to the skin or freeing the
    // blob — the engine owns both while it destroys the world. Idempotent;
    // the loaded cape file stays so the cape re-applies on rejoin.
    void onWorldExit();

    // Directory the module watches; exposed for the menu description.
    const std::string& capesDirectory() const { return m_capesDir; }

    // Number of usable cape files found by the last scan.
    std::size_t capeCount() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_files.size();
    }

    // Re-reads the folder and keeps the current selection by file name.
    // Returns true when the file list changed. Takes m_mutex.
    bool refreshFiles();

    // Fit mode used for the next resample.
    customcapes::FitMode fitMode() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_fitMode;
    }

    // File name currently loaded (empty when nothing is loaded). Exposed for
    // tests and diagnostics; the option index alone cannot identify a cape.
    std::string loadedFile() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_loadedFile;
    }

    // True once a cape file was decoded and resampled into m_pixels.
    bool capeLoaded() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_capeLoaded;
    }

    // True when the last load attempt failed (corrupt/oversized/non-PNG file).
    bool loadFailed() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_loadFailed;
    }

private:
    // True when the player pointer and its level link are both usable.
    // During Leave World the engine nulls Actor::mLevel before the player
    // and skin objects are freed, so this is the safe early-out for the
    // tick hook: a null level means the skin must not be touched.
    static bool playerHasLiveLevel(const void* player);

    void ensureCapesDirectory();
    bool refreshFilesLocked(); // m_mutex already held
    // Writes the built-in sample cape so a fresh install has something to
    // pick. The encoder lives in customcapes.cpp, which owns the single
    // STB_IMAGE_WRITE_IMPLEMENTATION of the shared object (Wings needs it too).
    void writeSamplePng(const std::string& path) const;
    bool loadSelectedCape();
    void releaseLoadedCape();
    void rebuildCapeId();
    void applyFitChange();
    void copyCapesPath() const;

    bool applyCustomCape(void* skin);
    void restoreOriginalCape(void* skin);
    void clearPatchState();

    std::string m_capesDir;
    std::vector<std::string> m_files;   // refreshed on load/save and by refresh
    std::vector<std::string> m_labels;  // display names for the picker
    int m_selectedIndex = 0;            // 0 = None, i>=1 -> m_files[i-1]

    // Decoded, already resampled cape pixels (module-owned), w*h = 64*32.
    std::vector<std::uint8_t> m_pixels;
    bool m_capeLoaded = false;
    bool m_loadFailed = false;
    int m_retryTicks = 0;

    // How arbitrary images are fitted on the cape face, and what the loaded
    // pixels were produced with (so a fit change can re-resample the same file
    // without touching the disk again).
    customcapes::FitMode m_fitMode = customcapes::FitMode::Cover;
    customcapes::FitMode m_loadedFit = customcapes::FitMode::Cover;

    // Source image kept for fast fit-mode changes: only used when the file
    // could be decoded; cleared as soon as the selection changes.
    std::vector<std::uint8_t> m_sourcePixels;
    std::uint32_t m_sourceWidth = 0;
    std::uint32_t m_sourceHeight = 0;

    // Name of the file whose pixels are loaded. The picker index alone cannot
    // identify a cape: when a file is added or removed the indices shift, and
    // two different files can occupy the same index.
    std::string m_loadedFile;

    // Guards every field the menu thread and the tick thread both touch.
    // The engine-facing patch state (m_patchedSkin/m_injectedBlob/m_backup) is
    // only ever touched from the tick, but it is covered by the same lock so
    // the module has one obvious rule instead of two.
    //
    // Recursive on purpose: Module::loadConfig() (called under this lock) can
    // flip masterEnabled/keybindActive, which calls updateEnabledState() and
    // from there our onEnable()/onDisable() on the same thread. A plain
    // std::mutex would deadlock there.
    mutable std::recursive_mutex m_mutex;

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
        uint32_t alphaUsage = 0;
        void* blob = nullptr;       // original pixel pointer (detached, kept alive)
        void* deleter = nullptr;    // original mce::Blob deleter
        size_t size = 0;
        std::uint8_t capeIdBytes[24] = {};  // raw copy of the original mCapeId std::string
    };
    CapeBackup m_backup;
    bool m_hasBackup = false;

    // Diagnostics for the menu status row (see ApplyStatus).
    ApplyStatus m_applyStatus = ApplyStatus::Idle;
};

extern CustomCapesModule* g_customCapes;
