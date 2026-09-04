#pragma once

#include "../Module.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Hotbar Slots: on-screen 1-9 buttons for selecting hotbar slots.
//
// Native port of the LeviLauncher "Hotbar Slot" inbuilt mod (see
// HotbarSlotOverlay / HotbarSlotMod in LeviLaunchroid). Each visible slot is a
// launcher overlay button; pressing one selects the matching hotbar slot
// through the same key path the launcher overlay uses (MoreButtonsMod.sendKey
// with '1'..'9' down/up).
//
// Mapping from the LeviLaunchroid original:
//   HotbarSlotOverlay (per-slot button 1..9) -> overlay buttons registered in
//     syncOverlayButtons(), one pl::modmenu::ButtonBuilder per visible slot.
//   InbuiltModManager hotbar_slot_enabled_N   -> m_slots[N].enabled
//     (menu toggles m_slot1..m_slot9).
//   Per-slot size/opacity                    -> m_slots[N].sizeDp / opacity
//     (menu sliders m_slotNSize / m_slotNOpacity nested under m_slotN).
//   hotbar_item_icons + HotbarSlotMod.hasItem -> m_itemIcons +
//     hotbarHasItem(), read straight from the player inventory memory with the
//     same offsets InventoryAccess uses. Occupied slots are highlighted;
//     rendering true item icons onto the buttons (like ArmorHudModule does for
//     armor) is left as future work.
// (Not marked final so host tests can subclass it with a recording key
// sender; production behavior is unchanged.)
class HotbarSlotsModule : public Module {
public:
    static constexpr int SlotCount = 9;
    static constexpr float DefaultSizeDp = 56.0f;
    static constexpr float MinSizeDp = 20.0f;
    static constexpr float MaxSizeDp = 100.0f;

    struct SlotButtonDef {
        int slot = 1; // 1..9
        std::string id;
        std::string label;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        std::uint32_t textColor = 0xFF373737u;
        std::uint32_t activeTextColor = 0xFF1F1F1Fu;
        bool occupied = false; // last hotbarHasItem() poll result
    };

    HotbarSlotsModule();
    ~HotbarSlotsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Selects a hotbar slot (1..9). Shared entry point for the overlay-button
    // callback and host tests.
    void selectSlot(int slot);

    static HotbarSlotsModule* instance();

    // Overlay definitions for the currently visible slots. Pure function of
    // the config + last occupancy poll, so host tests can verify it without a
    // launcher.
    std::vector<SlotButtonDef> buildButtonDefs() const;

    // True when hotbar slot `slot` (0-based, matching the `slot - 1`
    // convention HotbarSlotMod uses) holds at least one item. Reads the
    // player inventory container directly, mirroring InventoryAccess. Public
    // so host tests can exercise it with a crafted buffer.
    static bool hotbarHasItem(const void* player, int slot);

protected:
    // Platform seam: feeds one Bedrock key code down/up into the game. The
    // production path is JNI into the launcher's MoreButtonsMod (the exact
    // call HotbarSlotOverlay.sendSlotKey makes); host tests override it to
    // record calls.
    virtual bool platformSendKey(int bedrockCode, bool down);

private:
    struct SlotConfig {
        bool enabled = true;
        float sizeDp = DefaultSizeDp;
        float opacity = 1.0f;
    };

    static std::string slotButtonId(int slot);
    void syncOverlayButtons();
    void unregisterOverlayButtons();
    void flushPendingKeyUp();
    static void* localPlayer();

    bool m_itemIcons = false;
    float m_buttonScale = 1.0f;
    float m_buttonOpacity = 0.85f;
    std::array<SlotConfig, SlotCount> m_slots{};
    std::array<bool, SlotCount> m_occupied{};
    int m_pendingKeyUpSlot = 0; // 1..9, 0 = none
};
