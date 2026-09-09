#pragma once

#include "../Module.hpp"
#include "armorhud_layout.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Shows the player's armor pieces and the offhand slot as item icons on the
// HUD, with optional durability bars and remaining/maximum durability numbers.
//
// This used to be the "Armor & Offhand" option of the Inventory HUD module; it
// is now a module of its own, with its own toggle, keybind, HUD element and
// settings, so it can be used completely without the inventory grid.
//
// It shares its plumbing with Inventory HUD and Hotbar Slots (see
// huditems.hpp): the stacks come straight from the player's equipment
// containers and the icons are painted by the game's ItemRenderer from the
// HudCameraRenderer hook. Counts, durability numbers and bars use launcher
// overlay draw commands, like the other HUD modules' text.
class ArmorModule final : public Module {
public:
    static constexpr std::size_t SlotCount = bedrocktools::armorhud::SlotCount;

    ArmorModule();
    ~ArmorModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the shared HudCameraRenderer detour.
    void renderNative(void* context, void* client);

    // Old configs stored the armor column as options of the Inventory HUD
    // module ("m_showEquipment", "hudEquipmentPosX/Y", ...). This turns such a
    // section into a config for this module, so an existing setup keeps its
    // position and style after the split.
    static nlohmann::json migratedFromInventoryHud(const nlohmann::json& inventoryHud);

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
        bedrocktools::armorhud::ArmorLayout layout{};
        bool showOffhand = true;
        bool stackCount = true;
        bool durability = true;
        bool armorDurability = true;
        bool hideInContainer = true;
        bool slotBackground = true;
        std::uint32_t slotBgColor = 0x73000000u; // "#000000" at 45%
        float countTextSize = 12.0f;
        std::uint32_t countColor = 0xFFFFFFFFu;
        float gridSize = 16.0f;
        float gridGap = 4.0f;
        float snapThreshold = 12.0f;
        std::uint32_t snapFlags = 0;
    };

    ConfigSnapshot snapshotConfig() const;
    // Expects m_configMutex to be held.
    bedrocktools::armorhud::ArmorLayout armorLayout() const;
    void clearRuntime();
    void storeRuntime(SlotRuntime& runtime, void* stack, void* item, bool wantDurability);

    mutable std::mutex m_configMutex;
    std::array<SlotRuntime, SlotCount> m_slots;
    std::atomic_int m_containerDepth{0};

    float hudPosX = 24.0f;
    float hudPosY = 200.0f;
    float m_slotSize = 32.0f;
    float m_slotGap = 4.0f;
    bool m_horizontal = false;
    bool m_showOffhand = true;
    bool m_showStackCount = true;
    bool m_showDurability = true;
    bool m_showArmorDurability = true;
    bool m_hideInContainer = true;
    bool m_slotBackground = true;
    float m_slotBgOpacity = 0.45f;
    std::string m_slotBgColor = "#000000";
    float m_countTextSize = 12.0f;
    std::string m_countColor = "#FFFFFF";

    float m_gridSize = 16.0f;
    float m_gridGap = 4.0f;
    float m_snapThreshold = 12.0f;
    bool m_snapToGrid = true;
    bool m_snapToElements = true;
    bool m_snapToScreenCenter = true;
};
