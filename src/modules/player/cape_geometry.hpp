#pragma once

// World-space cape geometry (pure data + math, host-testable).
//
// The cape is drawn directly into the world instead of being written into the
// player's skin. Skin patching cannot work on every build: it depends on the
// SerializedSkin layout (see customcapes.cpp) and on the module receiving a
// player tick at all, and on some builds neither is available. A world overlay
// depends only on the RenderLevel hook, which is the same hook the Wings module
// uses successfully on those builds.
//
// The shape is the vanilla cape: a one-pixel-thick cuboid hanging from the top
// of the player's back, rotating about its top edge as the player moves. The
// texture is the same 64x32 classic layout, so the UVs here match the art the
// user already picks in the menu.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bedrocktools::modules::cape {

// Classic cape canvas, in texels.
inline constexpr int kCapeCanvasWidth = 64;
inline constexpr int kCapeCanvasHeight = 32;

// Model units per texture-independent cape size (a Minecraft model pixel is
// 1/16 of a block).
inline constexpr float kModelPixel = 1.0f / 16.0f;
inline constexpr float kCapeWidthPixels = 10.0f;
inline constexpr float kCapeHeightPixels = 16.0f;
inline constexpr float kCapeThicknessPixels = 1.0f;

// The cape hangs from the player's shoulders. Values are in blocks, measured
// from the feet centre: the torso ends at 1.5, the shoulders are at ~1.45 and
// the cape's top edge sits just below them so it never floats.
inline constexpr float kShoulderHeight = 1.4375f;
inline constexpr float kShoulderHalfWidth = 0.28125f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec2 {
    float u = 0.0f;
    float v = 0.0f;
};

// One cape vertex: world-space position plus the texture coordinate it samples.
struct Vertex {
    Vec3 p;
    Vec2 uv;
};

// UV rectangles of the classic cape layout, normalised to 0..1 over the 64x32
// canvas. Only three faces are ever visible on a cape this thin.
struct CapeUv {
    Vec2 back[4];      // outer face, seen from behind
    Vec2 front[4];     // inner lining, seen from in front
    Vec2 bottom[4];    // the 10x1 strip under the hem
};

inline float uvU(float texelX) { return texelX / static_cast<float>(kCapeCanvasWidth); }
inline float uvV(float texelY) { return texelY / static_cast<float>(kCapeCanvasHeight); }

// Classic layout (same rectangles the skin uses, see customcapes_files.hpp):
//   back   x1..11,  y1..17
//   front  x12..22, y1..17
//   bottom x1..11,  y17..18
inline CapeUv capeUv() {
    CapeUv uv{};
    uv.back[0] = {uvU(1.0f), uvV(1.0f)};
    uv.back[1] = {uvU(11.0f), uvV(1.0f)};
    uv.back[2] = {uvU(11.0f), uvV(17.0f)};
    uv.back[3] = {uvU(1.0f), uvV(17.0f)};
    uv.front[0] = {uvU(12.0f), uvV(1.0f)};
    uv.front[1] = {uvU(22.0f), uvV(1.0f)};
    uv.front[2] = {uvU(22.0f), uvV(17.0f)};
    uv.front[3] = {uvU(12.0f), uvV(17.0f)};
    uv.bottom[0] = {uvU(1.0f), uvV(17.0f)};
    uv.bottom[1] = {uvU(11.0f), uvV(17.0f)};
    uv.bottom[2] = {uvU(11.0f), uvV(18.0f)};
    uv.bottom[3] = {uvU(1.0f), uvV(18.0f)};
    return uv;
}

// Flap angle in degrees, from the player's motion.
//
// A still player has the cape hanging almost vertically (a couple of degrees
// of sway from the body); walking lifts it a little; falling lifts it a lot so
// it trails over the head the way the vanilla cape does. The angle rotates the
// cape about its top edge, backwards, so 90 degrees is horizontal.
inline float flapAngleDegrees(float horizontalSpeed, float verticalSpeed, bool onGround) {
    const float speed = std::fabs(horizontalSpeed);
    float angle = 4.0f + speed * 4.5f;

    // Rising (jumping) pushes the cape down; falling streams it upward. vanilla
    // clamps the same way, which is what keeps it from folding over the head.
    if (!onGround) {
        if (verticalSpeed < 0.0f) angle += std::min(-verticalSpeed * 18.0f, 55.0f);
        else angle -= std::min(verticalSpeed * 6.0f, 20.0f);
    }
    if (angle < 0.0f) angle = 0.0f;
    if (angle > 75.0f) angle = 75.0f;
    return angle;
}

// Builds the cape mesh.
//
// `feet` is the centre of the player's collision box at its bottom face,
// `right`/`forward` are the unit vectors of the player's body yaw, `flapDeg`
// comes from flapAngleDegrees() and `swayDeg` is a slow idle sway so a standing
// player does not look like a cardboard cut-out.
//
// `out` must have room for 3 quads (12 vertices); the return value is the
// number of vertices written. Faces that would be edge-on are still emitted:
// the depth test handles them, and dropping them would make the cape shimmer
// as the player turns.
inline int buildCapeMesh(const Vec3& feet,
                         const Vec3& right,
                         const Vec3& forward,
                         float flapDeg,
                         float swayDeg,
                         Vertex* out,
                         int capacity) {
    if (!out || capacity < 12) return 0;

    const float flap = flapDeg * 3.14159265358979323846f / 180.0f;
    const float sway = swayDeg * 3.14159265358979323846f / 180.0f;

    const float halfWidth = kShoulderHalfWidth;
    const float height = kCapeHeightPixels * kModelPixel;
    const float thickness = kCapeThicknessPixels * kModelPixel;

    // Top edge, in world space: on the shoulders, offset behind the body by
    // half the thickness so the cape does not z-fight with the torso.
    const Vec3 topCentre{
        feet.x + forward.x * (-thickness * 0.5f),
        feet.y + kShoulderHeight,
        feet.z + forward.z * (-thickness * 0.5f)};

    const Vec3 rightTop{topCentre.x - right.x * halfWidth, topCentre.y, topCentre.z - right.z * halfWidth};
    const Vec3 leftTop{topCentre.x + right.x * halfWidth, topCentre.y, topCentre.z + right.z * halfWidth};

    // Hanging direction: straight down, rotated backwards about the right axis
    // by the flap angle, plus a little sideways sway.
    const float downY = -std::cos(flap);
    const float backAmount = std::sin(flap);
    const float swayAmount = std::sin(sway);

    const auto hang = [&](float sideSign) {
        // sideSign -1 = right edge, +1 = left edge. The sway rotates the bottom
        // edge around the body's forward axis, so the hem swings sideways.
        const Vec3& top = (sideSign < 0.0f) ? rightTop : leftTop;
        const float sideOffset = swayAmount * sideSign;
        return Vec3{
            top.x - forward.x * (height * backAmount) + right.x * (height * sideOffset),
            top.y + height * downY,
            top.z - forward.z * (height * backAmount) + right.z * (height * sideOffset)};
    };

    const Vec3 rightBottom = hang(-1.0f);
    const Vec3 leftBottom = hang(1.0f);

    const CapeUv uv = capeUv();
    int n = 0;
    const auto quad = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                          const Vec2* uvs) {
        out[n++] = Vertex{a, uvs[0]};
        out[n++] = Vertex{b, uvs[1]};
        out[n++] = Vertex{c, uvs[2]};
        out[n++] = Vertex{d, uvs[3]};
    };

    // A quad is authored top-left, top-right, bottom-right, bottom-left of the
    // texture, so the two "top" vertices are the ones on the shoulders.
    quad(leftTop, rightTop, rightBottom, leftBottom, uv.back);

    // Inner lining: the same rectangle mirrored sideways, so it reads correctly
    // when the cape swings away and the inside is visible.
    const Vec3 innerOffset{forward.x * thickness, 0.0f, forward.z * thickness};
    quad(rightTop, leftTop,
         Vec3{leftBottom.x + innerOffset.x, leftBottom.y, leftBottom.z + innerOffset.z},
         Vec3{rightBottom.x + innerOffset.x, rightBottom.y, rightBottom.z + innerOffset.z},
         uv.front);

    // Hem: the 10x1 strip along the bottom edge, connecting outer and inner.
    quad(rightBottom,
         Vec3{rightBottom.x + innerOffset.x, rightBottom.y, rightBottom.z + innerOffset.z},
         Vec3{leftBottom.x + innerOffset.x, leftBottom.y, leftBottom.z + innerOffset.z},
         leftBottom,
         uv.bottom);

    return n;
}

// Body yaw (degrees, the actor's body rotation, never the camera) to the
// right/forward unit vectors used by buildCapeMesh. Kept here so the geometry
// stays testable without the game.
inline void bodyAxes(float bodyYawDegrees, Vec3& right, Vec3& forward) {
    const float yaw = bodyYawDegrees * 3.14159265358979323846f / 180.0f;
    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);
    right = Vec3{-cosYaw, 0.0f, -sinYaw};
    forward = Vec3{-sinYaw, 0.0f, cosYaw};
}

} // namespace bedrocktools::modules::cape
