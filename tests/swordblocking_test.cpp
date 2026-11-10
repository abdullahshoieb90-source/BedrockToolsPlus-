// Unit test for the Sword Blocking module's decision logic, keybind latch and
// config round-trip. No Minecraft or Android runtime required: the module is
// compiled straight into the test with the host fakes for the preloader hook
// API and the small JSON surface the config uses (see scripts/run_tests.sh).
//
// Manual build:
//   g++ -std=c++20 -I src -I include -I tests/fakepl -I tests/fakejson
//       tests/swordblocking_test.cpp -o build/tests/swordblocking_test
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>

// The module only reaches for signatures through bedrocktools::memory::resolve;
// provide inert stubs so it can be linked host-side.
namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
} // namespace bedrocktools::memory

#include "core/memory/Hooks.hpp"
#include "modules/player/swordblocking.cpp"

namespace {

// ---- fake player memory -----------------------------------------------------
// The module reads a few pointers straight out of the actor: the AABB shape
// component, the rotation component and the MoveInput component. Both are
// laid out by the offsets the SDK already pins, so a plain byte buffer is
// enough.

struct FakePlayer {
    alignas(16) unsigned char bytes[0x800]{};
    alignas(16) unsigned char aabbComponent[0x20]{};
    alignas(16) unsigned char rotationComponent[0x10]{};
    alignas(16) unsigned char moveInput[0x80]{};

    void init() {
        using namespace bedrocktools::sdk::offsets;

        // Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent
        *reinterpret_cast<std::uintptr_t*>(bytes + Actor::mStateVectorComponent +
                                          BuiltInActorComponents::mAABBShapeComponent) =
            reinterpret_cast<std::uintptr_t>(aabbComponent);
        // Actor::mActorRotationComponent -> rotationComponent
        *reinterpret_cast<std::uintptr_t*>(bytes + Actor::mActorRotationComponent) =
            reinterpret_cast<std::uintptr_t>(rotationComponent);
        // MoveInput::mComponent -> moveInput
        *reinterpret_cast<std::uintptr_t*>(bytes + MoveInput::mComponent) =
            reinterpret_cast<std::uintptr_t>(moveInput);

        setBox(-0.3f, 0.0f, -0.3f, 0.3f, 1.8f, 0.3f);
        setBodyRotation(0.0f);
    }

    void setBox(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) {
        using namespace bedrocktools::sdk::offsets;
        float* box = reinterpret_cast<float*>(aabbComponent + AABBShapeComponent::mAABB);
        box[0] = minX; box[1] = minY; box[2] = minZ;
        box[3] = maxX; box[4] = maxY; box[5] = maxZ;
    }

    void setBodyRotation(float yaw) {
        float* rotation = reinterpret_cast<float*>(rotationComponent);
        rotation[0] = 0.0f;
        rotation[1] = yaw;
    }

    void setSneakDown(bool down) {
        using namespace bedrocktools::sdk::offsets;
        auto* bits = reinterpret_cast<std::uint32_t*>(moveInput + MoveInput::mRawInputState +
                                                     MoveInputState::mInputBits);
        if (down) *bits |= (1u << 0);
        else *bits &= ~(1u << 0);
    }

    void setSneakingFlag(bool sneaking) {
        constexpr std::size_t kFlagValuesOffset = 0x60;
        auto* flags = reinterpret_cast<std::uint16_t*>(moveInput + kFlagValuesOffset);
        if (sneaking) *flags |= (1u << 0);
        else *flags &= static_cast<std::uint16_t>(~(1u << 0));
    }

    void* ptr() { return bytes; }
};

// ---- item stacks ------------------------------------------------------------
// ItemStackBase -> item counter -> Item, and the Item's vtable slot 5 returns
// the identifier. Only the fields the module touches are populated.

struct FakeItem {
    std::uintptr_t vtable[8]{};
    std::string identifier;
};

struct FakeStack {
    alignas(16) unsigned char bytes[0x100]{};
    alignas(16) void* counter[1]{};
    FakeItem item;
    void* itemSlot{nullptr};
};

// The identifier is served from a static so the test does not depend on the
// std::string layout inside a fake Item; what matters here is the module's
// detection logic, not libc++ internals.
std::string g_identifier;

const std::string& fakeDescriptionId(const void*) {
    return g_identifier;
}

void initStack(FakeStack& stack, const std::string& identifier) {
    using namespace bedrocktools::sdk::offsets;
    g_identifier = identifier;
    stack.item.vtable[Inventory::ItemGetDescriptionIdVtableIndex] =
        reinterpret_cast<std::uintptr_t>(&fakeDescriptionId);
    stack.item.identifier = identifier;
    // The game's ItemCounter dereferences a pointer stored at offset 0, so the
    // counter entry has to be the address of an Item* slot (the module mirrors
    // the same double indirection the HUD modules use).
    stack.itemSlot = &stack.item;
    stack.counter[0] = &stack.itemSlot;
    *reinterpret_cast<void**>(stack.bytes + ShulkerPreview::ItemStackBaseItem) =
        stack.counter;
    // A usable stack: non-empty and valid, like the real inventory slots.
    stack.bytes[Inventory::ItemStackCount] = 1;
    stack.bytes[Inventory::ItemStackValid] = 1;
}

// The module asks the player for its carried item through the Player vtable, so
// the fake player needs a slot 78 entry. ClientInstance is never touched on the
// host (no signatures resolve), so only the tick path is exercised.
FakeStack* g_carried = nullptr;

void hold(FakeStack& stack, const std::string& identifier) {
    g_identifier = identifier;
    g_carried = &stack;
}

void* fakeGetCarriedItem(void*) { return g_carried; }

} // namespace

int main() {
    using namespace bedrocktools;

    FakePlayer player;
    player.init();

    FakeStack sword;
    initStack(sword, "minecraft:diamond_sword");
    FakeStack apple;
    initStack(apple, "minecraft:apple");
    FakeStack moddedSword;
    initStack(moddedSword, "myaddon:ruby_sword");

    // Wire the carried-item vtable slot.
    static std::uintptr_t playerVtable[256]{};
    playerVtable[sdk::offsets::VTable::PlayerGetCarriedItem] =
        reinterpret_cast<std::uintptr_t>(&fakeGetCarriedItem);
    *reinterpret_cast<void**>(player.bytes) = playerVtable;

    SwordBlockingModule module;
    module.onInit();
    // The pose state machine only runs while the module is enabled (the
    // module's master toggle is on; the keybind starts active).
    module.setMasterEnabled(true);

    auto tick = [&] { module.onLocalPlayerTick(reinterpret_cast<sdk::Actor*>(player.ptr())); };

    // ---- 1) Sneak trigger, sword in hand, sneaking -------------------------
    module.m_trigger = 0;
    module.m_anySword = true;
    hold(sword, "minecraft:diamond_sword");
    player.setSneakDown(true);
    tick();
    assert(module.hasSword());
    assert(module.poseActive());
    std::puts("ok: sneak + sword -> pose active");

    // ---- 2) Sneak released -------------------------------------------------
    player.setSneakDown(false);
    player.setSneakingFlag(false);
    tick();
    assert(!module.poseActive());
    std::puts("ok: sneak released -> pose inactive");

    // ---- 3) Latched sneak flag (mobile toggle) still counts ---------------
    player.setSneakingFlag(true);
    tick();
    assert(module.poseActive());
    std::puts("ok: latched sneak flag -> pose active");
    player.setSneakingFlag(false);
    tick();

    // ---- 4) No sword in hand ----------------------------------------------
    hold(apple, "minecraft:apple");
    player.setSneakDown(true);
    tick();
    assert(!module.hasSword());
    assert(!module.poseActive());
    std::puts("ok: no sword -> pose inactive");

    // ---- 5) Modded sword identifier ---------------------------------------
    hold(moddedSword, "myaddon:ruby_sword");
    tick();
    assert(module.hasSword());
    assert(module.poseActive());
    std::puts("ok: modded *_sword accepted");

    // ---- 6) Strict mode rejects unknown swords ----------------------------
    module.m_anySword = false;
    tick();
    assert(!module.hasSword());
    std::puts("ok: strict mode rejects modded sword");
    module.m_anySword = true;

    // ---- 7) Always trigger ignores sneak ----------------------------------
    module.m_trigger = 1;
    player.setSneakDown(false);
    tick();
    assert(module.poseActive());
    std::puts("ok: Always trigger -> pose active while standing");

    // ---- 8) Toggle trigger follows the latch ------------------------------
    module.m_trigger = 2;
    module.m_latched = false;
    tick();
    assert(!module.poseActive());
    module.m_latched = true;
    tick();
    assert(module.poseActive());
    std::puts("ok: Toggle trigger follows the latch");
    module.m_latched = false;
    tick();

    // ---- 9) Disabling clears the pose -------------------------------------
    module.m_trigger = 1;
    tick();
    assert(module.poseActive());
    module.setMasterEnabled(false); // -> enabled=false, onDisable, resetPose
    assert(!module.poseActive());
    std::puts("ok: disable clears pose");

    // A disabled module must not re-activate on the next tick either.
    tick();
    assert(!module.poseActive());
    std::puts("ok: disabled module stays inactive on tick");
    module.setMasterEnabled(true); // -> onEnable

    // ---- 10) Config round-trip --------------------------------------------
    module.m_trigger = 2;
    module.m_anySword = false;
    module.m_showArm = false;
    module.m_showInFirstPerson = true;
    module.m_bladeColor = 0xFF112233u;
    module.m_guardColor = 0xFFAABBCCu;
    module.m_scale = 1.5f;
    module.m_offsetX = 0.25f;
    module.m_offsetY = -0.5f;
    module.m_offsetZ = 0.75f;

    nlohmann::json saved;
    module.saveConfig(saved);

    SwordBlockingModule reloaded;
    reloaded.loadConfig(saved);
    assert(reloaded.m_trigger == 2);
    assert(!reloaded.m_anySword);
    assert(!reloaded.m_showArm);
    assert(reloaded.m_showInFirstPerson);
    assert(reloaded.m_bladeColor == 0xFF112233u);
    assert(reloaded.m_guardColor == 0xFFAABBCCu);
    assert(reloaded.m_scale > 1.49f && reloaded.m_scale < 1.51f);
    assert(reloaded.m_offsetX > 0.24f && reloaded.m_offsetX < 0.26f);
    assert(reloaded.m_offsetY < -0.49f && reloaded.m_offsetY > -0.51f);
    assert(reloaded.m_offsetZ > 0.74f && reloaded.m_offsetZ < 0.76f);
    std::puts("ok: config round-trips");

    // ---- 11) Radio value renders as a picker ------------------------------
    assert(saved["m_trigger"].is_string());
    const std::string radio = saved["m_trigger"].get<std::string>();
    assert(radio.find(',') != std::string::npos);
    assert(radio.rfind("Toggle", 0) == 0);
    std::puts("ok: trigger stored as a radio picker value");

    // ---- 12) Out-of-range values are clamped ------------------------------
    nlohmann::json hostile;
    hostile["m_scale"] = 99.0f;
    hostile["m_offsetX"] = 500.0f;
    hostile["m_offsetY"] = -500.0f;
    hostile["m_trigger"] = 42;
    SwordBlockingModule clamped;
    clamped.loadConfig(hostile);
    assert(clamped.m_scale >= 0.5f && clamped.m_scale <= 2.0f);
    assert(clamped.m_offsetX >= -2.0f && clamped.m_offsetX <= 2.0f);
    assert(clamped.m_offsetY >= -2.0f && clamped.m_offsetY <= 2.0f);
    assert(clamped.m_trigger >= 0 && clamped.m_trigger <= 2);
    std::puts("ok: hostile config is clamped");

    // ---- 13) Keybind latches the Toggle pose on and off -------------------
    // (Test 10 left strict mode on; the carried sword is a modded one.)
    module.m_anySword = true;
    module.m_trigger = 2;
    module.m_latched = false;
    tick();
    assert(!module.poseActive());
    module.onKeybindEvent("keybind", true); // latch on
    tick();
    assert(module.poseActive());
    module.onKeybindEvent("keybind", true); // latch off
    assert(!module.poseActive());
    std::puts("ok: keybind latches the Toggle pose on/off");

    // ---- 14) Keybind still toggles the module for the other triggers ------
    module.m_trigger = 0;
    player.setSneakDown(true);
    tick();
    assert(module.poseActive());
    module.onKeybindEvent("keybind", true); // module off
    tick();
    assert(!module.poseActive());
    module.onKeybindEvent("keybind", true); // module back on
    tick();
    assert(module.poseActive());
    std::puts("ok: keybind toggles the module for non-Toggle triggers");

    std::puts("\nall sword blocking assertions passed");
    return 0;
}
