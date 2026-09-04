// Host-side visual mock: renders what the Block Outline thick frame looks
// like in each implementation, using the REAL module geometry header.
//
//   Panel 1: Line Thickness = 1  (classic hairline - every build)
//   Panel 2: Line Thickness = 10, CURRENT build (PR #186+): camera-facing
//            ribbons (makeBillboardQuads) - flat bold wireframe
//   Panel 3: Line Thickness = 10, OLD build (up to PR #183): strips painted
//            on the block faces (makeThickFrame) - reads as a 3D box
//   Panel 4: Line Thickness = 10, Show 3D toggle ON (any build): full
//            12-edge bar wireframe with see-through back edges
//
// Pure host code: pinhole camera, painter's algorithm, BMP out.
// Build: g++ -std=c++20 -I src -I include tools/blockoutline_preview.cpp -o /tmp/bo_preview

#include "modules/visual/blockoutline_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace bedrocktools::modules::blockoutline;

namespace {

struct Vec3f {
    float x, y, z;
};

struct RGB { std::uint8_t r, g, b; };

constexpr int kPanel = 460;           // panel size in pixels
constexpr int kGutter = 10;
constexpr int kImageW = kPanel * 2 + kGutter * 3;
constexpr int kImageH = kPanel * 2 + kGutter * 3;

struct Camera {
    Vec3f eye;
    Vec3f fwd, right, up;
    float focal;
    int ox, oy;  // panel origin in the final image
};

Vec3f sub(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3f cross(Vec3f a, Vec3f b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3f norm(Vec3f a) {
    const float l = std::sqrt(dot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

Point toPoint(Vec3f v) { return {v.x, v.y, v.z}; }
Vec3f toVec(Point p) { return {p.x, p.y, p.z}; }

struct ScreenPoly {
    std::vector<Vec3f> pts;  // pixel coords (x right, y down)
    float depth;             // mean view depth, for painter's algorithm
    RGB color;
};

bool project(const Camera& cam, Vec3f p, Vec3f& out, float& depth) {
    const Vec3f rel = sub(p, cam.eye);
    depth = dot(rel, cam.fwd);
    if (depth < 0.05f) return false;
    const float x = dot(rel, cam.right) / depth;
    const float y = dot(rel, cam.up) / depth;
    out.x = cam.ox + kPanel * 0.5f + x * cam.focal;
    out.y = cam.oy + kPanel * 0.5f - y * cam.focal;
    return true;
}

bool projectQuad(const Camera& cam, const Quad& q, ScreenPoly& poly) {
    const Vec3f verts[4] = {toVec(q.a), toVec(q.b), toVec(q.c), toVec(q.d)};
    float depthSum = 0.0f;
    poly.pts.clear();
    for (const Vec3f v : verts) {
        Vec3f s{};
        float d = 0.0f;
        if (!project(cam, v, s, d)) return false;
        poly.pts.push_back(s);
        depthSum += d;
    }
    poly.depth = depthSum / 4.0f;
    return true;
}

bool pointInPoly(const std::vector<Vec3f>& pts, float px, float py) {
    bool inside = false;
    const std::size_t n = pts.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec3f& a = pts[i];
        const Vec3f& b = pts[j];
        if ((a.y > py) != (b.y > py)) {
            const float t = (py - a.y) / (b.y - a.y);
            const float x = a.x + t * (b.x - a.x);
            if (px < x) inside = !inside;
        }
    }
    return inside;
}

struct Image {
    int w, h;
    std::vector<RGB> px;
    RGB at(int x, int y) const { return px[y * w + x]; }
    void set(int x, int y, RGB c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        px[y * w + x] = c;
    }
};

void fillPoly(Image& img, const ScreenPoly& poly) {
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const Vec3f& p : poly.pts) {
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    }
    const int x0 = std::max(0, static_cast<int>(minX) - 1);
    const int y0 = std::max(0, static_cast<int>(minY) - 1);
    const int x1 = std::min(img.w - 1, static_cast<int>(maxX) + 1);
    const int y1 = std::min(img.h - 1, static_cast<int>(maxY) + 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (pointInPoly(poly.pts, x + 0.5f, y + 0.5f)) img.set(x, y, poly.color);
        }
    }
}

void drawThickScreenLine(Image& img, Vec3f a, Vec3f b, float widthPx, RGB c) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    // Normal in screen space.
    const float nx = -dy / len * widthPx * 0.5f;
    const float ny = dx / len * widthPx * 0.5f;
    ScreenPoly poly;
    poly.color = c;
    poly.depth = 0.0f;
    poly.pts = {
        {a.x + nx, a.y + ny, 0},
        {b.x + nx, b.y + ny, 0},
        {b.x - nx, b.y - ny, 0},
        {a.x - nx, a.y - ny, 0},
    };
    fillPoly(img, poly);
}

// 5x7 digit font ('1'..'4') for panel labels.
const std::uint8_t kDigits[5][8] = {
    {0x00, 0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F},  // 1
    {0x00, 0x1F, 0x01, 0x01, 0x1F, 0x10, 0x10, 0x1F},  // 2
    {0x00, 0x1F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x1F},  // 3
    {0x00, 0x11, 0x11, 0x11, 0x1F, 0x01, 0x01, 0x01},  // 4
};

void drawLabel(Image& img, int digit, int ox, int oy) {
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (kDigits[digit - 1][row] & (0x10 >> col)) {
                for (int dy = 0; dy < 3; ++dy)
                    for (int dx = 0; dx < 3; ++dx)
                        img.set(ox + col * 3 + dx, oy + row * 3 + dy, {30, 30, 30});
            }
        }
    }
}

// --- scene -------------------------------------------------------------

struct SceneInput {
    const std::array<Line, 12>* box;
    const std::array<bool, 12>* edgeVisible;
    const FrameQuads* strips;        // old thick frame (panel 3)
    const BillboardQuads* ribbons;   // new thick frame (panel 2)
    const EdgeBars* barsFront;       // show3d visible edges (panel 4)
    const EdgeBars* barsBack;        // show3d hidden edges (panel 4)
    bool drawHairline;
    RGB outline;
    RGB outlineBack;
    int label = 0;
};

void renderPanel(Image& img, const Camera& cam, const SceneInput& in) {
    // Background.
    for (int y = cam.oy; y < cam.oy + kPanel; ++y)
        for (int x = cam.ox; x < cam.ox + kPanel; ++x)
            img.set(x, y, {245, 246, 248});

    // Ground plane (y = 0), always behind everything else here.
    {
        ScreenPoly ground;
        ground.color = {214, 216, 220};
        const Vec3f corners[4] = {{-2.5f, 0, -2.5f}, {3.5f, 0, -2.5f},
                                  {3.5f, 0, 3.5f}, {-2.5f, 0, 3.5f}};
        bool ok = true;
        for (const Vec3f v : corners) {
            Vec3f s{};
            float d = 0.0f;
            if (!project(cam, v, s, d)) { ok = false; break; }
            ground.pts.push_back(s);
        }
        if (ok) fillPoly(img, ground);
    }

    // Block faces (mimics the targeted block in game). Shade the visible
    // ones the way a flat-lit block reads: top brightest, sides mid gray.
    const auto faces = makeFaces(0.0f, 0.0f, 0.0f, 0.0f);
    const auto faceVisible = makeFaceVisibility(faces, toPoint(cam.eye));
    const RGB faceShade[kFaceCount] = {
        {150, 150, 150},  // -Y
        {188, 188, 188},  // +Y (top)
        {168, 168, 168},  // -Z
        {160, 160, 160},  // +Z
        {174, 174, 174},  // -X
        {166, 166, 166},  // +X
    };
    for (std::size_t f = 0; f < kFaceCount; ++f) {
        if (!faceVisible[f]) continue;
        ScreenPoly poly;
        poly.color = faceShade[f];
        if (projectQuad(cam, faces[f], poly)) fillPoly(img, poly);
    }

    // See-through back edges ("Show 3D"): in game these go through a
    // no-depth-test material, so they draw over the block faces.
    if (in.barsBack) {
        std::vector<ScreenPoly> polys;
        for (std::size_t i = 0; i < in.barsBack->count; ++i) {
            ScreenPoly poly;
            poly.color = in.outlineBack;
            if (projectQuad(cam, in.barsBack->quads[i], poly)) polys.push_back(poly);
        }
        std::stable_sort(polys.begin(), polys.end(),
                         [](const ScreenPoly& a, const ScreenPoly& b) { return a.depth > b.depth; });
        for (const ScreenPoly& poly : polys) fillPoly(img, poly);
    }

    // Depth-tested outline geometry, far-to-near.
    std::vector<ScreenPoly> polys;
    auto push = [&](const Quad& q, RGB c) {
        ScreenPoly poly;
        poly.color = c;
        if (projectQuad(cam, q, poly)) polys.push_back(poly);
    };
    if (in.strips) {
        for (std::size_t i = 0; i < in.strips->count; ++i)
            push(in.strips->quads[i], in.outline);
    }
    if (in.ribbons) {
        for (std::size_t i = 0; i < in.ribbons->count; ++i)
            push(in.ribbons->quads[i], in.outline);
    }
    if (in.barsFront) {
        for (std::size_t i = 0; i < in.barsFront->count; ++i)
            push(in.barsFront->quads[i], in.outline);
    }
    std::stable_sort(polys.begin(), polys.end(),
                     [](const ScreenPoly& a, const ScreenPoly& b) { return a.depth > b.depth; });
    for (const ScreenPoly& poly : polys) fillPoly(img, poly);

    // Hairline on top (screen-space ~2px, like the selection_box lines).
    if (in.drawHairline && in.edgeVisible) {
        for (std::size_t i = 0; i < in.box->size(); ++i) {
            if (!(*in.edgeVisible)[i]) continue;
            Vec3f a{}, b{};
            float da = 0.0f, db = 0.0f;
            if (!project(cam, toVec((*in.box)[i].from), a, da)) continue;
            if (!project(cam, toVec((*in.box)[i].to), b, db)) continue;
            drawThickScreenLine(img, a, b, 2.0f, in.outline);
        }
    }

    drawLabel(img, in.label, cam.ox + kPanel - 26, cam.oy + kPanel - 32);
}

void writeBmp(const char* path, const Image& img) {
    const int rowSize = (img.w * 3 + 3) & ~3;
    const int data_size = rowSize * img.h;
    const int file_size = 54 + data_size;
    std::uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    std::memcpy(header + 2, &file_size, 4);
    header[10] = 54;
    header[14] = 40;
    std::memcpy(header + 18, &img.w, 4);
    std::memcpy(header + 22, &img.h, 4);
    header[26] = 1;
    header[28] = 24;
    std::memcpy(header + 34, &data_size, 4);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header), 54);
    std::vector<std::uint8_t> row(rowSize, 0);
    for (int y = img.h - 1; y >= 0; --y) {
        for (int x = 0; x < img.w; ++x) {
            const RGB c = img.at(x, y);
            row[x * 3 + 0] = c.b;
            row[x * 3 + 1] = c.g;
            row[x * 3 + 2] = c.r;
        }
        out.write(reinterpret_cast<const char*>(row.data()), rowSize);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* outPath = argc > 1 ? argv[1] : "blockoutline_preview.bmp";

    Image img{kImageW, kImageH, std::vector<RGB>(kImageW * kImageH, {255, 255, 255})};

    // Typical 3/4 view of the targeted block, like standing a few blocks
    // away while mining (block ~20% of the screen height).
    const Vec3f eye{3.4f, 2.6f, 4.2f};
    const Vec3f centre{0.5f, 0.5f, 0.5f};
    Camera base;
    base.eye = eye;
    base.fwd = norm(sub(centre, eye));
    base.right = norm(cross(base.fwd, {0.0f, 1.0f, 0.0f}));
    base.up = cross(base.right, base.fwd);
    base.focal = 700.0f;

    const auto box = makeBox(0.0f, 0.0f, 0.0f);
    const auto edgeVisible = makeEdgeVisibility(box, toPoint(eye));
    const float width = frameWidthForLineSize(10.0f);  // 0.125 blocks
    const auto faceVisible = makeFaceVisibility(makeFaces(0.0f, 0.0f, 0.0f, 0.0f), toPoint(eye));

    const auto ribbons = makeBillboardQuads(box, edgeVisible, width, toPoint(eye));
    const auto strips = makeThickFrame(0.0f, 0.0f, 0.0f, faceVisible, width, kEdgeBarLift);
    const auto passes = makeEdgeBarPasses(edgeVisible);
    const auto barsFront = makeEdgeBars(box, passes.depthTested, width, kEdgeBarLift);
    const auto barsBack = makeEdgeBars(box, passes.seeThrough, width, 0.0f);

    const RGB red{214, 48, 49};
    const RGB redBack{244, 170, 170};

    SceneInput in{};
    in.box = &box;
    in.edgeVisible = &edgeVisible;

    // Panel 1: thickness 1 (hairline only) - every build.
    in.strips = nullptr; in.ribbons = nullptr; in.barsFront = nullptr; in.barsBack = nullptr;
    in.drawHairline = true;
    in.outline = red; in.outlineBack = redBack;
    in.label = 1;
    base.ox = kGutter;                 base.oy = kGutter;
    renderPanel(img, base, in);

    // Panel 2: thickness 10, CURRENT build (camera-facing ribbons).
    in.ribbons = &ribbons;
    in.drawHairline = true;
    in.label = 2;
    base.ox = kGutter * 2 + kPanel;    base.oy = kGutter;
    renderPanel(img, base, in);

    // Panel 3: thickness 10, OLD build (strips on the faces -> 3D look).
    in.ribbons = nullptr;
    in.strips = &strips;
    in.label = 3;
    base.ox = kGutter;                 base.oy = kGutter * 2 + kPanel;
    renderPanel(img, base, in);

    // Panel 4: thickness 10, Show 3D ON (all 12 edges as bars).
    in.strips = nullptr;
    in.barsFront = &barsFront;
    in.barsBack = &barsBack;
    in.label = 4;
    base.ox = kGutter * 2 + kPanel;    base.oy = kGutter * 2 + kPanel;
    renderPanel(img, base, in);

    writeBmp(outPath, img);
    std::printf("wrote %s (%dx%d)\n", outPath, img.w, img.h);
    return 0;
}
