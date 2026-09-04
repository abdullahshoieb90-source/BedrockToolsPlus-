// Unit tests for the Hotbar Slots module (native port of LeviLauncher's
// "Hotbar Slot" inbuilt mod).
//
// Covers the parts that are plain C++ and need no game or launcher:
//   * default button definitions (9 slots, labels 1..9, stable IDs, scaling)
//   * per-slot enable/size config mapping
//   * config save/load round-trip
//   * press flow: selectSlot() sends key-down, the next onFrame() sends key-up
//   * hotbarHasItem() against a crafted player-inventory buffer
//   * item-icons highlight + overlay refresh on occupancy change
//
// Build and run standalone (no xmake packages needed):
//
//     g++ -std=c++20 -I src -I include -I tests/fakepl -I tests/fakejson
//         tests/hotbarslots_test.cpp src/modules/hud/hotbarslots.cpp
//         -o /tmp/hotbarslots_test
//     /tmp/hotbarslots_test
//
// The overlay-button registration itself (pl::modmenu::ButtonBuilder) is only
// exercised through the host fake, which discards handlers; production wiring
// follows the Command Hotkey / Zoom modules one-to-one.

#include "modules/hud/hotbarslots.hpp"

#include <bedrocktools/sdk/Offsets.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Core / launcher stubs (host has no game process or launcher VM).
// ---------------------------------------------------------------------------

namespace {

void* g_client = nullptr;
int g_refreshCount = 0;

void* fakeLocalPlayer = nullptr;

void* stubGetLocalPlayer(void*) {
    return fakeLocalPlayer;
}

} // namespace

namespace bedrocktools::core::gamehooks {

void* clientInstance() {
    return g_client;
}

} // namespace bedrocktools::core::gamehooks

namespace bedrocktools::launcher {

void* javaVm() {
    return nullptr;
}

void refreshExternalButtonsForModule(std::string_view) {
    ++g_refreshCount;
}

} // namespace bedrocktools::launcher

// ---------------------------------------------------------------------------
// Test module with a recording key sender.
// ---------------------------------------------------------------------------

class TestHotbar : public HotbarSlotsModule {
public:
    struct KeyCall {
        int code = 0;
        bool down = false;
    };

    std::vector<KeyCall> keys;

protected:
    bool platformSendKey(int code, bool down) override {
        keys.push_back(KeyCall{code, down});
        return true;
    }
};

// ---------------------------------------------------------------------------
// Crafted player-inventory buffer for hotbarHasItem().
// ---------------------------------------------------------------------------

namespace {

using namespace bedrocktools::sdk::offsets::Inventory;

struct FakeInventory {
    std::vector<std::byte> player{0x600, std::byte{0}};
    std::vector<std::byte> proxy{0x200, std::byte{0}};
    std::vector<std::byte> container{0x200, std::byte{0}};
    std::vector<std::byte> items;

    explicit FakeInventory(std::size_t slots = 36) : items(slots * ItemStackSize, std::byte{0}) {
        *reinterpret_cast<void**>(player.data() + PlayerInventory) = proxy.data();
        *reinterpret_cast<void**>(proxy.data() + PlayerInventoryContainer) = container.data();
        *reinterpret_cast<std::uintptr_t*>(container.data() + FillingContainerItems) =
            reinterpret_cast<std::uintptr_t>(items.data());
        *reinterpret_cast<std::uintptr_t*>(container.data() + FillingContainerItems + sizeof(void*)) =
            reinterpret_cast<std::uintptr_t>(items.data() + items.size());
    }

    void setStack(std::size_t slot, bool valid, std::uint8_t count) {
        std::byte* stack = items.data() + slot * ItemStackSize;
        *reinterpret_cast<std::uint8_t*>(stack + ItemStackValid) = valid ? 1 : 0;
        *reinterpret_cast<std::uint8_t*>(stack + ItemStackCount) = count;
    }

    const void* playerPtr() const {
        return player.data();
    }
};

int failures = 0;

void check(bool cond, const char* name) {
    if (cond) {
        std::printf("  ok: %s\n", name);
    } else {
        std::printf("  FAIL: %s\n", name);
        ++failures;
    }
}

} // namespace

int main() {
    // ---- defaults ----
    {
        TestHotbar mod;
        auto defs = mod.buildButtonDefs();
        check(defs.size() == 9, "nine buttons by default");
        bool labelsOk = true;
        bool idsOk = true;
        for (std::size_t i = 0; i < defs.size(); ++i) {
            labelsOk &= defs[i].slot == static_cast<int>(i + 1) &&
                defs[i].label == std::to_string(i + 1);
            idsOk &= defs[i].id ==
                "bedrocktoolsplus.HotbarSlots.Button" + std::to_string(i + 1);
        }
        check(labelsOk, "labels are 1..9 in order");
        check(idsOk, "stable button IDs");
        const float expected = 56.0f / 52.0f;
        bool scaleOk = true;
        for (const auto& d : defs) scaleOk &= d.scaleX == expected && d.scaleY == expected;
        check(scaleOk, "default scale is 56dp / 52 base");
    }

    // ---- per-slot config ----
    {
        TestHotbar mod;
        nlohmann::json j;
        j["m_slot5"] = false;
        j["m_slot3Size"] = 100.0f;
        j["m_buttonScale"] = 0.5f;
        mod.loadConfig(j);
        auto defs = mod.buildButtonDefs();
        check(defs.size() == 8, "disabled slot disappears");
        bool noFive = true;
        bool threeScaled = false;
        for (const auto& d : defs) {
            noFive &= d.slot != 5;
            if (d.slot == 3) threeScaled = d.scaleX == (100.0f / 52.0f) * 0.5f;
        }
        check(noFive, "slot 5 gone");
        check(threeScaled, "per-slot size times global scale");
    }

    // ---- save/load round-trip ----
    {
        TestHotbar a;
        nlohmann::json j;
        j["m_itemIcons"] = true;
        j["m_slot1"] = false;
        j["m_slot9Size"] = 20.0f;
        j["m_slot2Opacity"] = 0.25f;
        a.loadConfig(j);
        nlohmann::json saved;
        a.saveConfig(saved);
        TestHotbar b;
        b.loadConfig(saved);
        auto da = a.buildButtonDefs();
        auto db = b.buildButtonDefs();
        bool same = da.size() == db.size();
        for (std::size_t i = 0; same && i < da.size(); ++i) {
            same = da[i].id == db[i].id && da[i].label == db[i].label &&
                da[i].scaleX == db[i].scaleX && da[i].textColor == db[i].textColor;
        }
        check(same, "config round-trip preserves buttons");
        check(saved["m_itemIcons"].get<bool>(), "itemIcons persisted");
        check(saved["m_slot2Opacity"].get<float>() == 0.25f, "per-slot opacity persisted");
    }

    // ---- press flow: down on select, up on next frame ----
    {
        TestHotbar mod;
        mod.setMasterEnabled(true);
        mod.selectSlot(3);
        check(mod.keys.size() == 1 && mod.keys[0].code == '0' + 3 && mod.keys[0].down,
              "select sends key-down '3'");
        mod.onFrame();
        check(mod.keys.size() == 2 && mod.keys[1].code == '0' + 3 && !mod.keys[1].down,
              "next frame sends key-up");
        mod.onFrame();
        check(mod.keys.size() == 2, "no repeat key-up");
    }

    // ---- press guards ----
    {
        TestHotbar mod;
        mod.selectSlot(1);
        check(mod.keys.empty(), "no keys while module disabled");
        mod.setMasterEnabled(true);
        mod.selectSlot(0);
        mod.selectSlot(10);
        check(mod.keys.empty(), "out-of-range slots ignored");
        nlohmann::json j;
        j["m_slot4"] = false;
        mod.loadConfig(j);
        mod.selectSlot(4);
        check(mod.keys.empty(), "disabled slot ignored");
    }

    // ---- hotbarHasItem against a crafted buffer ----
    {
        FakeInventory inv;
        inv.setStack(0, true, 3);
        inv.setStack(1, false, 5);
        inv.setStack(2, true, 0);
        check(HotbarSlotsModule::hotbarHasItem(inv.playerPtr(), 0), "occupied slot detected");
        check(!HotbarSlotsModule::hotbarHasItem(inv.playerPtr(), 1), "invalid stack empty");
        check(!HotbarSlotsModule::hotbarHasItem(inv.playerPtr(), 2), "zero-count stack empty");
        check(!HotbarSlotsModule::hotbarHasItem(inv.playerPtr(), 8), "untouched slot empty");
        check(!HotbarSlotsModule::hotbarHasItem(inv.playerPtr(), 9), "slot 9 out of hotbar");
        check(!HotbarSlotsModule::hotbarHasItem(inv.playerPtr(), -1), "negative slot rejected");
        check(!HotbarSlotsModule::hotbarHasItem(nullptr, 0), "null player rejected");
    }

    // ---- item-icons highlight + refresh on occupancy change ----
    {
        FakeInventory inv;
        inv.setStack(0, true, 1);

        static void* vtable[64] = {};
        vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer] =
            reinterpret_cast<void*>(&stubGetLocalPlayer);
        alignas(void*) unsigned char clientBlock[2 * sizeof(void*)] = {};
        *reinterpret_cast<void**>(clientBlock) = static_cast<void*>(vtable);
        g_client = static_cast<void*>(clientBlock);
        fakeLocalPlayer = const_cast<void*>(inv.playerPtr());

        TestHotbar mod;
        mod.setMasterEnabled(true);
        nlohmann::json j;
        j["m_itemIcons"] = true;
        mod.loadConfig(j);

        const int before = g_refreshCount;
        mod.onFrame();
        check(g_refreshCount == before + 1, "occupancy change refreshes overlay");
        auto defs = mod.buildButtonDefs();
        bool highlighted = false;
        for (const auto& d : defs) {
            if (d.slot == 1) highlighted = d.occupied && d.textColor == 0xFFFFFFFFu;
        }
        check(highlighted, "occupied slot highlighted");
        const int settled = g_refreshCount;
        mod.onFrame();
        check(g_refreshCount == settled, "steady state does not churn");

        g_client = nullptr;
        fakeLocalPlayer = nullptr;
    }

    if (failures == 0) {
        std::printf("hotbarslots: all tests passed\n");
        return 0;
    }
    std::printf("hotbarslots: %d test(s) failed\n", failures);
    return 1;
}
