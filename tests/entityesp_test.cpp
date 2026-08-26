// Host-side regression test for the Entity ESP (Hitbox / ESP 3D) module.
//
// Verifies the pure logic without Minecraft:
//   * group classification (players, mobs, items, projectiles, vehicles)
//   * partial-tick AABB interpolation (including teleport clamping)
//   * the 12-edge wireframe touches every cube corner exactly 3 times
//   * the thick-line quad faces the camera and overshoots segment ends
//   * config save/load round-trip keeps category toggles, colors and sliders
//   * config values are clamped to their documented ranges
//
// The real module source is compiled as a second translation unit, so the
// test also proves the module itself builds (same pattern as
// wings_patch_test / customcapes_patch_test).
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I tests/fakejson -I tests/fakepl
//         tests/entityesp_test.cpp src/modules/visual/entityesp.cpp
//         -o /tmp/entityesp_test
//     /tmp/entityesp_test

#include "modules/visual/entityesp.hpp"
#include "modules/visual/entityesp_geometry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

namespace esp = bedrocktools::modules::entityesp;
using namespace bedrocktools::sdk::offsets::ActorCategories;

// ---------------------------------------------------------------------------
// Stubs for host test (the real implementations live in src/core and need
// the Android game binary).
// ---------------------------------------------------------------------------

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
bool resolveAll(std::string_view) { return false; }
void clear() {}
} // namespace bedrocktools::memory

static bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

static float vecLen(const bedrocktools::sdk::Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

int main() {
    // ---- Group classification ------------------------------------------
    // A player also carries IsMob - the explicit player check must win.
    assert(esp::classify(IsPlayer | IsMob, true) == esp::Group::Player);
    // Monsters / animals / water creatures / armor stands are mobs.
    assert(esp::classify(IsMob | IsMonster, false) == esp::Group::Mob);
    assert(esp::classify(IsMob | IsCreature, false) == esp::Group::Mob);
    assert(esp::classify(IsMob | IsWaterMob, false) == esp::Group::Mob);
    // Dropped items (ActorType::Item).
    assert(esp::classify(IsItem, false) == esp::Group::Item);
    // Projectiles: ender pearls, wind charges, arrows, snowballs, ...
    assert(esp::classify(IsProjectile, false) == esp::Group::Projectile);
    assert(esp::classify(IsProjectile | IsMob, false) == esp::Group::Projectile);
    assert(esp::classify(IsFireball, false) == esp::Group::Projectile);
    // Primed TNT carries IsItem + IsTNT and behaves like a projectile.
    assert(esp::classify(IsItem | IsTNT, false) == esp::Group::Projectile);
    // Vehicles: boats and minecarts (IsMinecart and/or IsVehicle bits).
    assert(esp::classify(IsMinecart, false) == esp::Group::Vehicle);
    assert(esp::classify(IsVehicle, false) == esp::Group::Vehicle);
    assert(esp::classify(IsMinecart | IsMob, false) == esp::Group::Vehicle);

    assert(std::string(esp::groupName(esp::Group::Player)) == "Players");
    assert(std::string(esp::groupName(esp::Group::Vehicle)) == "Vehicles");

    // ---- Partial-tick AABB interpolation --------------------------------
    bedrocktools::sdk::AABB box{{0, 0, 0}, {0.6f, 1.8f, 0.6f}};
    const bedrocktools::sdk::Vec3 cur{10, 64, 10};
    const bedrocktools::sdk::Vec3 prev{9, 64, 10};

    // partialTicks = 0 -> box fully on the previous position (shift -1).
    const auto bStart = esp::interpolatedBox(box, cur, prev, 0.0f);
    assert(near(bStart.min.x, -1.0f));
    // partialTicks = 1 -> no shift (the engine AABB is current-position based).
    const auto bEnd = esp::interpolatedBox(box, cur, prev, 1.0f);
    assert(near(bEnd.min.x, 0.0f));
    assert(near(bEnd.max.y, 1.8f));
    // Halfway between ticks.
    const auto bMid = esp::interpolatedBox(box, cur, prev, 0.5f);
    assert(near(bMid.min.x, -0.5f));

    // Out-of-range partial ticks are clamped.
    assert(near(esp::interpolatedBox(box, cur, prev, -3.0f).min.x, -1.0f));
    assert(near(esp::interpolatedBox(box, cur, prev, 2.0f).min.x, 0.0f));

    // Teleport (respawn, dimension change, pearl landing): the shift is
    // clamped to maxShift so the box never stretches across the map.
    const bedrocktools::sdk::Vec3 farPrev{1000, 64, 1000};
    const auto bTele = esp::interpolatedBox(box, cur, farPrev, 0.5f);
    const bedrocktools::sdk::Vec3 teleShift{
        bTele.min.x - box.min.x, bTele.min.y - box.min.y, bTele.min.z - box.min.z};
    assert(vecLen(teleShift) <= 8.0f + 1e-4f);
    // The box keeps its size no matter how it is shifted.
    assert(near(bTele.max.x - bTele.min.x, 0.6f));
    assert(near(bTele.max.y - bTele.min.y, 1.8f));
    assert(near(bTele.max.z - bTele.min.z, 0.6f));

    // ---- Wireframe edges -------------------------------------------------
    const auto edges = esp::boxEdges(bedrocktools::sdk::AABB{{0, 0, 0}, {1, 1, 1}});
    assert(edges.size() == 12);
    const bedrocktools::sdk::Vec3 corners[8] = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    for (const auto& corner : corners) {
        int touches = 0;
        for (const auto& edge : edges) {
            if (edge.from == corner) ++touches;
            if (edge.to == corner) ++touches;
        }
        assert(touches == 3);
    }

    // ---- Thick-line quad -------------------------------------------------
    bedrocktools::sdk::Vec3 quad[4]{};
    // Segment in front of the camera (camera space: the camera is at the
    // origin). The side vector must point straight up/down, so the quad is
    // offset by exactly the half width in Y.
    assert(esp::thickQuad({-1, 0, 2}, {1, 0, 2}, 0.1f, quad));
    assert(near(std::fabs(quad[0].y), 0.1f));
    assert(near(std::fabs(quad[1].y), 0.1f));
    assert(near(std::fabs(quad[2].y), 0.1f));
    assert(near(std::fabs(quad[3].y), 0.1f));
    // Both ends overshoot the segment by the half width (solid corners).
    assert(quad[0].x <= -1.0f - 0.09f);
    assert(quad[1].x >= 1.0f + 0.09f);
    // Degenerate segment is rejected.
    assert(!esp::thickQuad({0, 0, 0}, {1e-9f, 0, 0}, 0.1f, quad));

    // ---- Config round-trip ------------------------------------------------
    EntityESPModule mod;
    nlohmann::json saved;
    mod.saveConfig(saved);
    assert(saved.contains("showPlayers"));
    assert(saved.contains("showMobsColor"));
    assert(saved.contains("showProjectilesColor"));
    assert(saved.contains("showVehiclesColor"));
    assert(saved.contains("lineThickness"));
    assert(saved.contains("boxAlpha"));

    nlohmann::json in;
    in["showPlayers"] = false;
    in["showMobs"] = true;
    in["showItems"] = false;
    in["showProjectiles"] = true;
    in["showVehicles"] = false;
    in["showPlayersColor"] = std::string("#123456");
    in["showMobsColor"] = std::string("#FF8040");
    in["showItemsColor"] = std::string("#00FF00");
    in["showProjectilesColor"] = std::string("#FF00FF");
    in["showVehiclesColor"] = std::string("#0000FF");
    in["lineThickness"] = 4.0f;
    in["boxAlpha"] = 0.5f;
    in["throughWalls"] = false;
    in["interpolate"] = false;
    in["showSelf"] = true;
    in["fetchRange"] = 100.0f;
    mod.loadConfig(in);

    assert(!mod.showPlayers && mod.showMobs && !mod.showItems);
    assert(mod.showProjectiles && !mod.showVehicles);
    assert(!mod.throughWalls && !mod.interpolate && mod.showSelf);
    assert(near(mod.lineThickness, 4.0f));
    assert(near(mod.boxAlpha, 0.5f));
    assert(near(mod.fetchRange, 100.0f));
    // #RRGGBB values are stored AARRGGBB with an opaque alpha byte.
    assert(mod.showPlayersColor == 0xFF123456u);
    assert(mod.showMobsColor == 0xFFFF8040u);
    assert(mod.showItemsColor == 0xFF00FF00u);
    assert(mod.showProjectilesColor == 0xFFFF00FFu);
    assert(mod.showVehiclesColor == 0xFF0000FFu);

    // ---- Config clamping ---------------------------------------------------
    EntityESPModule mod2;
    nlohmann::json bad;
    bad["lineThickness"] = 50.0f;
    bad["boxAlpha"] = 5.0f;
    bad["fetchRange"] = 1.0f;
    mod2.loadConfig(bad);
    assert(near(mod2.lineThickness, 10.0f));
    assert(near(mod2.boxAlpha, 1.0f));
    assert(near(mod2.fetchRange, 8.0f));

    // A save after a load keeps producing valid picker strings.
    nlohmann::json saved2;
    mod.saveConfig(saved2);
    assert(saved2["showMobsColor"].is_string());
    assert(saved2["showMobsColor"].get<std::string>() == std::string("#FF8040"));

    std::puts("entityesp_test: all checks passed");
    return 0;
}
