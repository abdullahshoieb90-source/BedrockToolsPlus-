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
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

    // --- Tracer anchor ------------------------------------------------------
    // The world-space tracer ends on the center (middle) of the entity's own
    // box -- the same world point the wireframe surrounds, which is why the
    // game pins the line to the hitbox. These cases pin the anchor itself.
    {
        const esp::Camera cam = esp::computeCamera({0.0f, 1.62f, 0.0f}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);

        // Dead ahead, the anchor is the box's world center: the screen-space
        // middle of the projected box is NOT it (the near face wins the
        // min/max), which is the drift a HUD tracer used to show.
        {
            const Vec3 mn{-0.3f, 0.0f, 4.7f};
            const Vec3 mx{0.3f, 1.8f, 5.3f};
            const Vec3 anchor = esp::boxCenter(mn, mx);
            float ax = 0.0f, ay = 0.0f;
            check(esp::project(cam, proj, anchor, ax, ay),
                  "the center anchor of a box ahead projects");
            check(near(ax, 500.0f, 0.5f) && near(ay, 572.0f, 0.5f),
                  "a dead-ahead anchor lands mid-hitbox");
            const esp::ScreenBox box = esp::projectBox(cam, proj, mn, mx, 0.0f);
            const float boxCenterY = (box.minY + box.maxY) * 0.5f;
            check(box.visible && std::fabs(boxCenterY - ay) > 1.0f,
                  "dead ahead, the 2D box middle drifts off the center anchor");
        }

        // Off-axis, the world-space anchor keeps its meaning: the same middle
        // the wireframe surrounds, wherever the projection would put it.
        {
            const Vec3 mn{2.0f, 0.0f, 4.0f};
            const Vec3 mx{4.0f, 1.8f, 6.0f};
            const Vec3 anchor = esp::boxCenter(mn, mx);
            check(near(anchor.x, 3.0f) && near(anchor.y, 0.9f) && near(anchor.z, 5.0f),
                  "the off-axis anchor is the box's middle");
            float ax = 0.0f, ay = 0.0f;
            check(esp::project(cam, proj, anchor, ax, ay) &&
                      near(ax, 200.0f, 0.5f) && near(ay, 572.0f, 0.5f),
                  "which projects mid-hitbox from any camera");
        }
    }

    // --- Head anchor --------------------------------------------------------
    // The nametag (and the label column above the box) is centered on the
    // projected head point -- the top-center of the entity's own box. The 2D
    // box's top-middle is a screen-space average that perspective shifts away
    // from the head, which is what these cases pin down.
    {
        const esp::Camera cam = esp::computeCamera({0.0f, 1.62f, 0.0f}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);

        // The anchor is exactly the projection of the box's top-center.
        {
            const Vec3 mn{-0.3f, 0.0f, 4.7f};
            const Vec3 mx{0.3f, 1.8f, 5.3f};
            float ax = 0.0f, ay = 0.0f;
            check(esp::projectBoxTopCenter(cam, proj, mn, mx, ax, ay),
                  "the head anchor of a box ahead projects");
            float px = 0.0f, py = 0.0f;
            check(esp::project(cam, proj, {0.0f, 1.8f, 5.0f}, px, py) &&
                      near(ax, px) && near(ay, py),
                  "the anchor is the projection of the top-center");
            check(near(ax, 500.0f, 0.5f) && near(ay, 482.0f, 0.5f),
                  "a dead-ahead head anchor sits above the head");
        }

        // Off-axis, the 2D box's top-middle is shifted sideways off the head:
        // the projected box is asymmetric, so its screen-space middle is not
        // the head column, and its top edge is not the head height either.
        {
            const Vec3 mn{2.0f, 0.0f, 4.0f};
            const Vec3 mx{4.0f, 1.8f, 6.0f};
            float ax = 0.0f, ay = 0.0f;
            check(esp::projectBoxTopCenter(cam, proj, mn, mx, ax, ay),
                  "the head anchor of an off-axis box projects");
            check(near(ax, 200.0f, 0.5f) && near(ay, 482.0f, 0.5f),
                  "the off-axis head anchor sits above the head");
            const esp::ScreenBox box = esp::projectBox(cam, proj, mn, mx, 0.0f);
            const float boxMidX = (box.minX + box.maxX) * 0.5f;
            check(box.visible && std::fabs(boxMidX - ax) > 5.0f &&
                      std::fabs(box.minY - ay) > 1.0f,
                  "off-axis, the 2D box top-middle drifts off the head");
        }

        // An anchor behind the camera is rejected, so the caller can fall
        // back to the 2D box instead of mirroring the name across the screen.
        {
            float ax = 0.0f, ay = 0.0f;
            check(!esp::projectBoxTopCenter(cam, proj, {-0.3f, 0.0f, -10.0f},
                                            {0.3f, 1.8f, -9.0f}, ax, ay),
                  "a head anchor behind the camera is rejected");
        }
    }

    // --- world-space geometry ----------------------------------------------
    //
    // The box outline, brackets and fill are no longer projected by the
    // module: they are emitted as world-space primitives and placed by the
    // game's own matrices, which is what keeps them glued to the entity while
    // the view turns. These builders are the whole of that half, so they are
    // pinned here.
    {
        std::printf("esp world geometry\n");

        const bedrocktools::sdk::AABB box{{0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 4.0f}};

        auto axisLength = [](const overlay::Segment& segment) {
            const float dx = segment.to.x - segment.from.x;
            const float dy = segment.to.y - segment.from.y;
            const float dz = segment.to.z - segment.from.z;
            return std::array{std::fabs(dx), std::fabs(dy), std::fabs(dz)};
        };
        auto isAxisAligned = [&](const overlay::Segment& segment) {
            const auto spans = axisLength(segment);
            int nonZero = 0;
            for (const float span : spans) {
                if (span > 0.0001f) ++nonZero;
            }
            return nonZero == 1;
        };
        auto lengthOf = [&](const overlay::Segment& segment) {
            const auto spans = axisLength(segment);
            return spans[0] + spans[1] + spans[2]; // axis-aligned, so a sum works
        };

        std::vector<overlay::Segment> edges;
        esp::world::addBoxEdges(edges, box);
        check(edges.size() == 12, "the box style emits the twelve edges of the AABB");
        check(std::all_of(edges.begin(), edges.end(), isAxisAligned),
              "every edge runs along exactly one world axis");

        float edgeLength = 0.0f;
        for (const auto& edge : edges) edgeLength += lengthOf(edge);
        check(near(edgeLength, 4.0f * (1.0f + 2.0f + 4.0f)),
              "the twelve edges cover the box perimeter exactly");

        int touchesMin = 0, touchesMax = 0;
        for (const auto& edge : edges) {
            for (const Vec3* corner : {&edge.from, &edge.to}) {
                if (near(corner->x, 0.0f) && near(corner->y, 0.0f) && near(corner->z, 0.0f)) ++touchesMin;
                if (near(corner->x, 1.0f) && near(corner->y, 2.0f) && near(corner->z, 4.0f)) ++touchesMax;
            }
        }
        check(touchesMin == 3 && touchesMax == 3,
              "the extreme corners of the box are wired up");

        std::vector<overlay::Segment> corners;
        esp::world::addBoxCorners(corners, box, 0.33f);
        check(corners.size() == 24, "the corner style trims every edge into two brackets");
        check(std::all_of(corners.begin(), corners.end(), isAxisAligned),
              "every bracket still runs along exactly one world axis");

        const float expectedTrim = 0.33f * 1.0f; // shortest box side * fraction
        bool evenBrackets = true;
        for (const auto& bracket : corners) {
            if (!near(lengthOf(bracket), expectedTrim, 0.0005f)) evenBrackets = false;
        }
        check(evenBrackets, "all brackets are the same world length, so they look even");

        bool bracketEndsOnBox = true;
        for (const auto& bracket : corners) {
            for (const Vec3* point : {&bracket.from, &bracket.to}) {
                const bool onFace = near(point->x, 0.0f) || near(point->x, 1.0f) ||
                                    near(point->y, 0.0f) || near(point->y, 2.0f) ||
                                    near(point->z, 0.0f) || near(point->z, 4.0f);
                const bool inside = point->x >= -0.001f && point->x <= 1.001f &&
                                    point->y >= -0.001f && point->y <= 2.001f &&
                                    point->z >= -0.001f && point->z <= 4.001f;
                if (!onFace || !inside) bracketEndsOnBox = false;
            }
        }
        check(bracketEndsOnBox, "no bracket endpoint leaves the box");

        std::vector<overlay::Quad> faces;
        esp::world::addBoxFaces(faces, box);
        check(faces.size() == 6, "the filled box is the six faces of the AABB");

        float area = 0.0f;
        bool facesOnBox = true;
        for (const auto& face : faces) {
            float low[3] = {1e30f, 1e30f, 1e30f};
            float high[3] = {-1e30f, -1e30f, -1e30f};
            for (const Vec3& corner : face.corners) {
                const float point[3] = {corner.x, corner.y, corner.z};
                for (int axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], point[axis]);
                    high[axis] = std::max(high[axis], point[axis]);
                }
                const bool onShell = near(corner.x, 0.0f) || near(corner.x, 1.0f) ||
                                     near(corner.y, 0.0f) || near(corner.y, 2.0f) ||
                                     near(corner.z, 0.0f) || near(corner.z, 4.0f);
                if (!onShell) facesOnBox = false;
            }
            // An axis-aligned face spans exactly two of the three axes.
            float span[3] = {high[0] - low[0], high[1] - low[1], high[2] - low[2]};
            std::sort(span, span + 3);
            area += span[1] * span[2];
        }
        check(facesOnBox, "every face corner sits on the box shell");
        check(near(area, 2.0f * (1.0f * 2.0f + 2.0f * 4.0f + 1.0f * 4.0f)),
              "the six faces together are the box surface area");
    }

    // --- world-space billboard text -----------------------------------------
    //
    // The distance readout is world geometry now: quads anchored under the
    // entity's feet, so the game pins them to the hitbox the way it pins the
    // wireframe. The properties that keep it "on the hitbox" are pinned here:
    // the anchor position never consults the projection (only the size does),
    // the block hangs below its anchor, faces the camera, and keeps a
    // constant apparent height at any range.
    {
        std::printf("esp world billboard text\n");

        const esp::Camera cam = esp::computeCamera({0.0f, 1.62f, 0.0f}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);
        const Vec3 anchor{3.0f, -0.1f, 10.0f}; // under an entity's feet

        std::vector<overlay::Quad> text;
        check(esp::world::addBillboardText(text, cam, proj, anchor, "10.0m", 12.0f),
              "a distance ahead of the camera produces quads");

        // "10.0m" = 2 + 4 + 1 + 4 + 4 merged glyph rectangles.
        check(text.size() == 15, "the blocky readout is fifteen glyph rectangles");

        // Every corner lies in the camera-facing plane through the anchor:
        // that is what makes the text readable instead of edge-on.
        bool planar = true;
        for (const auto& quad : text) {
            for (const Vec3& corner : quad.corners) {
                const Vec3 delta{corner.x - anchor.x, corner.y - anchor.y,
                                 corner.z - anchor.z};
                if (std::fabs(esp::dot(delta, cam.forward)) > 0.0001f) planar = false;
            }
        }
        check(planar, "every quad corner faces the camera plane");

        // The block hangs below the anchor (never above it) and is centered
        // on it along the camera's right vector, so the digits sit under the
        // entity's feet, not next to them.
        bool below = true;
        float minRight = 1e30f, maxRight = -1e30f;
        for (const auto& quad : text) {
            for (const Vec3& corner : quad.corners) {
                const Vec3 delta{corner.x - anchor.x, corner.y - anchor.y,
                                 corner.z - anchor.z};
                if (esp::dot(delta, cam.up) > 0.0001f) below = false;
                const float alongRight = esp::dot(delta, cam.right);
                minRight = std::min(minRight, alongRight);
                maxRight = std::max(maxRight, alongRight);
            }
        }
        check(below, "the whole text block hangs below the anchor");
        check(near(minRight, -maxRight, 0.0001f),
              "the text block is centered on the anchor");

        // Constant apparent size: doubling the depth doubles the world height
        // of the glyphs (the projection scales the size, never the anchor).
        auto worldHeightAt = [&](float depth) {
            const Vec3 a{0.0f, 1.62f, depth};
            std::vector<overlay::Quad> quads;
            if (!esp::world::addBillboardText(quads, cam, proj, a, "8", 12.0f)) return -1.0f;
            float low = 1e30f, high = -1e30f;
            for (const auto& quad : quads) {
                for (const Vec3& corner : quad.corners) {
                    const float up = esp::dot({corner.x - a.x, corner.y - a.y,
                                               corner.z - a.z}, cam.up);
                    low = std::min(low, up);
                    high = std::max(high, up);
                }
            }
            return high - low;
        };
        const float nearHeight = worldHeightAt(10.0f);
        const float farHeight = worldHeightAt(20.0f);
        check(near(nearHeight, farHeight * 0.5f, 0.0001f),
              "twice the depth, twice the world size: the apparent height is constant");

        // The wanted height in pixels actually survives the round trip: at
        // depth d the world height is pixelHeight * 2*tan(fov/2)*d/height.
        const float expectedNear = 12.0f * 2.0f * proj.tanHalfFov * 10.0f / proj.height;
        check(near(nearHeight, expectedNear, 0.0001f),
              "the requested pixel height is honored");

        // Nothing to draw: empty text, no representable glyph, or an anchor
        // at/behind the camera.
        {
            std::vector<overlay::Quad> quads;
            check(!esp::world::addBillboardText(quads, cam, proj, anchor, "", 12.0f),
                  "empty text draws nothing");
            check(!esp::world::addBillboardText(quads, cam, proj, anchor, "xyz", 12.0f),
                  "text without a representable glyph draws nothing");
            check(!esp::world::addBillboardText(quads, cam, proj, {0.0f, 1.62f, -10.0f},
                                                "10.0m", 12.0f),
                  "an anchor behind the camera draws nothing");
            check(quads.empty(), "and no partial quads are left behind");
        }
    }

    // --- Why the Crosshair origin cannot be geometry ----------------------
    // A world-space segment that starts at the camera lies on a single view
    // ray, so the game's projection collapses the whole of it onto one pixel.
    // That -- not the menu -- is what made the tracer "disappear" when
    // Crosshair was selected, and no amount of fixing on the geometry side
    // helps, so the crosshair line is placed on the screen instead.
    {
        const esp::Camera cam = esp::computeCamera({0.0f, 1.62f, 0.0f}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);
        const Vec3 target{3.0f, 0.9f, 5.0f};
        auto along = [&](float t) {
            return Vec3{cam.pos.x + (target.x - cam.pos.x) * t,
                        cam.pos.y + (target.y - cam.pos.y) * t,
                        cam.pos.z + (target.z - cam.pos.z) * t};
        };

        float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
        const bool first = esp::project(cam, proj, along(0.3f), x1, y1);
        const bool second = esp::project(cam, proj, along(0.8f), x2, y2);
        check(first && second, "two points of a camera-origin line do project");
        check(first && second && near(x1, x2) && near(y1, y2),
              "onto the same pixel, i.e. there is no line to see there");
        float cx = 0.0f, cy = 0.0f;
        check(!esp::project(cam, proj, cam.pos, cx, cy),
              "and the camera's own position is not projectable at all");
    }

    // --- Crosshair tracer --------------------------------------------------
    // The screen-space line the module draws in its place: out of the exact
    // middle of the surface, onto the projected hitbox, and -- for a target
    // that is not on the surface at all -- out to the edge along the same
    // direction, which is the only place its coordinates may be clipped.
    {
        const esp::Camera cam = esp::computeCamera({0.0f, 1.62f, 0.0f}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);
        const float kRadius = 0.5f * std::sqrt(1000.0f * 1000.0f + 1000.0f * 1000.0f);

        const Vec3 onScreen{3.0f, 0.9f, 5.0f};
        const esp::ScreenSegment ahead = esp::crosshairTracer(cam, proj, onScreen);
        float ex = 0.0f, ey = 0.0f;
        check(esp::project(cam, proj, onScreen, ex, ey), "the on-screen target projects");
        check(ahead.visible && near(ahead.x0, 500.0f) && near(ahead.y0, 500.0f),
              "the tracer starts on the middle of the surface");
        check(ahead.visible && near(ahead.x1, ex, 0.01f) && near(ahead.y1, ey, 0.01f),
              "and ends on the projected anchor");

        const esp::ScreenSegment side =
            esp::crosshairTracer(cam, proj, {40.5f, 0.9f, 4.5f});
        const float dx = side.x1 - 500.0f;
        const float dy = side.y1 - 500.0f;
        check(side.visible && side.x1 < 500.0f,
              "an entity far to the east keeps a line pointing left");
        check(near(std::sqrt(dx * dx + dy * dy), kRadius, 0.5f),
              "whose end sits on the screen edge, never past it");
        check(near(dx / dy, -4500.0f / 80.0f, 0.5f),
              "and was scaled along its own direction, not sideways");

        check(!esp::crosshairTracer(cam, proj, {0.0f, 1.62f, 8.0f}).visible,
              "an entity dead ahead gets no stub of a line under the crosshair");

        // Behind the camera plane a raw division mirrors the target to the
        // other half of the screen, so the lateral sign is kept on purpose:
        // west of a south-facing camera stays on the right when it is over the
        // player's shoulder.
        const esp::ScreenSegment behind =
            esp::crosshairTracer(cam, proj, {-20.0f, 1.0f, -5.0f});
        check(behind.visible && behind.x1 > 500.0f,
              "an entity behind the camera keeps its side instead of mirroring");

        // A box of NaNs must not be able to hand the launcher coordinates it
        // would reject the whole batch for.
        const float kNan = std::nanf("");
        check(!esp::crosshairTracer(cam, proj, {kNan, 1.0f, 5.0f}).visible,
              "a non-finite anchor produces no line at all");
        const esp::ScreenSegment broken =
            esp::crosshairTracer(cam, proj, {0.0f, 1.62f, kNan});
        check(!broken.visible && std::isfinite(broken.x0) && std::isfinite(broken.y0),
              "and the center it started from is not submitted either");
    }

    // --- Nametag text ------------------------------------------------------
    // The name field holds what the game's own font has to make sense of:
    // markup codes, zero-width formatting characters, padding whitespace, and
    // occasionally bytes that are not UTF-8 at all. Every one of them reads as
    // an extra character on a HUD font, and every one of them also fed the
    // width the label is centered on.
    {
        std::string marked;
        marked += "\xC2\xA7";
        marked += "r";
        marked += "\xC2\xA7";
        marked += "4";
        marked += "Steve";
        check(esp::sanitizeName(marked) == "Steve",
              "the section-sign markup the game's font eats is stripped");

        std::string latin1;
        latin1 += "\xA7";
        latin1 += "l";
        latin1 += "Steve";
        check(esp::sanitizeName(latin1) == "Steve",
              "including the single-byte form some builds store");

        std::string messy;
        messy += " ";
        messy += "\xE2\x80\x8B"; // zero-width space
        messy += "\xE2\x80\xAE"; // right-to-left override
        messy += "\xC2\xAD";     // soft hyphen
        messy += "\nAhmed\n";
        messy += "\xC2\xA0";     // no-break space
        messy += "X";
        check(esp::sanitizeName(messy) == "Ahmed X",
              "invisible format characters are dropped and stray whitespace collapses");

        std::string broken = "Jo";
        broken += "\xFF"; // not a valid lead byte at all
        broken += "n";
        check(esp::sanitizeName(broken) == "Jon",
              "and bytes that are not UTF-8 never reach the font");

        std::string truncated = "Jon";
        truncated += "\xE2\x80"; // a sequence cut off at the end of the string
        check(esp::sanitizeName(truncated) == "Jon",
              "including a truncated sequence");

        // One oversized text makes the launcher reject the whole draw batch, so
        // an unbounded name has to be cut here instead.
        check(esp::sanitizeName(std::string(5000, 'a')).size() ==
                  esp::kMaxNameCodePoints,
              "an over-long name is capped rather than hiding every label");

        // Widths are measured per glyph, which is what the centering needs.
        check(near(esp::measureTextWidth("100", 10.0f), 18.0f),
              "digits and letters share the 0.6 em cell");
        check(near(esp::measureTextWidth("ill", 10.0f), 9.6f),
              "thin strokes take about half of it");
        check(near(esp::measureTextWidth("MW", 10.0f), 18.4f),
              "and M and W nearly the whole of it");

        std::string arabic;
        arabic += "\xD9\x84\xD8\xA7\xD9\x84\xD8\xA8"; // four letters, eight bytes
        check(arabic.size() == 8, "the name really is multi-byte");
        check(near(esp::measureTextWidth(arabic, 10.0f), 24.0f),
              "and is measured in code points, not bytes");
        check(!near(esp::measureTextWidth(arabic, 10.0f),
                    static_cast<float>(arabic.size()) * 0.6f * 10.0f),
              "which is the half: the byte count used to double it and slide the "
              "label off the head");

        std::string cjk;
        cjk += "\xE7\x8E\xA9"; // U+73A9, a full-width ideograph
        check(near(esp::measureTextWidth(cjk, 10.0f), 10.0f),
              "a CJK glyph is one full em wide");

        std::string combining = "e";
        combining += "\xCC\x81"; // U+0301 combining acute
        check(near(esp::measureTextWidth(combining, 10.0f), 6.0f),
              "a combining mark rides on its base letter and adds no width");
    }

    // --- A broken collision box cannot poison the frame -------------------
    // project() and projectBox() are the only road from an actor's AABB to a
    // draw command, and one non-finite command costs the whole frame's labels.
    {
        const esp::Camera cam = esp::computeCamera({0.0f, 1.62f, 0.0f}, {0.0f, 0.0f});
        const esp::SurfaceProjection proj = esp::makeProjection(1000.0f, 1000.0f, 90.0f);
        const float kNan = std::nanf("");

        float x = 0.0f, y = 0.0f;
        check(!esp::project(cam, proj, {kNan, 1.0f, 5.0f}, x, y),
              "a non-finite world point is not projected");

        const esp::ScreenBox all =
            esp::projectBox(cam, proj, {kNan, kNan, kNan}, {kNan, kNan, kNan}, 4000.0f);
        check(!all.visible, "a box of NaNs is reported invisible");

        const esp::ScreenBox partial =
            esp::projectBox(cam, proj, {-0.3f, 0.0f, 4.7f}, {0.3f, kNan, 5.3f}, 4000.0f);
        check(partial.visible && std::isfinite(partial.minX) &&
                  std::isfinite(partial.maxX) && std::isfinite(partial.minY),
              "and a partly broken box keeps its finite bounds instead of poisoning the min/max");
    }

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
