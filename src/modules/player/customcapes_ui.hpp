#pragma once

// In-game cape picker (grid of image previews) for the Custom Capes module.
//
// The launcher mod menu has no config type that could render a grid of PNG
// thumbnails, so the picker is drawn by the module itself through the
// launcher's HUD overlay:
//
//   * each cape PNG is decoded, resampled onto the 64x32 cape canvas and
//     then UV-cropped to the visible outer face (customcapes::makeCapeFace-
//     Preview) before it is handed to pl::modmenu::registerImage — the
//     overlay's Image draw command stretches the whole registered bitmap,
//     so the crop has to happen inside the texture.
//   * on every frame the visible grid is submitted as DrawCommands
//     (background, thumbnail Image commands, labels) with a bright border
//     around the card that matches m_selectedIndex.
//   * taps are hit-tested against the same layout that was drawn; a tap on
//     a card selects it, a tap on the X closes the picker, anything outside
//     the panel falls through to the game.
//
// The layout is deterministic and pure (no screen size is needed: the panel
// is anchored to the screen center with the launcher's "-20000" convention),
// so it is unit-testable on the host.

#include "modules/ModuleRegistry.hpp" // PLModMenu_DrawCommand / PL_DRAW_*
#include <cstdint>
#include <string>
#include <vector>

class CustomCapesUi {
public:
    // hitTest() results: -1 = nothing (tap reaches the game), index >= 0 =
    // picker entry (0 == "None"), kHitClose = the panel's close button.
    static constexpr int kHitOutside = -1;
    static constexpr int kHitClose = -2;

    struct Entry {
        std::string fileName; // "None" for the first entry
        std::string imageId;  // registered preview; empty for "None"
    };

    struct Rect {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        bool contains(float px, float py) const {
            return px >= x && px <= x + w && py >= y && py <= y + h;
        }
    };

    // Layout constants (screen-space pixels).
    static constexpr int kColumns = 4;
    static constexpr float kCardW = 96.0f;
    static constexpr float kCardH = 158.0f;
    static constexpr float kThumbW = 80.0f;
    static constexpr float kThumbH = 128.0f;
    static constexpr float kGapX = 10.0f;
    static constexpr float kGapY = 10.0f;
    static constexpr float kPaddingX = 12.0f;
    static constexpr float kPaddingY = 12.0f;
    static constexpr float kHeaderH = 40.0f;
    static constexpr float kCloseSize = 26.0f;
    static constexpr float kCenter = -20000.0f; // launcher screen-center anchor

    // Colors (AARRGGBB).
    static constexpr std::uint32_t kPanelColor = 0xF2101A24u;
    static constexpr std::uint32_t kHeaderColor = 0xF21B2A3Au;
    static constexpr std::uint32_t kPanelBorderColor = 0xFF4A90D9u;
    static constexpr std::uint32_t kCardColor = 0xB02A3A4Du;
    static constexpr std::uint32_t kCloseColor = 0xE0C0392Bu;
    static constexpr std::uint32_t kTextColor = 0xFFFFFFFFu;
    static constexpr std::uint32_t kSelectionGlowColor = 0x4DFFC93Cu;
    static constexpr std::uint32_t kSelectionBorderColor = 0xFFFFD54Fu;

    CustomCapesUi() = default;

    void clear() {
        m_entries.clear();
        m_visible = false;
    }

    bool empty() const { return m_entries.empty(); }
    std::size_t entryCount() const { return m_entries.size(); }
    const std::vector<Entry>& entries() const { return m_entries; }

    bool visible() const { return m_visible; }
    void setVisible(bool value) { m_visible = value; }

    // Grid entry bookkeeping. addEntry() is used directly by the host tests;
    // addCapeEntry() additionally builds the UV-cropped RGBA preview from a
    // resampled 64x32 canvas and registers it with the overlay's texture
    // loader (pl::modmenu::registerImage). Index 0 (the "None" card) is
    // inserted by the module before the cape entries.
    void addEntry(std::string fileName, std::string imageId);
    bool addCapeEntry(const std::string& fileName, const std::uint8_t* canvas64x32);

    // Deterministic layout for the current entry count.
    struct Layout {
        Rect panel;
        Rect header;
        Rect close;
        std::vector<Rect> cards; // one per entry, same order as entries()
    };
    static Layout computeLayout(std::size_t entryCount);
    Layout layout() const { return computeLayout(m_entries.size()); }

    void buildDrawCommands(std::vector<PLModMenu_DrawCommand>& cmds,
                           int selectedIndex) const;
    int hitTest(float x, float y) const;

private:
    std::vector<Entry> m_entries;
    bool m_visible = false;
};
