#pragma once

#include "../Module.hpp"
#include "storageesp_geometry.hpp"

#include <cstdint>

// Highlights storage blocks around the player: chests, trapped chests, ender
// chests, shulker boxes, barrels, hoppers, furnaces and dispensers.
//
// The module scans loaded blocks around the player on the client tick with a
// limited per-tick budget and remembers what it found, so the render pass is
// cheap and the world never has to be walked twice in one frame.
class StorageEspModule final : public Module {
public:
    StorageEspModule();
    ~StorageEspModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Highlight groups. Every group owns a color so a base full of different
    // containers still reads at a glance.
    bool showChests = true;
    std::uint32_t showChestsColor = 0xFFFFC34Du;
    bool showTrappedChests = true;
    std::uint32_t showTrappedChestsColor = 0xFFFF5A5Au;
    bool showEnderChests = true;
    std::uint32_t showEnderChestsColor = 0xFF9A5BE6u;
    bool showShulkerBoxes = true;
    std::uint32_t showShulkerBoxesColor = 0xFFE066FFu;
    bool showBarrels = true;
    std::uint32_t showBarrelsColor = 0xFFB07A45u;
    bool showHoppers = true;
    std::uint32_t showHoppersColor = 0xFFB7C0CCu;
    bool showFurnaces = true;
    std::uint32_t showFurnacesColor = 0xFFFF8A2Bu;
    bool showDispensers = true;
    std::uint32_t showDispensersColor = 0xFF9AA3ADu;

    // Outline pass. Colors live per group; these settings shape the boxes.
    bool outline = true;
    float lineThickness = 2.0f; // 1 = hairline, 10 = widest.

    // Optional translucent overlay on top of the boxes.
    bool fill = false;
    float fillOpacity = 0.18f;

    // Chests and hoppers are smaller than a voxel in the game; follow that
    // shape instead of painting the whole block.
    bool modelSizedBoxes = true;

    // Uses a no-depth material when the game exposes one, which is what makes
    // containers visible through terrain.
    bool throughWalls = true;

    bool rainbow = false;
    float rainbowSpeed = 0.20f; // cycles per second, clamped to 0.05..1.
    bool pulse = false;
    float pulseSpeed = 0.75f;   // cycles per second, clamped to 0.05..1.

    // World scan. `scanRadius` is how far the sweep reaches horizontally,
    // `scanHeight` how many blocks above and below the player's layer, and
    // `scanSpeed` is the radio index (see storageesp::kScanSpeedNames).
    int scanRadius = 24;
    int scanHeight = 16;
    int scanSpeed = storageesp::kDefaultScanSpeed;
    int maxBoxes = 64;

    // Read-only view of the highlight groups for the pure scan/render helpers.
    storageesp::CategoryFilter filter() const {
        return {showChests, showTrappedChests, showEnderChests, showShulkerBoxes,
                showBarrels,  showHoppers,       showFurnaces,     showDispensers};
    }

    std::uint32_t colorFor(storageesp::StorageKind kind) const {
        switch (kind) {
            case storageesp::StorageKind::Chest: return showChestsColor;
            case storageesp::StorageKind::TrappedChest: return showTrappedChestsColor;
            case storageesp::StorageKind::EnderChest: return showEnderChestsColor;
            case storageesp::StorageKind::ShulkerBox: return showShulkerBoxesColor;
            case storageesp::StorageKind::Barrel: return showBarrelsColor;
            case storageesp::StorageKind::Hopper: return showHoppersColor;
            case storageesp::StorageKind::Furnace: return showFurnacesColor;
            case storageesp::StorageKind::Dispenser: return showDispensersColor;
            case storageesp::StorageKind::None:
            case storageesp::StorageKind::Count:
                break;
        }
        return 0xFFFFFFFFu;
    }

    void clampSettings();

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;

    void applyPatch();
};
