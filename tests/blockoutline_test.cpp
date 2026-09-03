#include "modules/visual/blockoutline_color.hpp"
#include "modules/visual/blockoutline_geometry.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using bedrocktools::modules::blockoutline::Point;
using bedrocktools::modules::blockoutline::Quad;
using bedrocktools::modules::blockoutline::makeBox;
using bedrocktools::modules::blockoutline::makeEdgeVisibility;
using bedrocktools::modules::blockoutline::makeFaces;
using bedrocktools::modules::blockoutline::makeThickFrame;

int main() {
    constexpr auto box = makeBox(10.0f, -2.0f, 4.0f, 0.0f);
    static_assert(box.size() == 12);

    // Every cube corner must be touched by exactly three edges.
    constexpr Point corners[] = {
        {10, -2, 4}, {11, -2, 4}, {10, -1, 4}, {11, -1, 4},
        {10, -2, 5}, {11, -2, 5}, {10, -1, 5}, {11, -1, 5},
    };
    for (const auto& corner : corners) {
        int touches = 0;
        for (const auto& line : box) {
            if (line.from == corner) ++touches;
            if (line.to == corner) ++touches;
        }
        assert(touches == 3);
    }

    // The small expansion keeps the overlay from fighting the block mesh.
    constexpr auto expanded = makeBox(0, 0, 0);
    static_assert(expanded[0].from.x < 0.0f);
    static_assert(expanded[5].to.z > 1.0f);

    // makeFaces emits exactly six faces.
    constexpr auto faces = makeFaces(10.0f, -2.0f, 4.0f, 0.0f);
    static_assert(faces.size() == 6);

    // Every cube corner must be touched by exactly three faces.
    for (const auto& corner : corners) {
        int touches = 0;
        for (const auto& face : faces) {
            const Point quad[4] = {face.a, face.b, face.c, face.d};
            for (const auto& v : quad) {
                if (v == corner) ++touches;
            }
        }
        assert(touches == 3);
    }

    // Each face is a unit square (no degenerate quads).
    for (const auto& face : faces) {
        const float dx01 = face.b.x - face.a.x;
        const float dy01 = face.b.y - face.a.y;
        const float dz01 = face.b.z - face.a.z;
        const float dx03 = face.d.x - face.a.x;
        const float dy03 = face.d.y - face.a.y;
        const float dz03 = face.d.z - face.a.z;
        // Two adjacent edges of a face are perpendicular.
        assert(dx01 * dx03 + dy01 * dy03 + dz01 * dz03 == 0.0f);
        // And both are unit length.
        const float l01 = dx01 * dx01 + dy01 * dy01 + dz01 * dz01;
        const float l03 = dx03 * dx03 + dy03 * dy03 + dz03 * dz03;
        assert(l01 == 1.0f);
        assert(l03 == 1.0f);
    }

    // --- Face visibility -------------------------------------------------
    using bedrocktools::modules::blockoutline::makeFaceVisibility;

    // From straight above only the +Y face (index 1) faces the eye.
    {
        constexpr auto faces = makeFaces(0.0f, 0.0f, 0.0f, 0.001f);
        constexpr auto vis = makeFaceVisibility(faces, Point{0.5f, 2.0f, 0.5f});
        static_assert(!vis[0] && vis[1] && !vis[2] && !vis[3] && !vis[4] && !vis[5]);
    }
    // From a corner three faces face the eye.
    {
        constexpr auto faces = makeFaces(0.0f, 0.0f, 0.0f, 0.001f);
        constexpr auto vis = makeFaceVisibility(faces, Point{2.0f, 3.0f, 4.0f});
        static_assert(vis[1] && vis[3] && vis[5]);  // +Y, +Z, +X.
        static_assert(!vis[0] && !vis[2] && !vis[4]);
    }
    // Inside the box every face is kept (the outline surrounds the player).
    {
        constexpr auto faces = makeFaces(10.0f, -2.0f, 4.0f, 0.0f);
        constexpr auto vis = makeFaceVisibility(faces, Point{10.5f, -1.5f, 4.5f});
        static_assert(vis[0] && vis[1] && vis[2] && vis[3] && vis[4] && vis[5]);
    }

    // --- Edge visibility -------------------------------------------------
    using bedrocktools::modules::blockoutline::makeEdgeVisibility;

    // From straight above only the four top edges (indices 4-7) are visible.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        constexpr auto vis = makeEdgeVisibility(box, Point{0.5f, 2.0f, 0.5f});
        static_assert(!vis[0] && !vis[1] && !vis[2] && !vis[3]);
        static_assert(vis[4] && vis[5] && vis[6] && vis[7]);
        static_assert(!vis[8] && !vis[9] && !vis[10] && !vis[11]);
    }
    // From straight along +X only the four edges lying in the +X plane are
    // visible: bottom edge 1, top edge 5, and the vertical edges 9 and 10.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        constexpr auto vis = makeEdgeVisibility(box, Point{3.0f, 0.5f, 0.5f});
        static_assert(vis[1] && vis[5] && vis[9] && vis[10]);
        static_assert(!vis[0] && !vis[2] && !vis[3] && !vis[4]);
        static_assert(!vis[6] && !vis[7] && !vis[8] && !vis[11]);
    }
    // Every edge is visible from inside the box.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.002f);
        constexpr auto vis = makeEdgeVisibility(box, Point{0.5f, 0.5f, 0.5f});
        int visibleCount = 0;
        for (bool v : vis) visibleCount += v ? 1 : 0;
        assert(visibleCount == 12);
    }
    // The expand used by the renderer still yields sensible counts from a
    // generic diagonal viewpoint: a silhouette frame, never zero edges.
    {
        constexpr auto box = makeBox(10.0f, -2.0f, 4.0f);
        constexpr auto vis = makeEdgeVisibility(box, Point{13.0f, 0.5f, 7.0f});
        int visibleCount = 0;
        for (bool v : vis) visibleCount += v ? 1 : 0;
        assert(visibleCount >= 4 && visibleCount <= 12);
    }

    // --- Thick frame (surface strips) --------------------------------------
    // Raising Line Size must only make the wireframe bolder: every strip has
    // to lie flat on a visible face of the block, inside the block's
    // footprint. Nothing may stick out towards the eye, which is what made
    // the old camera-facing bars read as a 3D cube.
    using bedrocktools::modules::blockoutline::frameWidthForLineSize;
    using bedrocktools::modules::blockoutline::kFaceCount;
    using bedrocktools::modules::blockoutline::makeThickFrame;
    using bedrocktools::modules::blockoutline::makeEdgeBars;
    using bedrocktools::modules::blockoutline::kMinimum3DEdgeWidth;
    using bedrocktools::modules::blockoutline::kMaxFrameWidth;

    // Slider mapping: hairline at 1.0, wider above, clamped at 10.
    static_assert(frameWidthForLineSize(1.0f) == 0.0f);
    static_assert(frameWidthForLineSize(0.5f) == 0.0f);
    static_assert(frameWidthForLineSize(2.0f) > 0.0f);
    static_assert(frameWidthForLineSize(5.0f) > frameWidthForLineSize(2.0f));
    static_assert(frameWidthForLineSize(50.0f) == frameWidthForLineSize(10.0f));

    // Straight above: exactly one face -> four strips, all in the +Y plane
    // (lifted by `lift`), all within the block's XZ footprint.
    {
        constexpr std::array<bool, kFaceCount> vis = {false, true, false, false, false, false};
        constexpr auto frame = makeThickFrame(10.0f, -2.0f, 4.0f, vis, 0.1f, 0.004f);
        static_assert(frame.count == 4);
        for (std::size_t i = 0; i < frame.count; ++i) {
            const Point quad[4] = {frame.quads[i].a, frame.quads[i].b,
                                   frame.quads[i].c, frame.quads[i].d};
            for (const auto& v : quad) {
                assert(std::fabs(v.y - (-1.0f + 0.004f)) < 1e-6f);
                assert(v.x >= 10.0f && v.x <= 11.0f);
                assert(v.z >= 4.0f && v.z <= 5.0f);
            }
        }
    }
    // Corner view: three faces -> twelve strips. No vertex may ever leave the
    // (slightly lifted) block bounds, so nothing protrudes towards the eye.
    {
        constexpr std::array<bool, kFaceCount> vis = {false, true, false, true, false, true};
        constexpr auto frame = makeThickFrame(0.0f, 0.0f, 0.0f, vis, 0.05f, 0.004f);
        static_assert(frame.count == 12);
        for (std::size_t i = 0; i < frame.count; ++i) {
            const Point quad[4] = {frame.quads[i].a, frame.quads[i].b,
                                   frame.quads[i].c, frame.quads[i].d};
            for (const auto& v : quad) {
                assert(v.x >= -0.004f - 1e-6f && v.x <= 1.004f + 1e-6f);
                assert(v.y >= -0.004f - 1e-6f && v.y <= 1.004f + 1e-6f);
                assert(v.z >= -0.004f - 1e-6f && v.z <= 1.004f + 1e-6f);
            }
        }
    }
    // Each face's four strips are planar (all four corners share the face
    // coordinate) and together cover exactly the frame area:
    // 1 - (1 - 2w)^2 for a unit face, i.e. mitred corners with no overlap.
    {
        constexpr std::array<bool, kFaceCount> vis = {false, false, false, true, false, false};
        constexpr float w = 0.1f;
        constexpr auto frame = makeThickFrame(3.0f, 3.0f, 3.0f, vis, w, 0.0f);
        static_assert(frame.count == 4);
        float area = 0.0f;
        for (std::size_t i = 0; i < frame.count; ++i) {
            const auto& q = frame.quads[i];
            assert(q.a.z == 4.0f && q.b.z == 4.0f && q.c.z == 4.0f && q.d.z == 4.0f);
            const float du = std::fabs(q.b.x - q.a.x) + std::fabs(q.b.y - q.a.y);
            const float dv = std::fabs(q.d.x - q.a.x) + std::fabs(q.d.y - q.a.y);
            area += du * dv;
        }
        const float expected = 1.0f - (1.0f - 2.0f * w) * (1.0f - 2.0f * w);
        assert(std::fabs(area - expected) < 1e-5f);
    }
    // Absurd widths are clamped so opposite strips never cross the middle.
    {
        constexpr std::array<bool, kFaceCount> vis = {true, false, false, false, false, false};
        constexpr auto frame = makeThickFrame(0.0f, 0.0f, 0.0f, vis, 5.0f, 0.0f);
        static_assert(frame.count == 4);
        for (std::size_t i = 0; i < frame.count; ++i) {
            const Point quad[4] = {frame.quads[i].a, frame.quads[i].b,
                                   frame.quads[i].c, frame.quads[i].d};
            for (const auto& v : quad) {
                assert(v.x >= 0.0f && v.x <= 1.0f);
                assert(v.z >= 0.0f && v.z <= 1.0f);
            }
        }
    }
    // No visible face / hairline width -> nothing to draw.
    {
        constexpr std::array<bool, kFaceCount> none = {};
        static_assert(makeThickFrame(0.0f, 0.0f, 0.0f, none, 0.1f).count == 0);
        constexpr std::array<bool, kFaceCount> top = {false, true, false, false, false, false};
        static_assert(makeThickFrame(0.0f, 0.0f, 0.0f, top, 0.0f).count == 0);
    }

    // --- Thick frame (flat camera-facing ribbons) --------------------------
    // The wide outline is now drawn as flat ribbons centred on each visible
    // edge, always presented side-on to the camera. Unlike the old face
    // strips (which lie in the plane of a block face and recede with
    // perspective), a ribbon keeps its full width facing the eye, so a thick
    // outline stays a bold 2D line instead of reading as a 3D block.
    using bedrocktools::modules::blockoutline::makeBillboardQuads;

    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.0f);
        constexpr std::array<bool, 12> none{};
        assert(makeBillboardQuads(box, none, 0.1f, Point{0.5f, 0.5f, 2.0f}).count == 0);

        constexpr std::array<bool, 12> all = {true, true, true, true, true, true,
                                              true, true, true, true, true, true};
        // Hairline width means "no ribbons"; the line pass covers that case.
        assert(makeBillboardQuads(box, all, 0.0f, Point{0.5f, 0.5f, 2.0f}).count == 0);
    }

    // Every ribbon spans exactly one edge: its two "side" spans equal the
    // requested width, its two "length" spans equal the unit edge length, and
    // it does not overshoot the edge's end points.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.0f);
        constexpr std::array<bool, 12> all = {true, true, true, true, true, true,
                                              true, true, true, true, true, true};
        constexpr float w = 0.1f;
        constexpr Point eye{0.5f, 0.5f, 2.0f};
        const auto ribbons = makeBillboardQuads(box, all, w, eye);
        assert(ribbons.count == 12);

        const auto dist = [](const Point& p, const Point& q) {
            const float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        for (std::size_t i = 0; i < ribbons.count; ++i) {
            const auto& q = ribbons.quads[i];
            const Point& a = q.a;
            const Point& b = q.b;
            const Point& c = q.c;
            const Point& d = q.d;

            // The short sides (a-d and b-c) are the ribbon width.
            assert(std::fabs(dist(a, d) - w) < 1e-6f);
            assert(std::fabs(dist(b, c) - w) < 1e-6f);
            // The long sides (a-b and c-d) are the full unit edge length.
            assert(std::fabs(dist(a, b) - 1.0f) < 1e-6f);
            assert(std::fabs(dist(c, d) - 1.0f) < 1e-6f);

            // The ribbon is centred on the edge: the edge's running axis keeps
            // the from/to coordinate at the from/to end exactly (no overshoot
            // towards the neighbouring edges).
            const auto& line = box[i];
            int axis = 0;
            float best = -1.0f;
            for (int k = 0; k < 3; ++k) {
                const float delta = (k == 0) ? (line.to.x - line.from.x)
                                  : (k == 1) ? (line.to.y - line.from.y)
                                             : (line.to.z - line.from.z);
                const float len = delta < 0.0f ? -delta : delta;
                if (len > best) { best = len; axis = k; }
            }
            const float fromCoord = (axis == 0) ? line.from.x
                                  : (axis == 1) ? line.from.y : line.from.z;
            const float toCoord = (axis == 0) ? line.to.x
                                : (axis == 1) ? line.to.y : line.to.z;
            const auto coord = [&](const Point& p) {
                return (axis == 0) ? p.x : (axis == 1) ? p.y : p.z;
            };
            assert(std::fabs(coord(a) - fromCoord) < 1e-6f);
            assert(std::fabs(coord(d) - fromCoord) < 1e-6f);
            assert(std::fabs(coord(b) - toCoord) < 1e-6f);
            assert(std::fabs(coord(c) - toCoord) < 1e-6f);
        }
    }

    // The ribbon faces the camera: its width direction (a->d) is perpendicular
    // to both the edge and the view ray, so the full width is presented from
    // any ordinary angle (unlike a strip lying in the plane of a block face,
    // which foreshortens as that face turns away).
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.0f);
        constexpr std::array<bool, 12> all = {true, true, true, true, true, true,
                                              true, true, true, true, true, true};
        constexpr float w = 0.1f;
        constexpr Point eye{0.5f, 0.5f, 2.0f};
        const auto ribbons = makeBillboardQuads(box, all, w, eye);

        for (std::size_t i = 0; i < ribbons.count; ++i) {
            const auto& q = ribbons.quads[i];
            const auto& line = box[i];
            const float mx = (line.from.x + line.to.x) * 0.5f;
            const float my = (line.from.y + line.to.y) * 0.5f;
            const float mz = (line.from.z + line.to.z) * 0.5f;

            // Edge direction (unit).
            float ex = line.to.x - line.from.x;
            float ey = line.to.y - line.from.y;
            float ez = line.to.z - line.from.z;
            const float el = std::sqrt(ex * ex + ey * ey + ez * ez);
            ex /= el; ey /= el; ez /= el;

            // View vector (edge midpoint -> eye), unit.
            float vx = eye.x - mx, vy = eye.y - my, vz = eye.z - mz;
            const float vl = std::sqrt(vx * vx + vy * vy + vz * vz);
            vx /= vl; vy /= vl; vz /= vl;

            // Ribbon width direction (a -> d), unit.
            float wx = q.d.x - q.a.x, wy = q.d.y - q.a.y, wz = q.d.z - q.a.z;
            const float wl = std::sqrt(wx * wx + wy * wy + wz * wz);
            wx /= wl; wy /= wl; wz /= wl;

            // Width is perpendicular to the edge and to the view ray.
            assert(std::fabs(wx * ex + wy * ey + wz * ez) < 1e-4f);
            assert(std::fabs(wx * vx + wy * vy + wz * vz) < 1e-4f);
        }
    }

    // --- Hidden back edges ("Show 3D") -----------------------------------
    // The "Show 3D" mode draws the hidden/back edges so the target reads as a
    // full twelve-edge 3D wireframe. Those edges are emitted as edge bars (a
    // cross of two perpendicular quads centred on the edge) instead of strips
    // painted onto the hidden faces: a face strip collapses to nothing as
    // soon as its face turns edge-on to the camera, which made back edges
    // disappear at some viewing angles.
    static_assert(kMinimum3DEdgeWidth > 0.0f);
    static_assert(kMinimum3DEdgeWidth < kMaxFrameWidth);

    // Two quads per selected edge, nothing for the unselected ones.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.0f);
        constexpr std::array<bool, 12> none{};
        static_assert(makeEdgeBars(box, none, 0.05f).count == 0);

        constexpr std::array<bool, 12> all = {true, true, true, true, true, true,
                                              true, true, true, true, true, true};
        static_assert(makeEdgeBars(box, all, 0.05f).count == 24);
        // Hairline width means "no bars"; the line pass covers that case.
        static_assert(makeEdgeBars(box, all, 0.0f).count == 0);
    }

    // Each bar is a cross: the two quads of an edge share the edge axis and
    // are perpendicular to each other, so no camera angle can flatten both.
    {
        constexpr auto box = makeBox(10.0f, -2.0f, 4.0f, 0.0f);
        constexpr std::array<bool, 12> all = {true, true, true, true, true, true,
                                              true, true, true, true, true, true};
        constexpr float w = 0.05f;
        constexpr auto bars = makeEdgeBars(box, all, w);
        static_assert(bars.count == 24);

        const auto normalOf = [](const auto& q) {
            const float ux = q.b.x - q.a.x, uy = q.b.y - q.a.y, uz = q.b.z - q.a.z;
            const float vx = q.d.x - q.a.x, vy = q.d.y - q.a.y, vz = q.d.z - q.a.z;
            return std::array<float, 3>{uy * vz - uz * vy,
                                        uz * vx - ux * vz,
                                        ux * vy - uy * vx};
        };

        for (std::size_t e = 0; e < 12; ++e) {
            const auto& q0 = bars.quads[e * 2];
            const auto& q1 = bars.quads[e * 2 + 1];
            const auto n0 = normalOf(q0);
            const auto n1 = normalOf(q1);
            // Both quads have a real area ...
            const float a0 = n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2];
            const float a1 = n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2];
            assert(a0 > 0.0f && a1 > 0.0f);
            // ... and their normals are perpendicular.
            const float dot = n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2];
            assert(std::fabs(dot) < 1e-6f);

            // Both are centred on the edge and no wider than the bar width.
            const auto& line = box[e];
            const float cx = (line.from.x + line.to.x) * 0.5f;
            const float cy = (line.from.y + line.to.y) * 0.5f;
            const float cz = (line.from.z + line.to.z) * 0.5f;
            for (const auto* q : {&q0, &q1}) {
                const Point quad[4] = {q->a, q->b, q->c, q->d};
                for (const auto& v : quad) {
                    assert(std::fabs(v.x - cx) <= 0.5f + w + 1e-6f);
                    assert(std::fabs(v.y - cy) <= 0.5f + w + 1e-6f);
                    assert(std::fabs(v.z - cz) <= 0.5f + w + 1e-6f);
                }
            }
        }
    }

    // Whatever the viewpoint, the eye-facing edges plus the hidden ones always
    // add up to all twelve edges, and the hidden set is never drawn twice.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        const Point eyes[] = {
            {0.5f, 5.0f, 0.5f},    // straight above (block under the feet)
            {5.0f, 0.5f, 0.5f},    // straight along +X
            {3.0f, 3.0f, 3.0f},    // corner view
            {0.5f, 3.0f, -4.0f},   // grazing angle that used to lose an edge
            {-6.0f, 0.5f, 2.0f},
            {2.0f, -3.0f, 0.5f},
        };
        for (const auto& eye : eyes) {
            const auto vis = makeEdgeVisibility(box, eye);
            std::array<bool, 12> hidden{};
            int hiddenCount = 0;
            for (std::size_t i = 0; i < 12; ++i) {
                hidden[i] = !vis[i];
                if (hidden[i]) ++hiddenCount;
            }
            int visibleCount = 0;
            for (bool v : vis) visibleCount += v ? 1 : 0;
            assert(visibleCount + hiddenCount == 12);
            const auto bars = makeEdgeBars(box, hidden, kMinimum3DEdgeWidth);
            assert(bars.count == static_cast<std::size_t>(hiddenCount) * 2);
        }
    }

    // --- Show 3D: no edge may depend on where the eye is -------------------
    // The reported bug: with "Show 3D" on, some of the twelve edges
    // disappeared. The hidden edges had already been switched to bars (#176),
    // but the eye-facing edges were still painted by makeThickFrame as strips
    // lying in the plane of an eye-facing face. A strip projects to (nearly)
    // zero pixels as soon as that face turns edge-on to the camera, so whether
    // an edge was visible depended on the eye position: some edges vanished at
    // grazing angles while their neighbours stayed solid. Show 3D now builds
    // every edge from a bar, which cannot collapse that way.
    using bedrocktools::modules::blockoutline::EdgeBarPasses;
    using bedrocktools::modules::blockoutline::edgeBarWidthForFrame;
    using bedrocktools::modules::blockoutline::kEdgeBarLift;
    using bedrocktools::modules::blockoutline::makeEdgeBarPasses;

    // Bar width follows the Line Size slider, and falls back to the minimum
    // 3D edge width at hairline so the back edges always have real geometry.
    static_assert(edgeBarWidthForFrame(0.0f) == kMinimum3DEdgeWidth);
    static_assert(edgeBarWidthForFrame(frameWidthForLineSize(1.0f)) == kMinimum3DEdgeWidth);
    static_assert(edgeBarWidthForFrame(frameWidthForLineSize(2.0f)) == frameWidthForLineSize(2.0f));
    static_assert(edgeBarWidthForFrame(frameWidthForLineSize(10.0f)) == frameWidthForLineSize(10.0f));
    static_assert(edgeBarWidthForFrame(-1.0f) == kMinimum3DEdgeWidth);

    // The two passes are exact complements: every edge is drawn by exactly one
    // of them, so the whole wireframe is always emitted - including when the
    // eye is inside the block and every edge counts as facing it.
    {
        constexpr std::array<bool, 12> all = {true, true, true, true, true, true,
                                              true, true, true, true, true, true};
        constexpr std::array<bool, 12> none{};
        constexpr std::array<bool, 12> top = {false, false, false, false,
                                              true, true, true, true,
                                              false, false, false, false};

        constexpr EdgeBarPasses fromAll = makeEdgeBarPasses(all);
        static_assert(fromAll.depthTestedCount == 12 && fromAll.seeThroughCount == 0);
        constexpr EdgeBarPasses fromNone = makeEdgeBarPasses(none);
        static_assert(fromNone.depthTestedCount == 0 && fromNone.seeThroughCount == 12);
        constexpr EdgeBarPasses fromTop = makeEdgeBarPasses(top);
        static_assert(fromTop.depthTestedCount == 4 && fromTop.seeThroughCount == 8);
        for (std::size_t i = 0; i < 12; ++i) {
            assert(fromTop.depthTested[i] != fromTop.seeThrough[i]);
        }
    }

    // Whatever the viewpoint, the two passes together always cover all twelve
    // edges and emit two bar quads per edge - never fewer, never twice.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f);
        const Point eyes[] = {
            {0.5f, 5.0f, 0.5f},    // straight above (block under the feet)
            {5.0f, 0.5f, 0.5f},    // straight along +X
            {3.0f, 3.0f, 3.0f},    // corner view
            {0.5f, 3.0f, -4.0f},   // grazing angle that used to lose an edge
            {0.5f, 1.0001f, 4.0f}, // almost exactly level with the top face
            {-6.0f, 0.5f, 2.0f},
            {2.0f, -3.0f, 0.5f},
            {0.5f, 0.5f, 0.5f},    // eye inside the block
        };
        for (const auto& eye : eyes) {
            const auto passes = makeEdgeBarPasses(makeEdgeVisibility(box, eye));
            assert(passes.depthTestedCount + passes.seeThroughCount == 12);

            const auto front = makeEdgeBars(box, passes.depthTested,
                                            edgeBarWidthForFrame(0.0f), kEdgeBarLift);
            const auto back = makeEdgeBars(box, passes.seeThrough,
                                           edgeBarWidthForFrame(0.0f), 0.0f);
            assert(front.count == passes.depthTestedCount * 2);
            assert(back.count == passes.seeThroughCount * 2);
            assert(front.count + back.count == 24);
        }
    }

    // The eye-facing bars are lifted clear of the block surface, because a bar
    // quad lies in the plane of one of the faces meeting at its edge and would
    // otherwise z-fight with the block under a depth-tested material. The
    // see-through back bars keep lift 0 and stay exactly where they always
    // were.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.0f);
        constexpr std::array<bool, 12> all = {true, true, true, true, true, true,
                                              true, true, true, true, true, true};
        constexpr auto flat = makeEdgeBars(box, all, 0.05f);
        constexpr auto lifted = makeEdgeBars(box, all, 0.05f, kEdgeBarLift);
        static_assert(flat.count == 24 && lifted.count == 24);

        const auto onFacePlane = [](float c) { return c == 0.0f || c == 1.0f; };

        for (std::size_t i = 0; i < flat.count; ++i) {
            const Point fv[4] = {flat.quads[i].a, flat.quads[i].b,
                                 flat.quads[i].c, flat.quads[i].d};
            const Point lv[4] = {lifted.quads[i].a, lifted.quads[i].b,
                                 lifted.quads[i].c, lifted.quads[i].d};
            for (int k = 0; k < 4; ++k) {
                const float f[3] = {fv[k].x, fv[k].y, fv[k].z};
                const float l[3] = {lv[k].x, lv[k].y, lv[k].z};
                // Unlifted: still coplanar with a face of the block.
                assert(onFacePlane(f[0]) || onFacePlane(f[1]) || onFacePlane(f[2]));
                // Lifted: pushed outside the block on that same axis, so no
                // vertex touches a face plane any more.
                assert(!onFacePlane(l[0]) && !onFacePlane(l[1]) && !onFacePlane(l[2]));
                // The offset is exactly the lift, applied outwards.
                bool moved = false;
                for (int a = 0; a < 3; ++a) {
                    const float d = l[a] - f[a];
                    if (d != 0.0f) {
                        assert(std::fabs(std::fabs(d) - kEdgeBarLift) < 1e-6f);
                        assert(f[a] == 0.0f ? d < 0.0f : d > 0.0f);
                        moved = true;
                    }
                }
                assert(moved);
            }
        }
        // A negative lift is clamped away rather than pushing bars inside.
        static_assert(makeEdgeBars(box, all, 0.05f, -1.0f).count == 24);
    }

    // The regression itself, measured. Whether an edge covers pixels is
    // decided by its projected area, and for a world-space quad that is the
    // quad area scaled by the cosine of the angle between the quad normal and
    // the direction from the eye to the quad. Sweep eyes all around the block
    // - from touching distance out to 24 blocks, plus eyes parked just outside
    // two adjacent face planes, which is the grazing case a block near eye
    // level produces - and compare the bar against the strip the old code
    // painted for the same edge.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.0f);
        constexpr std::array<bool, 12> all = {true, true, true, true, true, true,
                                              true, true, true, true, true, true};
        constexpr float w = 0.05f;
        constexpr auto bars = makeEdgeBars(box, all, w, kEdgeBarLift);
        constexpr auto faces = makeFaces(0.0f, 0.0f, 0.0f, 0.0f);

        // Outward normals of the six faces, in makeFaces order.
        constexpr std::array<std::array<float, 3>, kFaceCount> faceNormals = {{
            {{0.0f, -1.0f, 0.0f}}, {{0.0f, 1.0f, 0.0f}},
            {{0.0f, 0.0f, -1.0f}}, {{0.0f, 0.0f, 1.0f}},
            {{-1.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f}},
        }};

        // Projected area of a quad as seen from `eye`.
        const auto projectedArea = [](const Quad& q, const Point& eye) {
            const float cx = (q.a.x + q.b.x + q.c.x + q.d.x) * 0.25f;
            const float cy = (q.a.y + q.b.y + q.c.y + q.d.y) * 0.25f;
            const float cz = (q.a.z + q.b.z + q.c.z + q.d.z) * 0.25f;
            float wx = cx - eye.x, wy = cy - eye.y, wz = cz - eye.z;
            const float len = std::sqrt(wx * wx + wy * wy + wz * wz);
            if (len <= 0.0f) return 0.0f;
            wx /= len; wy /= len; wz /= len;
            const float ux = q.b.x - q.a.x, uy = q.b.y - q.a.y, uz = q.b.z - q.a.z;
            const float vx = q.d.x - q.a.x, vy = q.d.y - q.a.y, vz = q.d.z - q.a.z;
            return std::fabs((uy * vz - uz * vy) * wx +
                             (uz * vx - ux * vz) * wy +
                             (ux * vy - uy * vx) * wz);
        };

        // Distance from an eye to the unit block, used to tell ordinary
        // viewing distances from the camera practically touching the block.
        const auto boxDistance = [](const Point& e) {
            const auto outside = [](float c) {
                if (c < 0.0f) return -c;
                if (c > 1.0f) return c - 1.0f;
                return 0.0f;
            };
            const float dx = outside(e.x), dy = outside(e.y), dz = outside(e.z);
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };

        // A strip painted on a face spans the same rectangle as a bar quad
        // (width x (1 + width)), so the two ratios are directly comparable.
        const float quadArea = w * (1.0f + w);

        std::vector<Point> eyes;
        for (const float radius : {0.6f, 1.5f, 3.0f, 8.0f, 24.0f}) {
            for (int yawStep = 0; yawStep < 72; ++yawStep) {
                for (int pitchStep = -35; pitchStep <= 35; ++pitchStep) {
                    const float yaw = yawStep * 5.0f * 3.14159265f / 180.0f;
                    const float pitch = pitchStep * 2.5f * 3.14159265f / 180.0f;
                    eyes.push_back(Point{
                        0.5f + radius * std::cos(pitch) * std::cos(yaw),
                        0.5f + radius * std::sin(pitch),
                        0.5f + radius * std::cos(pitch) * std::sin(yaw)});
                }
            }
        }
        // Grazing eyes: barely outside two adjacent face planes at once, both
        // up close and from a few blocks back.
        for (const float eps : {0.0005f, 0.002f, 0.01f, 0.05f, 0.2f}) {
            for (const float back : {0.0f, 0.5f, 2.0f, 4.0f, 10.0f}) {
                eyes.push_back(Point{0.5f, 1.0f + eps, -eps - back});
                eyes.push_back(Point{-eps - back, 1.0f + eps, 0.5f});
                eyes.push_back(Point{0.5f, -eps - back, 1.0f + eps});
                eyes.push_back(Point{1.0f + eps, 0.5f, -eps - back});
                eyes.push_back(Point{0.5f, 1.0f + eps, 0.5f});
            }
        }

        int checked = 0, degenerate = 0, collapsed = 0, collapsedFar = 0;
        float worstBar = 1.0f;
        for (const auto& eye : eyes) {
            const auto faceVisible = makeFaceVisibility(faces, eye);
            const float distance = boxDistance(eye);

            for (std::size_t e = 0; e < 12; ++e) {
                // Unit direction of the edge, and of eye -> edge centre.
                float dir[3] = {box[e].to.x - box[e].from.x,
                                box[e].to.y - box[e].from.y,
                                box[e].to.z - box[e].from.z};
                const float edgeLen = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] +
                                                dir[2] * dir[2]);
                assert(edgeLen > 0.0f);
                for (float& d : dir) d /= edgeLen;
                const float mx = (box[e].from.x + box[e].to.x) * 0.5f - eye.x;
                const float my = (box[e].from.y + box[e].to.y) * 0.5f - eye.y;
                const float mz = (box[e].from.z + box[e].to.z) * 0.5f - eye.z;
                const float toLen = std::sqrt(mx * mx + my * my + mz * mz);
                assert(toLen > 0.0f);
                const float along = std::fabs(dir[0] * mx + dir[1] * my + dir[2] * mz) / toLen;

                // Looking straight down the edge: no geometry of any kind can
                // give it screen extent, so this is not a bug and is skipped.
                if (along > 0.95f) { ++degenerate; continue; }
                ++checked;

                // The bar always keeps a real share of its area. Measured
                // worst case over this whole sweep is ~0.22 of quadArea.
                const float barRatio =
                    std::max(projectedArea(bars.quads[e * 2], eye),
                             projectedArea(bars.quads[e * 2 + 1], eye)) / quadArea;
                assert(barRatio >= 0.20f);
                worstBar = std::min(worstBar, barRatio);

                // What the old code drew for this edge: strips on whichever of
                // its two faces makeFaceVisibility marked as facing the eye.
                float stripRatio = 0.0f;
                bool drawnAsStrip = false;
                for (std::size_t f = 0; f < kFaceCount; ++f) {
                    const Point corners[4] = {faces[f].a, faces[f].b, faces[f].c, faces[f].d};
                    bool touchesFrom = false, touchesTo = false;
                    for (const auto& c : corners) {
                        if (c == box[e].from) touchesFrom = true;
                        if (c == box[e].to) touchesTo = true;
                    }
                    if (!touchesFrom || !touchesTo || !faceVisible[f]) continue;
                    drawnAsStrip = true;
                    const float fx = (corners[0].x + corners[1].x + corners[2].x + corners[3].x) * 0.25f - eye.x;
                    const float fy = (corners[0].y + corners[1].y + corners[2].y + corners[3].y) * 0.25f - eye.y;
                    const float fz = (corners[0].z + corners[1].z + corners[2].z + corners[3].z) * 0.25f - eye.z;
                    const float fLen = std::sqrt(fx * fx + fy * fy + fz * fz);
                    assert(fLen > 0.0f);
                    const auto& n = faceNormals[f];
                    stripRatio = std::max(stripRatio, std::fabs(
                        n[0] * fx + n[1] * fy + n[2] * fz) / fLen);
                }
                // Faces an eye-facing face => the old thick pass drew it, and
                // at these angles that strip covered almost no pixels.
                if (drawnAsStrip && stripRatio < 0.02f) {
                    ++collapsed;
                    if (distance >= 1.0f) ++collapsedFar;
                }
            }
        }

        // The sweep really did cover the space around the block ...
        assert(checked > 200000);
        assert(degenerate > 0);
        assert(worstBar >= 0.20f && worstBar < 1.0f);
        // ... the strip approach really did lose edges, including from
        // perfectly ordinary viewing distances of a block or more ...
        assert(collapsed > 1000);
        assert(collapsedFar > 500);
    }

    // One concrete repro at a realistic viewing distance: the eye is 4 blocks
    // away and only just above the top face, which is how a block near eye
    // level gets looked at. The far top edge (index 6) still counts as
    // eye-facing - through the top face alone - so the old code painted it as
    // a strip lying in the top face plane. Seen from almost level with that
    // plane the strip covers ~0.3% of its area and the edge disappeared, while
    // the bar for the same edge keeps essentially its full area through the
    // perpendicular quad.
    {
        constexpr auto box = makeBox(0.0f, 0.0f, 0.0f, 0.0f);
        constexpr Point eye{0.5f, 1.02f, -4.0f};
        constexpr auto faces = makeFaces(0.0f, 0.0f, 0.0f, 0.0f);
        constexpr auto faceVisible = makeFaceVisibility(faces, eye);
        // Only the top face and the near (-Z) face face the eye.
        static_assert(!faceVisible[0] && faceVisible[1] && faceVisible[2] &&
                      !faceVisible[3] && !faceVisible[4] && !faceVisible[5]);

        constexpr auto edgeVisible = makeEdgeVisibility(box, eye);
        // The far top edge is reported visible, so the old thick pass did try
        // to draw it - as a strip on the grazing top face only.
        static_assert(edgeVisible[6]);

        constexpr std::array<bool, 12> justThatEdge = {false, false, false, false,
                                                       false, false, true, false,
                                                       false, false, false, false};
        constexpr auto bars = makeEdgeBars(box, justThatEdge, 0.05f, kEdgeBarLift);
        static_assert(bars.count == 2);
        constexpr auto strips = makeThickFrame(0.0f, 0.0f, 0.0f, faceVisible, 0.05f, 0.004f);
        static_assert(strips.count == 8);

        const auto projectedArea = [](const Quad& q, const Point& from) {
            const float cx = (q.a.x + q.b.x + q.c.x + q.d.x) * 0.25f;
            const float cy = (q.a.y + q.b.y + q.c.y + q.d.y) * 0.25f;
            const float cz = (q.a.z + q.b.z + q.c.z + q.d.z) * 0.25f;
            float wx = cx - from.x, wy = cy - from.y, wz = cz - from.z;
            const float len = std::sqrt(wx * wx + wy * wy + wz * wz);
            wx /= len; wy /= len; wz /= len;
            const float ux = q.b.x - q.a.x, uy = q.b.y - q.a.y, uz = q.b.z - q.a.z;
            const float vx = q.d.x - q.a.x, vy = q.d.y - q.a.y, vz = q.d.z - q.a.z;
            return std::fabs((uy * vz - uz * vy) * wx +
                             (uz * vx - ux * vz) * wy +
                             (ux * vy - uy * vx) * wz);
        };
        const float quadArea = 0.05f * 1.05f;

        // Every strip on the top face - including the one for edge 6 - is
        // seen almost exactly edge-on and covers no pixels.
        for (std::size_t i = 0; i < strips.count; ++i) {
            const bool onTop = strips.quads[i].a.y > 1.0f;
            if (!onTop) continue;
            assert(projectedArea(strips.quads[i], eye) < 0.02f * quadArea);
        }
        // The bar for edge 6 does not: its quad in the Z plane faces the eye.
        const float barRatio = std::max(projectedArea(bars.quads[0], eye),
                                        projectedArea(bars.quads[1], eye)) / quadArea;
        assert(barRatio > 0.5f);
    }

    // --- RGB rainbow cycle -----------------------------------------------
    using bedrocktools::modules::blockoutline::rainbowRgb;
    using bedrocktools::modules::blockoutline::wrapPhase;

    // Exact points of the cycle (phases chosen so every value is exactly
    // representable as a float).
    constexpr auto cRed = rainbowRgb(0.0f);
    static_assert(cRed.r == 1.0f && cRed.g == 0.0f && cRed.b == 0.0f);
    constexpr auto cSpringGreen = rainbowRgb(0.25f);
    static_assert(cSpringGreen.r == 0.5f && cSpringGreen.g == 1.0f && cSpringGreen.b == 0.0f);
    constexpr auto cCyan = rainbowRgb(0.5f);
    static_assert(cCyan.r == 0.0f && cCyan.g == 1.0f && cCyan.b == 1.0f);
    constexpr auto cViolet = rainbowRgb(0.75f);
    static_assert(cViolet.r == 0.5f && cViolet.g == 0.0f && cViolet.b == 1.0f);

    // The cycle wraps: phase 1.0 is back to red, negatives wrap around.
    constexpr auto cWrapped = rainbowRgb(1.0f);
    static_assert(cWrapped.r == 1.0f && cWrapped.g == 0.0f && cWrapped.b == 0.0f);
    static_assert(wrapPhase(2.25f) == 0.25f);
    static_assert(wrapPhase(-0.25f) == 0.75f);
    static_assert(wrapPhase(1.0f) == 0.0f);

    // The primary colors sit at thirds of the cycle (checked with a small
    // epsilon because 1/3 is not exactly representable).
    const auto near = [](bedrocktools::modules::blockoutline::RgbColor c,
                         float r, float g, float b) {
        return std::fabs(c.r - r) < 1e-4f &&
               std::fabs(c.g - g) < 1e-4f &&
               std::fabs(c.b - b) < 1e-4f;
    };
    assert(near(rainbowRgb(1.0f / 3.0f), 0.0f, 1.0f, 0.0f));
    assert(near(rainbowRgb(2.0f / 3.0f), 0.0f, 0.0f, 1.0f));

    // Every channel always stays inside [0, 1].
    for (int i = 0; i < 360; ++i) {
        const auto c = rainbowRgb(static_cast<float>(i) / 360.0f);
        assert(c.r >= 0.0f && c.r <= 1.0f);
        assert(c.g >= 0.0f && c.g <= 1.0f);
        assert(c.b >= 0.0f && c.b <= 1.0f);
    }

    std::cout << "block outline geometry tests passed\n";
    return 0;
}
