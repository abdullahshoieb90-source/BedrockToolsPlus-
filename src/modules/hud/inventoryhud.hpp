#pragma once

#include "../Module.hpp"
#include "inventoryhud_layout.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Shows the player's main inventory (container slots 9-35, the 9x3 grid of
// the inventory screen) as item icons on the HUD, so the contents are visible
// without opening the inventory. The optional armor + offhand column is a
// second HUD editor element with its own position, so the grid and the
// equipment can be moved independently.
//
// It shares its plumbing with ArmorHUD and Hotbar Slots (see huditems.hpp):
// the stacks come straight from the player's FillingContainer and the icons
// are painted by the game's ItemRenderer from the HudCameraRenderer hook.
// Stack counts, armor durability numbers and bars use launcher overlay draw
// commands, like the other HUD modules' text.
class InventoryHudModule final : public Module {
public:
    static constexpr std::size_t GridSlotCount = bedrocktools::inventoryhud::GridSlotCount;
    static constexpr std::size_t EquipmentSlotCount = bedrocktools::inventoryhud::EquipmentSlotCount;

    InventoryHudModule();
    ~InventoryHudModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the shared HudCameraRenderer detour.
    void renderNative(void* context, void* client);

    // Icons are hidden while the real inventory / container screen is open;
    // the counter mirrors the ScreenStateEvent container depth.
    bool hiddenByScreen() const;

private:
    struct SlotRuntime {
        std::atomic_bool hasItem{false};
        std::atomic<std::uint8_t> count{0};
        std::atomic_int damage{0};
        std::atomic_int maxDamage{0};
    };

    struct ConfigSnapshot {
        bedrocktools::inventoryhud::GridLayout grid{};
        // Resolved anchor of the armor + offhand element: its own position when
        // the user placed it, otherwise derived from the grid.
        bedrocktools::inventoryhud::EquipmentLayout equipment{};
        bool equipmentVisible = false;
        bool stackCount = true;
        bool durability = true;
        bool armorDurability = true;
        bool hideInContainer = true;
        float countTextSize = 12.0f;
        std::uint32_t countColor = 0xFFFFFFFFu;
        float gridSize = 16.0f;
        float gridGap = 4.0f;
        float snapThreshold = 12.0f;
        std::uint32_t snapFlags = 0;
    };

    ConfigSnapshot snapshotConfig() const;
    // Both helpers expect m_configMutex to be held.
    bedrocktools::inventoryhud::GridLayout gridLayout() const;
    // Size / gap / label style of the armor column, without its anchor.
    bedrocktools::inventoryhud::EquipmentLayout equipmentStyle() const;
    // Gives the armor column a position of its own the first time it is shown.
    void placeEquipmentIfUnplaced();
    void clearRuntime();
    void storeRuntime(SlotRuntime& runtime, void* stack, void* item, bool wantDurability);

    mutable std::mutex m_configMutex;
    std::array<SlotRuntime, GridSlotCount> m_grid;
    std::array<SlotRuntime, EquipmentSlotCount> m_equipment;
    std::atomic_int m_containerDepth{0};

    float hudPosX = 24.0f;
    float hudPosY = 200.0f;
    // Anchor of the armor + offhand element. UnplacedPosition means nobody
    // positioned it yet; loadConfig() then places the column beside the grid as
    // soon as it is shown, so the two elements are independent from then on.
    float hudEquipmentPosX = bedrocktools::inventoryhud::UnplacedPosition;
    float hudEquipmentPosY = bedrocktools::inventoryhud::UnplacedPosition;
    int m_columns = static_cast<int>(bedrocktools::inventoryhud::DefaultColumns);
    float m_slotSize = 32.0f;
    float m_slotGap = 4.0f;
    bool m_showStackCount = true;
    bool m_showDurability = true;
    bool m_showEquipment = false;
    bool m_showArmorDurability = true;
    bool m_hideInContainer = true;
    float m_countTextSize = 12.0f;
    std::string m_countColor = "#FFFFFF";

    float m_gridSize = 16.0f;
    float m_gridGap = 4.0f;
    float m_snapThreshold = 12.0f;
    bool m_snapToGrid = true;
    bool m_snapToElements = true;
    bool m_snapToScreenCenter = true;
};
