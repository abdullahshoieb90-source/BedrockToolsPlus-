#pragma once

// Pure classification + geometry helpers for the Entity ESP module.
//
// Kept free of game/Android dependencies so it can be unit-tested on the
// host (see tests/entityesp_test.cpp), the same way the block outline
// geometry and the wings shape code are tested.

#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/offsets/World.hpp>

#include <array>
#include <cmath>
#include <cstdint>

namespace bedrocktools::modules::entityesp {

// The render groups an actor can belong to. Each group maps to one menu
// toggle (enable) and one menu color picker.
enum class Group : int {
    None = 0,
    Player,     // players (remote and self)
    Mob,        // monsters, animals, water creatures, armor stands, ...
    Item,       // dropped items (ActorType::Item)
    Projectile, // ender pearls, wind charges, arrows, snowballs, fireballs, primed TNT, ...
    Vehicle,    // boats, minecarts
};

constexpr const char* groupName(Group group) {
    switch (group) {
        case Group::Player: return "Players";
        case Group::Mob: return "Mobs";
        case Group::Item: return "Items";
        case Group::Projectile: return "Projectiles";
        case Group::Vehicle: return "Vehicles";
        default: return "None";
    }
}

// Maps the actor category bitmask (Actor::mCategories) to an ESP group.
// Order matters: a player also carries IsMob, a boat/minecart also carries
// IsMob, and a primed TNT carries both IsItem and IsTNT (it is drawn as a
// projectile, since it behaves like one).
constexpr Group classify(std::uint32_t categories, bool isPlayer) {
    if (isPlayer) return Group::Player;
    if ((categories &
        (bedrocktools::sdk::offsets::ActorCategories::IsMinecart |
         bedrocktools::sdk::offsets::ActorCategories::IsVehicle)) != 0)
        return Group::Vehicle;
    if ((categories &
        (bedrocktools::sdk::offsets::ActorCategories::IsProjectile |
         bedrocktools::sdk::offsets::ActorCategories::IsFireball |
         bedrocktools::sdk::offsets::ActorCategories::IsTNT)) != 0)
        return Group::Projectile;
    if ((categories & bedrocktools::sdk::offsets::ActorCategories::IsItem) != 0)
        return Group::Item;
    return Group::Mob;
}

struct Line {
    bedrocktools::sdk::Vec3 from{};
    bedrocktools::sdk::Vec3 to{};

    constexpr bool operator==(const Line&) const = default;
};

constexpr Line makeEdge(bedrocktools::sdk::Vec3 from, bedrocktools::sdk::Vec3 to) {
    return Line{from, to};
}

// The 12 wireframe edges of the box (4 bottom, 4 top, 4 vertical).
constexpr std::array<Line, 12> boxEdges(const bedrocktools::sdk::AABB& box) {
    const auto& mn = box.min;
    const auto& mx = box.max;
    return {
        makeEdge({mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}),
        makeEdge({mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}),
        makeEdge({mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}),
        makeEdge({mn.x, mn.y, mx.z}, {mn.x, mn.y, mn.z}),
        makeEdge({mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}),
        makeEdge({mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}),
        makeEdge({mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}),
        makeEdge({mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}),
        makeEdge({mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}),
        makeEdge({mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}),
        makeEdge({mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}),
        makeEdge({mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}),
    };
}

// Returns the box slid to the interpolated render position
// (prev + (cur - prev) * partialTicks). The engine AABB is built around the
// *current* position, so the box is shifted by the difference between the
// interpolated and the current position.
//
// The shift is clamped to `maxShift` blocks so actors that teleport between
// ticks (respawns, dimension changes, ender-pearl landings) never stretch a
// box across the map: instead the box simply jumps with the actor.
inline bedrocktools::sdk::AABB interpolatedBox(
    const bedrocktools::sdk::AABB& box,
    const bedrocktools::sdk::Vec3& current,
    const bedrocktools::sdk::Vec3& previous,
    float partialTicks,
    float maxShift = 8.0f)
{
    float t = partialTicks;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    bedrocktools::sdk::Vec3 delta {
        (previous.x - current.x) * (1.0f - t),
        (previous.y - current.y) * (1.0f - t),
        (previous.z - current.z) * (1.0f - t),
    };
    const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (len > maxShift) {
        const float scale = maxShift / len;
        delta.x *= scale;
        delta.y *= scale;
        delta.z *= scale;
    }

    return {
        {box.min.x + delta.x, box.min.y + delta.y, box.min.z + delta.z},
        {box.max.x + delta.x, box.max.y + delta.y, box.max.z + delta.z},
    };
}

// Corners of the camera-facing quad that represents the thick segment
// p1 -> p2. Both points are in camera space (the camera is at the origin),
// which is exactly what the tessellator sees inside the renderLevel hook
// after the camera position is subtracted from world coordinates.
//
// The quad is perpendicular to both the segment and the eye ray, so the
// apparent width follows the line-thickness setting from any angle. Both
// ends are extended by half the width so box corners stay solid.
//
// Returns false for degenerate (zero-length) segments.
inline bool thickQuad(
    const bedrocktools::sdk::Vec3& p1,
    const bedrocktools::sdk::Vec3& p2,
    float halfWidth,
    bedrocktools::sdk::Vec3 out[4])
{
    float dx = p2.x - p1.x;
    float dy = p2.y - p1.y;
    float dz = p2.z - p1.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-5f) return false;
    dx /= len;
    dy /= len;
    dz /= len;

    // The camera sits at the origin of this space, so the vector to the
    // segment midpoint is the view direction.
    const float mx = (p1.x + p2.x) * 0.5f;
    const float my = (p1.y + p2.y) * 0.5f;
    const float mz = (p1.z + p2.z) * 0.5f;

    // side = dir x view, perpendicular to both the segment and the eye ray
    // => the quad always faces the player.
    float sx = dy * mz - dz * my;
    float sy = dz * mx - dx * mz;
    float sz = dx * my - dy * mx;
    float sLen = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sLen < 1e-5f) {
        // Looking straight down the segment: pick any perpendicular.
        if (std::fabs(dy) < 0.9f) { sx = -dz; sy = 0.0f; sz = dx; }
        else { sx = 1.0f; sy = 0.0f; sz = 0.0f; }
        sLen = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (sLen < 1e-5f) return false;
    }
    sx = sx / sLen * halfWidth;
    sy = sy / sLen * halfWidth;
    sz = sz / sLen * halfWidth;

    const float ex = dx * halfWidth;
    const float ey = dy * halfWidth;
    const float ez = dz * halfWidth;

    out[0] = {p1.x - ex - sx, p1.y - ey - sy, p1.z - ez - sz};
    out[1] = {p2.x + ex - sx, p2.y + ey - sy, p2.z + ez - sz};
    out[2] = {p2.x + ex + sx, p2.y + ey + sy, p2.z + ez + sz};
    out[3] = {p1.x - ex + sx, p1.y - ey + sy, p1.z - ez + sz};
    return true;
}

} // namespace bedrocktools::modules::entityesp
