#pragma once

#include "../Module.hpp"
#include <cstdint>

class EspModule;
extern EspModule* g_espMod;

// Esp
//
// Entity overlay that draws the boxes as *world-space geometry inside the
// game's own render pass, i.e. the exact path the Hitbox module uses: a
// LevelRenderer::renderLevel detour that feeds the game's Tessellator and
// flushes the mesh with a level render material. Because the game transforms
// that geometry with the very matrices it rendered the level with, a box can
// only ever land where its entity is -- no camera model to get wrong, no
// frame of look latency, and sprint FOV or view bob cannot pull it away.
//
// The one difference from Hitbox is depth: the overlay is emitted with a
// material that does no depth test and no wall-occlusion culling, so entities
// keep drawing through walls (the Hitbox module culls them on purpose; Esp
// turns that into the Through Walls toggle, on by default).
//
// Everything that has to sit *on* an entity is geometry: the box, both
// tracers, and the label column -- the distance readout, the nametag and the
// health value and bar -- spelled out of the game's own pixel face as
// billboarded quads (esp_pixel_font.hpp) and hung on the entity's AABB. The
// projection in esp_geometry.hpp only ever *sizes* those, never places them, so
// what used to slide off the hitbox while the view moved (sprint FOV, view bob,
// a frame of look latency, an Fov slider that has not been told about a sprint)
// now cannot.
//
// Two cases stay on the launcher HUD layer, both because geometry cannot reach
// them. A name the pixel face has no cells for -- Arabic, Hebrew, CJK, emoji --
// is drawn in the launcher's own font, and its column comes with it rather than
// being split in two: right characters a little adrift beat a pinned label with
// holes in it. And an entity at or behind the eye has no pixels for anything to
// be pinned to, which is what the Crosshair tracer's snapline is for: a
// world-space segment that *starts at the camera* lies on a single view ray, so
// the game collapses it onto one pixel; the line the game draws for an entity
// in front of the eye therefore starts on the view axis instead, and only that
// degenerate case is left on the screen (see esp::crosshairTracer).
//
// Entity selection mirrors Hitbox: players, mobs and items are toggled
// independently, invisible actors are skipped, and Show Local Player adds the
// self overlay while the camera is in third person.
//
// All of the work happens inside the render hook, so the geometry and the HUD
// labels describe the same frame. onFrame only keeps the overlay bookkeeping
// (clearing when the module is off or when the world stopped rendering).
class EspModule : public Module {
public:
    // Radio option order is persisted by index, so new styles must only be
    // appended at the end. Labels live in esp.cpp (kBoxStyleNames /
    // kTracerOriginNames) and are guarded by static_asserts.
    //
    // Both box styles are 3D wireframes now; the screen-space 2D rectangle the
    // first versions drew is what used to slide off its entity while the view
    // turned, so index 0 keeps its meaning ("the box outline") and just stops
    // being a projection.
    enum class BoxStyle : int { Box = 0, Corner = 1, Count };
    enum class TracerOrigin : int { Bottom = 0, Crosshair = 1, Count };

    EspModule();
    ~EspModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // ---- Entity filters ----------------------------------------------------
    bool showPlayers = true;
    bool showMobs = true;
    bool showItems = true;
    // Draw the local player's own ESP while the camera is in third person.
    bool showLocalPlayer = false;

    // ---- Visibility --------------------------------------------------------
    // True (default): the overlay is emitted with a material that ignores the
    // depth buffer, and no occlusion test runs, so entities behind terrain stay
    // visible. False: actors fully hidden behind solid blocks are culled with
    // the same voxel raycast the Hitbox module uses (a tall mob whose head
    // pokes over a low wall stays visible).
    bool throughWalls = true;
    float range = 64.0f; // fetch radius in blocks (8 .. 256)

    // ---- Box ---------------------------------------------------------------
    bool box = true;
    BoxStyle boxStyle = BoxStyle::Box;
    bool boxFilled = false;
    uint32_t boxColor = 0xFFFFFFFF;
    uint32_t boxFilledColor = 0xFFFFFFFF;
    float boxFilledOpacity = 0.25f;
    // Line weight of the wireframe. 1.0 keeps the game's hairline edges; above
    // that every edge also becomes a camera-facing beam of
    // boxThickness * 0.01 blocks, because GLES line width is ignored by
    // mobile drivers.
    float boxThickness = 1.5f;
    bool rgb = false;      // animated rainbow box color (overrides boxColor)
    float rgbSpeed = 0.3f; // full hue cycles per second (0.05 .. 1)

    // ---- Tracers -----------------------------------------------------------
    // A line that ends inside the entity's hitbox (its AABB center), so it
    // cannot detach from the wireframe while the view moves. The origin is the
    // local player's own feet (Bottom) or the crosshair (Crosshair), and both
    // are world-space geometry handed to the game next to the box edges: the
    // crosshair line starts on the view axis rather than at the eye, because a
    // segment from the eye projects to a single pixel and vanishes, while any
    // point of the axis lands on the middle of the screen. Only an entity at or
    // behind the eye is left as a screen-space snapline, whose direction is the
    // whole message. Hairline either way, and the same Through Walls material as
    // the box edges.
    bool tracer = false;
    TracerOrigin tracerOrigin = TracerOrigin::Bottom;
    uint32_t tracerColor = 0xFFFFFFFF;

    // ---- Nametag -----------------------------------------------------------
    // Centered above the head point (the top-center of the player's AABB, in
    // the world) together with the health stack, as billboarded geometry that
    // the game pins to the entity the same way it pins the box. The name is cleaned of the
    // markup codes and invisible format characters the game's own font
    // swallows before it reaches the HUD, and it is centered on a width
    // measured in glyphs rather than bytes, so a multi-byte (Arabic, CJK)
    // name sits above the head instead of beside it.
    //
    // That width is measured against the face the launcher will actually draw
    // with, which is why the module registers the packaged pixel font (see
    // core/PixelFont.hpp) and asks for it per label: the font is the only one
    // whose cell widths are known here, and its own glyphs stop at Basic Latin,
    // so a name in a script it cannot draw -- Arabic, Hebrew, CJK, emoji -- is
    // left to the launcher's default font, which has them and shapes them
    // instead of drawing a row of replacement boxes -- and that one label, and
    // its column, goes back to the launcher's HUD layer and the module's
    // projection (see Fov below).
    bool nametag = true;
    uint32_t nametagColor = 0xFFFFFFFF;
    float nametagScale = 1.0f; // 0.5 .. 2 (multiplier on the base 14px text)

    // ---- Health / Distance ---------------------------------------------------
    bool health = true;
    // The single distance readout: feet-to-feet between the local player and
    // the entity (in blocks), measured from the actors' collision boxes
    // rather than the render camera, so it is identical in first and third
    // person. Hidden while the local box is unavailable; there is no second,
    // camera-based measurement. Drawn as world-space billboarded digits just
    // under the entity's feet, so it is pinned to the hitbox like the box and
    // both tracers are; the projection only sizes the digits, never positions
    // them.
    bool distance = true;

    // ---- Label projection ---------------------------------------------------
    // What the projection is still asked for: the apparent size of a billboarded
    // label (how big a surface pixel is at that depth), the position of the
    // labels this pass cannot build (an unspellable name and its column), and the
    // Crosshair snapline. Boxes, tracers and every label the pixel face can spell
    // are placed by the game and ignore it entirely.
    //
    // Which is a far smaller job than it used to be: a wrong value can now only
    // resize a pinned label, never slide it off the entity. It still matters for
    // the fallback -- assuming a narrower field of view than the game renders with
    // used to push every off-axis label radially away from the crosshair. The
    // default is Bedrock's own FOV (the game's gfx_fov option is 70), which is
    // what the level is rendered with as long as the slider was not touched.
    float fov = 70.0f; // vertical field of view in degrees (30 .. 120)

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;

    void applyPatch();
};
