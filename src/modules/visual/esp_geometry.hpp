#pragma once

// Geometry for the Esp overlay, in its two halves:
//
//   * esp::world builds everything that has to sit *on* an entity as
//     *world-space* primitives, which the game then transforms with the
//     matrices it rendered the level with (see overlay_mesh.hpp): the box
//     outline, both tracers, and the label column -- the nametag, the health
//     value and bar and the distance readout, spelled out as billboarded quads
//     from the packaged pixel face (see esp_pixel_font.hpp). Nothing in that
//     half can drift, because the module never has to model the camera for it:
//     a tracer ends inside the hitbox, and a label's size -- never its position
//     -- is all the projection is asked for.
//   * esp::computeCamera / makeProjection / project / projectBox are the
//     camera basis + perspective projection for the launcher HUD layer. That
//     projection is only ever as good as the module's model of the camera, which
//     is exactly why nothing that has to sit on an entity is positioned by it
//     any more; what is left on it is the labels this pass cannot build (a name
//     written in a script the pixel face has no cells for) and the Crosshair
//     snapline for an entity behind the camera, which has no pixels for
//     geometry to be pinned to (see esp::crosshairTracer).
//   * esp::sanitizeName / measureTextWidth prepare the nametag string for the
//     label paths: the raw name field holds the markup codes and invisible
//     format characters the game's own font swallows (which a plain HUD font
//     paints as extra characters), and the label is centered on a width measured
//     in glyphs, never in bytes -- a UTF-8 name is two to four bytes per
//     character, which is what used to shove Arabic names off the head. The
//     cleanup feeds both paths -- the mesh spells the same cleaned name out of
//     the generated face -- while the measurement is HUD-specific, because that
//     pass has a font to measure against and the mesh one is its own font.
//     A width only means something for the font that draws the text,
//     so esp::labelFontFor also decides which one that is: the packaged pixel
//     font for the names it carries (its metrics are known exactly), the
//     launcher's default for the scripts it does not have.
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

#include "esp_pixel_font.hpp"
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
// The Crosshair snapline: the screen-space half of that origin, for the one
// case the geometry half cannot reach.
//
// esp::world::addCrosshairTracer draws the line from the crosshair to an entity
// in front of the camera as geometry, so the game pins its far end to the
// hitbox. What is left for this half is the case geometry cannot reach: an
// entity at or behind the eye has no pixels for anything to be pinned to, and a
// line on the surface is the only way to still point at the way to turn. It
// starts at the middle of the surface (a screen coordinate, so the module's
// camera model cannot move it) and heads for where the entity would project.
//
// The direction is the whole message here, so the far end is pushed out to the
// surface's circumscribed radius -- never beyond it, which both keeps the
// coordinates finite for the launcher and keeps the direction untouched.
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
// World-space billboarded labels: the nametag, the health value, the health bar
// and the distance readout.
//
// The HUD layer can only place a label through the module's own projection of
// the world, and that projection disagrees with the game's real camera whenever
// the view is moving (sprint FOV, view bob, a frame of look latency, aspect
// handling) -- which reads as the label sliding off the hitbox. Emitting the
// label as quads instead anchors it to the entity in the very pass the hitbox is
// drawn in, so it cannot move relative to the box: the only thing the projection
// still decides is the label's *size*, and an FOV that is off by ten percent
// then makes it ten percent too large rather than ten percent off the head.
//
// Text in this pass has to be geometry, so the glyphs are filled rectangles from
// the game's own pixel face -- generated out of the packaged TTF, for both
// reasons that matter: the overlay spells a name the way the game does, and the
// module can *center* it, which is only possible for a font whose cells it knows
// (see esp_pixel_font.hpp). A name that face cannot spell (Arabic, CJK, a
// leftover markup sign) has no geometry to be built from, and the caller keeps
// that label on the launcher's font, where it is at least the right characters.
// ---------------------------------------------------------------------------

// Minimum camera-space depth a billboard is still drawn at. Closer than this
// the anchor is at (or nearly at) the near plane and the entity fills the
// view anyway.
inline constexpr float kBillboardMinDepth = 0.25f;

// A line longer than this is not worth spelling out as quads: the vertex count
// of a label is what this pass pays for, and a name that long is either a
// renamed player (Bedrock's own usernames stop at 16) or a server's idea of
// art, neither of which is worth more wire than the hitbox it labels. The HUD
// path has no such cost, so that is where a longer name goes.
inline constexpr std::size_t kMaxBillboardGlyphs = 24;

// True when this face can draw every character of `text` and the line fits the
// budget above. Deliberately the same coverage test the HUD path uses to pick
// the pixel font, so a label is only ever in one of the two passes.
inline bool billboardTextFits(std::string_view text) {
    // Coverage first: what survives it is ASCII, and an ASCII name is one byte
    // per glyph, so the length below can be counted in bytes.
    return !text.empty() && pixelFontCovers(text) &&
           text.size() <= kMaxBillboardGlyphs;
}

// The world size of one HUD surface pixel at `depth`: the conversion every
// billboard is sized by, which is what keeps a label at a constant apparent
// size while its anchor stays a world point.
inline float worldPerPixelAtDepth(const SurfaceProjection& proj, float depth) {
    return 2.0f * proj.tanHalfFov * depth / proj.height;
}

// One label's worth of a plane: the world point everything hangs on, and the
// pixel scale at its depth. Every element of the column is then placed in
// *surface* pixels through `point`, which is what lets the nametag, the health
// value and the bar keep the layout the HUD column has while none of them is
// positioned by the projection: the offsets are pixel arithmetic around an
// anchor the game placed.
struct Billboard {
    Vec3 anchor;
    Vec3 right;
    Vec3 up;
    // World units per surface pixel, or 0 for a frame nothing can be drawn on.
    float scale = 0.0f;

    bool valid() const noexcept { return scale > 0.0f && std::isfinite(scale); }

    // The world point `rightPx` to the right and `downPx` below the anchor.
    // y grows downwards, exactly as it does in the HUD surface, so a caller can
    // reuse the pixel numbers it already has.
    Vec3 point(float rightPx, float downPx) const {
        const float rx = rightPx * scale;
        const float ry = downPx * scale;
        return {anchor.x + right.x * rx - up.x * ry,
                anchor.y + right.y * rx - up.y * ry,
                anchor.z + right.z * rx - up.z * ry};
    }

    // One filled rectangle of the plane: `leftPx`/`topPx` is its top-left corner
    // relative to the anchor and the box grows right and down. Emitted with the
    // label's own color only in the sense that the batch decides that -- see the
    // grouped flush in overlay::Mesh, which is how one mesh carries a black bar
    // and a green, yellow or red fill.
    void addBox(std::vector<Quad>& out, float leftPx, float topPx, float widthPx,
                float heightPx) const {
        if (!valid() || widthPx <= 0.0f || heightPx <= 0.0f) return;
        Quad box{};
        box.corners[0] = point(leftPx, topPx);
        box.corners[1] = point(leftPx + widthPx, topPx);
        box.corners[2] = point(leftPx + widthPx, topPx + heightPx);
        box.corners[3] = point(leftPx, topPx + heightPx);
        out.push_back(box);
    }

    // The same plane with its anchor moved by a pixel offset: what a line that has
    // to start at another line's edge (a health value over the left end of its
    // bar) is positioned on, so the column stays one frame's arithmetic instead
    // of becoming a second set of coordinates that can disagree.
    Billboard shifted(float rightPx, float downPx) const {
        Billboard out = *this;
        out.anchor = point(rightPx, downPx);
        return out;
    }
};

// Resolves the frame for `anchor`. Returns false -- and leaves `out` unusable --
// when the anchor sits at or behind the camera, which is the caller's cue to put
// that entity's labels on the HUD instead of drawing half a column.
inline bool makeBillboard(Billboard& out, const Camera& cam, const SurfaceProjection& proj,
                          const Vec3& anchor) {
    out = Billboard{anchor, cam.right, cam.up, 0.0f};
    const Vec3 toAnchor{anchor.x - cam.pos.x, anchor.y - cam.pos.y,
                        anchor.z - cam.pos.z};
    const float depth = dot(toAnchor, cam.forward);
    if (!(depth > kBillboardMinDepth)) return false;
    out.scale = worldPerPixelAtDepth(proj, depth);
    return out.valid();
}

// Where a line sits on the anchor's column: centered on it, which is what a name
// over a head wants, or starting at it and growing to the right, which is how the
// health value sits over the left end of its bar.
enum class TextAlignment {
    Center,
    Left,
};

// Builds one line of billboarded text.
//
//   * the line is centered on the anchor's column (see TextAlignment) and its
//     *top* row sits at the anchor, hanging down from it -- so the caller puts it
//     just under an entity's feet (the readout) or a whole label height above its
//     head (a nametag, whose anchor is the head point minus that height);
//   * `pixelHeight` is the em in surface pixels, the unit the HUD text commands
//     are quoted in, and the face's cap height is kPixelCapHeight of its
//     kPixelEm rows -- which is what makes the two paths look like one font;
//   * advances come from the font, so an 'i' costs a third of an 'M' here just
//     as it does in the game's own nameplate.
//
// Returns false when there is nothing to draw: no valid frame, empty text, or
// not one character this face can spell.
inline bool addBillboardText(std::vector<Quad>& out, const Billboard& billboard,
                             std::string_view text, float pixelHeight,
                             TextAlignment align = TextAlignment::Center) {
    if (!billboard.valid() || text.empty()) return false;

    const float em = std::clamp(pixelHeight, 4.0f, 64.0f) / static_cast<float>(kPixelEm);
    if (!(em > 0.0f)) return false;

    // Measure first: the block has to be centered, and a center is only
    // knowable from the advances of the glyphs that will actually be drawn.
    float advance = 0.0f;
    int glyphs = 0;
    for (const char c : text) {
        const PixelGlyph* glyph = pixelGlyph(static_cast<unsigned char>(c));
        if (!glyph) continue;
        advance += static_cast<float>(glyph->advance);
        ++glyphs;
    }
    if (glyphs == 0) return false;

    const float penStart = align == TextAlignment::Center ? -advance * 0.5f * em : 0.0f;
    float pen = penStart;
    for (const char c : text) {
        const PixelGlyph* glyph = pixelGlyph(static_cast<unsigned char>(c));
        if (!glyph) continue;
        for (std::size_t i = 0; i < glyph->rectCount; ++i) {
            const PixelRect& rect = kPixelRects[glyph->firstRect + i];
            billboard.addBox(out,
                             pen + static_cast<float>(rect.x) * em,
                             static_cast<float>(rect.y) * em,
                             static_cast<float>(rect.width) * em,
                             static_cast<float>(rect.height) * em);
        }
        pen += static_cast<float>(glyph->advance) * em;
    }
    return true;
}

// The hitbox's own extent along the camera's right axis, in surface pixels: the
// width a health bar has to be to match the wireframe it sits on, whatever way
// the entity is turned. The projected 2D box says the same thing, but only at
// the moment the module's camera model agrees with the game's -- and that is the
// disagreement this whole half exists to avoid.
inline float boxPixelWidth(const Camera& cam, const bedrocktools::sdk::AABB& box,
                           float scale) {
    if (!(scale > 0.0f)) return 0.0f;
    float lo = 1e30f;
    float hi = -1e30f;
    for (int i = 0; i < 4; ++i) {
        // The right axis is horizontal by construction (see computeCamera), so
        // the four side corners are all the box has to offer this axis.
        const float x = (i & 1) ? box.max.x : box.min.x;
        const float z = (i & 2) ? box.max.z : box.min.z;
        const float along = x * cam.right.x + z * cam.right.z;
        lo = std::min(lo, along);
        hi = std::max(hi, along);
    }
    return (hi - lo) / scale;
}

// ---------------------------------------------------------------------------
// The Crosshair tracer, as geometry.
//
// A line that *starts at the eye* is not something the game can draw: all of it
// lies on one view ray, so the projection collapses it onto a single pixel --
// which is the "selecting Crosshair removes the tracer" this module used to
// have. A line that starts on the view *axis*, a short way in front of the eye,
// is: every point of the axis projects to the middle of the screen, so the
// segment from an axis point to the entity lands on screen exactly as the line
// from the crosshair to that entity -- and both of its ends are then placed by
// the matrices the game rendered the level with. That is the difference that
// matters: the HUD version's far end sat where the module's model of the camera
// said the hitbox was, and it is that model -- the Fov slider, sprint FOV, a
// frame of look latency -- that moved.
//
// The start depth is free (a point on the axis projects to the screen center at
// any depth), so it is pushed out only as far as it has to be: past the game's
// near-plane clip, and close enough to the eye to leave the entity's own depth
// for the rest of the segment. `minPixels` keeps the rule the snapline has, from
// the same arithmetic, so an entity already under the crosshair never gets a
// one-pixel stub and the two paths never both draw the same line.
//
// Returns false when there is nothing to hand the game -- the entity is at or
// behind the camera, or it is dead ahead, or its box is so close that no segment
// fits in front of it -- and the caller keeps the screen-space snapline for the
// first case (see esp::crosshairTracer), which is the only one where a line on
// the surface carries information the geometry cannot: an entity over your
// shoulder has no pixels to be pinned to.
// ---------------------------------------------------------------------------

// How far in front of the eye the axis vertex goes, and how much of the segment
// has to be left over for it to be a line rather than a rounding error.
inline constexpr float kCrosshairTracerStartDepth = 0.5f;
inline constexpr float kCrosshairTracerMinSpan = 0.25f;

inline bool addCrosshairTracer(std::vector<Segment>& out, const Camera& cam,
                               const SurfaceProjection& proj, const Vec3& target,
                               float minPixels = 1.5f) {
    const Vec3 d{target.x - cam.pos.x, target.y - cam.pos.y, target.z - cam.pos.z};
    const float depth = dot(d, cam.forward);
    if (!(depth > kNearPlane)) return false; // at or behind the eye: snapline territory

    // The same "tan space" measure esp::crosshairTracer divides by, so the two
    // agree about what counts as dead ahead.
    const float offX = (dot(d, cam.right) / depth) / (proj.tanHalfFov * proj.aspect) *
                       0.5f * proj.width;
    const float offY = -(dot(d, cam.up) / depth) / proj.tanHalfFov * 0.5f * proj.height;
    if (!std::isfinite(offX) || !std::isfinite(offY)) return false;
    if (!(std::sqrt(offX * offX + offY * offY) > minPixels)) return false;

    const float start = std::min(kCrosshairTracerStartDepth, depth - kCrosshairTracerMinSpan);
    if (!(start > kNearPlane)) return false;

    Vec3 from{cam.pos.x + cam.forward.x * start, cam.pos.y + cam.forward.y * start,
              cam.pos.z + cam.forward.z * start};
    if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z)) {
        return false;
    }
    out.push_back({from, target});
    return true;
}

} // namespace world

} // namespace esp
