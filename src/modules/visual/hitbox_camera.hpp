#pragma once

// First-person vs third-person camera heuristics for the Hitbox overlay.
// Pure functions so host tests can cover the jump/sneak/wall cases without
// bringing in the tessellator hook.

#include <cmath>

namespace hitbox {

// XZ stays tight: in first person the camera is always inside the player's
// footprint. Y is much looser because jumping interpolates the camera by up
// to one tick of jump velocity (~0.42 blocks) above the un-interpolated
// collision AABB, and view-bob adds a bit more. A third-person camera is
// pulled several blocks away, so it still fails this test.
inline constexpr float kFirstPersonXZMargin = 0.08f;
inline constexpr float kFirstPersonYMargin = 1.25f;

inline bool cameraInsideLocalBox(float camX, float camY, float camZ,
                                 float minX, float minY, float minZ,
                                 float maxX, float maxY, float maxZ) {
    return camX >= minX - kFirstPersonXZMargin && camX <= maxX + kFirstPersonXZMargin &&
           camY >= minY - kFirstPersonYMargin && camY <= maxY + kFirstPersonYMargin &&
           camZ >= minZ - kFirstPersonXZMargin && camZ <= maxZ + kFirstPersonYMargin;
}

inline bool isThirdPersonCamera(float camX, float camY, float camZ,
                                float minX, float minY, float minZ,
                                float maxX, float maxY, float maxZ) {
    return !cameraInsideLocalBox(camX, camY, camZ, minX, minY, minZ, maxX, maxY, maxZ);
}

// Draw the local player's own box only in a real third-person view.
//
// - show3rdPerson: menu toggle.
// - gamePerspectiveIsThirdPerson: Options::getPlayerViewPerspective() != 0.
//   Pass true when the game value is unknown so the geometric test decides.
// - cameraLooksThirdPerson: camera has actually pulled away from the box.
//   Stops a wall-clipped third-person camera (which sits inside the AABB)
//   from filling the screen with the player's own box.
inline bool shouldDrawLocalHitbox(bool show3rdPerson,
                                  bool gamePerspectiveIsThirdPerson,
                                  bool cameraLooksThirdPerson) {
    return show3rdPerson && gamePerspectiveIsThirdPerson && cameraLooksThirdPerson;
}

// --- Hitbox indicator reach ------------------------------------------------

// The indicator's interaction distance in blocks, measured from the PLAYER's
// own box (the game's own reach feel), not from the camera. In third person
// the camera trails the player by several blocks, so a camera-based reach
// stretched the highlight far past the player as soon as the camera moved.
inline constexpr float kIndicatorReach = 3.0f;
// Small tolerance so a mob hugging the player is not lost to float error.
inline constexpr float kIndicatorReachEpsilon = 0.05f;

// Distance from a point to an axis-aligned box (0 when the point is inside).
inline float pointAabbDistance(float px, float py, float pz,
                               float minX, float minY, float minZ,
                               float maxX, float maxY, float maxZ) {
    const float dx = std::fmax(minX - px, std::fmax(px - maxX, 0.0f));
    const float dy = std::fmax(minY - py, std::fmax(py - maxY, 0.0f));
    const float dz = std::fmax(minZ - pz, std::fmax(pz - maxZ, 0.0f));
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// True when a point on the crosshair ray (usually an entity-box hit) is
// within the indicator's reach of the player's own box.
inline bool hitWithinPlayerReach(float hitX, float hitY, float hitZ,
                                 float minX, float minY, float minZ,
                                 float maxX, float maxY, float maxZ,
                                 float reach,
                                 float epsilon = kIndicatorReachEpsilon) {
    return pointAabbDistance(hitX, hitY, hitZ,
                             minX, minY, minZ, maxX, maxY, maxZ) <= reach + epsilon;
}

} // namespace hitbox
