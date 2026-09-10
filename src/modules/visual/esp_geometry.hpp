#pragma once

// Geometry for the Esp overlay, in its two halves:
//
//   * esp::world builds the box outline, the tracer and the distance readout
//     as *world-space* primitives, which the game then transforms with the
//     matrices it rendered the level with (see overlay_mesh.hpp). Nothing can
//     drift, because the module never has to model the camera for them: the
//     tracer ends inside the hitbox, and the distance is a billboard anchored
//     to the entity's feet whose size -- never its position -- follows the
//     projection.
//   * esp::computeCamera / makeProjection / project / projectBox are the
//     camera basis + perspective projection for the launcher HUD layer, which
//     is where the nametag and the health readout have to live because the
//     HUD owns the font. That projection is only ever as good as the module's
//     model of the camera, which is exactly why everything that has to sit
//     *on* an entity is no longer part of it. The one exception is the
//     Crosshair tracer (esp::crosshairTracer): a line that leaves the camera
//     cannot be geometry at all, because the game projects it onto a single
//     pixel -- see the comment on that function.
//   * esp::sanitizeName / measureTextWidth prepare the nametag string for the
//     HUD: the raw name field holds the markup codes and invisible format
//     characters the game's own font swallows (which a plain HUD font paints
//     as extra characters), and the label is centered on a width measured in
//     glyphs, never in bytes -- a UTF-8 name is two to four bytes per
//     character, which is what used to shove Arabic names off the head. The
//     width only means something for the font that draws the text, so
//     esp::labelFontFor also decides which one that is: the packaged pixel font
//     for the names it carries (its metrics are known exactly), the launcher's
//     default for the scripts it does not have.
//
// Pure functions with no game or preloader dependencies so host tests can
// cover both halves (see tests/esp_geometry_test.cpp) without bringing in the
// signature table or the HUD overlay.
//
// Conventions, matching the game:
//   * Bedrock's world is right-handed: +X east, +Y up, +Z south.
//   * rot.x is pitch (negative = looking up), rot.y is yaw, both degrees.
//     Yaw 0 looks towards +Z (south) and grows towards -X (west).
//   * The camera forward vector is therefore the same look line the Hitbox
//     module derives for its raycast:
//     (-sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch)).
//
// Because the world is right-handed, the camera's right-hand direction is
// cross(forward, worldUp) -- NOT cross(worldUp, forward), which is the left
// vector. Building the basis the other way round mirrors the whole overlay
// left-to-right: a target dead ahead still lands in the middle of the screen,
// but every target off-axis lands on the wrong side and the box runs away
// from its entity as soon as the view turns.
//
// The vertical FOV is passed in explicitly so the caller can source it from
// wherever it likes (the Esp module uses a menu slider).

#include "overlay_mesh.hpp"

#include <bedrocktools/sdk/Types.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esp {

inline constexpr float kPi = 3.14159265f;
inline constexpr float kDegToRad = kPi / 180.0f;

// Depth in front of the camera below which a point can no longer be
// projected. Points closer than this (or behind the camera) would divide by a
// zero/negative depth and land on the wrong side of the screen, so box edges
// are clipped against this plane instead (see projectBox).
inline constexpr float kNearPlane = 0.05f;

struct Camera {
    bedrocktools::sdk::Vec3 pos;
    bedrocktools::sdk::Vec3 right;
    bedrocktools::sdk::Vec3 up;
    bedrocktools::sdk::Vec3 forward;
};

inline bedrocktools::sdk::Vec3 normalize(const bedrocktools::sdk::Vec3& v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) return {0.0f, 1.0f, 0.0f};
    return {v.x / len, v.y / len, v.z / len};
}

inline bedrocktools::sdk::Vec3 cross(const bedrocktools::sdk::Vec3& a,
                                     const bedrocktools::sdk::Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float dot(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Builds the orthonormal camera basis from a position and a yaw/pitch pair
// (rot.x = pitch, rot.y = yaw, both degrees).
inline Camera computeCamera(const bedrocktools::sdk::Vec3& pos,
                            const bedrocktools::sdk::Vec2& rot) {
    const float yawR = rot.y * kDegToRad;
    const float pitchR = rot.x * kDegToRad;
    const float cy = std::cos(yawR);
    const float sy = std::sin(yawR);
    const float cp = std::cos(pitchR);
    const float sp = std::sin(pitchR);

    Camera cam;
    cam.pos = pos;
    cam.forward = normalize({-sy * cp, -sp, cy * cp});
    // Right-hand direction = cross(forward, worldUp), which normalizes to the
    // horizontal (-cos yaw, 0, -sin yaw). Deriving it from the yaw alone keeps
    // it horizontal (the game has no camera roll) and well defined at pitch
    // +/-90, where cross(forward, worldUp) collapses to the zero vector.
    cam.right = normalize({-cy, 0.0f, -sy});
    cam.up = normalize(cross(cam.right, cam.forward));
    return cam;
}

struct SurfaceProjection {
    float width = 1.0f;
    float height = 1.0f;
    float tanHalfFov = 1.0f;
    float aspect = 1.0f;
};

inline SurfaceProjection makeProjection(float width, float height, float fovDegrees) {
    SurfaceProjection proj;
    proj.width = width > 1.0f ? width : 1.0f;
    proj.height = height > 1.0f ? height : 1.0f;
    const float fov = std::clamp(fovDegrees, 30.0f, 120.0f);
    proj.tanHalfFov = std::tan(fov * 0.5f * kDegToRad);
    proj.aspect = proj.width / proj.height;
    return proj;
}

// Projects one world point to surface coordinates (pixels in the HUD surface
// space). Returns false when the point is at or behind the camera plane, or
// when the arithmetic did not produce a finite position (an actor whose
// collision box holds garbage still has to be skipped rather than handed to
// the launcher: one non-finite draw command makes the launcher reject the
// whole batch, and every label in it with it).
inline bool project(const Camera& cam, const SurfaceProjection& proj,
                    const bedrocktools::sdk::Vec3& world, float& outX, float& outY) {
    const bedrocktools::sdk::Vec3 d = {world.x - cam.pos.x, world.y - cam.pos.y,
                                       world.z - cam.pos.z};
    const float vz = dot(d, cam.forward);
    if (!(vz > kNearPlane)) return false;

    const float vx = dot(d, cam.right);
    const float vy = dot(d, cam.up);

    const float ndcX = (vx / vz) / (proj.tanHalfFov * proj.aspect);
    const float ndcY = (vy / vz) / proj.tanHalfFov;

    outX = (ndcX * 0.5f + 0.5f) * proj.width;
    outY = (0.5f - ndcY * 0.5f) * proj.height;
    return std::isfinite(outX) && std::isfinite(outY);
}

// Tight 2D bounding box of a projected world box, in surface coordinates.
struct ScreenBox {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    bool visible = false;
};

// Projects an axis-aligned world box (an entity AABB) to its tight 2D box.
//
// Corners behind the near plane cannot be projected, so the box's twelve
// edges are clipped against vz = kNearPlane and the intersection points are
// projected along with the surviving corners. Without that an entity standing
// right in front of the camera loses corners and its 2D box snaps to a
// fraction of the real size every time the view rotates -- the overlay then
// visibly detaches from the entity it belongs to.
//
// `limit` (in pixels) clamps the result so an entity that straddles the camera
// plane yields a huge-but-finite box covering the screen instead of
// coordinates in the tens of thousands. Pass 0 to disable clamping.
inline ScreenBox projectBox(const Camera& cam, const SurfaceProjection& proj,
                            const bedrocktools::sdk::Vec3& boxMin,
                            const bedrocktools::sdk::Vec3& boxMax,
                            float limit = 0.0f) {
    // The eight corners in camera space: (vx = right, vy = up, vz = forward).
    bedrocktools::sdk::Vec3 cs[8];
    for (int i = 0; i < 8; ++i) {
        const bedrocktools::sdk::Vec3 world{
            (i & 1) ? boxMax.x : boxMin.x,
            (i & 2) ? boxMax.y : boxMin.y,
            (i & 4) ? boxMax.z : boxMin.z,
        };
        const bedrocktools::sdk::Vec3 d{world.x - cam.pos.x, world.y - cam.pos.y,
                                        world.z - cam.pos.z};
        cs[i] = {dot(d, cam.right), dot(d, cam.up), dot(d, cam.forward)};
    }

    ScreenBox out;
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    bool any = false;

    const bool clamp = limit > 0.0f;
    auto accumulate = [&](float vx, float vy, float vz) {
        const float ndcX = (vx / vz) / (proj.tanHalfFov * proj.aspect);
        const float ndcY = (vy / vz) / proj.tanHalfFov;
        float sx = (ndcX * 0.5f + 0.5f) * proj.width;
        float sy = (0.5f - ndcY * 0.5f) * proj.height;
        // A corner that did not survive the division (a box holding garbage)
        // cannot be allowed into the min/max: it would make the whole result
        // non-finite and take the frame's other labels down with it.
        if (!std::isfinite(sx) || !std::isfinite(sy)) return;
        if (clamp) {
            sx = std::clamp(sx, -limit, proj.width + limit);
            sy = std::clamp(sy, -limit, proj.height + limit);
        }
        minX = std::min(minX, sx);
        minY = std::min(minY, sy);
        maxX = std::max(maxX, sx);
        maxY = std::max(maxY, sy);
        any = true;
    };

    for (const bedrocktools::sdk::Vec3& p : cs) {
        if (p.z >= kNearPlane) accumulate(p.x, p.y, p.z);
    }

    // Near-plane intersections of the twelve box edges (corner indices differ
    // in exactly one bit). Only edges that cross the plane contribute.
    static const int kEdges[12][2] = {
        {0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3},
        {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7},
    };
    for (const auto& edge : kEdges) {
        const bedrocktools::sdk::Vec3& a = cs[edge[0]];
        const bedrocktools::sdk::Vec3& b = cs[edge[1]];
        const float da = a.z - kNearPlane;
        const float db = b.z - kNearPlane;
        if ((da >= 0.0f) == (db >= 0.0f)) continue; // both in front or both behind
        const float t = da / (da - db);
        accumulate(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, kNearPlane);
    }

    if (!any) return out;

    out.minX = minX;
    out.minY = minY;
    out.maxX = maxX;
    out.maxY = maxY;
    out.visible = true;
    return out;
}

// Center of a world box: the middle of the hitbox. The world-space tracer
// ends here -- inside the entity's own box -- so the game pins the line to
// the very wireframe it drew around the same AABB.
inline bedrocktools::sdk::Vec3 boxCenter(const bedrocktools::sdk::Vec3& boxMin,
                                         const bedrocktools::sdk::Vec3& boxMax) {
    return {(boxMin.x + boxMax.x) * 0.5f, (boxMin.y + boxMax.y) * 0.5f,
            (boxMin.z + boxMax.z) * 0.5f};
}

// Top-center of a world box: the head point of the hitbox. The nametag -- and
// the label column above the box -- is centered here instead of on the 2D
// box's top-middle, which is a screen-space average of eight projected
// corners that perspective shifts away from the head (sideways once off-axis,
// and above the head even when dead ahead, because the nearest top corner
// wins the min/max).
inline bedrocktools::sdk::Vec3 boxTopCenter(const bedrocktools::sdk::Vec3& boxMin,
                                            const bedrocktools::sdk::Vec3& boxMax) {
    return {(boxMin.x + boxMax.x) * 0.5f, boxMax.y,
            (boxMin.z + boxMax.z) * 0.5f};
}

// Projects that head anchor to surface coordinates. Returns false when the
// anchor is at or behind the near plane (an entity straddling the camera),
// in which case the caller falls back to the 2D box.
inline bool projectBoxTopCenter(const Camera& cam, const SurfaceProjection& proj,
                                const bedrocktools::sdk::Vec3& boxMin,
                                const bedrocktools::sdk::Vec3& boxMax,
                                float& outX, float& outY) {
    return project(cam, proj, boxTopCenter(boxMin, boxMax), outX, outY);
}

// ---------------------------------------------------------------------------
// The Crosshair tracer.
//
// A line that starts at the camera cannot be world-space geometry: every one
// of its points sits on the same view ray, so the game's projection collapses
// the whole segment onto a single pixel and the tracer simply disappears when
// Crosshair is selected (and the vertex at the eye sits behind the near plane,
// so a driver is free to clip the line away entirely). The
// origin is what makes it screen furniture, so it is placed on the surface
// like the nametags: from the exact middle of the screen to where the entity
// projects.
//
// Off-screen entities keep a snapline: the direction is what carries the
// information there, so the far end is pushed out to the surface's
// circumscribed radius -- never beyond it, which both keeps the coordinates
// finite for the launcher and keeps the direction untouched.
// ---------------------------------------------------------------------------
struct ScreenSegment {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    bool visible = false;
};

// `minPixels` is how long the line has to be before it is worth submitting:
// an entity sitting dead ahead has its hitbox under the crosshair already, and
// a one-pixel stub is noise (and a stray dot on the HUD).
inline ScreenSegment crosshairTracer(const Camera& cam, const SurfaceProjection& proj,
                                     const bedrocktools::sdk::Vec3& world,
                                     float minPixels = 1.5f) {
    ScreenSegment out;
    out.x0 = proj.width * 0.5f;
    out.y0 = proj.height * 0.5f;

    const bedrocktools::sdk::Vec3 d = {world.x - cam.pos.x, world.y - cam.pos.y,
                                       world.z - cam.pos.z};
    const float vx = dot(d, cam.right);
    const float vy = dot(d, cam.up);
    const float vz = dot(d, cam.forward);

    // Lateral offset per unit of depth, in "tan space" (the same basis the
    // projection above uses). In front of the camera that is the plain
    // perspective divide; at or behind it, the depth is clamped and the
    // lateral sign is kept, so an entity over your shoulder points at the
    // side you have to turn to instead of being mirrored across the screen.
    float dirX = 0.0f;
    float dirY = 0.0f;
    if (vz > kNearPlane) {
        dirX = vx / vz;
        dirY = vy / vz;
    } else {
        const float behind = std::max(-vz, 1.0f);
        dirX = vx / behind;
        dirY = vy / behind;
    }

    float dx = (dirX / (proj.tanHalfFov * proj.aspect)) * 0.5f * proj.width;
    float dy = -(dirY / proj.tanHalfFov) * 0.5f * proj.height; // y grows down
    if (!std::isfinite(dx) || !std::isfinite(dy)) return out;

    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > minPixels)) return out;

    const float radius =
        0.5f * std::sqrt(proj.width * proj.width + proj.height * proj.height);
    if (length > radius) {
        const float scale = radius / length;
        dx *= scale;
        dy *= scale;
    }

    out.x1 = out.x0 + dx;
    out.y1 = out.y0 + dy;
    out.visible = true;
    return out;
}

// ---------------------------------------------------------------------------
// Nametag text.
//
// The name is read straight out of the game's Player, and what lives in that
// field is not always what the game draws:
//
//   * servers, nick add-ons and right-to-left clients pad names with the
//     legacy section-sign markup ("§r", "§4§l"), which the game's font eats
//     and a plain HUD font paints as extra characters beside the name;
//   * control characters and the invisible format code points (soft hyphen,
//     zero-width spaces, the bidi overrides) sit in the same field;
//   * bytes that are not valid UTF-8 at all turn into replacement glyphs.
//
// sanitizeName() decodes the raw bytes as UTF-8 and keeps only what has a
// glyph of its own, so the label reads as the name and nothing else. It also
// enforces a length cap, because the launcher rejects an *entire* draw batch
// whose text exceeds its own limit -- one corrupt name would otherwise blank
// every nametag on screen.
// ---------------------------------------------------------------------------

// Bedrock usernames are 16 characters, so this only has to be far enough away
// from the launcher's limit to never be the thing that hides the overlay.
inline constexpr std::size_t kMaxNameCodePoints = 48;

// One UTF-8 code point at `index`, strict: overlong forms, surrogates, values
// past U+10FFFF and truncated sequences are rejected and leave `index` alone
// so the caller can skip the single offending byte.
inline bool decodeCodePoint(std::string_view text, std::size_t& index,
                            std::uint32_t& outCp) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);

    std::uint32_t cp = 0;
    std::size_t extra = 0;
    if (lead < 0x80) {
        outCp = lead;
        index += 1;
        return true;
    } else if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) {
        cp = lead & 0x1Fu;
        extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
        cp = lead & 0x0Fu;
        extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
        cp = lead & 0x07u;
        extra = 3;
    } else {
        return false; // a continuation byte or 0xF5.. as a lead
    }

    if (index + extra >= text.size()) return false;
    for (std::size_t i = 1; i <= extra; ++i) {
        const unsigned char next = static_cast<unsigned char>(text[index + i]);
        if ((next & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (next & 0x3Fu);
    }
    if (extra == 2 && cp < 0x800) return false;                    // overlong
    if (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return false; // out of range
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;                  // surrogate

    index += extra + 1;
    outCp = cp;
    return true;
}

inline void appendCodePoint(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// The character a section-sign markup code may be. The documented set is
// 0-9, a-o, k-o and r in either case; every alphanumeric is swallowed here,
// because a username that really contains "§x" does not exist while a client
// that emits a code this table does not know does.
inline bool isMarkupCodeByte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool isSpaceCodePoint(std::uint32_t cp) {
    return cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C ||
           cp == 0x0D || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x205F || cp == 0x3000;
}

// Code points that carry no glyph of their own: the zero-width joiners and
// spaces, the bidi overrides and isolates, soft hyphen, BOM and the C1
// controls. A name does not need them and the HUD font has nothing to draw,
// which is exactly what looks like "extra characters" after the name.
inline bool isInvisibleCodePoint(std::uint32_t cp) {
    if (cp <= 0x20) return true;
    if (cp >= 0x7F && cp <= 0x9F) return true;
    if (cp == 0xAD || cp == 0x61C || cp == 0x180E || cp == 0xFEFF) return true;
    if (cp >= 0x200B && cp <= 0x200F) return true;
    if (cp >= 0x202A && cp <= 0x202E) return true;
    if (cp >= 0x2060 && cp <= 0x2064) return true;
    if (cp >= 0x2066 && cp <= 0x2069) return true;
    return false;
}

inline std::string sanitizeName(std::string_view raw,
                                std::size_t maxCodePoints = kMaxNameCodePoints) {
    std::string out;
    out.reserve(raw.size());

    std::size_t index = 0;
    std::size_t codePoints = 0;
    bool pendingSpace = false;

    while (index < raw.size() && codePoints < maxCodePoints) {
        const unsigned char lead = static_cast<unsigned char>(raw[index]);

        // Legacy markup: "§" as UTF-8 (0xC2 0xA7) or as the single latin-1
        // byte some builds store, each followed by its code character.
        if (lead == 0xA7 || (lead == 0xC2 && index + 1 < raw.size() &&
                             static_cast<unsigned char>(raw[index + 1]) == 0xA7)) {
            // One section sign, in either encoding the field may hold it in.
            auto signAt = [&](std::size_t at) {
                if (at >= raw.size()) return std::size_t{0};
                const unsigned char c = static_cast<unsigned char>(raw[at]);
                if (c == 0xA7) return std::size_t{1};
                if (c == 0xC2 && at + 1 < raw.size() &&
                    static_cast<unsigned char>(raw[at + 1]) == 0xA7) {
                    return std::size_t{2};
                }
                return std::size_t{0};
            };
            const std::size_t after = index + (lead == 0xA7 ? 1 : 2);

            // "§§" is the game's own escape for a literal section sign: the
            // pair draws one "§" and is not a markup code. Eating the first and
            // leaving the second -- which is what a naive strip does, because a
            // sign is not an alphanumeric code character -- is what puts a
            // stray "§" in front of a renamed player's name.
            const std::size_t escaped = signAt(after);
            if (escaped != 0) {
                index = after + escaped;
                if (pendingSpace && !out.empty()) {
                    out.push_back(' ');
                    ++codePoints;
                }
                pendingSpace = false;
                appendCodePoint(out, 0xA7);
                ++codePoints;
                continue;
            }

            index = after;
            if (index < raw.size() &&
                isMarkupCodeByte(static_cast<unsigned char>(raw[index]))) {
                ++index;
            }
            continue;
        }

        std::uint32_t cp = 0;
        if (!decodeCodePoint(raw, index, cp)) {
            ++index; // an invalid byte: drop it, never hand it to the font
            continue;
        }

        if (isSpaceCodePoint(cp)) {
            // Whitespace runs collapse to a single space, and a name padded
            // with newlines (a real nametag trick) stops pushing the label
            // around. The pending space is only written once something follows
            // it, which trims both ends at the same time.
            pendingSpace = true;
            continue;
        }
        if (isInvisibleCodePoint(cp)) continue;

        if (pendingSpace && !out.empty()) {
            out.push_back(' ');
            ++codePoints;
        }
        pendingSpace = false;

        appendCodePoint(out, cp);
        ++codePoints;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Which font draws the label, and how wide it comes out
//
// The launcher draws module text with one of two faces: the packaged
// resources/minecraft.ttf it registers under the id "minecraft" (the game's
// own pixel font), or its default UI font, which is what a module asks for by
// leaving fontId empty. Which of the two it is decides both questions a label
// has to answer:
//
//   * can the name even be drawn? The pixel font ships 722 glyphs -- Latin,
//     Greek, Cyrillic and a little punctuation -- and Arabic, Hebrew, CJK, Thai
//     and emoji are not among them. A missing glyph comes out as a replacement
//     box, which is the other way a nametag grows characters that are not in
//     the name; the launcher's own font has those glyphs and, unlike the pixel
//     font, shapes right-to-left text;
//   * how wide is it? A label is centered on its measured width, and a width is
//     only knowable for a font whose metrics the module can read. For the pixel
//     font those metrics are the hmtx values of the TTF this repository ships,
//     measured rather than guessed, so a name in the game's own character set
//     lands dead center on the head.
//
// The font is monospaced -- 1152 units (0.75 of its 1536-unit em) for every
// ordinary glyph -- except for a handful of narrow cells (384, 576, 768, 960)
// and two that overflow it (1344). How the launcher turns a text command's
// `size` into an em is not documented, so that part is not rederived here: one
// full cell keeps the 0.6 the module has always charged for a normal glyph, and
// only the *ratio* between the glyphs comes from the font. That leaves how big
// a label looks exactly as it was, and fixes where its middle ends up -- an 'i'
// used to be billed 0.32 and a 'W' 0.92, while the font gives them 1/3 and 1
// of the same cell -- so "WiiiW" came out half again too wide, and a label
// centered on that number landed beside the head rather than above it.
// ---------------------------------------------------------------------------

// The two faces a label can be drawn with.
enum class LabelFont {
    Pixel,  // "minecraft": the packaged pixel font
    System, // empty fontId: the launcher default, which shapes RTL and CJK
};

// Which fontId string each of the two is -- the pixel font's own id, and the
// empty id that asks for the launcher default -- is a property of the draw
// command, so the module maps them (see addText in esp.cpp) rather than this
// header, which stays free of launcher constants.

// True when every byte of an already-sanitized string is printable ASCII, the
// range the pixel font is known to cover in full (the file has gaps in Latin-1
// and beyond, and a gap in the middle of a name is exactly the failure this
// exists to avoid). Bedrock usernames are [0-9A-Za-z_], so this covers every
// name the game can carry by itself; a server that renames a player into
// another script falls through to the launcher's font.
inline bool pixelFontCovers(std::string_view text) {
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7E) return false;
    }
    return true;
}

// The face to draw `text` with: the pixel font when it is registered and the
// text fits inside it, the launcher's default otherwise.
inline LabelFont labelFontFor(std::string_view text, bool pixelFontAvailable) {
    return pixelFontAvailable && pixelFontCovers(text) ? LabelFont::Pixel
                                                       : LabelFont::System;
}

// Advance of one code point of the pixel font in cells, where 1.0 is the
// 0.75 em every ordinary glyph occupies. The sets are the font's own, read out
// of the hmtx table of resources/minecraft.ttf.
inline float pixelFontCell(std::uint32_t cp) {
    switch (cp) {
        case ' ': case '!': case '\'': case ',': case '.': case ':': case ';':
        case 'i': case '|':
            return 1.0f / 3.0f; // 384 units
        case '`': case 'l':
            return 0.5f;        // 576
        case '"': case 'I': case '[': case ']': case 't':
            return 2.0f / 3.0f; // 768
        case '(': case ')': case '*': case '<': case '>': case 'f': case 'k':
        case '{': case '}':
            return 5.0f / 6.0f; // 960
        case '@': case '~':
            return 7.0f / 6.0f; // 1344
        default:
            return 1.0f;        // 1152: letters, digits, the rest of the ASCII
    }
}

// Advance of one code point, in multiples of the pixel size. The system face is
// proportional and its metrics cannot be read from here, so it keeps an
// estimate -- a normal glyph 0.6, the thin strokes about half that, CJK a full
// em -- while the pixel font is measured on its real cells. Combining marks
// ride on the previous glyph and take no room in either.
inline float codePointAdvance(std::uint32_t cp, LabelFont font) {
    if (font == LabelFont::Pixel) {
        // sanitizeName() drops what has no glyph; the guard is for a caller
        // that hands this something it never sanitized.
        if (cp < 0x20) return 0.0f;
        return pixelFontCell(cp) * 0.60f;
    }

    if (cp == 0x20) return 0.30f;
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return 0.0f;
    if (cp <= 0x7F) {
        switch (cp) {
            case 'M': case 'W': case 'm': case 'w': return 0.92f;
            case 'i': case 'j': case 'l': case 'I': case '.': case ',':
            case ':': case ';': case '\'': case '`': case '|': case '!':
            case '[': case ']': case '(': case ')': case '{': case '}':
            case '/': case '\\': case 't': case 'f': return 0.32f;
            default: return 0.60f;
        }
    }
    if ((cp >= 0x0300 && cp <= 0x036F) ||   // combining diacriticals
        (cp >= 0x20D0 && cp <= 0x20FF)) {
        return 0.0f;
    }
    // Full-width East Asian blocks, then everything else in a normal cell.
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6)) {
        return 1.0f;
    }
    return 0.60f;
}

// Width of an already-sanitized string in surface pixels, measured in code
// points of the font it will be drawn with. Counting bytes instead (the bug
// this replaces) doubles the width of every two-byte name, which slid Arabic
// names half a label to the left of the head they are supposed to sit above.
inline float measureTextWidth(std::string_view text, float pixelSize,
                              LabelFont font) {
    std::size_t index = 0;
    float advance = 0.0f;
    while (index < text.size()) {
        std::uint32_t cp = 0;
        if (!decodeCodePoint(text, index, cp)) {
            ++index;
            continue;
        }
        advance += codePointAdvance(cp, font);
    }
    return advance * pixelSize;
}

// The estimate for a label whose font is not settled here, i.e. for nothing
// that centers on the result.
inline float measureTextWidth(std::string_view text, float pixelSize) {
    return measureTextWidth(text, pixelSize, LabelFont::System);
}

// ---------------------------------------------------------------------------
// World-space geometry (the half that must never drift).
// ---------------------------------------------------------------------------
namespace world {

using bedrocktools::sdk::Vec3;
using overlay::Quad;
using overlay::Segment;

// Corner indices of an AABB, bit 0 = +X, bit 1 = +Y, bit 2 = +Z.
inline Vec3 cornerOf(const bedrocktools::sdk::AABB& box, int index) {
    return {(index & 1) ? box.max.x : box.min.x,
            (index & 2) ? box.max.y : box.min.y,
            (index & 4) ? box.max.z : box.min.z};
}

// Corner pairs of the twelve box edges (each pair differs in exactly one bit).
inline constexpr int kBoxEdges[12][2] = {
    {0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3},
    {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7},
};

// The twelve edges of an axis-aligned box, i.e. the 3D wireframe the Hitbox
// module draws.
inline void addBoxEdges(std::vector<Segment>& out, const bedrocktools::sdk::AABB& box) {
    for (const auto& edge : kBoxEdges) {
        out.push_back({cornerOf(box, edge[0]), cornerOf(box, edge[1])});
    }
}

// The same twelve edges trimmed to corner brackets: every edge keeps a piece
// of the same world length at both ends, so the brackets look even on a tall
// player box instead of turning into two long stubs.
inline void addBoxCorners(std::vector<Segment>& out,
                          const bedrocktools::sdk::AABB& box,
                          float fraction) {
    const float extentX = box.max.x - box.min.x;
    const float extentY = box.max.y - box.min.y;
    const float extentZ = box.max.z - box.min.z;
    const float shortest = std::min(std::min(extentX, extentY), extentZ);
    const float requested = shortest * std::clamp(fraction, 0.02f, 0.5f);

    for (const auto& edge : kBoxEdges) {
        const Vec3 from = cornerOf(box, edge[0]);
        const Vec3 to = cornerOf(box, edge[1]);
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float dz = to.z - from.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 1e-5f) continue;

        const float trim = std::min(requested, length * 0.5f);
        const float ux = dx / length * trim;
        const float uy = dy / length * trim;
        const float uz = dz / length * trim;

        out.push_back({from, {from.x + ux, from.y + uy, from.z + uz}});
        out.push_back({{to.x - ux, to.y - uy, to.z - uz}, to});
    }
}

// The six faces of the box, for a translucent fill. Each quad is emitted with
// both windings downstream, so the faces read from either side.
inline void addBoxFaces(std::vector<Quad>& out, const bedrocktools::sdk::AABB& box) {
    static constexpr int kFaces[6][4] = {
        {0, 1, 3, 2}, // -Z
        {4, 6, 7, 5}, // +Z
        {0, 2, 6, 4}, // -X
        {1, 5, 7, 3}, // +X
        {0, 4, 5, 1}, // -Y
        {2, 3, 7, 6}, // +Y
    };
    for (const auto& face : kFaces) {
        Quad quad{};
        for (int i = 0; i < 4; ++i) quad.corners[i] = cornerOf(box, face[i]);
        out.push_back(quad);
    }
}

// ---------------------------------------------------------------------------
// World-space billboard text (the distance readout).
//
// The HUD layer can only place text through the module's own projection of
// the world, and that projection disagrees with the game's real camera
// whenever the view is moving (sprint FOV, view bob, a frame of look
// latency, aspect handling) -- which reads as the readout sliding off the
// hitbox. Drawing the readout as world-space quads instead anchors it to the
// entity's feet in the very pass the hitbox is drawn in, so it cannot move
// relative to the box. The projection is still consulted, but only for the
// text's *size*: an FOV that is off by ten percent makes the digits ten
// percent too large, never ten percent off the hitbox.
// ---------------------------------------------------------------------------

// One filled rectangle of a glyph, in a 5-row cell with y pointing down.
struct GlyphRect {
    float x, y, w, h;
};

// A glyph: merged rectangles plus the width of its cell. The digits live in a
// 3x5 cell, 'm' in a 5x5 one and '.' occupies a 1-wide strip, all on the same
// five-row baseline. Rectangles deliberately overlap at the corners, which
// keeps every glyph at three to five quads.
struct Glyph {
    const GlyphRect* rects;
    int rectCount;
    float width;
};

inline constexpr GlyphRect kGlyph0[] = {{0, 0, 3, 1}, {0, 4, 3, 1}, {0, 0, 1, 5}, {2, 0, 1, 5}};
inline constexpr GlyphRect kGlyph1[] = {{1, 0, 1, 5}, {0, 4, 3, 1}};
inline constexpr GlyphRect kGlyph2[] = {{0, 0, 3, 1}, {2, 1, 1, 1}, {0, 2, 3, 1}, {0, 3, 1, 1}, {0, 4, 3, 1}};
inline constexpr GlyphRect kGlyph3[] = {{0, 0, 3, 1}, {2, 0, 1, 5}, {0, 2, 3, 1}, {0, 4, 3, 1}};
inline constexpr GlyphRect kGlyph4[] = {{0, 0, 1, 3}, {2, 0, 1, 5}, {0, 2, 3, 1}};
inline constexpr GlyphRect kGlyph5[] = {{0, 0, 3, 1}, {0, 1, 1, 1}, {0, 2, 3, 1}, {2, 3, 1, 1}, {0, 4, 3, 1}};
inline constexpr GlyphRect kGlyph6[] = {{0, 0, 3, 1}, {0, 0, 1, 5}, {0, 2, 3, 1}, {2, 3, 1, 1}, {0, 4, 3, 1}};
inline constexpr GlyphRect kGlyph7[] = {{0, 0, 3, 1}, {2, 1, 1, 4}};
inline constexpr GlyphRect kGlyph8[] = {{0, 0, 3, 1}, {0, 4, 3, 1}, {0, 0, 1, 5}, {2, 0, 1, 5}, {0, 2, 3, 1}};
inline constexpr GlyphRect kGlyph9[] = {{0, 0, 3, 1}, {0, 0, 1, 3}, {2, 0, 1, 5}, {0, 2, 3, 1}, {0, 4, 3, 1}};
inline constexpr GlyphRect kGlyphDot[] = {{0, 3, 1, 2}};
inline constexpr GlyphRect kGlyphM[] = {{0, 1, 1, 4}, {2, 1, 1, 4}, {4, 1, 1, 4}, {0, 1, 5, 1}};

// The characters "%.1fm" can produce. Anything else (a '-' cannot appear: the
// distance is non-negative) is skipped together with its spacing.
inline bool glyphFor(char c, Glyph& out) {
    switch (c) {
        case '0': out = {kGlyph0, 4, 3.0f}; return true;
        case '1': out = {kGlyph1, 2, 3.0f}; return true;
        case '2': out = {kGlyph2, 5, 3.0f}; return true;
        case '3': out = {kGlyph3, 4, 3.0f}; return true;
        case '4': out = {kGlyph4, 3, 3.0f}; return true;
        case '5': out = {kGlyph5, 5, 3.0f}; return true;
        case '6': out = {kGlyph6, 5, 3.0f}; return true;
        case '7': out = {kGlyph7, 2, 3.0f}; return true;
        case '8': out = {kGlyph8, 5, 3.0f}; return true;
        case '9': out = {kGlyph9, 5, 3.0f}; return true;
        case '.': out = {kGlyphDot, 1, 1.0f}; return true;
        case 'm': out = {kGlyphM, 4, 5.0f}; return true;
        default: return false;
    }
}

// Minimum camera-space depth a billboard is still drawn at. Closer than this
// the anchor is at (or nearly at) the near plane and the entity fills the
// view anyway.
inline constexpr float kBillboardMinDepth = 0.25f;

// Builds the quads of one line of billboarded text.
//
//   * `anchor` is the *top-center* of the text block: the block hangs below
//     and spreads left/right of it, so a caller can place it just under an
//     entity's feet.
//   * `pixelHeight` is the wanted on-screen height in HUD surface pixels.
//     The camera-space depth of the anchor converts it to a world size, so
//     the text keeps its apparent size at any range; only this size consults
//     the projection -- the anchor itself never does.
//   * The quads face the camera (they live in the camera's right/up plane),
//     so the game's own transform puts them flat on the screen.
//
// Returns false when there is nothing to draw: empty text, no representable
// glyph, or an anchor at/behind the camera.
inline bool addBillboardText(std::vector<Quad>& out, const Camera& cam,
                             const SurfaceProjection& proj, const Vec3& anchor,
                             const char* text, float pixelHeight) {
    if (!text || text[0] == '\0') return false;

    const Vec3 toAnchor{anchor.x - cam.pos.x, anchor.y - cam.pos.y,
                        anchor.z - cam.pos.z};
    const float depth = dot(toAnchor, cam.forward);
    if (depth <= kBillboardMinDepth) return false;

    // World size of one HUD pixel at the anchor's depth, then of one font
    // cell unit (the glyphs are five units tall).
    const float wantedHeight = std::clamp(pixelHeight, 4.0f, 64.0f);
    const float worldPerPixel = 2.0f * proj.tanHalfFov * depth / proj.height;
    const float unit = wantedHeight * 0.2f * worldPerPixel;

    // Measure the line in font units (one unit of spacing between glyphs).
    float advance = 0.0f;
    int glyphs = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        Glyph glyph{};
        if (!glyphFor(*p, glyph)) continue;
        advance += glyph.width + 1.0f;
        ++glyphs;
    }
    if (glyphs == 0) return false;
    advance -= 1.0f; // the last glyph does not need trailing spacing

    // Top-left corner of the block, in world space: centered on the anchor
    // along the camera's right vector, top edge at the anchor itself.
    const float halfWidth = advance * unit * 0.5f;
    const Vec3 origin{anchor.x - cam.right.x * halfWidth,
                      anchor.y - cam.right.y * halfWidth,
                      anchor.z - cam.right.z * halfWidth};

    float pen = 0.0f;
    for (const char* p = text; *p != '\0'; ++p) {
        Glyph glyph{};
        if (!glyphFor(*p, glyph)) continue;
        for (int i = 0; i < glyph.rectCount; ++i) {
            const GlyphRect& r = glyph.rects[i];
            const float x0 = (pen + r.x) * unit;
            const float x1 = (pen + r.x + r.w) * unit;
            const float y0 = r.y * unit;
            const float y1 = (r.y + r.h) * unit;

            Quad quad{};
            quad.corners[0] = {origin.x + cam.right.x * x0 - cam.up.x * y0,
                               origin.y + cam.right.y * x0 - cam.up.y * y0,
                               origin.z + cam.right.z * x0 - cam.up.z * y0};
            quad.corners[1] = {origin.x + cam.right.x * x1 - cam.up.x * y0,
                               origin.y + cam.right.y * x1 - cam.up.y * y0,
                               origin.z + cam.right.z * x1 - cam.up.z * y0};
            quad.corners[2] = {origin.x + cam.right.x * x1 - cam.up.x * y1,
                               origin.y + cam.right.y * x1 - cam.up.y * y1,
                               origin.z + cam.right.z * x1 - cam.up.z * y1};
            quad.corners[3] = {origin.x + cam.right.x * x0 - cam.up.x * y1,
                               origin.y + cam.right.y * x0 - cam.up.y * y1,
                               origin.z + cam.right.z * x0 - cam.up.z * y1};
            out.push_back(quad);
        }
        pen += glyph.width + 1.0f;
    }
    return true;
}

} // namespace world

} // namespace esp
