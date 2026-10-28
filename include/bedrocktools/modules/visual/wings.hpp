#pragma once

#include "modules/Module.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

// Wings - world-space overlay version
//
// The first implementation patched SerializedSkinImpl (mSkinImage,
// mGeometryData, mDefaultGeometryName) to inject a custom geometry. Bedrock
// has removed support for custom geometry on classic skins, so that approach
// made the player disappear when the module was enabled.
//
// This version draws the wings as a world-space overlay attached to the local
// player via a RenderLevel hook + tessellator, instead of touching the skin at
// all. Each wing is one continuous membrane: an articulated arm (shoulder,
// elbow, wrist) bent into a smooth Catmull-Rom leading edge, with a cambered
// sail hanging off it. The sail's trailing edge is swept inboard and carved
// into concave finger scallops, with struts along the fingers, a spar + claw
// along the arm and a shoulder fairing at the root; the feathered styles lay
// three overlapping rows of quills over it.
//
// The geometry lives in src/modules/visual/:
//
//   wings_design.hpp   the style table - arm proportions, membrane shape
//                      (chord, fan, scallops, fingers), feather layer and
//                      palette, one entry per selectable style
//   wings_surface.hpp  the builder: turns a WingDesign + a pose into the quads
//                      of a single continuous membrane (leading edge, sail,
//                      finger struts, spar, claw, feathers)
//   wings_shape.hpp    pure math + the shading model
//
// The overlay used to draw seven disjoint prism boxes per wing, which read as a
// comb of planks. The membrane is one sheet hanging off a Catmull-Rom arm: it
// has a real silhouette, concave finger scallops, a cambered surface that
// catches the light, a travelling flutter wave while flapping and, for the
// feathered styles, three rows of overlapping quills over the sail.
//
// Both wings are built as double-sided quads in Bedrock model pixels and pushed
// through the tessellator; the palette side is chosen from where the camera is
// relative to the player, so the sheet never turns into a front/back patchwork.
// tools/wings_preview.cpp rasterizes exactly the same quads into PNGs, so a
// change can be judged without launching the game.
//
// Animation (mirrors resources/wings/wings_animation.json): a flap clock runs
// at angle = amplitude * sin(flapTime * baseRate * flapSpeed), and the pose is
// blended between idle (gentle breathing pulse), flap (strong motion while
// moving) and glide (spread wings while descending) based on the player's
// horizontal/vertical speed measured from consecutive player AABBs:
//
//   moveT    = clamp(horizontalSpeed / kWalkSpeedFull, 0, 1)
//   glideT   = 1 while sustained descent (vy < -1.5 for > 0.35 s), else 0
//   target   = moveT * (1 - glideT)
//   intensity smoothly lerps to target; glide lerps to glideT.
//
// flapSpeed is exposed as "Flap Speed" in the launcher menu in [0.1, 10.0].
// On init the module writes the embedded geometry/animation/controller JSON
// and texture into <config dir>/wings (if missing) so the same assets can be
// used by tools or edited by the user; the module never touches skin memory.
//
// Per-bone angles produced by the animation controller, in degrees,
// "raise positive" (positive lifts the wing tip). Right-side bones use the
// negated angle around the Z axis, left-side bones use it directly - the same
// convention as wings_animation.json.
struct WingBoneAngles {
    float shoulderDeg = 0.0f;                 // shoulder joint (bone_wing_*)
    float upperDeg = 0.0f;                    // relative to shoulder
    float tipDeg = 0.0f;                      // relative to upper
    float featherDeg[4] = {0, 0, 0, 0};       // relative to parent segment
    float flapPhase = 0.0f;                   // radians
    float intensity = 0.0f;                   // 0 = idle .. 1 = flight
    float glide = 0.0f;                       // 0 = folded pose .. 1 = glide
};

class WingsModule : public Module {
public:
    WingsModule();
    ~WingsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Draws the launcher overlay for this module: the silhouette of the style
    // that is currently selected, so the menu shows a shape and not only a name.
    // Called from the module tick while the module is enabled.
    void drawWings();

    // Registers the launcher menu page. The style picker is rebuilt as a v2
    // schema so the menu can carry a silhouette preview of the selected style,
    // which the classic key/value list cannot express.
    void onMenuRegistered() override;

    // Keeps the preview in step with the picker: changing the style redraws the
    // silhouette immediately, so browsing the list shows each shape.
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;

    // Called from the LocalPlayerTickEvent subscription.
    void onLocalPlayerTick(void* player);

    // True when the render camera is outside the local player's collision box,
    // i.e. the player is being viewed from behind in third-person. In
    // first-person the camera sits inside the player's head (within the AABB),
    // so the back-mounted wings would overlap/clip the view; the module only
    // draws its wings when this returns true. Testing the box rather than a
    // fixed eye height keeps this correct while sneaking/swimming, where the
    // eye drops but the camera stays inside the box. Mirrors the Hitbox
    // module's convention of not rendering the player's own geometry in
    // first-person.
    static bool isThirdPersonCamera(float camX, float camY, float camZ,
                                    float aabbMinX, float aabbMinY, float aabbMinZ,
                                    float aabbMaxX, float aabbMaxY, float aabbMaxZ);

    // Advances the wing animation by dtSeconds given the player's current
    // horizontal and vertical speed (blocks/second). This blends the pose
    // between idle, flap and glide and stores the resulting bone angles for
    // the render hook.
    void advanceWingAnimation(float dtSeconds, float horizontalSpeed, float verticalSpeed);

    // Advances only the flap clock (idle input speeds). Kept for host tests
    // that drive a deterministic clock.
    void advanceFlapAnimation(float dtSeconds);

    // Current per-bone pose computed from the flap clock, flight intensity
    // and glide factor.
    WingBoneAngles currentBoneAngles() const;
    // Interpolated version that adds time since last tick for ultra-low
    // latency rendering (called from the RenderLevel hook).
    WingBoneAngles currentBoneAnglesInterpolated() const;

    // Directory the module writes its assets to; kept for menu description
    // compatibility.
    const std::string& wingsDirectory() const { return m_wingsDir; }

    // Writes the embedded geometry/animation/controller JSON and texture into
    // wingsDirectory() (without overwriting existing files).
    void ensureWingsAssetFiles();

    // Pushes the current menu schema (style picker + silhouette preview) to the
    // launcher. Cheap enough to call on every style change.
    void refreshMenuSchema();

    // Flap speed multiplier shown in the launcher menu (0.1 = slow,
    // 10 = very fast, 1.0 = default).
    float m_flapSpeed = 1.0f;

    // Wing style shown in the launcher menu as a radio selector. One of the
    // ids in the fixed style table (dragon, angel, demon, bat, butterfly,
    // phoenix, fairy); "dragon" is the default articulated membrane wing.
    // m_wingStyle is the serialized/display id; m_wingStyleIndex is the
    // resolved table index kept in sync so the render hook can read an int
    // without touching the std::string from the render thread.
    std::string m_wingStyle = "dragon";
    int m_wingStyleIndex = 0;

    // Draw the selected style's silhouette in the launcher overlay. Always on;
    // exposed as a member so the setting can be added without touching the
    // renderer.
    bool m_showStylePreview = true;

    // Test / rendering helpers
    float flapTime() const { return m_flapTime; }
    float flightIntensity() const { return m_intensity; }
    float glideFactor() const { return m_glide; }
    float currentFlapAngleDegrees() const;
    float currentFlapAngleRadians() const;

    // Flap driver constants (kept compatible with the original overlay:
    // currentFlapAngleDegrees() == kFlapAmplitudeDegrees * sin(t * kFlapBaseRate * speed)).
    static constexpr float kFlapAmplitudeDegrees = 35.0f;
    static constexpr float kFlapBaseRate = 6.0f;      // rad/s at speed 1.0
    static constexpr float kWingWidth = 0.75f;        // blocks, one wing span
    static constexpr float kWingHeight = 0.5f;        // blocks, feather drop

    // Idle breathing pulse (matches animation.wings.idle).
    static constexpr float kIdleBaseDegrees = 20.0f;
    static constexpr float kIdleAmplitudeDegrees = 6.0f;
    static constexpr float kIdleRate = 1.5707963f;    // 2*pi / 4 s

    // Flight flap pose (matches animation.wings.flap).
    static constexpr float kFlightBaseDegrees = 25.0f;
    static constexpr float kFlightUpperAmplitudeDegrees = 14.0f;
    static constexpr float kFlightTipAmplitudeDegrees = 18.0f;
    static constexpr float kFlightFeatherAmplitudeDegrees = 10.0f;

    // Glide pose (matches animation.wings.glide).
    static constexpr float kGlideBaseDegrees = 50.0f;
    static constexpr float kGlideRate = 2.0943951f;   // 2*pi / 3 s

    // Motion wave lags (radians) from shoulder to feather tips.
    static constexpr float kIdleUpperLag = 0.7853982f;    // 45 deg
    static constexpr float kIdleTipLag = 1.5707963f;      // 90 deg
    static constexpr float kIdleFeatherLagBase = 2.0943951f; // 120 deg
    static constexpr float kIdleFeatherLagStep = 0.2617994f; // 15 deg
    static constexpr float kUpperLag = 0.8726646f;        // 50 deg
    static constexpr float kTipLag = 1.8325957f;          // 105 deg
    static constexpr float kFeatherLagBase = 2.4434610f;  // 140 deg
    static constexpr float kFeatherLagStep = 0.3490659f;  // 20 deg

    // Speed-driven blending - tuned for minimal latency (instant response).
    static constexpr float kWalkSpeedFull = 4.3f;     // blocks/s: vanilla walk
    static constexpr float kRiseSpeedFlap = 0.5f;     // vy above => flap hard (lower = faster jump response, was 1.5)
    static constexpr float kGlideFallSpeed = -1.5f;   // vy below => count airtime
    static constexpr float kGlideAirTime = 0.25f;     // seconds before gliding (reduced from 0.35 for faster response, but >0.2 to avoid hop gliding)
    static constexpr float kIntensityAttackRate = 30.0f;  // 1/s lerp rate up (was 6.0 - now ultra-fast)
    static constexpr float kIntensityDecayRate = 25.0f;   // 1/s lerp rate down (was 2.5)
    static constexpr float kGlideAttackRate = 30.0f;      // was 4.0
    static constexpr float kGlideDecayRate = 25.0f;       // was 3.0

    // Face palette shared with the generated texture (see wings_default.hpp);
    // Demon Wings edition: black frame #000000, red glowing membrane #FF0000/#E60000 -> #800000
    static constexpr unsigned char kColorFrame[3] = {0, 0, 0};
    static constexpr unsigned char kColorMembraneOuter[3] = {230, 0, 0};
    static constexpr unsigned char kColorMembraneInner[3] = {128, 0, 0};
    static constexpr unsigned char kColorFeatherTip[3] = {255, 0, 0};
    static constexpr unsigned char kColorJointInner[3] = {0, 0, 0};

private:
    void applyPatch();

    std::string m_wingsDir;

    // Animation state (clock driven per tick). The render hook runs on a
    // different thread from LocalPlayerTick, so all clock reads/writes must be
    // synchronized; otherwise a torn sample can make the flap phase jump.
    mutable std::mutex m_animationMutex;
    float m_flapTime = 0.0f;
    float m_intensity = 0.0f;   // smoothed idle->flight blend
    float m_glide = 0.0f;       // smoothed glide blend
    float m_airTime = 0.0f;     // sustained descent time used for glide
    bool m_flapClockStarted = false;
    std::chrono::steady_clock::time_point m_lastFlapTick;

    // Player speed estimation from consecutive AABBs.
    bool m_hasPrevCenter = false;
    float m_prevCenterX = 0.0f;
    float m_prevCenterY = 0.0f;
    float m_prevCenterZ = 0.0f;

    // Render hook state
    bool m_patched = false;
    void* m_patchTarget = nullptr;
    void* m_tessBeginAddr = nullptr;
    void* m_tessColorAddr = nullptr;
    void* m_tessVertexAddr = nullptr;
    void* m_renderMaterialGroupAddr = nullptr;
    void* m_renderMeshAddr = nullptr;
    void* m_renderMesh2Addr = nullptr;
};
