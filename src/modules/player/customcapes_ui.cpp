#include "customcapes_ui.hpp"
#include "customcapes_files.hpp"

#include <algorithm>
#include <atomic>

namespace {

// Four filled strips that form a crisp border of the given thickness.
// Filled strips are used instead of the stroke Rect primitive because the
// launcher never sets the Paint stroke width for Rect commands, which would
// make the highlight a hairline (or invisible) on some builds.
void addBorder(std::vector<PLModMenu_DrawCommand>& cmds, float x, float y,
               float w, float h, float thickness, std::uint32_t color) {
    PLModMenu_DrawCommand top = {};
    top.type = PL_DRAW_RECT_FILLED;
    top.x = x; top.y = y; top.w = w; top.h = thickness; top.color = color;
    cmds.push_back(top);

    PLModMenu_DrawCommand bottom = {};
    bottom.type = PL_DRAW_RECT_FILLED;
    bottom.x = x; bottom.y = y + h - thickness; bottom.w = w; bottom.h = thickness;
    bottom.color = color;
    cmds.push_back(bottom);

    PLModMenu_DrawCommand left = {};
    left.type = PL_DRAW_RECT_FILLED;
    left.x = x; left.y = y; left.w = thickness; left.h = h; left.color = color;
    cmds.push_back(left);

    PLModMenu_DrawCommand right = {};
    right.type = PL_DRAW_RECT_FILLED;
    right.x = x + w - thickness; right.y = y; right.w = thickness; right.h = h;
    right.color = color;
    cmds.push_back(right);
}

void addText(std::vector<PLModMenu_DrawCommand>& cmds, float x, float y, float w,
             float h, float size, const std::string& text, std::uint32_t color) {
    PLModMenu_DrawCommand cmd = {};
    cmd.type = PL_DRAW_TEXT;
    cmd.x = x;
    cmd.y = y;
    cmd.w = w;
    cmd.h = h;
    cmd.size = size;
    cmd.color = color;
    cmd.text = text;
    cmds.push_back(cmd);
}

void addFilled(std::vector<PLModMenu_DrawCommand>& cmds, float x, float y,
               float w, float h, std::uint32_t color) {
    PLModMenu_DrawCommand cmd = {};
    cmd.type = PL_DRAW_RECT_FILLED;
    cmd.x = x;
    cmd.y = y;
    cmd.w = w;
    cmd.h = h;
    cmd.color = color;
    cmds.push_back(cmd);
}

// Process-wide monotonic counter so every registered preview texture id is
// unique even if a CustomCapesUi were ever re-created (registerImage()
// rejects duplicate ids and has no unregister API).
std::atomic<std::uint32_t> g_nextImageSerial{0};

} // namespace

void CustomCapesUi::addEntry(std::string fileName, std::string imageId) {
    m_entries.push_back(Entry{std::move(fileName), std::move(imageId)});
}

bool CustomCapesUi::addCapeEntry(const std::string& fileName,
                                 const std::uint8_t* canvas64x32) {
    if (!canvas64x32) return false;

    const std::vector<std::uint8_t> preview =
        customcapes::makeCapeFacePreview(canvas64x32);
    if (preview.size() != customcapes::kCapePreviewWidth *
                              customcapes::kCapePreviewHeight * 4u) {
        return false;
    }

    // imageId must be globally unique because registerImage() rejects
    // duplicates. The serial is bumped for every freshly decoded cape, so a
    // rebuild after the capes folder changed can never collide with a
    // previously registered preview. Registered images are owned by the
    // preloader for the whole process; they are deliberately not unregistered
    // (the API has no remove), but a rebuild only happens when the file list
    // actually changed, so the growth is bounded by that.
    const std::uint32_t serial =
        g_nextImageSerial.fetch_add(1, std::memory_order_relaxed);
    const std::string imageId = "bedrocktools/capes/" + std::to_string(serial) +
                                "/" + std::to_string(m_entries.size());

    if (!pl::modmenu::registerImage(
            imageId, preview, static_cast<int>(customcapes::kCapePreviewWidth),
            static_cast<int>(customcapes::kCapePreviewHeight))) {
        return false;
    }

    addEntry(fileName, imageId);
    return true;
}

CustomCapesUi::Layout CustomCapesUi::computeLayout(std::size_t entryCount) {
    Layout layout;
    if (entryCount == 0) return layout;

    const std::size_t cols =
        std::min<std::size_t>(static_cast<std::size_t>(kColumns), entryCount);
    const std::size_t rows = (entryCount + cols - 1) / cols;

    const float panelW = 2.0f * kPaddingX +
                         static_cast<float>(cols) * kCardW +
                         static_cast<float>(cols - 1) * kGapX;
    const float panelH = kPaddingY + kHeaderH +
                         static_cast<float>(rows) * kCardH +
                         static_cast<float>(rows - 1) * kGapY + kPaddingY;

    layout.panel = Rect{kCenter - panelW * 0.5f, kCenter - panelH * 0.5f,
                        panelW, panelH};
    layout.header = Rect{layout.panel.x, layout.panel.y, panelW, kHeaderH};
    layout.close = Rect{layout.panel.x + panelW - kPaddingX - kCloseSize,
                        layout.panel.y + (kHeaderH - kCloseSize) * 0.5f,
                        kCloseSize, kCloseSize};

    layout.cards.reserve(entryCount);
    for (std::size_t i = 0; i < entryCount; ++i) {
        const std::size_t col = i % cols;
        const std::size_t row = i / cols;
        const float x = layout.panel.x + kPaddingX +
                        static_cast<float>(col) * (kCardW + kGapX);
        const float y = layout.panel.y + kPaddingY + kHeaderH +
                        static_cast<float>(row) * (kCardH + kGapY);
        layout.cards.push_back(Rect{x, y, kCardW, kCardH});
    }
    return layout;
}

void CustomCapesUi::buildDrawCommands(std::vector<PLModMenu_DrawCommand>& cmds,
                                      int selectedIndex) const {
    if (m_entries.empty()) return;

    const Layout layout = this->layout();

    // Panel + header background.
    addFilled(cmds, layout.panel.x, layout.panel.y, layout.panel.w,
              layout.panel.h, kPanelColor);
    addFilled(cmds, layout.header.x, layout.header.y, layout.header.w,
              layout.header.h, kHeaderColor);
    addBorder(cmds, layout.panel.x, layout.panel.y, layout.panel.w,
              layout.panel.h, 1.5f, kPanelBorderColor);

    // Title. The text box keeps w/h > 0 so the launcher centers the label.
    addText(cmds, layout.header.x + 8.0f, layout.header.y, layout.header.w - 40.0f,
            layout.header.h, 18.0f, "Custom Capes", kTextColor);

    // Close button.
    addFilled(cmds, layout.close.x, layout.close.y, layout.close.w,
              layout.close.h, kCloseColor);
    addText(cmds, layout.close.x, layout.close.y, layout.close.w, layout.close.h,
            15.0f, "X", kTextColor);

    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        const Rect card = layout.cards[i];
        const Entry& entry = m_entries[i];
        const bool selected = static_cast<int>(i) == selectedIndex;

        addFilled(cmds, card.x, card.y, card.w, card.h, kCardColor);

        if (selected) {
            // Soft outer glow (thicker, translucent) plus a bright inner
            // border so the active cape is obvious at a glance.
            addBorder(cmds, card.x - 3.5f, card.y - 3.5f, card.w + 7.0f,
                      card.h + 7.0f, 1.5f, kSelectionGlowColor);
        }

        const float thumbX = card.x + (kCardW - kThumbW) * 0.5f;
        const float thumbY = card.y + 6.0f;
        if (entry.imageId.empty()) {
            // The "None" card has no thumbnail; the label is shown centered
            // in the thumbnail area instead of being repeated below it.
            addText(cmds, thumbX, thumbY, kThumbW, kThumbH, 18.0f, "None",
                    kTextColor);
        } else {
            PLModMenu_DrawCommand image = {};
            image.type = PL_DRAW_IMAGE;
            image.x = thumbX;
            image.y = thumbY;
            image.w = kThumbW;
            image.h = kThumbH;
            image.imageId = entry.imageId;
            cmds.push_back(image);

            addText(cmds, card.x + 4.0f, thumbY + kThumbH + 2.0f, card.w - 8.0f,
                    20.0f, 13.0f, entry.fileName, kTextColor);
        }

        if (selected) {
            addBorder(cmds, card.x - 1.5f, card.y - 1.5f, card.w + 3.0f,
                      card.h + 3.0f, 2.5f, kSelectionBorderColor);
        }
    }
}

int CustomCapesUi::hitTest(float x, float y) const {
    if (m_entries.empty()) return kHitOutside;

    const Layout layout = this->layout();
    if (layout.close.contains(x, y)) return kHitClose;
    if (!layout.panel.contains(x, y)) return kHitOutside;

    for (std::size_t i = 0; i < layout.cards.size(); ++i) {
        if (layout.cards[i].contains(x, y)) return static_cast<int>(i);
    }
    return kHitOutside;
}
