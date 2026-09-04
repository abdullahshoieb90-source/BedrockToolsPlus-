#pragma once

#include "../Module.hpp"
#include "../../launcher/ExternalButtonRefresh.hpp"
#include "hotbarslots_layout.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

// Port of the LeviLauncher "Hotbar Slot" built-in mod.
//
// The launcher version adds nine on-screen buttons that select hotbar slots
// 1-9 and, when the item-icon option is on, paints the item that currently
// occupies the slot on top of the button. Inside BedrockToolsPlus the same
// feature is expressed with the tools this mod already has:
//
//   * the buttons are registered as launcher overlay buttons (the same
//     mechanism the Command Hotkey module uses), each one carrying the
//     Android key code of its slot so the launcher forwards the selection to
//     Minecraft;
//   * the item icons are drawn natively from the HudCameraRenderer hook using
//     the game's ItemRenderer, like the ArmorHUD module does, so they can be
//     placed and sized independently in the HUD editor.
//   * optionally ("Draw Icons On Buttons"), each icon is painted inside its
//     own on-screen button instead of the strip. The button rectangles are
//     read from the launcher's overlay manager over JNI and refreshed a few
//     times per second; any slot whose button is missing, hidden, or has no
//     size yet automatically falls back to the strip rectangle.
class HotbarSlotsModule final : public Module {
public:
    static constexpr std::size_t SlotCount = bedrocktools::hotbar::SlotCount;

    HotbarSlotsModule();
    ~HotbarSlotsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the HudCameraRenderer detour.
    void renderNative(void* context, void* client);

private:
    struct ConfigSnapshot {
        std::array<bool, SlotCount> slots{};
        bool buttons = true;
        bool itemIcons = true;
        bool iconsOnButtons = false;
        bool slotNumbers = true;
        bool highlightSelected = true;
        std::array<bedrocktools::launcher::ButtonGeometry, SlotCount> buttonGeometry{};
        std::array<bool, SlotCount> buttonGeometryValid{};
        bedrocktools::hotbar::StripLayout layout{};
        float buttonScale = 1.0f;
        float numberTextSize = 12.0f;
        std::uint32_t numberColor = 0xFFFFFFFFu;
        std::uint32_t highlightColor = 0x66FFFFFFu;
        float gridSize = 16.0f;
        float gridGap = 4.0f;
        float snapThreshold = 12.0f;
        std::uint32_t snapFlags = 0;
    };

    ConfigSnapshot snapshotConfig() const;
    void clearRuntime();
    void refreshButtonGeometryIfDue();
    void syncOverlayButtons();
    void unregisterOverlayButtons();
    static std::string buttonId(std::size_t index);

    mutable std::mutex m_configMutex;
    std::array<std::atomic_bool, SlotCount> m_hasItem{};
    std::atomic_int m_selectedSlot{-1};

    std::array<bool, SlotCount> m_slotEnabled{};
    bool m_buttons = true;
    bool m_itemIcons = true;
    bool m_iconsOnButtons = false;
    std::array<bedrocktools::launcher::ButtonGeometry, SlotCount> m_buttonGeometry{};
    std::array<bool, SlotCount> m_buttonGeometryValid{};
    std::chrono::steady_clock::time_point m_geometryRefreshAt{};
    bool m_slotNumbers = true;
    bool m_highlightSelected = true;
    bool m_vertical = false;

    float hudPosX = 24.0f;
    float hudPosY = 520.0f;
    float m_slotSize = 32.0f;
    float m_slotGap = 4.0f;
    float m_buttonScale = 1.0f;
    float m_numberTextSize = 12.0f;
    std::string m_numberColor = "#FFFFFF";
    std::string m_highlightColor = "#FFFFFF";
    float m_highlightOpacity = 0.4f;

    float m_gridSize = 16.0f;
    float m_gridGap = 4.0f;
    float m_snapThreshold = 12.0f;
    bool m_snapToGrid = true;
    bool m_snapToElements = true;
    bool m_snapToScreenCenter = true;
};
