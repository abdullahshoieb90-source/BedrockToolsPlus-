#pragma once

// Pure geometry for the Custom Capes self-rendered overlay (RenderMode =
// Overlay / Both). Everything here is plain C++ with no Minecraft or Android
// dependencies so it can be unit-tested on the host (see
// tests/customcapes_render_math_test.cpp). The module's render hook turns
// these world-space corners + UVs into tessellator vertices.
//
// The overlay draws the classic cape as a thin rectangle that hangs on the
// local player's back, exactly where the vanilla cape mesh sits:
//
//   * 10 px wide x 16 px tall (10/16 x 1 block), 1 px thick (1/16 block)
//   * anchored at the shoulders, behind the torso, rotated by the actor's
//     body yaw (never the camera yaw, so the cape turns with the player)
//   * split into an upper and a lower segment; the lower segment sways
//     around the hinge with a time-based sine so the overlay reads as a
//     waving cape instead of a cardboard sign
//   * textured with the outer-back-face region (x=1..11, y=1..17) of the
//     64x32 cape canvas the resampler produces
//
// Coordinate conventions (identical to Bedrock model space, 16 px = 1 block,
// same as modules/visual/wings_shape.hpp):
//   +x the player's left   +y up   +z backwards (back/cape side)
// With the world mapping used by the Wings module (right = (-cos, -sin),
// forward = (-sin, +cos) for a yaw in degrees, model +z -> world -forward),
// the world direction that points behind the player is
//   back = -forward = (+sin(yaw), -cos(yaw)).

#include <cmath>
#include <cstdint>

namespace customcapes::render {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float k) { return {a.x * k, a.y * k, a.z * k}; }

// ---------------------------------------------------------------------------
// Cape canvas -> texture coordinates
// ---------------------------------------------------------------------------

// The visible (worn) face of the classic cape is the 10x16 box-unwrap region
// x=1..11, y=1..17 of the 64x32 canvas (see customcapes_files.hpp). A
// half-texel inset avoids sampling the neighboring faces when the GPU filters.
inline constexpr float kCapeU0 = (1.0f + 0.5f) / 64.0f;
inline constexpr float kCapeV0 = (1.0f + 0.5f) / 32.0f;
inline constexpr float kCapeU1 = (11.0f - 0.5f) / 64.0f;
inline constexpr float kCapeV1 = (17.0f - 0.5f) / 32.0f;

// The cape hangs 10 px wide x 16 px tall = 10/16 x 1 block, 1 px thick.
inline constexpr float kCapeWidthBlocks = 10.0f / 16.0f;
inline constexpr float kCapeHeightBlocks = 1.0f;

// Anchor: the cape top hangs at the shoulders (~0.85 blocks above the feet).
inline constexpr float kCapeTopHeight = 0.85f;

// Gap between the torso's back face and the cape: half the player's AABB
// depth (~0.30) plus a small clearance so the overlay never z-fights the
// body mesh. Measured from the AABB center.
inline constexpr float kCapeBackOffset = 0.34f;

// Sway: the lower half rotates around the hinge line (the cape's midline)
// by at most this many degrees, at this angular speed (rad/s). The phase
// comes from the render clock.
inline constexpr float kCapeSwayAmplitudeDeg = 9.0f;
inline constexpr float kCapeSwaySpeed = 2.2f;

// ---------------------------------------------------------------------------
// Camera test (same convention as WingsModule::isThirdPersonCamera)
// ---------------------------------------------------------------------------

// True when the camera is outside the player's collision box, i.e. the player
// is seen from a real third-person point of view. In first-person the camera
// sits inside the player's head, inside the box, and the back-mounted cape
// would clip the view; the renderer skips it. The small margin absorbs jitter
// at the box edges.
inline bool isThirdPersonCamera(float camX, float camY, float camZ,
                                float aabbMinX, float aabbMinY, float aabbMinZ,
                                float aabbMaxX, float aabbMaxY, float aabbMaxZ) {
    constexpr float kMargin = 0.05f;
    const bool cameraInsideBox =
        camX >= aabbMinX - kMargin && camX <= aabbMaxX + kMargin &&
        camY >= aabbMinY - kMargin && camY <= aabbMaxY + kMargin &&
        camZ >= aabbMinZ - kMargin && camZ <= aabbMaxZ + kMargin;
    return !cameraInsideBox;
}

// ---------------------------------------------------------------------------
// Cape quad
// ---------------------------------------------------------------------------

struct CapeVertex {
    Vec3 pos;      // world position (not camera-relative; the hook subtracts cam)
    float u = 0.0f;
    float v = 0.0f;
};

// Rotates `p` around the world axis `axis` (unit) passing through `pivot` by
// `angleRad` (right-hand rule). Used to swing the lower cape segment around
// the hinge, whose direction is the player's right vector.
inline Vec3 rotateAroundAxis(const Vec3& p, const Vec3& pivot, const Vec3& axis, float angleRad) {
    const Vec3 r = p - pivot;
    const float c = std::cos(angleRad);
    const float s = std::sin(angleRad);
    // Rodrigues' rotation formula
    const Vec3 cross{axis.y * r.z - axis.z * r.y,
                     axis.z * r.x - axis.x * r.z,
                     axis.x * r.y - axis.y * r.x};
    const float dot = axis.x * r.x + axis.y * r.y + axis.z * r.z;
    const Vec3 rotated{r.x * c + cross.x * s + axis.x * dot * (1.0f - c),
                       r.y * c + cross.y * s + axis.y * dot * (1.0f - c),
                       r.z * c + cross.z * s + axis.z * dot * (1.0f - c)};
    return pivot + rotated;
}

// Builds the six corners of the swaying, two-segment cape overlay in world
// space (order: TL, TR, MR, ML, BL, BR).
//
//   feetX/feetY/feetZ  the center of the player's AABB bottom (feet)
//   yawDeg             the actor's body yaw in degrees (ActorRotationComponent)
//   swayPhase          render-clock phase in radians (0 disables the sway)
//   out[6]             corners: the top half is the rigid upper segment, the
//                      bottom half sways around the hinge line
//
// UVs come from the outer-back-face region of the 64x32 cape canvas: the
// upper segment covers the top 8 canvas rows of the face, the lower segment
// the bottom 8 rows, so the sway bends the texture with the mesh.
inline void buildCapeQuad(float feetX, float feetY, float feetZ, float yawDeg,
                          float swayPhase, CapeVertex out[6]) {
    const float yawRad = yawDeg * kDegToRad;
    const float cosYaw = std::cos(yawRad);
    const float sinYaw = std::sin(yawRad);

    // Body frame basis (see header comment): right and back are horizontal.
    const Vec3 right{-cosYaw, 0.0f, -sinYaw};
    const Vec3 back{sinYaw, 0.0f, -cosYaw};

    const Vec3 feet{feetX, feetY, feetZ};
    const Vec3 top = feet + Vec3{0.0f, kCapeTopHeight, 0.0f} + back * kCapeBackOffset;
    const Vec3 hinge = top + Vec3{0.0f, -kCapeHeightBlocks * 0.5f, 0.0f};
    const Vec3 hem = top + Vec3{0.0f, -kCapeHeightBlocks, 0.0f};

    const float halfWidth = kCapeWidthBlocks * 0.5f;
    const Vec3 tl = top + right * (-halfWidth);
    const Vec3 tr = top + right * (+halfWidth);
    const Vec3 ml = hinge + right * (-halfWidth);
    const Vec3 mr = hinge + right * (+halfWidth);
    Vec3 bl = hem + right * (-halfWidth);
    Vec3 br = hem + right * (+halfWidth);

    const float sway = std::sin(swayPhase) * kCapeSwayAmplitudeDeg * kDegToRad;
    if (sway != 0.0f) {
        // The hem swings backwards/forwards around the hinge axis (the
        // player's right vector); the midline stays put. Negative sign keeps
        // a positive swayPhase pushing the hem away from the back.
        bl = rotateAroundAxis(bl, ml, right, -sway);
        br = rotateAroundAxis(br, mr, right, -sway);
    }

    // UVs: face rows y=1..17 -> v = (1..17)/32; split at row 9.
    const float vMid = (1.0f + 8.0f) / 32.0f;
    out[0] = {tl, kCapeU0, kCapeV0};
    out[1] = {tr, kCapeU1, kCapeV0};
    out[2] = {mr, kCapeU1, vMid};
    out[3] = {ml, kCapeU0, vMid};
    out[4] = {bl, kCapeU0, kCapeV1};
    out[5] = {br, kCapeU1, kCapeV1};
}

} // namespace customcapes::render
