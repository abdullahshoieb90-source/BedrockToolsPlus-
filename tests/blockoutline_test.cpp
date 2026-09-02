#include "modules/visual/blockoutline_color.hpp"
#include "modules/visual/blockoutline_geometry.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using bedrocktools::modules::blockoutline::Point;
using bedrocktools::modules::blockoutline::makeBox;
using bedrocktools::modules::blockoutline::makeFaces;

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

    // Whatever the viewpoint, the visible edges (drawn by the normal passes)
    // plus the hidden ones (drawn as bars) always add up to all twelve edges,
    // and the hidden set is never drawn twice.
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
