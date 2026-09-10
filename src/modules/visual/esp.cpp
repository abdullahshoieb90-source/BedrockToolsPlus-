#include "esp.hpp"

#include "../ModuleRegistry.hpp"
#include "esp_geometry.hpp"
#include "core/PixelFont.hpp"
#include "core/memory/Hooks.hpp"
#include "overlay_mesh.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// Defined here at global scope to satisfy the `extern` declaration in
// esp.hpp (kept out of the anonymous namespace below so the name is not
// shadowed inside member functions).
EspModule* g_espMod = nullptr;

namespace {

using bedrocktools::memory::SignatureId;
using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;

namespace offsets = bedrocktools::sdk::offsets;

// ---------------------------------------------------------------------------
// Resolved game functions (same calling conventions the Hitbox module uses).
// ---------------------------------------------------------------------------
typedef bool (*Actor_isPlayer_t)(void* actor);
typedef bool (*Actor_isInvisible_t)(void* actor);

struct DistanceSortedActor {
    void* mActor;
    float mDistance;
    float _pad;
};

struct ActorVec {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* cap;
};

typedef ActorVec (*Actor_fetchNearbyActorsSorted_t)(void* actor, void* extent, int actorType);

struct BlockPosI {
    int x, y, z;
};
typedef bool (*BlockSource_isSolidBlockingBlock_t)(void* region, const BlockPosI& pos);

using RenderLevelFn = void (*)(void* levelRenderer, void* screenContext, void* renderParams);

// ---------------------------------------------------------------------------
// Module-wide state (file scope, like Hitbox / Crosshair).
// ---------------------------------------------------------------------------
Actor_isPlayer_t s_actorIsPlayer = nullptr;
Actor_isInvisible_t s_actorIsInvisible = nullptr;
Actor_fetchNearbyActorsSorted_t s_actorFetchNearby = nullptr;
BlockSource_isSolidBlockingBlock_t s_isSolidBlockingBlock = nullptr;

// The world-space half of the overlay: the game's Tessellator entry points,
// filled in once per frame with the ScreenContext's tessellator handle.
overlay::Mesh s_mesh;
std::uintptr_t s_renderMaterialGroup = 0;
overlay::MaterialPtr s_lineMaterial;         // selection_box: crisp wire
overlay::MaterialPtr s_faceMaterial;         // vertex-color fill for the box faces
overlay::MaterialPtr s_throughWallsMaterial; // no depth test at all

RenderLevelFn s_renderLevelOriginal = nullptr;

// Local player pointer, handed over from the tick thread (LocalPlayerTickEvent)
// and read on the render thread. Being a plain pointer handed through an
// atomic, it never needs locking; a stale frame is harmless.
std::atomic<void*> g_localPlayer{nullptr};

// Options::getPlayerViewPerspective(): 0 = first person, 1/2 = third person.
// Observed so "Show Local Player" can hide the self ESP in first person.
int (*_getPerspective_orig)(void*) = nullptr;
std::atomic<int> s_perspective{0};
std::atomic<bool> s_perspectiveKnown{false};
bool s_perspectiveHooked = false;

// Whether the launcher has the packaged pixel font (see core/PixelFont.hpp).
// Sampled once, at onInit: an unregistered font does not fail loudly, it just
// draws something else, so the module may only ask for it when it really is
// there -- and the nametag is centered on a width measured in that font, which
// is the whole reason the module cares. Queried here rather than at draw time
// because the query opens a file on the first attempt, and the render pass must
// not do that. Being written during init -- i.e. before applyPatch() has handed
// the render thread the hook that reads it -- it needs no atomic of its own.
bool s_pixelFontReady = false;

// The overlay is published from the render hook, which also runs on the render
// thread (before the frame is swapped), so these two flags are only ever read
// and written in order: renderLevel first, then the swap that drives onFrame.
// s_overlayRanThisFrame records "the hook took care of the HUD labels this
// frame"; s_overlayLabelsVisible remembers whether anything is actually on
// screen so the clear is submitted once instead of every menu frame.
std::atomic<bool> s_overlayRanThisFrame{false};
std::atomic<bool> s_overlayLabelsVisible{false};

// How far the corner-bracket style keeps each box edge (fraction of the
// box's shortest side, matching what the old 2D corner box looked like).
constexpr float kCornerBracketFraction = 0.33f;

// How far below an entity's feet the world-space distance readout hangs
// (blocks), so the digits sit just under the box instead of inside it.
constexpr float kDistanceAnchorGap = 0.1f;

// How the label column above an entity's head is stacked: how far its first line
// starts above the head, how far apart two lines sit, how tall the health bar is
// and how narrow it may get, all in surface pixels, and how much of the bar's
// track shows through. Both label paths use them -- the pinned geometry and the
// HUD fallback -- so switching between them never moves a line relative to the
// head it belongs to.
constexpr float kLabelGap = 4.0f;
constexpr float kLabelLineGap = 2.0f;
constexpr float kLabelBarHeight = 4.0f;
constexpr float kLabelMinBarWidth = 20.0f;
constexpr float kLabelBarTrackAlpha = 0.5f;

// Weight of the Crosshair snapline in HUD pixels -- the one tracer that stays on
// the surface, for an entity at or behind the eye. It is the only line this
// module draws itself, so it is the only one that needs a stroke the launcher
// can actually paint: zero would not be drawn at all.
constexpr float kTracerLineWidth = 1.5f;

// Maximum number of actors drawn per frame. fetchNearbyActorsSorted hands the
// list back nearest-first, so a cap keeps the worst case bounded on a busy
// server (a 256-block Range with a mob farm in it would otherwise push tens of
// thousands of box edges through the tessellator every frame).
constexpr int kMaxDrawnActors = 64;

int getPerspectiveHook(void* _this) {
    int result = 0;
    if (_getPerspective_orig) result = _getPerspective_orig(_this);
    s_perspective.store(result, std::memory_order_relaxed);
    s_perspectiveKnown.store(true, std::memory_order_relaxed);
    return result;
}

void tickCallback(void* player) {
    g_localPlayer.store(player, std::memory_order_release);
}

uint32_t forceOpaqueColor(uint32_t color) {
    return color | 0xFF000000u;
}

// s = v = 1 rainbow color for the RGB mode, hue in degrees [0, 360).
uint32_t hsvToRgb(float hue) {
    const float x = 1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hue < 60.0f)       { r = 1.0f; g = x; }
    else if (hue < 120.0f) { r = x;    g = 1.0f; }
    else if (hue < 180.0f) { g = 1.0f; b = x; }
    else if (hue < 240.0f) { g = x;    b = 1.0f; }
    else if (hue < 300.0f) { r = x;    b = 1.0f; }
    else                   { r = 1.0f; b = x; }
    return (static_cast<uint32_t>(r * 255.0f) << 16) |
           (static_cast<uint32_t>(g * 255.0f) << 8) |
            static_cast<uint32_t>(b * 255.0f);
}

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool hasCategory(void* actor, uint32_t categoryBit) {
    if (!actor) return false;
    return reinterpret_cast<bedrocktools::sdk::Actor*>(actor)->hasCategory(categoryBit);
}

AABB getActorAABB(void* actor) {
    auto* sdkActor = reinterpret_cast<bedrocktools::sdk::Actor*>(actor);
    return sdkActor->bounds();
}

bool isDegenerateBox(const AABB& aabb) {
    return aabb.min.x == 0.0f && aabb.min.y == 0.0f && aabb.min.z == 0.0f &&
           aabb.max.x == 0.0f && aabb.max.y == 0.0f && aabb.max.z == 0.0f;
}

// A collision box the overlay can be built from. The all-zero box is the
// game's "no box yet" state (a freshly spawned or unloaded actor) and has
// nothing to outline, and a bound that is not a number would propagate into
// both the geometry and the labels -- where a single non-finite draw command
// makes the launcher reject the whole frame's batch, taking every other
// entity's nametag with it.
bool isUsableBox(const AABB& aabb) {
    const bool finite = std::isfinite(aabb.min.x) && std::isfinite(aabb.min.y) &&
                        std::isfinite(aabb.min.z) && std::isfinite(aabb.max.x) &&
                        std::isfinite(aabb.max.y) && std::isfinite(aabb.max.z);
    return finite && !isDegenerateBox(aabb);
}

Vec2 getActorRotation(void* actor) {
    return reinterpret_cast<bedrocktools::sdk::Actor*>(actor)->rotation();
}

// ---------------------------------------------------------------------------
// Wall occlusion (Amanatides & Woo voxel traversal), mirrored from Hitbox. Only
// needed while Through Walls is off: with it on, nothing is culled and the
// overlay material ignores depth, so terrain never hides an actor.
// ---------------------------------------------------------------------------
bool rayHitsSolid(void* region, float ox, float oy, float oz,
                  float tx, float ty, float tz) {
    if (!region || !s_isSolidBlockingBlock) return false;

    const float dx = tx - ox;
    const float dy = ty - oy;
    const float dz = tz - oz;
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 0.01f) return false;

    int x = (int)floorf(ox);
    int y = (int)floorf(oy);
    int z = (int)floorf(oz);
    const int ex = (int)floorf(tx);
    const int ey = (int)floorf(ty);
    const int ez = (int)floorf(tz);
    if (x == ex && y == ey && z == ez) return false;

    int stepX, stepY, stepZ;
    float tMaxX, tMaxY, tMaxZ;
    float tDeltaX, tDeltaY, tDeltaZ;
    constexpr float kInf = 1e30f;

    if (dx > 0.0f)      { stepX = 1;  tDeltaX = 1.0f / dx;      tMaxX = (x + 1 - ox) * tDeltaX; }
    else if (dx < 0.0f) { stepX = -1; tDeltaX = -1.0f / dx;     tMaxX = (ox - x) * tDeltaX; }
    else                { stepX = 0;  tDeltaX = kInf;           tMaxX = kInf; }

    if (dy > 0.0f)      { stepY = 1;  tDeltaY = 1.0f / dy;      tMaxY = (y + 1 - oy) * tDeltaY; }
    else if (dy < 0.0f) { stepY = -1; tDeltaY = -1.0f / dy;     tMaxY = (oy - y) * tDeltaY; }
    else                { stepY = 0;  tDeltaY = kInf;           tMaxY = kInf; }

    if (dz > 0.0f)      { stepZ = 1;  tDeltaZ = 1.0f / dz;      tMaxZ = (z + 1 - oz) * tDeltaZ; }
    else if (dz < 0.0f) { stepZ = -1; tDeltaZ = -1.0f / dz;     tMaxZ = (oz - z) * tDeltaZ; }
    else                { stepZ = 0;  tDeltaZ = kInf;           tMaxZ = kInf; }

    while (true) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX;
            if (tMaxX > dist) break;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxZ) {
            y += stepY;
            if (tMaxY > dist) break;
            tMaxY += tDeltaY;
        } else {
            z += stepZ;
            if (tMaxZ > dist) break;
            tMaxZ += tDeltaZ;
        }

        // Reached the voxel holding the target point: the target itself is
        // never treated as blocking (a mob hugging a wall stays visible).
        if (x == ex && y == ey && z == ez) break;

        BlockPosI bp{x, y, z};
        if (s_isSolidBlockingBlock(region, bp)) return true;
    }

    return false;
}

// True when both the box center and the top-center are hidden behind solid
// blocks (a tall mob whose head pokes over a wall stays visible).
bool isOccluded(void* region, const Vec3& cam, const AABB& aabb) {
    if (!region || !s_isSolidBlockingBlock) return false;

    const float cx = (aabb.min.x + aabb.max.x) * 0.5f;
    const float cz = (aabb.min.z + aabb.max.z) * 0.5f;
    const float cy = (aabb.min.y + aabb.max.y) * 0.5f;

    if (!rayHitsSolid(region, cam.x, cam.y, cam.z, cx, cy, cz)) return false;
    if (!rayHitsSolid(region, cam.x, cam.y, cam.z, cx, aabb.max.y, cz)) return false;
    return true;
}

// The dimension's BlockSource, which the occlusion test walks.
void* blockSourceFor(void* actor) {
    if (!s_isSolidBlockingBlock || !actor) return nullptr;
    const uintptr_t dimension = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(actor) + offsets::Actor::mDimension);
    if (dimension < 0x1000) return nullptr;
    const uintptr_t blockSource = *reinterpret_cast<uintptr_t*>(
        dimension + offsets::Dimension::mBlockSource);
    return blockSource >= 0x1000 ? reinterpret_cast<void*>(blockSource) : nullptr;
}

// ---------------------------------------------------------------------------
// Health reading. Mob::mHealthAttribute points at an AttributeInstance whose
// mCurrentValue / mMaxValue floats are read directly; non-living actors have
// a null pointer and return a negative current value.
// ---------------------------------------------------------------------------
float readHealth(void* actor, float& outMax) {
    outMax = 0.0f;
    if (!actor) return -1.0f;
    const uintptr_t base = reinterpret_cast<uintptr_t>(actor);
    void* attribute = *reinterpret_cast<void**>(base + offsets::Mob::mHealthAttribute);
    // Only the Mob class owns a health attribute; callers gate this on the
    // living-entity categories, but keep the pointer range check anyway so a
    // stray non-null field never turns into an out-of-bounds read.
    if (!attribute || reinterpret_cast<uintptr_t>(attribute) < 0x1000) return -1.0f;

    const float current = *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(attribute) + offsets::AttributeInstance::mCurrentValue);
    // Best-effort maximum: AttributeInstance stores mCurrentValue, mMinValue
    // and mMaxValue back-to-back, but only mCurrentValue is in the public
    // offsets table. Validate the candidate max and fall back to the current
    // value (full bar) when it reads back degenerate.
    const float maximum = *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(attribute) + offsets::AttributeInstance::mCurrentValue + 8);
    if (std::isfinite(current) && std::isfinite(maximum) && maximum > 0.0f) {
        outMax = (maximum >= current) ? maximum : current;
    } else {
        outMax = std::isfinite(current) ? current : 0.0f;
    }
    return std::isfinite(current) ? current : -1.0f;
}

// ---------------------------------------------------------------------------
// Render materials. selection_box is the wire the block highlight uses; a
// vertex-color fill keeps the filled box solid instead of washing it out; and
// the through-walls pass needs a material whose shader never looks at the
// depth buffer, otherwise "through walls" would only be true for the cull and
// the terrain would still clip the geometry.
// ---------------------------------------------------------------------------
void ensureMaterials() {
    if (!s_renderMaterialGroup) return;

    if (!s_lineMaterial) {
        s_lineMaterial = overlay::getMaterial(s_renderMaterialGroup, "selection_box");
    }

    if (!s_faceMaterial) {
        static const char* const kFillNames[] = {
            "ui_fill_color",
            "ui_textured_and_glcolor",
            "debug_filled_box",
            "selection_box",
        };
        s_faceMaterial = overlay::getFirstMaterial(
            s_renderMaterialGroup, kFillNames, std::size(kFillNames));
    }

    if (!s_throughWallsMaterial) {
        static const char* const kNoDepthNames[] = {
            "ui_fill_color",
            "ui_textured_and_glcolor",
        };
        s_throughWallsMaterial = overlay::getFirstMaterial(
            s_renderMaterialGroup, kNoDepthNames, std::size(kNoDepthNames));
    }
}

// ---------------------------------------------------------------------------
// Radio label tables (indices must match the enums in esp.hpp).
// ---------------------------------------------------------------------------
const char* const kBoxStyleNames[] = {"Box", "Corner"};
const char* const kTracerOriginNames[] = {"Bottom", "Crosshair"};

static_assert(sizeof(kBoxStyleNames) / sizeof(kBoxStyleNames[0]) ==
                  static_cast<int>(EspModule::BoxStyle::Count),
              "every BoxStyle needs exactly one kBoxStyleNames label");
static_assert(sizeof(kTracerOriginNames) / sizeof(kTracerOriginNames[0]) ==
                  static_cast<int>(EspModule::TracerOrigin::Count),
              "every TracerOrigin needs exactly one kTracerOriginNames label");

// ---------------------------------------------------------------------------
// Radio values.
//
// A ConfigType::Radio entry is persisted as "<index>,<label>,<label>..." and
// the launcher reports the plain index when the selection changes (the same
// contract the Crosshair, Effect Display and Wings pickers use). Reading only
// that index, though, meant a config that was written by hand -- or a menu that
// reports the option's text -- was silently ignored and left the module on the
// option it already had, so the label form is resolved here as well.
// ---------------------------------------------------------------------------
inline char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

inline std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

bool textEquals(std::string_view a, std::string_view b) {
    a = trimmed(a);
    b = trimmed(b);
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

// The index the value selects, or -1 when it selects none -- in which case the
// caller keeps the option it has, which is the right response to a config (or a
// menu build) that means something else entirely.
int radioIndexOf(const nlohmann::json& value, const char* const* labels, int count) {
    std::string text;
    if (value.is_string()) {
        text = value.get<std::string>();
    } else if (value.is_number_integer()) {
        text = std::to_string(value.get<int>());
    } else {
        return -1;
    }

    const std::size_t comma = text.find(',');
    const std::string_view head = trimmed(
        std::string_view(text).substr(0, comma == std::string::npos ? text.size() : comma));

    std::size_t used = 0;
    try {
        const int index = std::stoi(std::string(head), &used);
        if (used == head.size() && index >= 0 && index < count) return index;
    } catch (...) {}

    // A lone label. When a comma is present the string is the option list
    // rather than a selection, and matching its first entry would be wrong.
    if (comma == std::string::npos) {
        for (int i = 0; i < count; ++i) {
            if (textEquals(head, labels[i])) return i;
        }
    }
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// Draw-command helpers for the HUD layer (operate on the caller's list).
//
// The launcher validates a module's whole batch before it draws any of it: one
// non-finite coordinate (or one text longer than its own limit) makes it reject
// *every* command of the frame, so a single actor with a broken collision box
// would blank the nametags of all the others. Commands are checked on the way
// in, which caps the damage of bad data at the entity that carries it.
// ---------------------------------------------------------------------------
namespace {

void pushCommand(std::vector<PLModMenu_DrawCommand>& cmds,
                 const PLModMenu_DrawCommand& cmd) {
    const bool finite = std::isfinite(cmd.x) && std::isfinite(cmd.y) &&
                        std::isfinite(cmd.w) && std::isfinite(cmd.h) &&
                        std::isfinite(cmd.size);
    if (finite) cmds.push_back(cmd);
}

void addRect(std::vector<PLModMenu_DrawCommand>& cmds, float x, float y,
             float w, float h, uint32_t color) {
    PLModMenu_DrawCommand cmd{};
    cmd.type = PL_DRAW_RECT_FILLED;
    cmd.x = x;
    cmd.y = y;
    cmd.w = w;
    cmd.h = h;
    cmd.color = color;
    pushCommand(cmds, cmd);
}

// The two font ids the launcher's text commands take: the packaged pixel font
// (registered by core/PixelFont.hpp) and the empty id that means "the
// launcher's own face", which is the only one with the glyphs and the bidi
// shaping for a name written outside the pixel font.
const char* fontIdFor(esp::LabelFont font) {
    return font == esp::LabelFont::Pixel ? bedrocktools::core::kPixelFontId : "";
}

// Appends a text label to the batch. The face it is drawn with and the width it
// comes out at are decided here, in one place, so that the box the launcher lays
// the text out in and the position it is centered from can never disagree --
// separately is how a label ends up beside the head instead of above it.
//
//   * `x` is the left edge of the label, or -- with `centered` -- the point the
//     label is centered on, which is what the nametag and the health value use
//     to sit on the head column;
//   * the launcher's pixel font is asked for whenever it can draw the text,
//     because that is the only face whose metrics are known (see
//     esp::labelFontFor); anything it has no glyphs for goes to the launcher's
//     default, which shapes it properly instead of drawing boxes for it.
void addText(std::vector<PLModMenu_DrawCommand>& cmds, const std::string& text,
             float x, float y, float size, uint32_t color, bool centered = false) {
    const esp::LabelFont font = esp::labelFontFor(text, s_pixelFontReady);
    const float width = esp::measureTextWidth(text, size, font);

    PLModMenu_DrawCommand cmd{};
    cmd.type = PL_DRAW_TEXT;
    cmd.x = centered ? x - width * 0.5f : x;
    cmd.y = y;
    cmd.w = width;
    cmd.h = size + 3.0f;
    cmd.size = size;
    cmd.color = color;
    cmd.text = text;
    cmd.fontId = fontIdFor(font);
    pushCommand(cmds, cmd);
}

// A HUD line: the launcher reads the start point from x/y and the *delta* to
// the end point from w/h (the convention the Crosshair module draws its arms
// with), so this takes both endpoints and converts.
void addLine(std::vector<PLModMenu_DrawCommand>& cmds, float x0, float y0,
             float x1, float y1, uint32_t color) {
    PLModMenu_DrawCommand cmd{};
    cmd.type = PL_DRAW_LINE;
    cmd.x = x0;
    cmd.y = y0;
    cmd.w = x1 - x0;
    cmd.h = y1 - y0;
    cmd.size = kTracerLineWidth;
    cmd.color = color;
    pushCommand(cmds, cmd);
}

// Submits the HUD layer, or drops it when nothing is left to show. Called from
// the render hook (same frame as the geometry) and from every early-out so a
// stale overlay never hangs on screen.
void publishLabels(std::vector<PLModMenu_DrawCommand>& labels) {
    if (!g_espMod) return;
    submitDrawCommands(g_espMod->moduleId, labels);
    s_overlayLabelsVisible.store(!labels.empty(), std::memory_order_relaxed);
}

void clearLabels() {
    if (!g_espMod) return;
    if (s_overlayLabelsVisible.exchange(false, std::memory_order_relaxed)) {
        submitDrawCommands(g_espMod->moduleId, std::vector<PLModMenu_DrawCommand>{});
    }
}

// ---------------------------------------------------------------------------
// The overlay itself: world-space geometry for the boxes, plus the screen-space
// labels the HUD layer has to draw. Runs inside LevelRenderer::renderLevel, so
// camPos is the exact camera the level is being rendered with.
// ---------------------------------------------------------------------------
void drawWorldOverlay(void* levelRenderer, void* screenContext,
                      std::vector<PLModMenu_DrawCommand>& labels) {
    if (!g_espMod || !g_espMod->enabled) return;
    const EspModule& options = *g_espMod;

    void* localPlayer = g_localPlayer.load(std::memory_order_acquire);
    if (!localPlayer || !s_actorFetchNearby) return;
    if (!levelRenderer || reinterpret_cast<uintptr_t>(levelRenderer) < 0x1000) return;
    if (!screenContext || reinterpret_cast<uintptr_t>(screenContext) < 0x1000) return;

    const uintptr_t rendererAddress = reinterpret_cast<uintptr_t>(levelRenderer);
    const uintptr_t playerRenderer = *reinterpret_cast<uintptr_t*>(
        rendererAddress + offsets::LevelRenderer::mLevelRendererPlayer);
    if (playerRenderer < 0x1000) return;

    // Camera position from the renderer the game is filling in right now.
    const Vec3 camPos = *reinterpret_cast<const Vec3*>(
        playerRenderer + offsets::LevelRendererPlayer::mCamPos);
    if (!std::isfinite(camPos.x) || !std::isfinite(camPos.y) || !std::isfinite(camPos.z)) return;

    // ---- world-space half: tessellator + materials ------------------------
    const uintptr_t tessellatorAddress = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(screenContext) + offsets::ScreenContext::mTessellator);
    overlay::Mesh mesh = s_mesh;
    mesh.tessellator =
        tessellatorAddress >= 0x1000 ? reinterpret_cast<void*>(tessellatorAddress) : nullptr;

    ensureMaterials();
    void* const embeddedOverlayMaterial = reinterpret_cast<void*>(
        playerRenderer + offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    void* lineMaterial = nullptr;
    void* faceMaterial = nullptr;
    if (options.throughWalls) {
        // Same material for the wire and the faces so their occlusion agrees,
        // and no depth test means walls never clip either of them.
        void* const noDepth =
            s_throughWallsMaterial ? static_cast<void*>(&s_throughWallsMaterial)
                                   : embeddedOverlayMaterial;
        lineMaterial = noDepth;
        faceMaterial = noDepth;
    } else {
        lineMaterial = s_lineMaterial ? static_cast<void*>(&s_lineMaterial)
                                      : embeddedOverlayMaterial;
        faceMaterial = embeddedOverlayMaterial;
    }

    // The renderer multiplies the vertex colors by ScreenContext::mColorHolder,
    // so the overlay has to override it for its own draws and restore it
    // afterwards (exactly what Hitbox and Block Outline do). Without a color
    // holder the world-space half is skipped rather than drawn in the wrong
    // tint; the labels do not depend on it and stay up.
    float* colorHolder = nullptr;
    const uintptr_t colorHolderAddress = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(screenContext) + offsets::ScreenContext::mColorHolder);
    if (colorHolderAddress >= 0x1000) {
        colorHolder = reinterpret_cast<float*>(colorHolderAddress);
    }
    bool meshReady = mesh.ready() && colorHolder != nullptr;
    float savedColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (meshReady) {
        savedColor[0] = colorHolder[0];
        savedColor[1] = colorHolder[1];
        savedColor[2] = colorHolder[2];
        savedColor[3] = colorHolder[3];
        colorHolder[0] = 1.0f;
        colorHolder[1] = 1.0f;
        colorHolder[2] = 1.0f;
        colorHolder[3] = 1.0f;
    }

    // ---- screen-space half: the HUD layer's projection --------------------
    const Vec2 camRot = getActorRotation(localPlayer);
    const esp::Camera camera = esp::computeCamera(camPos, camRot);
    const pl::modmenu::HudSurfaceSize surface = pl::modmenu::getHudSurfaceSize();
    const esp::SurfaceProjection proj =
        esp::makeProjection(surface.width, surface.height, options.fov);
    // Off-screen clamp for projections: an entity straddling the camera plane
    // projects far outside the surface, and keeping the coordinates to a few
    // screen sizes keeps the launcher's draw commands finite.
    const float boxLimit = std::max(proj.width, proj.height) * 4.0f;

    // Distance anchor: the local player's own feet (bottom-center of its
    // collision box), resolved once per frame. Measuring from the render
    // camera instead would make the readout perspective-dependent -- third
    // person pulls the camera several blocks behind the player -- so the same
    // entity would report one distance in first person and another in third
    // person. The camera stays the anchor for everything that is genuinely
    // about the view (projection, occlusion, camera-relative vertices).
    const AABB selfBox = getActorAABB(localPlayer);
    const Vec3 selfFeet{(selfBox.min.x + selfBox.max.x) * 0.5f, selfBox.min.y,
                        (selfBox.min.z + selfBox.max.z) * 0.5f};
    const bool selfFeetValid = isUsableBox(selfBox);

    // Resolve the dimension's BlockSource once per frame, and only when the
    // occlusion cull is actually wanted (Through Walls off).
    void* region = options.throughWalls ? nullptr : blockSourceFor(localPlayer);

    // Fetch nearby actors (radius = configured range).
    const float radius = std::clamp(options.range, 8.0f, 256.0f);
    Vec3 extent = {radius, radius, radius};
    const ActorVec actors = s_actorFetchNearby(localPlayer, &extent, 1);

    // Resolve the box color once per frame (RGB animation wins over boxColor).
    uint32_t boxRgb;
    if (options.rgb) {
        const float speed = std::clamp(options.rgbSpeed, 0.05f, 1.0f);
        float hue = std::fmod(static_cast<float>(nowSeconds()) * speed * 360.0f, 360.0f);
        if (hue < 0.0f) hue += 360.0f;
        boxRgb = hsvToRgb(hue) | 0xFF000000u;
    } else {
        boxRgb = forceOpaqueColor(options.boxColor);
    }

    // Menu thickness slider -> world-space half width. 1.0 (or lower) keeps the
    // game's hairline edges; above that every edge becomes a real beam, because
    // GL line width is ignored by nearly every mobile GLES driver. Same mapping
    // and scale as the Hitbox module, so the two sliders agree.
    const float thickness = std::clamp(options.boxThickness, 1.0f, 20.0f);
    const float beamHalfWidth =
        (thickness > 1.05f && meshReady) ? thickness * 0.01f * 0.5f : 0.0f;

    const float nametagSize = std::clamp(options.nametagScale, 0.5f, 2.0f) * 14.0f;
    const float subSize = nametagSize * 0.8f;

    // First-person camera check for the local-player ESP. Until the game has
    // reported a perspective value, stay conservative and hide the self ESP
    // (the camera is almost always first person right after launch).
    const bool thirdPerson = s_perspectiveKnown.load(std::memory_order_relaxed) &&
                             s_perspective.load(std::memory_order_relaxed) != 0;

    std::vector<overlay::Segment> boxSegments;
    std::vector<overlay::Quad> fillQuads;
    if (options.box) {
        const std::size_t expected = static_cast<std::size_t>(kMaxDrawnActors);
        boxSegments.reserve(expected * 12);
        if (options.boxFilled) fillQuads.reserve(expected * 6);
    }
    // Both tracers are one world-space segment per entity now -- the feet line
    // from the local player's own feet, the crosshair line from a point on the
    // view axis (see the tracer block in renderActor) -- and so is the label
    // column: one text batch for every billboarded line (the distance readout,
    // the nametag, the health value), because they are all one color, and one
    // grouped batch for the bars, whose track and fill are not.
    std::vector<overlay::Segment> tracerSegments;
    std::vector<overlay::Quad> textQuads;
    std::vector<overlay::Quad> barQuads;
    std::vector<overlay::Mesh::QuadGroup> barGroups;
    if (options.tracer) tracerSegments.reserve(kMaxDrawnActors);
    // A reserve, not a budget: the readout is six glyphs of at most five
    // rectangles, a label column up to 24 glyphs of at most ten merged ones, and
    // most actors are drawn with a far shorter label -- growing the vector is
    // cheaper than the alternative.
    if (options.nametag || options.health) {
        textQuads.reserve(static_cast<std::size_t>(kMaxDrawnActors) * 48);
        barQuads.reserve(static_cast<std::size_t>(kMaxDrawnActors) * 2);
        barGroups.reserve(static_cast<std::size_t>(kMaxDrawnActors) * 2);
    } else if (options.distance) {
        textQuads.reserve(static_cast<std::size_t>(kMaxDrawnActors) * 30);
    }

    int drawn = 0;
    auto renderActor = [&](void* ent, bool isSelf) {
        if (!ent) return;
        ++drawn;

        const AABB aabb = getActorAABB(ent);
        if (!isUsableBox(aabb)) return;

        // Occlusion cull (only ever active with Through Walls off, since that
        // is when `region` is resolved, and never for the local player, whose
        // camera often sits inside geometry in third person).
        if (!isSelf && region && isOccluded(region, camPos, aabb)) return;

        // ---- the box: world-space geometry, placed by the game --------------
        if (meshReady && options.box && lineMaterial) {
            if (options.boxStyle == EspModule::BoxStyle::Corner) {
                esp::world::addBoxCorners(boxSegments, aabb, kCornerBracketFraction);
            } else {
                esp::world::addBoxEdges(boxSegments, aabb);
            }
            if (options.boxFilled && faceMaterial) {
                esp::world::addBoxFaces(fillQuads, aabb);
            }
        }

        // ---- Tracer: a line that ends inside the hitbox -------------------
        // Both origins aim at the same world point -- the center of the entity's
        // own box -- and both are geometry now, because geometry is the only way
        // the far end stays *on* the hitbox: a projected end sits where the
        // module's model of the camera said the box was, and that model is what
        // moves (the Fov slider, sprint FOV, a frame of look latency). What
        // differs is where the line starts:
        //
        //   * Bottom starts at the local player's own feet. Without a usable
        //     local box the line is skipped: falling back to the camera would
        //     put it in the degenerate case below and draw nothing at all.
        //   * Crosshair starts on the view axis, a short way in front of the eye.
        //     Every point of that axis projects to the middle of the screen at any
        //     depth, which is what makes a crosshair line drawable as geometry in
        //     the first place -- a line that *starts at the eye* lies on one
        //     single view ray, the game collapses all of it onto one pixel, and
        //     the tracer disappeared with it. That was the "Crosshair removes the
        //     tracer" this module used to have, and the reason the origin was
        //     never as steady as Bottom (see esp::world::addCrosshairTracer).
        //
        // Two cases stay on the surface, both because there is nothing for the
        // game to be given: an entity at or behind the eye has no pixels for a
        // segment to end on, only a direction worth showing (the snapline), and
        // without a mesh at all there is no world pass to draw in.
        //
        // The local player never gets a tracer to itself, and the line stays
        // hairline either way: a thickness beam would fill the screen where it
        // starts at (or passes right by) the camera.
        if (options.tracer && !isSelf) {
            const Vec3 target = esp::boxCenter(aabb.min, aabb.max);
            if (options.tracerOrigin == EspModule::TracerOrigin::Crosshair) {
                const bool pinned =
                    meshReady && lineMaterial &&
                    esp::world::addCrosshairTracer(tracerSegments, camera, proj, target);
                if (!pinned) {
                    const esp::ScreenSegment line =
                        esp::crosshairTracer(camera, proj, target);
                    if (line.visible) {
                        addLine(labels, line.x0, line.y0, line.x1, line.y1,
                                forceOpaqueColor(options.tracerColor));
                    }
                }
            } else if (meshReady && selfFeetValid) {
                tracerSegments.push_back({selfFeet, target});
            }
        }

        // ---- Distance: world-space, pinned under the entity's feet ----------
        // A HUD-projected readout slides off the box whenever the module's
        // camera model disagrees with the game's (sprint FOV, view bob, a
        // frame of look latency), so the value is drawn as billboarded world
        // quads anchored just below the entity's feet instead. The projection
        // only sizes the digits (a constant apparent height at any range); it
        // never positions them, so an FOV mismatch cannot detach the readout.
        // The measurement itself stays feet-to-feet between the two collision
        // boxes -- identical in first and third person -- and stays hidden
        // while the local anchor is unavailable rather than switching to a
        // camera-based number.
        if (meshReady && faceMaterial && options.distance && selfFeetValid) {
            const float entFeetX = (aabb.min.x + aabb.max.x) * 0.5f;
            const float entFeetZ = (aabb.min.z + aabb.max.z) * 0.5f;
            const float dx = selfFeet.x - entFeetX;
            const float dy = selfFeet.y - aabb.min.y;
            const float dz = selfFeet.z - entFeetZ;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

            char buffer[24];
            std::snprintf(buffer, sizeof(buffer), "%.1fm", dist);
            const Vec3 anchor{entFeetX, aabb.min.y - kDistanceAnchorGap, entFeetZ};
            esp::world::Billboard billboard;
            if (esp::world::makeBillboard(billboard, camera, proj, anchor)) {
                esp::world::addBillboardText(textQuads, billboard, buffer, subSize);
            }
        }

        // ---- The label column above the head: nametag + health --------------
        // The same trick as the readout, for the two labels that used to be HUD
        // furniture: they hang off the *world* head point -- the top-center of the
        // entity's own box -- and are laid out in surface pixels around it. Only
        // the pixel scale consults the projection, so a name and its health bar sit
        // on the head in the same pass that drew the wireframe around it, at any
        // FOV, sprinting or not. The 2D box's top-middle, which the HUD path below
        // still centers on, is a screen-space average of eight projected corners
        // that perspective shifts away from the head -- and an FOV the module has
        // not been told shifts the whole column with it.
        //
        // The name is read straight out of the game's Player, and the raw field is
        // not what the game draws: it is cleaned on the way in, because the
        // section-sign markup codes a server or a nick add-on pads it with and the
        // invisible format characters a right-to-left name arrives wrapped in would
        // otherwise be painted literally (see esp::sanitizeName).
        std::string name;
        if (options.nametag && s_actorIsPlayer && s_actorIsPlayer(ent)) {
            name = esp::sanitizeName(
                reinterpret_cast<bedrocktools::sdk::Player*>(ent)->name());
        }

        // Living entities only: players and mobs own a health attribute. The value
        // and the fraction are resolved once, because both label paths draw them.
        float healthValue = -1.0f;
        float healthFraction = 0.0f;
        const bool living = (s_actorIsPlayer && s_actorIsPlayer(ent)) ||
                            hasCategory(ent, offsets::ActorCategories::IsMob);
        if (options.health && living) {
            float maxHealth = 0.0f;
            const float current = readHealth(ent, maxHealth);
            if (current >= 0.0f && std::isfinite(current)) {
                healthValue = current;
                healthFraction =
                    std::clamp(current / (maxHealth > 0.0f ? maxHealth : current), 0.0f, 1.0f);
            }
        }

        // The face this column is spelled with is the game's own, and it has no
        // cells outside printable ASCII. A name it cannot write -- Arabic, CJK, a
        // stray markup sign, a server's forty-character art -- therefore keeps the
        // launcher's font for that entity, whole column and all: right characters
        // a little adrift beat a pinned label with holes in it. The same test the
        // HUD path uses to decide it may measure itself in the pixel font, so a
        // name is only ever in one of the two.
        esp::world::Billboard billboard;
        const bool pinnedColumn =
            (options.nametag || options.health) && meshReady && lineMaterial &&
            faceMaterial &&
            (name.empty() || esp::world::billboardTextFits(name)) &&
            esp::world::makeBillboard(billboard, camera, proj,
                                      esp::boxTopCenter(aabb.min, aabb.max));

        if (pinnedColumn) {
            // Top down, as surface pixels above the head: the nametag line, then
            // the health value with its bar under it. The numbers are the HUD
            // column's, with the y axis flipped (a billboard's `downPx` grows
            // downwards, exactly like the surface's), so the two paths stack a
            // label the same way and only differ in what pins it.
            float lineTop = -(nametagSize + kLabelGap);

            // Centered on the head column, on the cell widths of the face that
            // draws it -- which is the whole reason this pass can center a name at
            // all, and why a byte-counted label used to land beside the head.
            if (options.nametag && !name.empty()) {
                // shifted() first, because a billboard hangs down from its anchor
                // and the anchor is the head itself: without the lift the whole
                // name would be written into the box.
                esp::world::addBillboardText(textQuads, billboard.shifted(0.0f, lineTop),
                                             name, nametagSize);
                lineTop -= nametagSize + kLabelLineGap;
            }

            if (healthValue >= 0.0f) {
                // As wide as the hitbox it sits on, however that box is turned --
                // the same number the wireframe gives the screen, without waiting
                // for the projection to agree about it.
                const float barW = std::max(
                    esp::world::boxPixelWidth(camera, aabb, billboard.scale),
                    kLabelMinBarWidth);
                const float barTop = lineTop + subSize - kLabelBarHeight;

                // Track first, then the fill: the groups are emitted in order, so
                // this is what puts the fill on top of the track. Both stay in one
                // mesh even though they are not the same color, because the
                // Tessellator stamps the color it was last handed.
                const std::size_t trackBegin = barQuads.size();
                billboard.addBox(barQuads, -barW * 0.5f, barTop, barW, kLabelBarHeight);
                barGroups.push_back({0xFF000000u, kLabelBarTrackAlpha, trackBegin,
                                     barQuads.size()});

                const uint32_t fillColor = healthFraction > 0.5f   ? 0xFF22C55Eu
                                           : healthFraction > 0.25f ? 0xFFEAB308u
                                                                    : 0xFFEF4444u;
                const std::size_t fillBegin = barQuads.size();
                billboard.addBox(barQuads, -barW * 0.5f, barTop, barW * healthFraction,
                                 kLabelBarHeight);
                barGroups.push_back({forceOpaqueColor(fillColor), 1.0f, fillBegin,
                                     barQuads.size()});

                // The value starts at the left end of its bar rather than on the
                // head column, exactly where the HUD path puts it -- so a shifted
                // anchor, not a second measurement to disagree about.
                char buffer[24];
                std::snprintf(buffer, sizeof(buffer), "%.0f", healthValue);
                const esp::world::Billboard valueLine =
                    billboard.shifted(-barW * 0.5f, barTop - subSize - kLabelLineGap);
                esp::world::addBillboardText(textQuads, valueLine, buffer, subSize,
                                             esp::world::TextAlignment::Left);
            }
        } else if (options.nametag || options.health) {
            // ---- the HUD column: the fallback the launcher font is kept for ----
            // Positioned by the module's projection, which is the older half of
            // this module and the driftier one; it stays here for the names the
            // pixel face cannot spell and for a frame with no mesh to draw on.
            const esp::ScreenBox screen =
                esp::projectBox(camera, proj, aabb.min, aabb.max, boxLimit);
            if (!screen.visible) return;

            // Nothing to draw once the box has slid completely off the surface:
            // a label at a coordinate that far out is only worth submitting as a
            // number the launcher has to be guarded against.
            if (screen.maxX < 0.0f || screen.minX > proj.width ||
                screen.maxY < 0.0f || screen.minY > proj.height) {
                return;
            }

            const float boxLeft = screen.minX;
            const float boxRight = screen.maxX;
            const float boxTop = screen.minY;
            const float boxW = boxRight - boxLeft;
            const float centerX = (boxLeft + boxRight) * 0.5f;

            // Centered on the projected head point -- the top-center of the
            // entity's own box -- rather than the 2D box's top-middle, which
            // perspective shifts away from the head (see esp::projectBoxTopCenter).
            // When the head itself is behind the near plane, the 2D box is the
            // fallback.
            float headX = centerX;
            float headY = boxTop;
            float topX = 0.0f, topY = 0.0f;
            if (esp::projectBoxTopCenter(camera, proj, aabb.min, aabb.max, topX, topY)) {
                headX = std::clamp(topX, -boxLimit, proj.width + boxLimit);
                headY = std::clamp(topY, -boxLimit, proj.height + boxLimit);
            }
            float textY = headY - nametagSize - kLabelGap;

            if (options.nametag && !name.empty()) {
                addText(labels, name, headX, textY, nametagSize,
                        forceOpaqueColor(options.nametagColor), /*centered=*/true);
                textY -= nametagSize + kLabelLineGap;
            }

            if (healthValue >= 0.0f) {
                const float barW = std::max(boxW, kLabelMinBarWidth);
                const float barY = textY + subSize - kLabelBarHeight; // under the label

                addRect(labels, headX - barW * 0.5f, barY, barW, kLabelBarHeight,
                        0x80000000u);
                const uint32_t fillColor = healthFraction > 0.5f   ? 0xFF22C55Eu
                                           : healthFraction > 0.25f ? 0xFFEAB308u
                                                                    : 0xFFEF4444u;
                addRect(labels, headX - barW * 0.5f, barY, barW * healthFraction,
                        kLabelBarHeight, fillColor);

                char buffer[24];
                std::snprintf(buffer, sizeof(buffer), "%.0f", healthValue);
                addText(labels, buffer, headX - barW * 0.5f, barY - subSize - kLabelLineGap,
                        subSize, forceOpaqueColor(options.nametagColor));
            }
        }
    };

    // Local player (third person only).
    if (options.showLocalPlayer && thirdPerson) {
        renderActor(localPlayer, true);
    }

    // Nearby actors, nearest first (that is the order the game's fetch sorts
    // them in), so the per-frame cap drops the far ones instead of the actors
    // the player is actually looking at.
    if (actors.begin && actors.end) {
        for (DistanceSortedActor* it = actors.begin;
             it < actors.end && drawn < kMaxDrawnActors; ++it) {
            void* ent = it->mActor;
            if (!ent || ent == localPlayer) continue;

            bool isPlayer = false;
            if (s_actorIsPlayer) isPlayer = s_actorIsPlayer(ent);

            if (isPlayer) {
                if (!options.showPlayers) continue;
            } else if (hasCategory(ent, offsets::ActorCategories::IsMob)) {
                if (!options.showMobs) continue;
            } else {
                if (!options.showItems) continue;
            }

            if (s_actorIsInvisible && s_actorIsInvisible(ent)) continue;

            renderActor(ent, false);
        }
    }

    // ---- flush the world-space half ---------------------------------------
    if (meshReady && lineMaterial) {
        // Faces first so the wireframe stays readable on top of the fill.
        if (options.box && options.boxFilled && faceMaterial && !fillQuads.empty()) {
            mesh.drawQuads(screenContext, faceMaterial, camPos,
                           forceOpaqueColor(options.boxFilledColor),
                           std::clamp(options.boxFilledOpacity, 0.0f, 1.0f), fillQuads);
        }
        if (options.box && !boxSegments.empty()) {
            mesh.drawSegments(screenContext, lineMaterial, camPos, boxRgb, 1.0f,
                              boxSegments, beamHalfWidth);
        }
        // Both tracers after the wireframe, hairline only (see renderActor): the
        // same material as the box edges, so Through Walls governs them too, and
        // both origins are geometry for the same reason -- a line the game places
        // cannot come off the hitbox it ends in. The only line that is *not* here
        // is the snapline for an entity behind the camera, whose far end has no
        // pixels to be pinned to.
        if (!tracerSegments.empty()) {
            mesh.drawSegments(screenContext, lineMaterial, camPos,
                              forceOpaqueColor(options.tracerColor), 1.0f,
                              tracerSegments, 0.0f);
        }
        // The health bars before the text, and the text last, so a name stays
        // readable over the wireframe, the tracer and its own bar. The bars group
        // their colors in one mesh -- a track is black at half alpha and a fill is
        // green, yellow or red, and the Tessellator colors from the call onwards
        // rather than per draw (see overlay::Mesh::drawQuadsGrouped); the text is
        // one batch because every line this pass spells shares the nametag color.
        if (!barQuads.empty() && faceMaterial) {
            mesh.drawQuadsGrouped(screenContext, faceMaterial, camPos, barQuads,
                                  barGroups);
        }
        if (!textQuads.empty() && faceMaterial) {
            mesh.drawQuads(screenContext, faceMaterial, camPos,
                           forceOpaqueColor(options.nametagColor), 1.0f, textQuads);
        }
    }

    // Only the mesh pass touched the color holder; restoring it unconditionally
    // would rewrite a value the module never read.
    if (meshReady && colorHolder) {
        colorHolder[0] = savedColor[0];
        colorHolder[1] = savedColor[1];
        colorHolder[2] = savedColor[2];
        colorHolder[3] = savedColor[3];
    }
}

void renderOverlay(void* levelRenderer, void* screenContext) {
    // onFrame uses this to tell "the world is rendering, the hook owns the HUD
    // labels" apart from "nothing drew this frame, so drop what is left".
    s_overlayRanThisFrame.store(true, std::memory_order_relaxed);

    std::vector<PLModMenu_DrawCommand> labels;
    drawWorldOverlay(levelRenderer, screenContext, labels);
    publishLabels(labels);
}

void renderLevelHook(void* levelRenderer, void* screenContext, void* renderParams) {
    if (s_renderLevelOriginal) s_renderLevelOriginal(levelRenderer, screenContext, renderParams);
    renderOverlay(levelRenderer, screenContext);
}

} // namespace

// ---------------------------------------------------------------------------
// Module lifecycle.
// ---------------------------------------------------------------------------
EspModule::EspModule()
    : Module("Esp",
             "Draws entity boxes, tracers and distance readouts as world-space geometry in "
             "the game's own render pass, so they stay locked to their target, and keeps "
             "them visible through walls.") {
    showInMenu = true;
    hideInHudEditor = true; // world overlay, not a draggable HUD element
    g_espMod = this;
}

EspModule::~EspModule() {
    if (g_espMod == this) g_espMod = nullptr;
}

void EspModule::onInit() {
    // The nametag is HUD text, and HUD text is centered on a width the module
    // has to supply, so the answer only means something for a font whose
    // metrics are known. The packaged one is the game's own pixel font and is
    // the only such face, so it is registered here (once per package, shared
    // with Effect Display) and its availability is what addText consults.
    s_pixelFontReady = bedrocktools::core::pixelFontAvailable();

    const auto resolveFn = [](SignatureId id) { return bedrocktools::memory::resolve(id); };

    const uintptr_t aip = resolveFn(SignatureId::ActorIsPlayer);
    if (aip) s_actorIsPlayer = reinterpret_cast<Actor_isPlayer_t>(aip);

    const uintptr_t aii = resolveFn(SignatureId::ActorIsInvisible);
    if (aii) s_actorIsInvisible = reinterpret_cast<Actor_isInvisible_t>(aii);

    const uintptr_t afn = resolveFn(SignatureId::ActorFetchNearbyActorsSorted);
    if (afn) s_actorFetchNearby = reinterpret_cast<Actor_fetchNearbyActorsSorted_t>(afn);

    const uintptr_t isb = resolveFn(SignatureId::BlockSourceIsSolidBlockingBlock);
    if (isb) s_isSolidBlockingBlock = reinterpret_cast<BlockSource_isSolidBlockingBlock_t>(isb);

    // The render pass the geometry is drawn in: same hook target as Hitbox and
    // Block Outline (the hook manager chains them).
    const uintptr_t renderLevel = resolveFn(SignatureId::RenderLevel);
    if (renderLevel) m_patchTarget = reinterpret_cast<void*>(renderLevel);

    const uintptr_t tessBegin = resolveFn(SignatureId::TessellatorBegin);
    if (tessBegin) s_mesh.begin = reinterpret_cast<overlay::Mesh::BeginFn>(tessBegin);

    const uintptr_t tessColor = resolveFn(SignatureId::TessellatorColor);
    if (tessColor) s_mesh.color = reinterpret_cast<overlay::Mesh::ColorFn>(tessColor);

    const uintptr_t tessVertex = resolveFn(SignatureId::TessellatorVertex);
    if (tessVertex) s_mesh.vertex = reinterpret_cast<overlay::Mesh::VertexFn>(tessVertex);

    uintptr_t renderMesh = resolveFn(SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!renderMesh) renderMesh = resolveFn(SignatureId::MeshHelpersRenderMeshImmediately);
    if (renderMesh) s_mesh.render = reinterpret_cast<overlay::Mesh::RenderFn>(renderMesh);

    const uintptr_t materialGroup = resolveFn(SignatureId::RenderMaterialGroupCommon);
    if (materialGroup) {
        const std::uintptr_t groupAddress = overlay::resolveAdrp(
            reinterpret_cast<const std::uint32_t*>(materialGroup), 2, 0);
        if (groupAddress) {
            s_renderMaterialGroup =
                groupAddress + offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { tickCallback(reinterpret_cast<void*>(event.player)); });

    if (!s_perspectiveHooked) {
        const uintptr_t perspective = resolveFn(SignatureId::GetPerspective);
        if (perspective &&
            bedrocktools::hooks::install(reinterpret_cast<void*>(perspective),
                                         reinterpret_cast<void*>(getPerspectiveHook),
                                         reinterpret_cast<void**>(&_getPerspective_orig))) {
            s_perspectiveHooked = true;
        }
    }
}

void EspModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    const auto handle = bedrocktools::hooks::install(
        m_patchTarget, reinterpret_cast<void*>(&renderLevelHook),
        reinterpret_cast<void**>(&s_renderLevelOriginal));
    m_patched = handle != nullptr;
}

void EspModule::onEnable() {
    applyPatch();
}

void EspModule::onDisable() {
    // The overlay is published from the render hook, so dropping it means
    // clearing the HUD layer as well; onFrame stops running once disabled.
    submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
    s_overlayLabelsVisible.store(false, std::memory_order_relaxed);
}

void EspModule::onFrame() {
    if (!enabled) {
        clearLabels();
        return;
    }

    // Nothing to do while the world is being rendered: renderOverlay already
    // published this frame's overlay. If the render hook did not run (menu
    // screen, level not drawn), the labels it left behind are dropped once so
    // a frozen ESP never hangs over the rest of the UI.
    if (!s_overlayRanThisFrame.exchange(false, std::memory_order_relaxed)) {
        clearLabels();
    }
}

// ---------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------
void EspModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    showPlayers = j.value("showPlayers", showPlayers);
    showMobs = j.value("showMobs", showMobs);
    showItems = j.value("showItems", showItems);
    showLocalPlayer = j.value("showLocalPlayer", showLocalPlayer);

    // Default flipped when the overlay moved into the render pass: an ESP is
    // expected to keep drawing through terrain. Configs that saved a value
    // explicitly keep it.
    throughWalls = j.value("throughWalls", throughWalls);
    range = j.value("range", range);

    box = j.value("box", box);
    boxFilled = j.value("boxFilled", boxFilled);

    // Radio values are read through one helper so a picker that reports the
    // option's *label* instead of its index still selects it (see radioIndexOf).
    if (j.contains("boxStyle")) {
        const int style = radioIndexOf(j["boxStyle"], kBoxStyleNames,
                                        static_cast<int>(BoxStyle::Count));
        if (style >= 0) boxStyle = static_cast<BoxStyle>(style);
    }

    if (j.contains("tracerOrigin")) {
        const int origin = radioIndexOf(j["tracerOrigin"], kTracerOriginNames,
                                        static_cast<int>(TracerOrigin::Count));
        if (origin >= 0) tracerOrigin = static_cast<TracerOrigin>(origin);
    }

    tracer = j.value("tracer", tracer);
    nametag = j.value("nametag", nametag);
    health = j.value("health", health);
    distance = j.value("distance", distance);
    rgb = j.value("rgb", rgb);

    if (j.contains("rgbSpeed")) {
        try { rgbSpeed = std::clamp(j["rgbSpeed"].get<float>(), 0.05f, 1.0f); } catch (...) {}
    }
    if (j.contains("boxThickness")) {
        try { boxThickness = std::clamp(j["boxThickness"].get<float>(), 0.5f, 20.0f); } catch (...) {}
    }
    if (j.contains("boxFilledOpacity")) {
        try { boxFilledOpacity = std::clamp(j["boxFilledOpacity"].get<float>(), 0.0f, 1.0f); } catch (...) {}
    }
    if (j.contains("nametagScale")) {
        try { nametagScale = std::clamp(j["nametagScale"].get<float>(), 0.5f, 2.0f); } catch (...) {}
    }
    if (j.contains("fov")) {
        try { fov = std::clamp(j["fov"].get<float>(), 30.0f, 120.0f); } catch (...) {}
    }

    auto parseColor = [&](const std::string& key, uint32_t& outColor) {
        if (!j.contains(key) || !j[key].is_string()) return;
        std::string hexStr = j[key].get<std::string>();
        if (hexStr.empty()) return;
        if (hexStr[0] == '#') hexStr = hexStr.substr(1);
        else if (hexStr.size() > 1 && hexStr[0] == '0' && (hexStr[1] == 'x' || hexStr[1] == 'X')) hexStr = hexStr.substr(2);
        try {
            const unsigned long parsed = std::stoul(hexStr, nullptr, 16);
            outColor = 0xFF000000u | (static_cast<uint32_t>(parsed) & 0x00FFFFFFu);
        } catch (...) {}
    };

    parseColor("boxColor", boxColor);
    parseColor("boxFilledColor", boxFilledColor);
    parseColor("tracerColor", tracerColor);
    parseColor("nametagColor", nametagColor);
}

void EspModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["showPlayers"] = showPlayers;
    j["showMobs"] = showMobs;
    j["showItems"] = showItems;
    j["showLocalPlayer"] = showLocalPlayer;

    j["throughWalls"] = throughWalls;
    j["range"] = range;

    j["box"] = box;

    std::string boxStyleValue = std::to_string(static_cast<int>(boxStyle));
    for (const char* const label : kBoxStyleNames) {
        boxStyleValue += ',';
        boxStyleValue += label;
    }
    j["boxStyle"] = boxStyleValue;

    j["boxFilled"] = boxFilled;
    j["boxThickness"] = boxThickness;

    char color[10];
    std::snprintf(color, sizeof(color), "#%06X", boxColor & 0x00FFFFFFu);
    j["boxColor"] = std::string(color);

    std::snprintf(color, sizeof(color), "#%06X", boxFilledColor & 0x00FFFFFFu);
    j["boxFilledColor"] = std::string(color);

    j["boxFilledOpacity"] = boxFilledOpacity;
    j["rgb"] = rgb;
    j["rgbSpeed"] = rgbSpeed;

    j["tracer"] = tracer;

    std::string tracerOriginValue = std::to_string(static_cast<int>(tracerOrigin));
    for (const char* const label : kTracerOriginNames) {
        tracerOriginValue += ',';
        tracerOriginValue += label;
    }
    j["tracerOrigin"] = tracerOriginValue;

    std::snprintf(color, sizeof(color), "#%06X", tracerColor & 0x00FFFFFFu);
    j["tracerColor"] = std::string(color);

    j["nametag"] = nametag;
    std::snprintf(color, sizeof(color), "#%06X", nametagColor & 0x00FFFFFFu);
    j["nametagColor"] = std::string(color);
    j["nametagScale"] = nametagScale;

    j["health"] = health;
    j["distance"] = distance;
    j["fov"] = fov;
}
