#pragma once

// First-person vs third-person camera heuristics for the Hitbox overlay.
// Pure functions so host tests can cover the jump/sneak/wall cases without
// bringing in the tessellator hook.

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

} // namespace hitbox
