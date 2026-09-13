#include "esp.hpp"

#include "../ModuleRegistry.hpp"
#include "esp_geometry.hpp"
#include "core/GameHooks.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Defined here at global scope to satisfy the `extern` declaration in
// esp.hpp (kept out of the anonymous namespace below so the name is not
// shadowed inside member functions).
EspModule* g_espMod = nullptr;

namespace {

using bedrocktools::memory::SignatureId;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;

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

// ---------------------------------------------------------------------------
// Module-wide state (file scope, like Hitbox / Crosshair).
// ---------------------------------------------------------------------------
Actor_isPlayer_t s_actorIsPlayer = nullptr;
Actor_isInvisible_t s_actorIsInvisible = nullptr;
Actor_fetchNearbyActorsSorted_t s_actorFetchNearby = nullptr;
BlockSource_isSolidBlockingBlock_t s_isSolidBlockingBlock = nullptr;

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

struct AABB {
    Vec3 min;
    Vec3 max;
};

bool hasCategory(void* actor, uint32_t categoryBit) {
    if (!actor) return false;
    return reinterpret_cast<bedrocktools::sdk::Actor*>(actor)->hasCategory(categoryBit);
}

AABB getActorAABB(void* actor) {
    AABB aabb = {{0, 0, 0}, {0, 0, 0}};
    auto* sdkActor = reinterpret_cast<bedrocktools::sdk::Actor*>(actor);
    const bedrocktools::sdk::AABB bounds = sdkActor->bounds();
    aabb.min = bounds.min;
    aabb.max = bounds.max;
    return aabb;
}

Vec2 getActorRotation(void* actor) {
    return reinterpret_cast<bedrocktools::sdk::Actor*>(actor)->rotation();
}

// ---------------------------------------------------------------------------
// Wall occlusion (Amanatides & Woo voxel traversal), mirrored from Hitbox so
// the ESP honors solid terrain the same way.
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

// ---------------------------------------------------------------------------
// Health reading. Mob::mHealthAttribute points at an AttributeInstance whose
// mCurrentValue / mMaxValue floats are read directly; non-living actors have
// a null pointer and return a negative current value.
// ---------------------------------------------------------------------------
float readHealth(void* actor, float& outMax) {
    outMax = 0.0f;
    if (!actor) return -1.0f;
    const uintptr_t base = reinterpret_cast<uintptr_t>(actor);
    void* attribute = *reinterpret_cast<void**>(base + bedrocktools::sdk::offsets::Mob::mHealthAttribute);
    // Only the Mob class owns a health attribute; callers gate this on the
    // living-entity categories, but keep the pointer range check anyway so a
    // stray non-null field never turns into an out-of-bounds read.
    if (!attribute || reinterpret_cast<uintptr_t>(attribute) < 0x1000) return -1.0f;

    const float current = *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(attribute) + bedrocktools::sdk::offsets::AttributeInstance::mCurrentValue);
    // Best-effort maximum: AttributeInstance stores mCurrentValue, mMinValue
    // and mMaxValue back-to-back, but only mCurrentValue is in the public
    // offsets table. Validate the candidate max and fall back to the current
    // value (full bar) when it reads back degenerate.
    const float maximum = *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(attribute) + bedrocktools::sdk::offsets::AttributeInstance::mCurrentValue + 8);
    if (std::isfinite(current) && std::isfinite(maximum) && maximum > 0.0f) {
        outMax = (maximum >= current) ? maximum : current;
    } else {
        outMax = std::isfinite(current) ? current : 0.0f;
    }
    return std::isfinite(current) ? current : -1.0f;
}

// ---------------------------------------------------------------------------
// Radio label tables (indices must match the enums in esp.hpp).
// ---------------------------------------------------------------------------
const char* const kBoxStyleNames[] = {"2D", "Corner"};
const char* const kTracerOriginNames[] = {"Bottom", "Crosshair"};

static_assert(sizeof(kBoxStyleNames) / sizeof(kBoxStyleNames[0]) ==
                  static_cast<int>(EspModule::BoxStyle::Count),
              "every BoxStyle needs exactly one kBoxStyleNames label");
static_assert(sizeof(kTracerOriginNames) / sizeof(kTracerOriginNames[0]) ==
                  static_cast<int>(EspModule::TracerOrigin::Count),
              "every TracerOrigin needs exactly one kTracerOriginNames label");

} // namespace

// ---------------------------------------------------------------------------
// Draw-command helpers (operate on the caller's command list).
// ---------------------------------------------------------------------------
namespace {

void addLine(std::vector<PLModMenu_DrawCommand>& cmds, float x1, float y1,
             float x2, float y2, float thickness, uint32_t color) {
    PLModMenu_DrawCommand cmd{};
    cmd.type = PL_DRAW_LINE;
    cmd.x = x1;
    cmd.y = y1;
    cmd.w = x2 - x1; // launcher treats w/h as the end-point delta
    cmd.h = y2 - y1;
    cmd.size = thickness;
    cmd.color = color;
    cmds.push_back(cmd);
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
    cmds.push_back(cmd);
}

void addText(std::vector<PLModMenu_DrawCommand>& cmds, const std::string& text,
             float x, float y, float size, uint32_t color) {
    PLModMenu_DrawCommand cmd{};
    cmd.type = PL_DRAW_TEXT;
    cmd.x = x;
    cmd.y = y;
    cmd.w = text.size() * size; // approximate width for the overlay hitbox
    cmd.h = size + 3.0f;
    cmd.size = size;
    cmd.color = color;
    cmd.text = text;
    cmds.push_back(cmd);
}

} // namespace

// ---------------------------------------------------------------------------
// Module lifecycle.
// ---------------------------------------------------------------------------
EspModule::EspModule()
    : Module("Esp", "Screen-space ESP overlays on nearby entities.") {
    showInMenu = true;
    hideInHudEditor = true; // world overlay, not a draggable HUD element
    g_espMod = this;
}

EspModule::~EspModule() {
    if (g_espMod == this) g_espMod = nullptr;
}

void EspModule::onInit() {
    const auto resolveFn = [](SignatureId id) { return bedrocktools::memory::resolve(id); };

    const uintptr_t aip = resolveFn(SignatureId::ActorIsPlayer);
    if (aip) s_actorIsPlayer = reinterpret_cast<Actor_isPlayer_t>(aip);

    const uintptr_t aii = resolveFn(SignatureId::ActorIsInvisible);
    if (aii) s_actorIsInvisible = reinterpret_cast<Actor_isInvisible_t>(aii);

    const uintptr_t afn = resolveFn(SignatureId::ActorFetchNearbyActorsSorted);
    if (afn) s_actorFetchNearby = reinterpret_cast<Actor_fetchNearbyActorsSorted_t>(afn);

    const uintptr_t isb = resolveFn(SignatureId::BlockSourceIsSolidBlockingBlock);
    if (isb) s_isSolidBlockingBlock = reinterpret_cast<BlockSource_isSolidBlockingBlock_t>(isb);

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

void EspModule::onEnable() {}

void EspModule::onDisable() {
    // Clear the overlay immediately; onFrame stops running once disabled.
    submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
}

void EspModule::onFrame() {
    if (!enabled) {
        submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    void* localPlayer = g_localPlayer.load(std::memory_order_acquire);
    if (!localPlayer || !s_actorFetchNearby) {
        submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    // Camera position comes from the renderer the game already populated for
    // this frame (LevelRendererPlayer::mCamPos); orientation comes from the
    // local player's rotation component.
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) {
        submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }
    auto* sdkClient = reinterpret_cast<bedrocktools::sdk::ClientInstance*>(client);
    auto* levelRenderer = sdkClient->levelRenderer();
    auto* playerRenderer = levelRenderer ? levelRenderer->playerRenderer() : nullptr;
    if (!playerRenderer) {
        submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    const Vec3 camPos = playerRenderer->cameraPosition();
    const Vec2 camRot = getActorRotation(localPlayer);

    const esp::Camera camera = esp::computeCamera(camPos, camRot);
    const pl::modmenu::HudSurfaceSize surface = pl::modmenu::getHudSurfaceSize();
    const esp::SurfaceProjection proj = esp::makeProjection(surface.width, surface.height, fov);

    // Resolve the dimension's BlockSource once per frame for the occlusion
    // cull (only needed when Through Walls is off).
    void* region = nullptr;
    if (!throughWalls && s_isSolidBlockingBlock) {
        const uintptr_t dimension = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uintptr_t>(localPlayer) + bedrocktools::sdk::offsets::Actor::mDimension);
        if (dimension >= 0x1000) {
            const uintptr_t blockSource = *reinterpret_cast<uintptr_t*>(
                dimension + bedrocktools::sdk::offsets::Dimension::mBlockSource);
            if (blockSource >= 0x1000) region = reinterpret_cast<void*>(blockSource);
        }
    }

    // Fetch nearby actors (radius = configured range).
    const float radius = std::clamp(range, 8.0f, 256.0f);
    Vec3 extent = {radius, radius, radius};
    const ActorVec actors = s_actorFetchNearby(localPlayer, &extent, 1);

    // Resolve the box color once per frame (RGB animation wins over boxColor).
    uint32_t boxRgb;
    if (rgb) {
        const float speed = std::clamp(rgbSpeed, 0.05f, 1.0f);
        float hue = std::fmod(static_cast<float>(nowSeconds()) * speed * 360.0f, 360.0f);
        if (hue < 0.0f) hue += 360.0f;
        boxRgb = hsvToRgb(hue) | 0xFF000000u;
    } else {
        boxRgb = forceOpaqueColor(boxColor);
    }

    const float thickness = std::clamp(boxThickness, 0.5f, 20.0f);
    const float nametagSize = std::clamp(nametagScale, 0.5f, 2.0f) * 14.0f;
    const float subSize = nametagSize * 0.8f;
    // Off-screen clamp for projections: an entity straddling the camera plane
    // projects far outside the surface, and keeping the coordinates to a few
    // screen sizes keeps the launcher's draw commands finite.
    const float boxLimit = std::max(proj.width, proj.height) * 4.0f;

    // First-person camera check for the local-player ESP. Until the game has
    // reported a perspective value, stay conservative and hide the self ESP
    // (the camera is almost always first person right after launch).
    const bool thirdPerson = s_perspectiveKnown.load(std::memory_order_relaxed) &&
                             s_perspective.load(std::memory_order_relaxed) != 0;

    std::vector<PLModMenu_DrawCommand> cmds;

    auto renderActor = [&](void* ent, bool isSelf) {
        if (!ent) return;

        const AABB aabb = getActorAABB(ent);
        if (aabb.min.x == 0.0f && aabb.min.y == 0.0f && aabb.min.z == 0.0f &&
            aabb.max.x == 0.0f && aabb.max.y == 0.0f && aabb.max.z == 0.0f) {
            return;
        }

        // Occlusion cull (skipped for the local player, whose camera often
        // sits inside geometry in third person).
        if (!isSelf && !throughWalls && region && isOccluded(region, camPos, aabb)) {
            return;
        }

        // Project the AABB to its tight 2D box. The corners that fall behind
        // the near plane are clipped away by projectBox, so an entity pressed
        // right up against the camera still spans the whole screen instead of
        // collapsing to the few corners that survived.
        const esp::ScreenBox screen =
            esp::projectBox(camera, proj, aabb.min, aabb.max, boxLimit);
        if (!screen.visible) return;

        // Nothing to draw once the box has slid completely off the surface.
        if (screen.maxX < 0.0f || screen.minX > proj.width ||
            screen.maxY < 0.0f || screen.minY > proj.height) {
            return;
        }

        const float boxLeft = screen.minX;
        const float boxTop = screen.minY;
        const float boxRight = screen.maxX;
        const float boxBottom = screen.maxY;
        const float boxW = boxRight - boxLeft;
        const float boxH = boxBottom - boxTop;
        const float centerX = (boxLeft + boxRight) * 0.5f;

        // Distance from the camera to the entity center (blocks).
        const float dist = std::sqrt(
            (camPos.x - (aabb.min.x + aabb.max.x) * 0.5f) * (camPos.x - (aabb.min.x + aabb.max.x) * 0.5f) +
            (camPos.y - (aabb.min.y + aabb.max.y) * 0.5f) * (camPos.y - (aabb.min.y + aabb.max.y) * 0.5f) +
            (camPos.z - (aabb.min.z + aabb.max.z) * 0.5f) * (camPos.z - (aabb.min.z + aabb.max.z) * 0.5f));

        // ---- Filled box (behind everything else) ---------------------------
        if (box && boxFilled && boxW > 0.0f && boxH > 0.0f) {
            const uint8_t alpha = static_cast<uint8_t>(
                std::clamp(boxFilledOpacity, 0.0f, 1.0f) * 255.0f);
            addRect(cmds, boxLeft, boxTop, boxW, boxH,
                    (static_cast<uint32_t>(alpha) << 24) | (boxFilledColor & 0x00FFFFFFu));
        }

        // ---- Box outline ---------------------------------------------------
        if (box) {
            if (boxStyle == BoxStyle::Corner) {
                const float len = std::min(boxW, boxH) * 0.33f;
                // Four corner brackets.
                addLine(cmds, boxLeft, boxTop, boxLeft + len, boxTop, thickness, boxRgb);
                addLine(cmds, boxLeft, boxTop, boxLeft, boxTop + len, thickness, boxRgb);
                addLine(cmds, boxRight - len, boxTop, boxRight, boxTop, thickness, boxRgb);
                addLine(cmds, boxRight, boxTop, boxRight, boxTop + len, thickness, boxRgb);
                addLine(cmds, boxLeft, boxBottom - len, boxLeft, boxBottom, thickness, boxRgb);
                addLine(cmds, boxLeft, boxBottom, boxLeft + len, boxBottom, thickness, boxRgb);
                addLine(cmds, boxRight, boxBottom - len, boxRight, boxBottom, thickness, boxRgb);
                addLine(cmds, boxRight - len, boxBottom, boxRight, boxBottom, thickness, boxRgb);
            } else {
                // Full 2D rectangle.
                addLine(cmds, boxLeft, boxTop, boxRight, boxTop, thickness, boxRgb);
                addLine(cmds, boxRight, boxTop, boxRight, boxBottom, thickness, boxRgb);
                addLine(cmds, boxRight, boxBottom, boxLeft, boxBottom, thickness, boxRgb);
                addLine(cmds, boxLeft, boxBottom, boxLeft, boxTop, thickness, boxRgb);
            }
        }

        // ---- Tracer --------------------------------------------------------
        if (tracer) {
            const float originX = proj.width * 0.5f;
            const float originY = (tracerOrigin == TracerOrigin::Crosshair)
                                      ? proj.height * 0.5f
                                      : proj.height;
            addLine(cmds, originX, originY, centerX, boxBottom, thickness * 0.75f,
                    forceOpaqueColor(tracerColor));
        }

        // ---- Text stack above the box -------------------------------------
        float textY = boxTop - nametagSize - 4.0f;

        // Nametag (players only; the name field is only valid for Player).
        std::string name;
        if (nametag && s_actorIsPlayer && s_actorIsPlayer(ent)) {
            name = reinterpret_cast<bedrocktools::sdk::Player*>(ent)->name();
        }
        if (nametag && !name.empty()) {
            const float textW = name.size() * nametagSize * 0.6f;
            addText(cmds, name, centerX - textW * 0.5f, textY, nametagSize,
                    forceOpaqueColor(nametagColor));
            textY -= nametagSize + 2.0f;
        }

        // Health (living entities only: players + mobs own a health attribute).
        const bool living = (s_actorIsPlayer && s_actorIsPlayer(ent)) ||
                            hasCategory(ent, bedrocktools::sdk::offsets::ActorCategories::IsMob);
        if (health && living) {
            float maxHealth = 0.0f;
            const float current = readHealth(ent, maxHealth);
            if (current >= 0.0f && std::isfinite(current)) {
                const float fraction = std::clamp(current / (maxHealth > 0.0f ? maxHealth : current),
                                                  0.0f, 1.0f);

                const float barW = std::max(boxW, 20.0f);
                const float barH = 4.0f;
                const float barY = textY + subSize - barH; // place bar under the label

                // Track + fill.
                addRect(cmds, centerX - barW * 0.5f, barY, barW, barH, 0x80000000u);
                const uint32_t fillColor = fraction > 0.5f   ? 0xFF22C55Eu
                                           : fraction > 0.25f ? 0xFFEAB308u
                                                              : 0xFFEF4444u;
                addRect(cmds, centerX - barW * 0.5f, barY, barW * fraction, barH, fillColor);

                char buffer[24];
                std::snprintf(buffer, sizeof(buffer), "%.0f", current);
                addText(cmds, buffer, centerX - barW * 0.5f, barY - subSize - 2.0f,
                        subSize, forceOpaqueColor(nametagColor));
                textY = barY - subSize - 4.0f;
            }
        }

        // Distance.
        if (distance) {
            char buffer[24];
            std::snprintf(buffer, sizeof(buffer), "%.1fm", dist);
            addText(cmds, buffer, centerX - 16.0f, textY, subSize,
                    forceOpaqueColor(nametagColor));
        }
    };

    // Local player (third person only).
    if (showLocalPlayer && thirdPerson) {
        renderActor(localPlayer, true);
    }

    // Nearby actors.
    if (actors.begin && actors.end) {
        for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
            void* ent = it->mActor;
            if (!ent || ent == localPlayer) continue;

            bool isPlayer = false;
            if (s_actorIsPlayer) isPlayer = s_actorIsPlayer(ent);

            if (isPlayer) {
                if (!showPlayers) continue;
            } else if (hasCategory(ent, bedrocktools::sdk::offsets::ActorCategories::IsMob)) {
                if (!showMobs) continue;
            } else {
                if (!showItems) continue;
            }

            if (s_actorIsInvisible && s_actorIsInvisible(ent)) continue;

            renderActor(ent, false);
        }
    }

    submitDrawCommands(moduleId, cmds);
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

    throughWalls = j.value("throughWalls", throughWalls);
    range = j.value("range", range);

    box = j.value("box", box);
    boxFilled = j.value("boxFilled", boxFilled);

    if (j.contains("boxStyle")) {
        const auto& value = j["boxStyle"];
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            const auto comma = text.find(',');
            try {
                const int style = std::stoi(text.substr(0, comma));
                if (style >= 0 && style < static_cast<int>(BoxStyle::Count)) {
                    boxStyle = static_cast<BoxStyle>(style);
                }
            } catch (...) {}
        } else if (value.is_number_integer()) {
            const int style = value.get<int>();
            if (style >= 0 && style < static_cast<int>(BoxStyle::Count)) {
                boxStyle = static_cast<BoxStyle>(style);
            }
        }
    }

    if (j.contains("tracerOrigin")) {
        const auto& value = j["tracerOrigin"];
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            const auto comma = text.find(',');
            try {
                const int origin = std::stoi(text.substr(0, comma));
                if (origin >= 0 && origin < static_cast<int>(TracerOrigin::Count)) {
                    tracerOrigin = static_cast<TracerOrigin>(origin);
                }
            } catch (...) {}
        } else if (value.is_number_integer()) {
            const int origin = value.get<int>();
            if (origin >= 0 && origin < static_cast<int>(TracerOrigin::Count)) {
                tracerOrigin = static_cast<TracerOrigin>(origin);
            }
        }
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
    for (const char* const name : kBoxStyleNames) {
        boxStyleValue += ',';
        boxStyleValue += name;
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
    for (const char* const name : kTracerOriginNames) {
        tracerOriginValue += ',';
        tracerOriginValue += name;
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
