// Regression test for the Esp overlay projection math.
//
// The camera basis and the perspective projection live in
// modules/visual/esp_geometry.hpp as pure functions, so the tricky math
// (yaw/pitch orientation, handedness of the camera basis, behind-camera
// rejection, aspect-corrected NDC, near-plane clipping) can be verified on
// the host without the game.
//
//     g++ -std=c++20 -I src -I include tests/esp_geometry_test.cpp -o /tmp/esp_geometry_test
//     /tmp/esp_geometry_test

#include "modules/visual/esp_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

bool near(float a, float b, float eps = 0.001f) {
    return std::fabs(a - b) <= eps;
}

using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;

// Bedrock world axes: +X east, +Y up, +Z south.
const Vec3 kEast{1.0f, 0.0f, 0.0f};
const Vec3 kNorth{0.0f, 0.0f, -1.0f};
const Vec3 kSouth{0.0f, 0.0f, 1.0f};
const Vec3 kWest{-1.0f, 0.0f, 0.0f};
const Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
} // namespace

int main() {
    std::printf("esp projection geometry\n");

    // --- Camera basis -------------------------------------------------------
    // Bedrock's world is right-handed (+X east, +Y up, +Z south), so the
    // camera's right-hand direction is cross(forward, worldUp): facing south
    // puts west on the right and east on the left.
    // yaw = 0, pitch = 0 -> facing +Z (south), right hand towards -X (west).
    {
        const esp::Camera cam = esp::computeCamera({0, 0, 0}, {0.0f, 0.0f});
        check(near(cam.forward.x, 0.0f) && near(cam.forward.y, 0.0f) && near(cam.forward.z, 1.0f),
              "yaw 0 / pitch 0 faces +Z (south)");
        check(near(cam.right.x, -1.0f) && near(cam.right.y, 0.0f) && near(cam.right.z, 0.0f),
              "yaw 0 right-hand vector is -X (west)");
        check(near(cam.up.x, 0.0f) && near(cam.up.y, 1.0f) && near(cam.up.z, 0.0f),
              "yaw 0 up vector is +Y");
    }

    // The other three cardinal directions: right hand = forward turned 90
    // degrees clockwise seen from above.
    {
        const esp::Camera west = esp::computeCamera({0, 0, 0}, {0.0f, 90.0f});
        check(near(west.forward.x, -1.0f) && near(west.forward.z, 0.0f),
              "yaw 90 faces -X (west)");
        check(near(west.right.z, -1.0f) && near(west.right.x, 0.0f),
              "yaw 90 right-hand vector is -Z (north)");

        const esp::Camera north = esp::computeCamera({0, 0, 0}, {0.0f, 180.0f});
        check(near(north.forward.z, -1.0f) && near(north.right.x, 1.0f),
              "yaw 180 faces -Z (north) with east on the right");

        const esp::Camera east = esp::computeCamera({0, 0, 0}, {0.0f, 270.0f});
        check(near(east.forward.x, 1.0f) && near(east.right.z, 1.0f),
              "yaw 270 faces +X (east) with south on the right");
    }

    // pitch = -90 -> facing straight up. The right vector stays horizontal
    // (the game has no camera roll) and screen-up becomes north.
    {
        const esp::Camera cam = esp::computeCamera({0, 0, 0}, {-90.0f, 0.0f});
        check(near(cam.forward.y, 1.0f), "pitch -90 faces +Y (straight up)");
        check(near(cam.right.x, -1.0f) && near(cam.right.y, 0.0f) && near(cam.right.z, 0.0f),
              "pitch -90 keeps the right vector horizontal");
        check(near(cam.up.z, -1.0f) && near(cam.up.y, 0.0f),
              "pitch -90 puts north at the top of the screen");
    }

    // Orthonormal, and (right, up, forward) is a left-handed triple because
    // the camera looks *into* the screen: cross(right, up) == -forward.
    {
        bool allValid = true;
        for (float yaw = 0.0f; yaw < 360.0f && allValid; yaw += 15.0f) {
            for (float pitch = -85.0f; pitch <= 85.0f && allValid; pitch += 17.0f) {
                const esp::Camera cam = esp::computeCamera({0, 0, 0}, {pitch, yaw});
                const Vec3 ru = esp::cross(cam.right, cam.up);
                const bool orthonormal =
                    near(esp::dot(cam.right, cam.right), 1.0f, 1e-4f) &&
                    near(esp::dot(cam.up, cam.up), 1.0f, 1e-4f) &&
                    near(esp::dot(cam.forward, cam.forward), 1.0f, 1e-4f) &&
                    near(esp::dot(cam.right, cam.up), 0.0f, 1e-4f) &&
                    near(esp::dot(cam.right, cam.forward), 0.0f, 1e-4f) &&
                    near(esp::dot(cam.up, cam.forward), 0.0f, 1e-4f);
                const bool handed = near(esp::dot(ru, cam.forward), -1.0f, 1e-4f);
                // The right vector never tilts: it is the horizontal
                // cross(forward, worldUp), perpendicular to the look line.
                const bool horizontal = near(cam.right.y, 0.0f, 1e-4f) &&
                                        near(esp::dot(cam.right, kWorldUp), 0.0f, 1e-4f);
                if (!orthonormal || !handed || !horizontal) {
                    std::printf("       (basis breaks at yaw %.0f pitch %.0f)\n", yaw, pitch);
                    allValid = false;
                }
            }
        }
        check(allValid, "basis stays orthonormal and right-handed for every yaw/pitch");
    }

    // --- Which side of the screen does an entity land on? --------------------
    // The bug this guards: with right = cross(worldUp, forward) the overlay is
    // mirrored left-to-right, so anything off-axis is drawn on the wrong side
    // and its box runs away from the entity as soon as the view turns.
    {
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);

        const esp::Camera south = esp::computeCamera({0, 0, 0}, {0.0f, 0.0f});
        float sx = 0.0f, sy = 0.0f;
        // Facing south, east (+X) is on the LEFT.
        check(esp::project(south, proj, {10.0f, 0.0f, 10.0f}, sx, sy) && sx < 500.0f,
              "facing south, a target to the east draws left of center");
        check(esp::project(south, proj, {-10.0f, 0.0f, 10.0f}, sx, sy) && sx > 500.0f,
              "facing south, a target to the west draws right of center");

        const esp::Camera east = esp::computeCamera({0, 0, 0}, {0.0f, 270.0f});
        check(esp::project(east, proj, {10.0f, 0.0f, 10.0f}, sx, sy) && sx > 500.0f,
              "facing east, a target to the south draws right of center");
        check(esp::project(east, proj, {10.0f, 0.0f, -10.0f}, sx, sy) && sx < 500.0f,
              "facing east, a target to the north draws left of center");
    }

    // --- Turning the view ---------------------------------------------------
    // Increasing yaw turns right (south -> west), so a world-fixed target must
    // slide LEFT across the screen. A mirrored basis sweeps it the other way,
    // which is exactly "the box moves when I turn the camera and stops sitting
    // on the entity".
    {
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);
        const Vec3 target{2.0f, 0.0f, 10.0f}; // slightly east and ahead

        float previous = 1e30f;
        bool monotonic = true;
        bool startedLeft = false;
        for (float yaw = 0.0f; yaw <= 40.0f; yaw += 10.0f) {
            const esp::Camera cam = esp::computeCamera({0, 0, 0}, {0.0f, yaw});
            float sx = 0.0f, sy = 0.0f;
            if (!esp::project(cam, proj, target, sx, sy)) {
                monotonic = false;
                break;
            }
            if (yaw == 0.0f) startedLeft = sx < 500.0f;
            if (sx >= previous) monotonic = false;
            previous = sx;
        }
        check(startedLeft, "the east-side target starts left of center");
        check(monotonic, "turning right sweeps the target left across the screen");
    }

    // --- Pitch --------------------------------------------------------------
    // Looking up by the target's elevation puts that target dead center.
    {
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);
        const Vec3 target{0.0f, 10.0f, 10.0f}; // 45 degrees above the horizon

        const esp::Camera level = esp::computeCamera({0, 0, 0}, {0.0f, 0.0f});
        float sx = 0.0f, sy = 0.0f;
        check(esp::project(level, proj, target, sx, sy) && sy < 500.0f,
              "a target above the horizon draws above center");

        const esp::Camera lookingUp = esp::computeCamera({0, 0, 0}, {-45.0f, 0.0f});
        check(esp::project(lookingUp, proj, target, sx, sy) && near(sy, 500.0f, 0.5f),
              "looking up 45 degrees centers that same target");
    }

    // --- Projection ---------------------------------------------------------
    // Square surface, 90 degree vertical FOV (tan(half) == 1).
    {
        const esp::Camera cam = esp::computeCamera({0, 0, 0}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);

        float sx = 0.0f, sy = 0.0f;

        // A point straight ahead at distance d projects to the center.
        check(esp::project(cam, proj, {0.0f, 0.0f, 10.0f}, sx, sy) &&
                  near(sx, 500.0f) && near(sy, 500.0f),
              "point dead ahead projects to the center");

        // A point to the right of the camera (west here) lands on the right
        // edge for a square surface and 90 FOV: ndcX = (vx/vz)/(tan*aspect) = 1.
        check(esp::project(cam, proj, {-10.0f, 0.0f, 10.0f}, sx, sy) &&
                  near(sx, 1000.0f, 0.5f),
              "point at +45 degrees to the right projects near the right edge");

        // Above the camera: +Y is up on screen, so sy shrinks (top edge = 0).
        check(esp::project(cam, proj, {0.0f, 10.0f, 10.0f}, sx, sy) &&
                  near(sy, 0.0f, 0.5f),
              "point at +45 degrees up projects near the top edge");

        // Behind the camera: rejected.
        check(!esp::project(cam, proj, {0.0f, 0.0f, -10.0f}, sx, sy),
              "point behind the camera is rejected");

        // Right at the near plane: rejected (vz <= kNearPlane).
        check(!esp::project(cam, proj, {0.0f, 0.0f, 0.02f}, sx, sy),
              "point at the near plane is rejected");
    }

    // --- Aspect ratio -------------------------------------------------------
    // A 2000x1000 surface at 90 degrees vertical FOV (tan(half) == 1). The
    // horizontal FOV widens with the aspect ratio: a point at +45 degrees
    // horizontal is still well inside the frame (ndcX = (1)/(1*2) = 0.5),
    // while a point at horizontal ratio vx/vz = 2 lands on the right edge
    // (ndcX = 2/2 = 1).
    {
        const esp::Camera cam = esp::computeCamera({0, 0, 0}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(2000.0f, 1000.0f, 90.0f);

        check(near(proj.aspect, 2.0f), "aspect is width / height");

        float sx = 0.0f, sy = 0.0f;
        check(esp::project(cam, proj, {-10.0f, 0.0f, 10.0f}, sx, sy) &&
                  near(sx, 1500.0f, 0.5f),
              "45-degree horizontal point stays inside a 2:1 surface");

        check(esp::project(cam, proj, {-20.0f, 0.0f, 10.0f}, sx, sy) &&
                  near(sx, 2000.0f, 0.5f),
              "vx/vz == 2 point lands on the right edge of a 2:1 surface");
    }

    // --- Camera offset ------------------------------------------------------
    // A camera translated to (5, 10, -5) still projects its own ahead point
    // to the center.
    {
        const esp::Camera cam = esp::computeCamera({5.0f, 10.0f, -5.0f}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);

        float sx = 0.0f, sy = 0.0f;
        check(esp::project(cam, proj, {5.0f, 10.0f, 5.0f}, sx, sy) &&
                  near(sx, 500.0f) && near(sy, 500.0f),
              "translated camera still projects its own ahead point to center");
    }

    // --- Box projection -----------------------------------------------------
    {
        const esp::Camera cam = esp::computeCamera({0, 0, 0}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);

        // A box fully in front of the camera matches the min/max of its eight
        // projected corners.
        const Vec3 mn{-0.3f, -1.0f, 9.7f};
        const Vec3 mx{0.3f, 0.8f, 10.3f};
        {
            const esp::ScreenBox box = esp::projectBox(cam, proj, mn, mx, 0.0f);
            float refMinX = 1e30f, refMinY = 1e30f, refMaxX = -1e30f, refMaxY = -1e30f;
            for (int i = 0; i < 8; ++i) {
                const Vec3 corner{(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y,
                                  (i & 4) ? mx.z : mn.z};
                float sx = 0.0f, sy = 0.0f;
                if (!esp::project(cam, proj, corner, sx, sy)) continue;
                refMinX = std::min(refMinX, sx);
                refMinY = std::min(refMinY, sy);
                refMaxX = std::max(refMaxX, sx);
                refMaxY = std::max(refMaxY, sy);
            }
            check(box.visible && near(box.minX, refMinX) && near(box.minY, refMinY) &&
                      near(box.maxX, refMaxX) && near(box.maxY, refMaxY),
                  "a box in front of the camera matches its projected corners");
        }

        // The bug this guards: an entity pressed up against the camera has
        // corners behind the near plane. Dropping them (instead of clipping the
        // box against the plane) collapses the 2D box to a fraction of its real
        // size, so the overlay visibly detaches while the view turns.
        const Vec3 closeMin{-0.3f, 0.0f, -3.0f};
        const Vec3 closeMax{0.3f, 1.8f, 0.3f};
        {
            int behind = 0;
            for (int i = 0; i < 8; ++i) {
                const Vec3 corner{(i & 1) ? closeMax.x : closeMin.x,
                                  (i & 2) ? closeMax.y : closeMin.y,
                                  (i & 4) ? closeMax.z : closeMin.z};
                float sx = 0.0f, sy = 0.0f;
                if (!esp::project(cam, proj, corner, sx, sy)) ++behind;
            }
            check(behind > 0, "the close box really does straddle the near plane");

            const esp::ScreenBox box = esp::projectBox(cam, proj, closeMin, closeMax, 4000.0f);
            check(box.visible, "the straddling box still projects");
            check(box.minX <= 0.0f && box.maxX >= proj.width,
                  "the straddling box spans the full width");
            check(box.minY <= 500.0f && box.maxY >= 500.0f,
                  "the straddling box still covers the screen center");
            check(box.minX >= -4000.0f && box.maxX <= proj.width + 4000.0f &&
                      box.minY >= -4000.0f && box.maxY <= proj.height + 4000.0f,
                  "the clamp keeps off-screen coordinates finite");

            const esp::ScreenBox unclamped = esp::projectBox(cam, proj, closeMin, closeMax, 0.0f);
            check(unclamped.visible && (unclamped.minX < -proj.width || unclamped.maxX > 2.0f * proj.width),
                  "unclamped, the straddling box reaches far outside the surface");
        }

        // A box wrapped around the camera (a mob right in your face) covers
        // the whole screen.
        {
            const esp::ScreenBox box =
                esp::projectBox(cam, proj, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 4000.0f);
            check(box.visible && box.minX <= 0.0f && box.maxX >= proj.width &&
                      box.minY <= 0.0f && box.maxY >= proj.height,
                  "a box around the camera covers the screen");
        }

        // A box entirely behind the camera is not drawn.
        {
            const esp::ScreenBox box =
                esp::projectBox(cam, proj, {-0.3f, 0.0f, -10.0f}, {0.3f, 1.8f, -9.0f}, 4000.0f);
            check(!box.visible, "a box behind the camera is rejected");
        }

        // The same box seen from a turned camera still tracks: the entity sits
        // east of a south-facing camera, so its box belongs on the left.
        {
            const esp::ScreenBox box = esp::projectBox(cam, proj, {9.7f, 0.0f, 9.7f},
                                                       {10.3f, 1.8f, 10.3f}, 4000.0f);
            check(box.visible && box.maxX < 500.0f,
                  "an entity to the east gets a box left of center");
        }
    }

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
