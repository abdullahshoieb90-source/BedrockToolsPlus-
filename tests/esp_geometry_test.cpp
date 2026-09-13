// Regression test for the Esp overlay projection math.
//
// The camera basis and the perspective projection live in
// modules/visual/esp_geometry.hpp as pure functions, so the tricky math
// (yaw/pitch orientation, behind-camera rejection, aspect-corrected NDC)
// can be verified on the host without the game.
//
//     g++ -std=c++20 -I src -I include tests/esp_geometry_test.cpp -o /tmp/esp_geometry_test
//     /tmp/esp_geometry_test

#include "modules/visual/esp_geometry.hpp"

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
} // namespace

int main() {
    std::printf("esp projection geometry\n");

    // --- Camera basis -------------------------------------------------------
    // yaw = 0, pitch = 0 -> facing +Z, +X right, +Y up.
    {
        const esp::Camera cam = esp::computeCamera({0, 0, 0}, {0.0f, 0.0f});
        check(near(cam.forward.x, 0.0f) && near(cam.forward.y, 0.0f) && near(cam.forward.z, 1.0f),
              "yaw 0 / pitch 0 faces +Z");
        check(near(cam.right.x, 1.0f) && near(cam.right.y, 0.0f) && near(cam.right.z, 0.0f),
              "yaw 0 right vector is +X");
        check(near(cam.up.x, 0.0f) && near(cam.up.y, 1.0f) && near(cam.up.z, 0.0f),
              "yaw 0 up vector is +Y");
    }

    // yaw = 90 -> facing -X (turning left), right hand sweeps to +Z.
    {
        const esp::Camera cam = esp::computeCamera({0, 0, 0}, {0.0f, 90.0f});
        check(near(cam.forward.x, -1.0f) && near(cam.forward.z, 0.0f, 0.001f),
              "yaw 90 faces -X");
        check(near(cam.right.z, 1.0f), "yaw 90 right vector is +Z");
    }

    // pitch = -90 -> facing straight up.
    {
        const esp::Camera cam = esp::computeCamera({0, 0, 0}, {-90.0f, 0.0f});
        check(near(cam.forward.y, 1.0f), "pitch -90 faces +Y (straight up)");
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

        // A point to the right (equal to its depth) lands on the right edge
        // for a square surface and 90 FOV: ndcX = (vx/vz)/(tan*aspect) = 1.
        check(esp::project(cam, proj, {10.0f, 0.0f, 10.0f}, sx, sy) &&
                  near(sx, 1000.0f, 0.5f),
              "point at +45 degrees right projects near the right edge");

        // Above the camera: +Y is up on screen, so sy shrinks (top edge = 0).
        check(esp::project(cam, proj, {0.0f, 10.0f, 10.0f}, sx, sy) &&
                  near(sy, 0.0f, 0.5f),
              "point at +45 degrees up projects near the top edge");

        // Behind the camera: rejected.
        check(!esp::project(cam, proj, {0.0f, 0.0f, -10.0f}, sx, sy),
              "point behind the camera is rejected");

        // Right at the near plane: rejected (vz <= 0.05).
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
        check(esp::project(cam, proj, {10.0f, 0.0f, 10.0f}, sx, sy) &&
                  near(sx, 1500.0f, 0.5f),
              "45-degree horizontal point stays inside a 2:1 surface");

        check(esp::project(cam, proj, {20.0f, 0.0f, 10.0f}, sx, sy) &&
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

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
