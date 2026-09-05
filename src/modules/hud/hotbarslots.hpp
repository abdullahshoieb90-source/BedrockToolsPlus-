#pragma once

#include "../Module.hpp"
#include "hotbarslots_autobuild.hpp"
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
    bool onKeyEvent(int key, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the HudCameraRenderer detour.
    void renderNative(void* context, void* client);

    // Called from the LocalPlayerTickEvent subscription: places a block while
    // an armed slot button is held down.
    void onLocalPlayerTick(void* player);

private:
    struct ConfigSnapshot {
        std::array<bool, SlotCount> slots{};
        bool buttons = true;
        bool itemIcons = true;
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
        bedrocktools::hotbar::AutoBuildSettings autoBuild{};
    };

    ConfigSnapshot snapshotConfig() const;
    static bool resolveAutoBuildFunctions();
    bool placeHeldBlock(void* player, std::size_t slot);
    void clearRuntime();
    void syncOverlayButtons();
    void unregisterOverlayButtons();
    static std::string buttonId(std::size_t index);

    mutable std::mutex m_configMutex;
    std::array<std::atomic_bool, SlotCount> m_hasItem{};
    // True when the slot holds an item that places a block (auto build only
    // triggers for those). Written on the render thread, read on the game
    // thread.
    std::array<std::atomic_bool, SlotCount> m_hasBlock{};
    std::atomic_int m_selectedSlot{-1};

    std::array<bool, SlotCount> m_slotEnabled{};
    bool m_buttons = true;
    bool m_itemIcons = true;
    bool m_slotNumbers = true;
    bool m_highlightSelected = true;
    bool m_vertical = false;

    // ---- Auto Build ----------------------------------------------------
    // Holding an armed slot button keeps placing the block it holds, so the
    // player never has to reach for the build button. A short tap still just
    // selects the slot.
    bool m_autoBuild = false;
    std::array<bool, SlotCount> m_autoBuildSlots{};
    float m_autoBuildHoldDelay = 250.0f;
    float m_autoBuildInterval = 100.0f;
    bedrocktools::hotbar::AutoBuildState m_autoBuildState{};
    std::mutex m_autoBuildMutex;

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
