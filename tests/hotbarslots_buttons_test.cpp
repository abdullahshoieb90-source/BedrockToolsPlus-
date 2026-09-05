// Host-side tests for the Hotbar Slots slot-button artwork.
//
// The launcher draws its overlay buttons in an Android window *above* the
// Minecraft surface, while the module paints the item icons natively *inside*
// that surface. The icon is therefore only visible where the button artwork
// leaves the item window transparent, so these tests rasterize the exact SVG
// markup the module hands to the launcher (fill-rule="evenodd" + strokes) and
// assert that:
//
//   * every point of the item window is transparent, in the resting and in the
//     pressed artwork (the icon shows through);
//   * the frame around the window is still drawn (the button still looks like
//     a slot);
//   * the window is exactly the rect buttonIconRect() paints the icon into;
//   * the HUD strip artwork is untouched (negative control: a solid face still
//     covers the window, which is what made the icon invisible).
//
//     g++ -std=c++20 -I src -I include tests/hotbarslots_buttons_test.cpp -o /tmp/t && /tmp/t

#include "modules/hud/hotbarslots_buttons.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using bedrocktools::hotbar::buttonIconRect;
using bedrocktools::hotbar::ButtonRect;
using bedrocktools::hotbar::iconInset;
using bedrocktools::hotbar::IconWindowEnd;
using bedrocktools::hotbar::IconWindowMax;
using bedrocktools::hotbar::IconWindowMin;
using bedrocktools::hotbar::IconWindowStart;
using bedrocktools::hotbar::slotButtonActiveSvg;
using bedrocktools::hotbar::slotButtonSvg;
using bedrocktools::hotbar::SlotRect;
using bedrocktools::hotbar::SurfaceMapping;

int failures = 0;

void expectTrue(const char* what, bool ok) {
    if (!ok) {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

void expectNear(const char* what, float actual, float expected) {
    if (std::fabs(actual - expected) > 0.001f) {
        std::printf("  FAIL %s: expected %.4f, got %.4f\n", what, expected, actual);
        ++failures;
    }
}

// ---------------------------------------------------------------------------
// A minimal SVG path rasterizer, enough for the markup hotbarslots_buttons.hpp
// emits: <path> elements with M/L/Z path data, a fill (+ optional fill-rule)
// and an optional stroke.
// ---------------------------------------------------------------------------

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct Subpath {
    std::vector<Point> points;
};

struct SvgPath {
    std::vector<Subpath> subpaths;
    bool filled = false;
    bool evenOdd = false;
    bool stroked = false;
    float strokeWidth = 1.0f;
};

std::string attribute(const std::string& tag, const std::string& name) {
    const std::string key = name + "=\"";
    const std::size_t start = tag.find(key);
    if (start == std::string::npos) return {};
    const std::size_t value = start + key.size();
    const std::size_t end = tag.find('"', value);
    if (end == std::string::npos) return {};
    return tag.substr(value, end - value);
}

std::vector<Subpath> parsePathData(const std::string& data) {
    std::vector<Subpath> subpaths;
    std::vector<float> numbers;
    char command = 0;

    const auto flush = [&]() {
        for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
            if (command == 'M' && i == 0) subpaths.push_back(Subpath{});
            if (subpaths.empty()) subpaths.push_back(Subpath{});
            subpaths.back().points.push_back(Point{numbers[i], numbers[i + 1]});
        }
        numbers.clear();
    };

    std::size_t index = 0;
    while (index < data.size()) {
        const char c = data[index];
        if (c == 'M' || c == 'L' || c == 'Z') {
            flush();
            command = c;
            ++index;
            continue;
        }
        if (c == ',' || c == ' ' || c == '\n' || c == '\t') {
            ++index;
            continue;
        }
        char* end = nullptr;
        const float value = std::strtof(data.c_str() + index, &end);
        if (end == data.c_str() + index) {
            ++index; // unknown separator, skip it
            continue;
        }
        numbers.push_back(value);
        index = static_cast<std::size_t>(end - data.c_str());
    }
    flush();
    return subpaths;
}

std::vector<SvgPath> parseSvg(const std::string& svg) {
    std::vector<SvgPath> paths;
    std::size_t start = svg.find("<path");
    while (start != std::string::npos) {
        const std::size_t end = svg.find('>', start);
        if (end == std::string::npos) break;
        const std::string tag = svg.substr(start, end - start + 1);

        SvgPath path;
        path.subpaths = parsePathData(attribute(tag, "d"));
        const std::string fill = attribute(tag, "fill");
        path.filled = fill != "none"; // the SVG default is a black fill
        path.evenOdd = attribute(tag, "fill-rule") == "evenodd";
        const std::string stroke = attribute(tag, "stroke");
        path.stroked = !stroke.empty() && stroke != "none";
        const std::string width = attribute(tag, "stroke-width");
        path.strokeWidth = width.empty() ? 1.0f : std::strtof(width.c_str(), nullptr);
        paths.push_back(std::move(path));

        start = svg.find("<path", end);
    }
    return paths;
}

// Fill rule of one path element: even-odd counts crossings, non-zero sums the
// winding (which keeps the nested subpaths of the solid frame filled).
bool fillCovers(const SvgPath& path, const Point& point) {
    if (!path.filled) return false;
    int crossings = 0;
    int winding = 0;
    for (const Subpath& subpath : path.subpaths) {
        const std::size_t count = subpath.points.size();
        for (std::size_t i = 0; i < count; ++i) {
            const Point& a = subpath.points[i];
            const Point& b = subpath.points[(i + 1) % count]; // Z closes the subpath
            if ((a.y > point.y) == (b.y > point.y)) continue;
            const float x = a.x + (point.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (x <= point.x) continue;
            ++crossings;
            winding += (b.y > a.y) ? 1 : -1;
        }
    }
    return path.evenOdd ? (crossings % 2) == 1 : winding != 0;
}

float distanceToSegment(const Point& p, const Point& a, const Point& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length2 = dx * dx + dy * dy;
    float t = length2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float cx = a.x + t * dx - p.x;
    const float cy = a.y + t * dy - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

bool strokeCovers(const SvgPath& path, const Point& point) {
    if (!path.stroked) return false;
    const float radius = path.strokeWidth * 0.5f;
    for (const Subpath& subpath : path.subpaths) {
        const std::size_t count = subpath.points.size();
        for (std::size_t i = 0; i < count; ++i) {
            const Point& a = subpath.points[i];
            const Point& b = subpath.points[(i + 1) % count];
            if (distanceToSegment(point, a, b) <= radius) return true;
        }
    }
    return false;
}

// Would the launcher paint an opaque pixel here, hiding the natively drawn
// item icon underneath?
bool opaqueAt(const std::vector<SvgPath>& paths, const Point& point) {
    for (const SvgPath& path : paths) {
        if (fillCovers(path, point) || strokeCovers(path, point)) return true;
    }
    return false;
}

void expectWindowTransparent(const char* what, const std::string& svg) {
    const std::vector<SvgPath> paths = parseSvg(svg);
    expectTrue(what, !paths.empty());

    int covered = 0;
    Point firstCovered{};
    // Sample the whole window, edges included: any opaque pixel here hides part
    // of the item icon.
    constexpr int samples = 17;
    const float span = IconWindowMax - IconWindowMin - 0.1f;
    for (int row = 0; row < samples; ++row) {
        for (int column = 0; column < samples; ++column) {
            const Point point{IconWindowMin + 0.05f + span * static_cast<float>(column) / (samples - 1.0f),
                              IconWindowMin + 0.05f + span * static_cast<float>(row) / (samples - 1.0f)};
            if (!opaqueAt(paths, point)) continue;
            if (covered == 0) firstCovered = point;
            ++covered;
        }
    }
    if (covered != 0) {
        std::printf("  FAIL %s: %d of %d window samples are opaque (first at %.3f, %.3f)\n", what, covered,
                    samples * samples, firstCovered.x, firstCovered.y);
        ++failures;
    }
}

void expectFrameDrawn(const char* what, const std::string& svg) {
    const std::vector<SvgPath> paths = parseSvg(svg);
    // Bevel, face and the ring just outside the window must still be painted,
    // otherwise the button would not read as a slot any more.
    const float offsets[] = {3.0f, 5.0f, 7.0f, 9.0f, 10.5f, 11.3f};
    for (const float offset : offsets) {
        char label[160];
        std::snprintf(label, sizeof(label), "%s is drawn at x=%.1f", what, offset);
        expectTrue(label, opaqueAt(paths, Point{offset, 32.0f}));
        std::snprintf(label, sizeof(label), "%s is drawn at y=%.1f", what, offset);
        expectTrue(label, opaqueAt(paths, Point{32.0f, offset}));
    }
}

// Maps a length painted in HUD units into the 64x64 viewBox of the artwork, so
// the icon rect and the cut-out window can be compared directly.
float toButtonLocal(float value, float buttonOrigin, float buttonSize) {
    return (value - buttonOrigin) / buttonSize * 64.0f;
}

float sizeInButtonLocal(float size, float buttonSize) {
    return size / buttonSize * 64.0f;
}

} // namespace

int main() {
    std::printf("hotbar slot button artwork\n");

    // The window matches the region the launcher's HotbarSlotOverlay clears.
    expectNear("window min = 93/512 * 64", IconWindowMin, 93.0f / 512.0f * 64.0f);
    expectNear("window max = 419/512 * 64", IconWindowMax, 419.0f / 512.0f * 64.0f);
    expectNear("window min", IconWindowMin, 11.625f);
    expectNear("window max", IconWindowMax, 52.375f);

    const std::string resting = slotButtonSvg(true);
    const std::string pressed = slotButtonActiveSvg(true);

    // The HUD strip artwork is unchanged: the solid face is still there, which
    // is exactly what hides an icon painted behind it.
    const std::string legacyResting =
        "<svg viewBox=\"0 0 64 64\" xmlns=\"http://www.w3.org/2000/svg\">\n"
        "    <path fill=\"#C6C6C6\" stroke=\"#373737\" stroke-width=\"2\" d=\"M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 "
        "L60,60 L4,60 Z\"/>\n"
        "    <path fill=\"#8B8B8B\" stroke=\"#5B5B5B\" stroke-width=\"2\" d=\"M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 "
        "L56,56 L8,56 Z\"/>\n"
        "</svg>";
    const std::string legacyPressed =
        "<svg viewBox=\"0 0 64 64\" xmlns=\"http://www.w3.org/2000/svg\">\n"
        "    <path fill=\"#C6C6C6\" stroke=\"#373737\" stroke-width=\"2\" d=\"M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 "
        "L60,60 L4,60 Z\"/>\n"
        "    <g transform=\"translate(32, 32) scale(0.85) translate(-32, -32)\">\n"
        "        <path fill=\"#8B8B8B\" stroke=\"#5B5B5B\" stroke-width=\"2\" d=\"M6,6 L58,6 L58,58 L6,58 Z M8,8 "
        "L56,8 L56,56 L8,56 Z\"/>\n"
        "    </g>\n"
        "</svg>";
    expectTrue("HUD strip artwork unchanged", slotButtonSvg(false) == legacyResting);
    expectTrue("HUD strip pressed artwork unchanged", slotButtonActiveSvg(false) == legacyPressed);

    // Negative control: a solid face covers the window, so the checks below
    // really do detect a covered icon.
    expectTrue("solid face covers the window", opaqueAt(parseSvg(legacyResting), Point{32.0f, 32.0f}));
    expectTrue("solid pressed face covers the window", opaqueAt(parseSvg(legacyPressed), Point{32.0f, 32.0f}));

    // The cut-out window is transparent in both button states.
    expectWindowTransparent("resting button window is transparent", resting);
    expectWindowTransparent("pressed button window is transparent", pressed);

    // ... and the frame around it is still drawn.
    expectFrameDrawn("resting button frame", resting);
    expectFrameDrawn("pressed button frame", pressed);

    // The pressed face shrinks towards the centre; the window must not move.
    expectTrue("resting artwork cuts the window at 11.625", resting.find("M11.625,11.625") != std::string::npos);
    expectTrue("pressed artwork cuts the window at 11.625", pressed.find("M11.625,11.625") != std::string::npos);
    expectTrue("pressed artwork still shrinks the face", pressed.find("M9.9,9.9") != std::string::npos);

    std::printf("icon placement\n");

    // The rect renderNative() paints the item into, mapped back into the button
    // artwork, must fit inside the window - otherwise the frame covers the icon.
    ButtonRect button;
    button.x = 200.0f;
    button.y = 400.0f;
    button.width = 100.0f;
    button.height = 100.0f;
    button.visible = true;

    SurfaceMapping surface;
    surface.screenWidth = 1000.0f;
    surface.screenHeight = 500.0f;
    surface.hudWidth = 500.0f;
    surface.hudHeight = 250.0f;

    const float buttonOriginX = button.x * (surface.hudWidth / surface.screenWidth);
    const float buttonOriginY = button.y * (surface.hudHeight / surface.screenHeight);
    const float buttonWidth = button.width * (surface.hudWidth / surface.screenWidth);
    const float buttonHeight = button.height * (surface.hudHeight / surface.screenHeight);

    for (const float scale : {0.2f, 0.5f, 0.8f, 1.0f}) {
        const SlotRect icon = buttonIconRect(button, surface, iconInset(scale));
        const float left = toButtonLocal(icon.x, buttonOriginX, buttonWidth);
        const float top = toButtonLocal(icon.y, buttonOriginY, buttonHeight);
        const float size = sizeInButtonLocal(icon.size, buttonWidth);

        char label[160];
        std::snprintf(label, sizeof(label), "icon at size %.1fx fits the window horizontally", scale);
        expectTrue(label, left >= IconWindowMin - 0.001f && left + size <= IconWindowMax + 0.001f);
        std::snprintf(label, sizeof(label), "icon at size %.1fx fits the window vertically", scale);
        expectTrue(label, top >= IconWindowMin - 0.001f && top + size <= IconWindowMax + 0.001f);

        // The icon is centred in the window, so nothing is clipped to one side.
        std::snprintf(label, sizeof(label), "icon at size %.1fx is centred in the window", scale);
        expectNear(label, left + size * 0.5f, (IconWindowMin + IconWindowMax) * 0.5f);
    }

    // The default icon size fills the window exactly.
    expectNear("default inset", iconInset(1.0f), IconWindowEnd - IconWindowStart);
    {
        const SlotRect icon = buttonIconRect(button, surface, iconInset(1.0f));
        expectNear("default icon left edge", toButtonLocal(icon.x, buttonOriginX, buttonWidth), IconWindowMin);
        expectNear("default icon size", sizeInButtonLocal(icon.size, buttonWidth), IconWindowMax - IconWindowMin);
    }

    // A non-square launcher button keeps a square icon inside the window.
    ButtonRect wide = button;
    wide.width = 240.0f;
    {
        const float wideWidth = wide.width * (surface.hudWidth / surface.screenWidth);
        const SlotRect icon = buttonIconRect(wide, surface, iconInset(1.0f));
        expectNear("wide button icon top edge", toButtonLocal(icon.y, buttonOriginY, buttonHeight), IconWindowMin);
        expectNear("wide button icon size", sizeInButtonLocal(icon.size, buttonHeight), IconWindowMax - IconWindowMin);
        expectNear("wide button icon is centred in the button",
                   toButtonLocal(icon.x, buttonOriginX, wideWidth) + sizeInButtonLocal(icon.size, wideWidth) * 0.5f,
                   32.0f);
    }

    // Above 1.0x the icon grows past the window and the frame covers its edges;
    // the inset is capped so it can never leave the button.
    expectNear("inset clamps at the button", iconInset(4.0f), 1.0f);
    expectNear("inset clamps at the minimum", iconInset(0.0f), 0.05f);
    expectTrue("inset above 1.0x exceeds the window", iconInset(1.5f) > IconWindowEnd - IconWindowStart);

    if (failures != 0) {
        std::printf("\n%d hotbar slot button artwork checks failed\n", failures);
        return 1;
    }
    std::printf("\nall hotbar slot button artwork checks passed\n");
    return 0;
}
