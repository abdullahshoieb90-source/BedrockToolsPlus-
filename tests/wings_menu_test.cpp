// Host tests for the Wings launcher menu page.
//
// The menu is built as a v2 config schema (JSON) that the launcher parses, so
// the only way to know a node will actually show up is to build the schema and
// read it back. A silently mis-serialized node is invisible in the game and
// leaves no trace anywhere else, which is exactly the kind of bug that survives
// to a release.
//
// The assertions run against the serialized string rather than a parsed tree so
// the test needs nothing beyond the module headers: the host JSON fake in
// tests/fakejson covers the module's own config needs but is not a parser.
//
//     ./scripts/run_tests.sh
//
// Build by hand with:
//     g++ -std=c++20 -I src -I include -I tests/fakepl
//         tests/wings_menu_test.cpp -o /tmp/wings_menu && /tmp/wings_menu

#include "modules/visual/wings_art.hpp"
#include "modules/visual/wings_menu.hpp"

#include <cstdio>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// Brace/quote balance walk. Catches a schema that is not even parseable, which
// would make the launcher reject the page and fall back with no explanation.
bool isStructurallySound(const std::string& text) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth < 0) return false;
        }
    }
    return !inString && depth == 0;
}

// Extracts the object that follows `"key":"<key>"` far enough to inspect its
// fields. Small and predictable because the schema writer emits each node as one
// flat object.
std::string nodeFor(const std::string& schema, const std::string& key,
                    const char* field = "key") {
    const std::string needle = std::string("\"") + field + "\":\"" + key + "\"";
    std::size_t at = schema.find(needle);
    if (at == std::string::npos) return {};
    // Walk back to the start of the enclosing object.
    std::size_t begin = schema.rfind('{', at);
    if (begin == std::string::npos) begin = 0;
    // Walk forward to its matching close brace.
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t i = begin; i < schema.size(); ++i) {
        const char c = schema[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return schema.substr(begin, i - begin + 1);
        }
    }
    return {};
}

}  // namespace

int main() {
    namespace wings = bedrocktools::modules::wings;

    std::printf("\n=== menu schema ===\n");

    const std::string schema = wings::wingMenuSchema(1 /* angel */, 1.0f);
    check(!schema.empty(), "the module produces a menu schema");
    check(isStructurallySound(schema), "the schema is well formed JSON (balanced and closed)");

    auto findNode = [&schema](const std::string& key) { return nodeFor(schema, key); };
    // Info blocks are display-only and carry no config key, so they are found by
    // their node id instead.
    auto findInfoNode = [&schema](const std::string& id) { return nodeFor(schema, id, "id"); };

    // --- Style picker -------------------------------------------------------
    const std::string picker = findNode("m_wingStyle");
    check(!picker.empty(), "the menu still offers the wing style picker");
    check(contains(picker, "\"type\":\"choice\""),
          "the picker is a choice control, not a free text field");
    check(contains(picker, "\"current_value\":\"angel\""),
          "the picker shows the style that is actually selected");

    {
        bool everyStyleOffered = true;
        bool everyStyleDescribed = true;
        bool descriptionIsNotJustTheName = true;
        for (int i = 0; i < wings::kWingStyleCount; ++i) {
            const wings::WingDesign& design = wings::kWingDesigns[i];
            const std::string value = std::string("\"value\":\"") + design.id + "\"";
            if (!contains(picker, value)) everyStyleOffered = false;

            const std::string description = std::string("\"description\":\"") + design.shape +
                                            "\"";
            if (!contains(picker, description)) everyStyleDescribed = false;
            if (std::string(design.shape) == design.label) descriptionIsNotJustTheName = false;
        }
        check(everyStyleOffered, "every wing style is offered in the picker");
        check(everyStyleDescribed, "every style option describes its shape");
        check(descriptionIsNotJustTheName,
              "the shape description is not just the style name repeated");
        check(countOccurrences(picker, "\"value\":\"") ==
                  static_cast<std::size_t>(wings::kWingStyleCount),
              "the picker offers exactly one option per style");
    }

    // --- Silhouette preview -------------------------------------------------
    const std::string preview = findInfoNode("wing_preview");
    check(!preview.empty(), "the menu carries a shape preview node");
    check(contains(preview, "\"type\":\"info\""),
          "the preview is an info block (display only, never editable)");
    check(contains(preview, "Shape: Angel"),
          "the preview is titled with the style it is drawing");

    const std::string art = wings::wingStyleArt(1);
    check(!art.empty(), "the selected style has a silhouette to show");
    check(contains(preview, wings::wingStyleArt(1).substr(0, 20)),
          "the preview really contains the drawn shape, not a placeholder");

    // --- Flap speed ---------------------------------------------------------
    const std::string speed = findNode("m_flapSpeed");
    check(!speed.empty(), "the flap speed slider survives the schema rewrite");
    check(contains(speed, "\"type\":\"slider_float\""), "the flap speed is a float slider");
    check(contains(speed, "\"current_value\":\"1.000000\""),
          "the flap speed slider shows the value in use");

    // --- The preview follows the selection ----------------------------------
    std::printf("\n=== preview follows the selection ===\n");
    {
        std::vector<std::string> arts;
        bool allNonEmpty = true;
        for (int i = 0; i < wings::kWingStyleCount; ++i) {
            arts.push_back(wings::wingStyleArt(i));
            if (arts.back().empty()) allNonEmpty = false;
        }
        check(allNonEmpty, "every style has a silhouette");

        // Distinct designs must not collapse to the same picture, otherwise the
        // preview would be decoration rather than information.
        bool allDistinct = true;
        for (std::size_t i = 0; i < arts.size() && allDistinct; ++i) {
            for (std::size_t j = i + 1; j < arts.size(); ++j) {
                if (arts[i] == arts[j]) {
                    allDistinct = false;
                    break;
                }
            }
        }
        check(allDistinct, "no two styles draw the same silhouette");

        const std::string dragonSchema = wings::wingMenuSchema(0, 1.0f);
        const std::string dragonPreview = nodeFor(dragonSchema, "wing_preview", "id");
        check(contains(dragonPreview, wings::wingStyleArt(0).substr(0, 20)),
              "selecting another style redraws the preview for it");
        check(!contains(dragonPreview, wings::wingStyleArt(1).substr(0, 20)),
              "the preview is not a single hard-coded picture");
        check(contains(dragonPreview, "Shape: Dragon"),
              "the preview title follows the selection too");
    }

    // --- Character safety ---------------------------------------------------
    std::printf("\n=== preview is safe to render ===\n");
    {
        bool asciiOnly = true;
        bool allLinesDrawn = true;
        bool withinBounds = true;
        for (int i = 0; i < wings::kWingStyleCount; ++i) {
            const std::string a = wings::wingStyleArt(i);
            int lines = 1;
            int widest = 0;
            int current = 0;
            bool lineHasInk = false;
            bool anyInk = false;
            for (unsigned char c : a) {
                if (c == '\n') {
                    ++lines;
                    if (current > widest) widest = current;
                    if (lineHasInk) anyInk = true;
                    current = 0;
                    lineHasInk = false;
                    continue;
                }
                ++current;
                if (c != ' ') lineHasInk = true;
                if (c < 0x20 || c > 0x7E) asciiOnly = false;
            }
            if (current > widest) widest = current;
            if (lineHasInk) anyInk = true;
            if (!anyInk) allLinesDrawn = false;
            if (lines > wings::kWingArtRows || widest > wings::kWingArtCols) withinBounds = false;
        }
        // The launcher's menu font is not guaranteed to be monospaced or to have
        // Unicode block glyphs; a missing glyph draws as an empty box and would
        // shred the picture, so the art stays inside printable ASCII.
        check(asciiOnly, "the silhouette uses printable ASCII only (no missing-glyph boxes)");
        check(allLinesDrawn, "every style actually draws something");
        check(withinBounds, "the silhouette fits the grid the menu reserves for it");
    }

    // --- Overlay draw commands ----------------------------------------------
    // The silhouette is drawn on screen as filled rects around the launcher's
    // screen-centre origin. Rebuild the command list the way the module does and
    // check it is something the launcher will actually paint.
    std::printf("\n=== overlay commands ===\n");
    {
        auto buildCommands = [](int style) {
            std::vector<std::pair<float, float>> cells;  // x, y of each run
            float widths[64] = {};
            const std::string a = wings::wingStyleArt(style, 41, 15);
            constexpr float kCenter = -20000.0f;
            constexpr float kCellW = 6.0f;
            int row = 0;
            std::size_t begin = 0;
            while (begin <= a.size() && row < 15) {
                const std::size_t nl = a.find('\n', begin);
                const std::string line =
                    a.substr(begin, nl == std::string::npos ? std::string::npos : nl - begin);
                const float rowX = kCenter - static_cast<float>(line.size()) * kCellW * 0.5f;
                int runStart = -1;
                for (std::size_t i = 0; i <= line.size(); ++i) {
                    const char c = (i < line.size()) ? line[i] : ' ';
                    if (c == '#' && runStart < 0) runStart = static_cast<int>(i);
                    if (c != '#' && runStart >= 0) {
                        cells.emplace_back(rowX + static_cast<float>(runStart) * kCellW,
                                           kCenter - 45.0f + static_cast<float>(row) * kCellW);
                        ++widths[0];
                        runStart = -1;
                    }
                }
                ++row;
                if (nl == std::string::npos) break;
                begin = nl + 1;
            }
            return cells;
        };

        const auto angelCells = buildCommands(1);
        check(!angelCells.empty(), "the selected style submits draw cells");
        check(angelCells.size() < 4096, "the overlay stays inside the launcher's command cap");

        float minX = angelCells.front().first;
        float maxX = angelCells.front().first;
        bool finite = true;
        bool leftOfCenter = false;
        bool rightOfCenter = false;
        for (const auto& [x, y] : angelCells) {
            if (!std::isfinite(x) || !std::isfinite(y)) finite = false;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (x <= -20000.0f) leftOfCenter = true;
            if (x > -20000.0f) rightOfCenter = true;
        }
        check(finite, "every cell has a finite position");
        check(leftOfCenter && rightOfCenter,
              "the silhouette straddles the launcher's screen-centre origin");
        check(maxX - minX < 260.0f, "the silhouette fits a narrow phone screen");
        check(buildCommands(0) != angelCells, "switching style changes what is drawn");
    }

    // --- Out of range input -------------------------------------------------
    std::printf("\n=== edge cases ===\n");
    check(wings::wingStyleArt(-1).empty(), "a negative style index produces no art");
    check(wings::wingStyleArt(wings::kWingStyleCount).empty(),
          "an out of range style index produces no art");
    check(wings::wingStyleArt(0, 2, 2).empty(), "a grid too small to draw in is rejected");
    check(!wings::wingStyleArt(0, 8, 3).empty(), "a small but usable grid still draws");

    // An out of range index must not produce a broken page; it falls back to the
    // first style.
    const std::string fallback = wings::wingMenuSchema(-7, 1.0f);
    check(contains(fallback, "\"current_value\":\"dragon\""),
          "an out of range style falls back to the default instead of an empty page");

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("all wings menu checks passed (%d checks)\n", g_checks);
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
