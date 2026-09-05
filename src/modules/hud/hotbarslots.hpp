#pragma once

#include "../Module.hpp"
#include "hotbarslots_layout.hpp"

#include <array>
#include <atomic>
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
    // Where the item icon of a slot is painted.
    enum class IconPlacement : int {
        Strip = 0,  // on the separate HUD strip the HUD editor positions
        Buttons = 1 // on top of the on-screen slot buttons (launcher parity)
    };

    struct ConfigSnapshot {
        std::array<bool, SlotCount> slots{};
        bool buttons = true;
        bool itemIcons = true;
        IconPlacement placement = IconPlacement::Strip;
        float iconScale = 1.0f;
        bool slotNumbers = true;
        bool highlightSelected = true;
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
    // Refreshes the cached launcher button rectangles (JNI, main-ish thread).
    void refreshButtonGeometry(const ConfigSnapshot& config);
    void clearRuntime();
    void syncOverlayButtons();
    void unregisterOverlayButtons();
    static std::string buttonId(std::size_t index);

    mutable std::mutex m_configMutex;
    std::array<std::atomic_bool, SlotCount> m_hasItem{};
    std::atomic_int m_selectedSlot{-1};

    // Screen rectangles of the launcher slot buttons, refreshed on the game
    // thread in onFrame() and consumed by the render thread. Guarded by its
    // own mutex so a slow JNI query never blocks a config read.
    mutable std::mutex m_geometryMutex;
    std::array<bedrocktools::hotbar::ButtonRect, SlotCount> m_buttonRects{};
    bedrocktools::hotbar::SurfaceMapping m_surface{};

    std::array<bool, SlotCount> m_slotEnabled{};
    bool m_buttons = true;
    bool m_itemIcons = true;
    int m_iconPlacement = static_cast<int>(IconPlacement::Buttons);
    float m_iconScale = 1.0f;
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
