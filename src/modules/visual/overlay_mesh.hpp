#pragma once

// Plumbing for overlays that draw world-space geometry through Bedrock's own
// renderer (Esp today; Hitbox and Block Outline carry the same logic inline).
//
// The alternative is projecting the overlay to the HUD surface by hand, and
// that projection can only ever be as good as the module's model of the camera
// (FOV slider, surface aspect, tick-rate look rotation). Every frame where the
// real camera disagrees with that model -- sprint FOV changes, view bob, one
// frame of look latency between the game's render and the launcher's overlay --
// the overlay visibly slides off the thing it belongs to.
//
// Handing the game world-space primitives instead lets the game transform them
// with the very same matrices it rendered the level with, so an overlay can
// only ever land where its entity is. That is what this header is for:
//
//   * HashedString / MaterialPtr / getMaterial() mirror the small slice of the
//     Bedrock render-material ABI needed to look up a RenderMaterial by name,
//     which is how an overlay picks a depth-tested or through-walls material.
//   * Mesh wraps the four Tessellator entry points and turns camera-relative
//     segments / quads into a flushed mesh, including the "thick line" beam
//     pass that the mobile GLES drivers need because they ignore line width.
//
// Everything is header-only and free of preloader/game symbols (the function
// pointers are handed in by the caller), so the host tests can drive it with
// captured stand-ins instead of a device.

#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace overlay {

using bedrocktools::sdk::Vec3;

// A straight world-space edge and a four-corner world-space face. The geometry
// builders (see esp_geometry.hpp) emit these, and Mesh turns them into
// tessellator vertices relative to the camera.
struct Segment {
    Vec3 from;
    Vec3 to;
};

struct Quad {
    Vec3 corners[4];
};

// ---------------------------------------------------------------------------
// Render materials
// ---------------------------------------------------------------------------

// Bedrock's HashedString: the material group hashes the name and caches the
// last match. Field order and types mirror the game struct.
struct HashedString {
    std::uint64_t mStrHash = 0;
    std::string mStr;
    mutable const HashedString* mLastMatch = nullptr;

    HashedString() = default;

    explicit HashedString(const char* text) : mStr(text ? text : "") {
        mStrHash = computeHash(mStr);
    }

private:
    static std::uint64_t computeHash(const std::string& text) {
        if (text.empty()) return 0;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        std::uint64_t hash = kOffset;
        for (const char ch : text) {
            hash = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        }
        return hash;
    }
};

// Bedrock's MaterialPtr is two pointers to the material and its owning
// control block. The game owns every material a RenderMaterialGroup hands out,
// so this non-owning mirror deliberately runs no destructor.
struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    // User-provided (rather than `= default`) so the non-trivial return ABI the
    // game uses for MaterialPtr is matched exactly.
    ~MaterialPtr() {}

    explicit operator bool() const { return sharedPtrData[0] != nullptr; }
};

// Resolves the ADRP (+ADD) or ADR-only pair a signature scan lands on to the
// static address it materializes, e.g. the RenderMaterialGroup singleton.
inline std::uintptr_t resolveAdrp(const std::uint32_t* instructions,
                                  std::size_t count,
                                  std::uint32_t targetRegister) {
    if (!instructions) return 0;

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t instruction = instructions[i];
        if ((instruction & 0x1F) != targetRegister) continue;

        if ((instruction & 0x9F000000) == 0x90000000) {
            const std::uint64_t immediateBits =
                (static_cast<std::uint64_t>((instruction >> 3) & 0x1FFFFC) |
                 static_cast<std::uint64_t>((instruction >> 29) & 3u))
                << 43;
            const std::int64_t immediate = static_cast<std::int64_t>(immediateBits) >> 31;
            const std::uintptr_t page =
                (reinterpret_cast<std::uintptr_t>(&instructions[i]) & ~0xFFFULL) + immediate;

            for (std::size_t j = i + 1; j < count; ++j) {
                const std::uint32_t add = instructions[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetRegister && (add & 0x1F) == targetRegister) {
                    std::uint32_t immediate12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) immediate12 <<= 12;
                    return page + immediate12;
                }
                if ((add & 0x1F) == targetRegister) break;
            }
        }

        if ((instruction & 0x9F000000) == 0x10000000) {
            const std::uint64_t immediateBits =
                (static_cast<std::uint64_t>((instruction >> 3) & 0x1FFFFC) |
                 static_cast<std::uint64_t>(instruction >> 29))
                << 43;
            const std::int64_t immediate = static_cast<std::int64_t>(immediateBits) >> 43;
            return reinterpret_cast<std::uintptr_t>(&instructions[i]) + immediate;
        }
    }
    return 0;
}

// Looks a RenderMaterial up by name through the group's vtable. Returns an
// empty MaterialPtr when the group is unavailable, so callers can fall back to
// a material the renderer already owns.
inline MaterialPtr getMaterial(std::uintptr_t renderMaterialGroup, const char* name) {
    if (!renderMaterialGroup) return {};

    void** vtable = *reinterpret_cast<void***>(renderMaterialGroup);
    if (!vtable ||
        !vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]) {
        return {};
    }

    const HashedString hashedName(name);
    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    auto lookup = reinterpret_cast<GetMaterialFn>(
        vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]);
    return lookup(reinterpret_cast<void*>(renderMaterialGroup), &hashedName);
}

// Picks the first named material that exists. Used to find a material without
// a depth test (so an overlay stays visible through terrain) with fallbacks
// for builds that do not expose it.
inline MaterialPtr getFirstMaterial(std::uintptr_t renderMaterialGroup,
                                    const char* const* names,
                                    std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        MaterialPtr material = getMaterial(renderMaterialGroup, names[i]);
        if (material) return material;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Tessellator
// ---------------------------------------------------------------------------

// The four entry points of the game's immediate-mode mesh path plus the
// ScreenContext's tessellator. A caller that could not resolve all of them
// simply reports !ready() and skips its world-space layer.
struct Mesh {
    using BeginFn = void (*)(void* tessellator, void* debugCallback, int primitiveMode,
                             int vertexCount, int noIndices);
    using ColorFn = void (*)(void* tessellator, float r, float g, float b, float a);
    using VertexFn = void (*)(void* tessellator, float x, float y, float z);
    // MeshHelpers::renderMeshImmediately takes a trailing padding reference the
    // caller has to keep alive but never fills.
    using RenderFn = void (*)(void* screenContext, void* tessellator, void* material, char* pad);

    BeginFn begin = nullptr;
    ColorFn color = nullptr;
    VertexFn vertex = nullptr;
    RenderFn render = nullptr;
    void* tessellator = nullptr;

    bool ready() const noexcept {
        return begin && color && vertex && render && tessellator;
    }

    void setColor(std::uint32_t rgba, float alpha) const {
        color(tessellator,
              static_cast<float>((rgba >> 16) & 0xFF) / 255.0f,
              static_cast<float>((rgba >> 8) & 0xFF) / 255.0f,
              static_cast<float>(rgba & 0xFF) / 255.0f,
              std::clamp(alpha, 0.0f, 1.0f));
    }

    // Vertices are relative to the camera, which sits at the origin of the
    // space the level renderer draws in.
    void emit(const Vec3& point, const Vec3& camera) const {
        vertex(tessellator, point.x - camera.x, point.y - camera.y, point.z - camera.z);
    }

    // Filled faces. Each quad is emitted with both windings so materials that
    // cull back faces keep the face visible from either side (a filled ESP box
    // is seen from the outside *and* through it).
    void drawQuads(void* screenContext, void* material, const Vec3& camera,
                   std::uint32_t colorRgba, float alpha,
                   const std::vector<Quad>& quads) const {
        if (quads.empty() || !ready() || !material) return;

        begin(tessellator, nullptr, 1 /* GL_QUADS */, static_cast<int>(quads.size() * 8), 0);
        setColor(colorRgba, alpha);
        for (const Quad& quad : quads) {
            for (const Vec3& corner : quad.corners) emit(corner, camera);
            for (int i = 3; i >= 0; --i) emit(quad.corners[i], camera);
        }

        char pad[0x58];
        std::memset(pad, 0, sizeof(pad));
        render(screenContext, tessellator, material, pad);
    }

    // Wire edges. halfWidth <= 0 keeps the crisp native line list; above it
    // every edge also becomes a camera-facing beam of that world-space half
    // width, because GLES line width is ignored by nearly every mobile driver.
    // The line list is always emitted on top so distant edges stay visible
    // once the beams shrink below a pixel.
    void drawSegments(void* screenContext, void* material, const Vec3& camera,
                      std::uint32_t colorRgba, float alpha,
                      const std::vector<Segment>& segments, float halfWidth) const {
        if (segments.empty() || !ready() || !material) return;

        char pad[0x58];

        if (halfWidth > 0.0f) {
            begin(tessellator, nullptr, 1 /* GL_QUADS */,
                  static_cast<int>(segments.size() * 8), 0);
            setColor(colorRgba, alpha);

            for (const Segment& segment : segments) {
                Vec3 p1{segment.from.x - camera.x, segment.from.y - camera.y,
                        segment.from.z - camera.z};
                Vec3 p2{segment.to.x - camera.x, segment.to.y - camera.y,
                        segment.to.z - camera.z};

                float dx = p2.x - p1.x;
                float dy = p2.y - p1.y;
                float dz = p2.z - p1.z;
                const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (length < 1e-5f) continue;
                dx /= length;
                dy /= length;
                dz /= length;

                // The camera is the origin here, so the vector to the segment
                // midpoint is the eye ray. side = dir x eyeRay is perpendicular
                // to both, which keeps the beam facing the player from any angle.
                const float mx = (p1.x + p2.x) * 0.5f;
                const float my = (p1.y + p2.y) * 0.5f;
                const float mz = (p1.z + p2.z) * 0.5f;
                float sx = dy * mz - dz * my;
                float sy = dz * mx - dx * mz;
                float sz = dx * my - dy * mx;
                float sideLength = std::sqrt(sx * sx + sy * sy + sz * sz);
                if (sideLength < 1e-5f) {
                    // Looking straight down the edge: pick any perpendicular so
                    // the edge does not vanish for a frame.
                    if (std::fabs(dy) < 0.9f) {
                        sx = -dz;
                        sy = 0.0f;
                        sz = dx;
                    } else {
                        sx = 1.0f;
                        sy = 0.0f;
                        sz = 0.0f;
                    }
                    sideLength = std::sqrt(sx * sx + sy * sy + sz * sz);
                    if (sideLength < 1e-5f) continue;
                }
                sx = sx / sideLength * halfWidth;
                sy = sy / sideLength * halfWidth;
                sz = sz / sideLength * halfWidth;

                // Overshoot both ends by half the width so the box corners
                // stay closed where edges meet.
                const float ex = dx * halfWidth;
                const float ey = dy * halfWidth;
                const float ez = dz * halfWidth;
                const Quad beam{{
                    {p1.x - ex - sx, p1.y - ey - sy, p1.z - ez - sz},
                    {p2.x + ex - sx, p2.y + ey - sy, p2.z + ez - sz},
                    {p2.x + ex + sx, p2.y + ey + sy, p2.z + ez + sz},
                    {p1.x - ex + sx, p1.y - ey + sy, p1.z - ez + sz},
                }};
                for (const Vec3& corner : beam.corners) {
                    vertex(tessellator, corner.x, corner.y, corner.z);
                }
                for (int i = 3; i >= 0; --i) {
                    const Vec3& corner = beam.corners[i];
                    vertex(tessellator, corner.x, corner.y, corner.z);
                }
            }

            std::memset(pad, 0, sizeof(pad));
            render(screenContext, tessellator, material, pad);
        }

        begin(tessellator, nullptr, 4 /* GL_LINES */, static_cast<int>(segments.size() * 2), 0);
        setColor(colorRgba, alpha);
        for (const Segment& segment : segments) {
            emit(segment.from, camera);
            emit(segment.to, camera);
        }

        std::memset(pad, 0, sizeof(pad));
        render(screenContext, tessellator, material, pad);
    }
};

} // namespace overlay
