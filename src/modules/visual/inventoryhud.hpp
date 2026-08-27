#pragma once

// InventoryHUD — draw the player's regular inventory on screen at all times.
//
// What it shows
//   The module reads the local Player's PlayerInventory every tick and
//   redraws its 36 slots (9 hotbar + 27 main) as a 9-column grid in the
//   HUD overlay. The hotbar and the main inventory are both rendered
//   (the hotbar first, then the 27 main slots), exactly the same way
//   vanilla draws them inside the inventory screen. The hotbar appears
//   separately in vanilla too — there is no in-game layout that hides
//   it — so rendering it here stays consistent with how the player
//   sees their own items in vanilla.
//
// What it does NOT show
//   - Armor slots (helmet, chest, legs, boots)
//   - Offhand slot
//   - The crafting 2x2 input (not part of PlayerInventory on survival)
//   - Creative-mode inventory tabs / search / scroll position
//   - Container contents (chests, shulkers, furnace, etc.) — those live
//     on a separate ContainerModel and are out of scope for this module.
//
// Architecture & SDK use
//   - Player comes from ClientInstance::localPlayer() through the existing
//     ClientInstanceGetLocalPlayer signature (no new signature required).
//   - The inventory buffer is NOT reached through hardcoded offsets
//     (Player::mInventory / PlayerInventory::mItems / ItemStack::mCount
//     have all been observed to shift between Bedrock builds). Instead
//     the module scans pointer-sized words inside the Player memory
//     region, tests each candidate pointer against the same
//     `itemStackHasItem` predicate the rest of the codebase uses, and
//     caches the first (base, stride) pair that yields 36 well-formed
//     ItemStack slots. The cache is reused every tick; the scan
//     re-runs once per m_rescanIntervalMs when the cache is empty so a
//     freshly loaded world is picked up without a relaunch. When
//     Player::getSelectedItem resolves, the scan prefers candidates
//     whose slot 0..8 matches the returned ItemStack address, which
//     avoids confusing nearby same-shaped buffers (the UI container
//     preview, etc.) with the real inventory.
//   - ItemStack::mCount is not a fixed offset either: the count byte
//     is probed at a small set of candidate offsets within the stack
//     and the first sensible value (1..127, only on slots that pass
//     itemStackHasItem) wins. The discovered count offset is cached
//     alongside the inventory base+stride.
//   - The 3D item icon uses ItemStackBaseLoadItem to materialize a real
//     ItemStack from NBT and ItemRendererRenderItemGroup to draw it. Both
//     are signatures the project already exposes.
//
// HUD editor
//   All the visual knobs (width, height, scale, slot size, slot spacing,
//   background, background opacity, border, border opacity, item count,
//   durability bar, empty slots) are exposed as the module's own
//   config keys and appear in the launcher menu like any other HUD
//   module. The module owns its position through hudPosX/hudPosY like
//   Break Indicator / Effect Display. isHudModule = true so the editor
//   grabs the panel.
//
// Safety
//   - Every pointer read from the game is checked against a minimum
//     address (>= kMinValidPointer) before being dereferenced.
//   - Every ItemStack read is validated through itemStackHasItem, the
//     same predicate the rest of the codebase uses for stack validity.
//   - If Player / inventory / any slot is null the module simply
//     skips drawing that frame; it never dereferences a dangling
//     pointer, even during world unload (ClientInstance::current()
//     returns null at that point).
//   - The pointer walk during the scan only dereferences values that
//     look like live heap pointers (> kMinValidPointer, canonically
//     aligned); non-pointers (integers, floats, tagged booleans) are
//     skipped without touching them.
//   - Submitting an empty draw command list when there is no data
//     makes the overlay disappear, so the HUD cannot get stuck on
//     screen across a world transition.

#include "modules/Module.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace bedrocktools::sdk { class Player; }

class InventoryHUDModule : public Module {
public:
    InventoryHUDModule();
    ~InventoryHUDModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // -------------------------------------------------------------------
    // HUD editor & layout
    // -------------------------------------------------------------------
    float hudPosX = 40.0f;
    float hudPosY = 40.0f;
    bool isHudModule = true;

    // Backing pixel size of the panel. Both are independent and
    // re-distribute the slots when they change. The slot grid is
    // computed inside the panel; resizing either dimension simply
    // re-flows the layout, nothing is clipped off the panel.
    float m_width = 360.0f;
    float m_height = 140.0f;

    // Uniform scale multiplier applied on top of m_width / m_height.
    // A value of 2 doubles every pixel of the panel, including the
    // slot size, the spacing and the item text.
    float m_scale = 1.0f;

    // Per-slot size in panel pixels. The actual draw size is
    //   m_slotSize * m_scale
    // so the launcher can keep the slot count fixed while the user
    // dials in the look.
    float m_slotSize = 32.0f;

    // Pixels between two slots (both horizontally and vertically).
    float m_slotSpacing = 4.0f;

    // Background & border colors in #AARRGGBB layout (alpha in the
    // high byte). Background opacity below is a 0..1 multiplier on
    // the alpha channel so a single color picks every level of
    // transparency.
    uint32_t m_backgroundColor = 0xF0101820;
    bool m_background = true;
    float m_backgroundOpacity = 0.55f;

    bool m_border = true;
    float m_borderOpacity = 0.9f;
    uint32_t m_borderColor = 0xFF303030;
    float m_borderThickness = 1.0f;

    // Empty slots are still drawn (the vanilla slot frame) so the
    // HUD reads as a familiar grid even when nothing is in the
    // inventory. Disabling this option hides empty cells entirely.
    bool m_showEmptySlots = true;
    uint32_t m_emptySlotColor = 0x80302838;

    // Item count (the small "64" in the corner of a stack). Disabled
    // hides the label; the icon is still drawn.
    bool m_showItemCount = true;
    float m_itemCountSize = 12.0f;
    uint32_t m_itemCountColor = 0xFFFFFFFF;

    // Durability bar under items that have one. Items without
    // durability (max damage == 0) never get a bar regardless.
    bool m_showDurability = true;
    float m_durabilityHeight = 2.0f;
    uint32_t m_durabilityColor = 0xFF55FF55;

    // First slot drawn. 0 draws the full inventory (vanilla layout:
    // hotbar at the bottom, main grid at the top). 9 skips the
    // hotbar.
    int m_slotOffset = 0;
    int m_slotCount = 36;

    // Re-read cadence in milliseconds. PlayerInventory only changes
    // on a server tick, so a 50 ms refresh is plenty fast for a
    // visible HUD but keeps the work well below the per-frame budget
    // even on a 4-core device.
    int m_refreshIntervalMs = 50;

    // How often the runtime pointer/stride scan is allowed to run
    // when the cache is empty. The default of 1000 ms means that
    // after a world load (where the inventory pointer changes or the
    // cache is cold) the module retries the scan once a second until
    // it finds a well-formed 36-slot buffer; once found the scan is
    // skipped entirely until/unless the cache is invalidated (world
    // change, etc.).
    int m_rescanIntervalMs = 1000;

    // Discovered layout of the inventory buffer. Populated by the
    // runtime scan; once valid the inventory reader uses this cache
    // directly instead of walking hardcoded offsets. Exposed here
    // (rather than hidden in the .cpp) so the scanner free function
    // can return it without friendship gymnastics.
    struct InventoryCache {
        std::uintptr_t base = 0;       // address of slot 0
        std::size_t    stride = 0;     // byte stride between slots
        std::size_t    countOffset = 0;// byte offset within ItemStack of mCount
        bool           valid = false;  // true once the cache is trustworthy
    };

private:
    void onLocalPlayerTick(bedrocktools::sdk::Player* player);
    bool readInventory(bedrocktools::sdk::Player* player);

    struct SlotSnapshot {
        bool hasItem = false;
        uint16_t itemId = 0;
        int16_t damage = 0;
        int16_t maxDamage = 0;
        uint8_t count = 0;
        bool enchanted = false;   // any enchant NBT tag present
    };

    mutable std::mutex m_mutex;
    std::vector<SlotSnapshot> m_slots;
    std::chrono::steady_clock::time_point m_lastReadAt{};
    std::chrono::steady_clock::time_point m_lastScanAt{};
    bool m_readFailed = false;
    InventoryCache m_cache{};
};
