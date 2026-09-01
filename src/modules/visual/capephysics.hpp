#pragma once

#include "../Module.hpp"
#include "capephysics_sim.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cp = bedrocktools::modules::capephysics;

// Tick-thread snapshot consumed by the render hook: the player's collision
// box and body rotation samples (current + previous tick, for the same
// partial-tick interpolation Wings uses), the local player pointer and the
// version counter of the published cape canvas.
struct CapePhysicsSnapshot {
    bool hasPlayer = false;
    bool hasColors = false;
    void* localPlayer = nullptr;
    cp::Vec3 aabbMin{};
    cp::Vec3 aabbMax{};
    cp::Vec3 prevAabbMin{};
    cp::Vec3 prevAabbMax{};
    float rotX = 0.0f;
    float rotY = 0.0f;
    float prevRotX = 0.0f;
    float prevRotY = 0.0f;
    bool hasPrevSample = false;
    float tickInterval = 0.05f;
    std::chrono::steady_clock::time_point lastTick{};
    bool lastTickValid = false;
    std::uint32_t canvasVersion = 0;
};

// Cape Physics
//
// Replaces the rigid vanilla cape with a real cloth simulation, rendered as
// a world-space overlay (RenderLevel hook + tessellator, the same pattern
// the Wings module uses — skin memory is never touched for the geometry).
//
// The cape becomes a Verlet particle sheet pinned along the shoulder line:
// gravity, a wind field and the relative wind of the player's own movement
// (capephysics_sim.hpp, pure and host-testable) make it hang, sway, flutter
// while running, stream backwards while falling or elytra-gliding and sweep
// around the body when turning. The player's own body is an elliptic
// collision cylinder, so the cape drapes along the back instead of clipping
// through it. See capephysics_sim.hpp for the full solver description.
//
// Colors come from the cape the game currently renders, so EVERY cape size
// is supported: the live SerializedSkinImpl::mCapeImage (a vanilla/market
// cape, or the pixels the Custom Capes module patched in) is sampled
// proportionally, and the module's own "Cape" picker can additionally load
// any PNG from the shared capes folder (22x23, 64x32, HD 128x64, 704x736 —
// anything stb_image can decode) through the same resampler Custom Capes
// uses. Each cloth cell box-filters the texels it covers, so the native
// 10x16 grid is pixel-identical to the cape texture and larger sources
// simply average down.
//
// While the module is enabled it hides the game's own flat cape mesh by
// clearing SerializedSkinImpl::mCapeId (the game only renders the classic
// cape when the id is non-empty — the same mechanism Custom Capes relies on
// in reverse). The original id is backed up and restored on disable; the
// Custom Capes module is told to hold its own id writes back while the mesh
// is hidden (CustomCapesModule::setCapeMeshSuppressed), so the two modules
// compose instead of fighting over the field. Set "Hide Vanilla Cape" off
// to compare the simulated cape against the original side by side.
//
// The overlay is third-person only (in first-person the camera sits inside
// the player's collision box, the same convention as Wings/Hitbox) and, like
// every world overlay in this mod, purely visual and local.
class CapePhysicsModule : public Module {
public:
    CapePhysicsModule();
    ~CapePhysicsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the LocalPlayerTickEvent subscription.
    void onLocalPlayerTick(void* player);

    // World-exit teardown: drops every engine reference (patched skin, saved
    // cape id, injected pixels) without writing to the skin — the engine
    // owns the skin while it destroys the world (see Custom Capes for the
    // Leave World crash this pattern prevents). Idempotent.
    void onWorldExit();

    // Directory the cape files are read from (shared with Custom Capes);
    // exposed for the menu description.
    const std::string& capesDirectory() const { return m_capesDir; }

    // --- Settings (shown in the launcher menu) --------------------------

    // Cape source: 0 = the cape the game currently renders (vanilla /
    // marketplace / Custom Capes); i >= 1 -> m_files[i-1] from the capes
    // folder. Serialized as a radio picker like Custom Capes' Cape selector.
    int m_cape = 0;

    // How a non-64x32 source file is mapped onto the cape face (index into
    // customcapes::kCapeFitLabels — Fit / Fill / Crop, the same semantics as
    // Custom Capes' Cape Fit).
    int m_capeFit = 0;

    // Cloth grid density: index into cp::kDetailPresets
    // (Native = 10x16 cells, one per cape pixel; Fine = 14x22).
    int m_detail = 0;

    // Physics knobs, all clamped at load time:
    float m_windStrength = 0.35f; // 0..2, gust intensity
    float m_gravity = 1.0f;       // 0..2, 1 == 10 blocks/s^2
    float m_stiffness = 0.85f;    // 0.05..1, cloth rigidity

    // Hide the game's own cape mesh while the simulated cape is drawn
    // (default on — otherwise both capes render on top of each other).
    bool m_hideVanilla = true;

    // --- Test hooks (host tests drive the tick/render logic directly) ----

    // True when the module currently has cape colors to render.
    bool hasRenderColors() const;
    // The normalized 64x32 canvas the render side paints the cloth with.
    const std::vector<std::uint8_t>& renderCanvasForTest() const;
    // The cloth state the render hook steps (positions in world space).
    const cp::Cloth& clothForTest() const;
    // True while the module is suppressing the game's cape mesh.
    bool hidingVanillaCapeForTest() const;

    // --- Render-side entry points (called by the RenderLevel hook) ------

    // Snapshot of the tick-thread state (player box, rotation, colors),
    // copied under the state mutex.
    CapePhysicsSnapshot copySnapshot() const;

    // Rebuilds the per-cell color palette when the published canvas or the
    // detail preset changed. Render thread only.
    void refreshPalette(std::uint32_t canvasVersion);
    // Advances the cloth by frameDt seconds using fixed 1/60 s substeps.
    // Render thread only.
    void advanceCloth(float frameDt, const cp::Vec3 anchors[],
                      const cp::BodyFrame& body, const cp::ClothParams& params,
                      int cols, int rows);
    // Frame clock bookkeeping for advanceCloth.
    void noteFrameTime(std::chrono::steady_clock::time_point now);
    bool frameClockStarted() const { return m_frameClockStarted; }
    std::chrono::steady_clock::time_point lastFrameTime() const { return m_lastFrame; }
    // The cached palette the overlay paints with. Render thread only.
    const cp::CapePalette& paletteForRender() const { return m_palette; }

private:
    void applyPatch();

    // --- pixel source ----------------------------------------------------
    void ensureCapesDirectory() const;
    void loadSelectedCapeFile();
    void releaseLoadedCapeFile();
    bool updateCanvasFromSkin(void* skin);

    // --- vanilla cape mesh hiding ---------------------------------------
    void hideCapeMeshIfWanted(void* skin, bool persona);
    void unhideCapeMesh(void* skin);
    static bool capeIdIsEmpty(const void* capeIdAddr);

    // SerializedSkinImpl* walk (Player::mSkin -> SkinRef -> ThreadOwner),
    // identical to Custom Capes' resolver.
    static void* resolvePlayerSkin(void* player);
    static bool playerHasLiveLevel(const void* player);

    std::string m_capesDir;
    std::vector<std::string> m_files; // refreshed by saveConfig (menu build / save)
    int m_selectedFileIndex = 0;      // 0 = worn cape, i>=1 -> m_files[i-1]

    // File source state (module-owned, decoded+resampled to 64x32 RGBA).
    std::vector<std::uint8_t> m_fileCanvas;
    bool m_fileLoaded = false;
    bool m_fileLoadFailed = false;
    int m_fileRetryTicks = 0;

    // Worn-cape source state: blob token of the last canvas copied from the
    // live skin, so the 8 KB copy only happens when the pixels actually
    // change.
    void* m_liveBlobToken = nullptr;
    std::size_t m_liveBlobSizeToken = 0;

    // State of the vanilla-mesh hide (id backup + the skin it belongs to).
    void* m_idHiddenSkin = nullptr;
    bool m_hasSavedCapeId = false;
    std::uint8_t m_savedCapeId[24] = {};

    // --- state shared with the render hook (m_stateMutex) ---------------
    CapePhysicsSnapshot m_snapshot;
    std::vector<std::uint8_t> m_canvas; // 64*32*4, published for the render side
    std::uint32_t m_canvasSerial = 0;
    mutable std::mutex m_stateMutex;

    // --- cloth (render thread only, guarded by m_stateMutex) ------------
    cp::Cloth m_cloth;
    bool m_clothNeedsReset = true;
    float m_substepAccumulator = 0.0f;
    float m_simTime = 0.0f; // deterministic wind-field clock
    std::chrono::steady_clock::time_point m_lastFrame{};
    bool m_frameClockStarted = false;
    cp::CapePalette m_palette;
    std::uint32_t m_paletteCanvasVersion = 0;
    int m_paletteDetail = -1;
    std::vector<std::uint8_t> m_renderCanvas; // render-side copy of m_canvas
    cp::Vec3 m_anchors[64]{};

    // --- hook state ------------------------------------------------------
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};

extern CapePhysicsModule* g_capePhysics;
