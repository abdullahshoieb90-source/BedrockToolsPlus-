#pragma once

#include "modules/visual/wings_surface.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bedrocktools::modules::cape {
using wings::WingQuad;
constexpr float kTop = 24.0f;
constexpr float kFront = 2.15f;
constexpr float kDepth = 1.0f;

struct Pose { float pitch = 6.0f; float roll = 0.0f; }; // degrees

inline Pose interpolate(Pose a, Pose b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.pitch + (b.pitch - a.pitch) * t, a.roll + (b.roll - a.roll) * t};
}

// Damped rigid cape hinged at the shoulders. Speeds are measured in world
// space, then resolved in the body frame; turn lag uses shortest-arc yaw.
// reset() is also used on world changes / teleports, so no old velocity leaks.
struct Motion {
    Pose previous{}, current{};
    float x = 0, y = 0, z = 0, yaw = 0, phase = 0;
    bool hasSample = false;
    void reset() { *this = Motion{}; }
    void update(float nx, float ny, float nz, float bodyYaw, float dt) {
        if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz) || !std::isfinite(bodyYaw)) {
            reset(); return;
        }
        float dx = nx - x, dy = ny - y, dz = nz - z;
        if (!hasSample || !std::isfinite(dt) || dt <= 0.001f || dt > 0.25f || dx*dx + dy*dy + dz*dz >= 25.0f) {
            reset(); x = nx; y = ny; z = nz; yaw = bodyYaw; hasSample = true; return;
        }
        constexpr float pi = 3.14159265358979323846f;
        float angle = bodyYaw * pi / 180.0f;
        float vx = dx / dt, vz = dz / dt, vy = dy / dt;
        float forward = vx * -std::sin(angle) + vz * std::cos(angle);
        float lateral = vx * std::cos(angle) + vz * std::sin(angle);
        float turn = std::remainder(bodyYaw - yaw, 360.0f) / dt;
        float speed = std::min(std::sqrt(vx*vx + vz*vz), 8.0f);
        phase = std::fmod(phase + dt * speed * 2.5f, 2.0f * pi);
        // The reference Bedrock cape lifts much farther from the back when
        // sprinting than the old 5 degrees per block/s approximation. A walk
        // (~4.3 blocks/s) reaches ~49 degrees; a sprint (~5.6) ~62 degrees.
        // Forward is signed: moving backwards must not lift it through the body.
        float targetPitch = 6.0f + std::clamp(forward, 0.0f, 6.4f) * 10.0f
                          + std::clamp(-vy, 0.0f, 8.0f) * 1.5f
                          + std::sin(phase) * std::min(speed, 4.0f) * 0.6f;
        float targetRoll = std::clamp(-lateral * 2.0f - turn * 0.025f, -10.0f, 10.0f);
        // Lift promptly on acceleration, then settle a little more gently.
        const float pitchRate = targetPitch > current.pitch ? 10.0f : 6.0f;
        float pitchBlend = 1.0f - std::exp(-pitchRate * dt);
        float rollBlend = 1.0f - std::exp(-7.0f * dt);
        previous = current;
        current.pitch += (std::clamp(targetPitch, 4.0f, 72.0f) - current.pitch) * pitchBlend;
        current.roll += (targetRoll - current.roll) * rollBlend;
        x = nx; y = ny; z = nz; yaw = bodyYaw;
    }
};

// Full artwork, or the back face of a standard 64x32 atlas. Nearest-neighbour
// sampling bounds tessellation. Add a one-pixel edge around opaque pixels so
// the cape reads as a thin vanilla-style cuboid rather than a zero-depth plane.
inline std::vector<WingQuad> build(const unsigned char* rgba, int width, int height) {
    std::vector<WingQuad> out;
    if (!rgba || width <= 0 || height <= 0 || width > 2048 || height > 2048) return out;
    int x0 = 0, y0 = 0, w = width, h = height;
    if (width % 64 == 0 && height == width / 2) {
        int scale = width / 64;
        x0 = scale; y0 = scale; w = 10 * scale; h = 16 * scale;
    }
    const int cols = std::min(w, 32), rows = std::min(h, 48);
    std::vector<const unsigned char*> pixels(cols * rows);
    for (int y = 0; y < rows; ++y) for (int x = 0; x < cols; ++x) {
        int sx = x0 + (2*x+1)*w/(2*cols), sy = y0 + (2*y+1)*h/(2*rows);
        pixels[y*cols+x] = rgba + (static_cast<std::size_t>(sy)*width+sx)*4;
    }
    auto opaque = [&](int x, int y) {
        return x >= 0 && x < cols && y >= 0 && y < rows && pixels[y*cols+x][3] >= 128;
    };
    auto point = [](float x, float y, float z) {
        return wings::Vec3{-5.0f + 10.0f*x, kTop - 16.0f*y, z};
    };
    auto add = [&](wings::Vec3 a, wings::Vec3 b, wings::Vec3 c, wings::Vec3 d,
                   wings::Vec3 normal, const unsigned char* p) {
        WingQuad q{};
        q.p[0]=a; q.p[1]=b; q.p[2]=c; q.p[3]=d; q.n=normal;
        q.front = q.back = {p[0], p[1], p[2]};
        for (int i=0; i<4; ++i) q.frontC[i] = q.backC[i] = q.back;
        q.emissive = 1.0f;
        out.push_back(q);
    };
    out.reserve(cols * rows * 2 + (cols + rows) * 2);
    for (int y=0; y<rows; ++y) for (int x=0; x<cols; ++x) {
        if (!opaque(x,y)) continue;
        const auto* p = pixels[y*cols+x];
        float l=float(x)/cols, r=float(x+1)/cols, t=float(y)/rows, b=float(y+1)/rows;
        auto a=point(l,t,kFront), c=point(r,b,kFront), e=point(r,t,kFront), f=point(l,b,kFront);
        auto A=point(l,t,kFront+kDepth), C=point(r,b,kFront+kDepth);
        auto E=point(r,t,kFront+kDepth), F=point(l,b,kFront+kDepth);
        add(A,E,C,F,{0,0,1},p); add(a,f,c,e,{0,0,-1},p);
        if (!opaque(x-1,y)) add(a,A,F,f,{-1,0,0},p);
        if (!opaque(x+1,y)) add(e,c,C,E,{1,0,0},p);
        if (!opaque(x,y-1)) add(a,e,E,A,{0,1,0},p);
        if (!opaque(x,y+1)) add(f,F,C,c,{0,-1,0},p);
    }
    return out;
}

inline wings::Vec3 transformPoint(wings::Vec3 p, Pose pose) {
    constexpr float rad = 0.01745329251994329577f;
    float pitch = pose.pitch * rad, roll = pose.roll * rad;
    float down = kTop - p.y, depth = p.z - kFront;
    float d = down * std::cos(pitch) - depth * std::sin(pitch);
    float z = down * std::sin(pitch) + depth * std::cos(pitch);
    return {p.x * std::cos(roll) + d * std::sin(roll),
            kTop - (d * std::cos(roll) - p.x * std::sin(roll)), kFront + z};
}
inline void applyPose(std::vector<WingQuad>& quads, Pose pose) {
    for (auto& q : quads) {
        for (auto& p : q.p) p = transformPoint(p, pose);
        // Normals are directions: transform a displaced point and subtract pivot.
        auto n = transformPoint({q.n.x, kTop + q.n.y, kFront + q.n.z}, pose);
        q.n = {n.x, n.y - kTop, n.z - kFront};
    }
}
} // namespace bedrocktools::modules::cape
