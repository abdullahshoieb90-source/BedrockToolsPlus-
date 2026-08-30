// Host-side integration test for the Free Look module.
//
// Builds the real module (src/modules/visual/freelook.cpp) as a second
// translation unit against the host fakes, then drives it through the public
// entry points the game would call: the keybind handler, the pre/post tick
// rotation writes and the redirected turn callback. A fake player object
// carries a real ActorRotationComponent pointer at the offset the module
// reads, so the test observes exactly what the module writes into the player.
//
//     g++ -std=c++20 -I src -I include -I tests/fakepl -I tests/fakejson
//         tests/freelook_module_test.cpp src/modules/visual/freelook.cpp
//         -o /tmp/freelook_module_test
//     /tmp/freelook_module_test

#include "modules/visual/freelook.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Host stubs for the symbols the module expects from the runtime library.
// ---------------------------------------------------------------------------

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId id) {
    // Pretend the applyTurnDelta signature resolved so the module engages its
    // hook path (the fake pl::memory::hook "succeeds").
    return id == SignatureId::LocalPlayerApplyTurnDelta ? 0x10000 : 0;
}
} // namespace bedrocktools::memory

// ---------------------------------------------------------------------------
// Fake LocalPlayer: enough storage for the ActorRotationComponent pointer at
// Actor::mActorRotationComponent (0x218) plus the component itself.
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kRotationComponentOffset = 0x218;

struct FakePlayer {
    alignas(8) unsigned char bytes[kRotationComponentOffset + 8];
    alignas(8) float rotation[4];

    FakePlayer() {
        std::memset(bytes, 0, sizeof(bytes));
        std::memset(rotation, 0, sizeof(rotation));
        const uintptr_t component = reinterpret_cast<std::uintptr_t>(&rotation[0]);
        std::memcpy(bytes + kRotationComponentOffset, &component, sizeof(component));
    }

    bedrocktools::sdk::Vec2 rot() const {
        return bedrocktools::sdk::Vec2{rotation[0], rotation[1]};
    }

    void setRot(float pitch, float yaw) {
        rotation[0] = pitch;
        rotation[1] = yaw;
    }
};

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

void checkRot(const FakePlayer& player, float pitch, float yaw, const std::string& what) {
    const auto r = player.rot();
    check(std::fabs(r.x - pitch) < 0.001f && std::fabs(r.y - yaw) < 0.001f,
          what + " (got {" + std::to_string(r.x) + ", " + std::to_string(r.y) +
              "}, want {" + std::to_string(pitch) + ", " + std::to_string(yaw) + "})");
}

} // namespace

int main() {
    std::printf("free look module\n");

    FreeLookModule mod;
    check(mod.moduleId == "bedrocktoolsplus.Free Look", "module id");
    mod.onInit(); // hooks + event subscriptions + overlay button

    // Module off by default: ticks must not touch the player.
    FakePlayer player;
    player.setRot(10.0f, 20.0f);
    mod.onPreTick(&player);
    mod.onPostTick(&player);
    checkRot(player, 10.0f, 20.0f, "disabled module leaves the rotation alone");

    // Enable (master toggle) and hold the keybind.
    mod.setMasterEnabled(true);
    check(mod.enabled, "module enabled");
    mod.onKeybindEvent("keybind", true);
    mod.onPreTick(&player);
    checkRot(player, 10.0f, 20.0f, "engaging locks at the current rotation");

    // A redirected turn (the game applied +5 pitch / -40 yaw; the hook undid
    // it on the player and reported the measured delta).
    mod.onTurnMeasured(5.0f, -40.0f, 5.0f, -40.0f);
    mod.onPostTick(&player);
    checkRot(player, 15.0f, -20.0f, "camera angle written after the tick");

    // Next tick: the body is forced back to the locked rotation.
    mod.onPreTick(&player);
    checkRot(player, 10.0f, 20.0f, "body locked during the tick");

    // Another turn accumulates on the camera.
    mod.onTurnMeasured(0.0f, 10.0f, 0.0f, 10.0f);
    mod.onPostTick(&player);
    checkRot(player, 15.0f, -10.0f, "camera accumulates further turns");

    // Release: the smooth return animates the camera home.
    mod.onKeybindEvent("keybind", false);
    mod.onPreTick(&player);
    checkRot(player, 10.0f, 20.0f, "release keeps the body locked");
    mod.onPostTick(&player);
    check(player.rot().y > -10.0f && player.rot().y < 20.0f,
          "release steps the camera toward the body");

    // Run the return out: it must settle and then stop writing.
    int steps = 0;
    while (steps < 200) {
        mod.onPreTick(&player);
        mod.onPostTick(&player);
        ++steps;
        // postTick of the settling step writes locked; detect the inactive
        // state by overwriting the rotation and tick once more.
        if (player.rot().y == 20.0f && player.rot().x == 10.0f) {
            player.setRot(1.0f, 1.0f);
            mod.onPreTick(&player);
            mod.onPostTick(&player);
            if (player.rot().x == 1.0f && player.rot().y == 1.0f) break;
        }
    }
    check(steps < 200, "smooth return settles");
    checkRot(player, 1.0f, 1.0f, "settled module writes nothing");

    // Toggle mode: one press engages, another releases.
    nlohmann::json cfg;
    mod.saveConfig(cfg);
    cfg["m_holdMode"] = false;
    mod.loadConfig(cfg);
    mod.onKeybindEvent("keybind", true);
    mod.onKeybindEvent("keybind", false); // release of the same press
    player.setRot(-5.0f, 30.0f);
    mod.onPreTick(&player);
    checkRot(player, -5.0f, 30.0f, "toggle mode engages on press");
    mod.onKeybindEvent("keybind", true);
    mod.onKeybindEvent("keybind", false);
    mod.onPreTick(&player);
    mod.onPostTick(&player);
    // smooth return runs; force it out through the module's disable path
    // after first checking the body went back to the lock immediately.
    checkRot(player, -5.0f, 30.0f, "toggle off restores the body");
    mod.setMasterEnabled(false);
    checkRot(player, -5.0f, 30.0f, "disable keeps the body restored");

    // Re-enable and verify the instant-restore path from onDisable: engage,
    // turn the camera, then disable while active.
    mod.setMasterEnabled(true);
    mod.onKeybindEvent("keybind", true); // toggle back on (toggle mode)
    player.setRot(0.0f, 0.0f);
    mod.onPreTick(&player);
    mod.onTurnMeasured(10.0f, 90.0f, 10.0f, 90.0f);
    mod.onPostTick(&player);
    checkRot(player, 10.0f, 90.0f, "camera moved before disable");
    mod.setMasterEnabled(false);
    checkRot(player, 0.0f, 0.0f, "onDisable snaps the camera home");

    // Player switch (dimension change / respawn): the module must relock at
    // the new player's rotation and never write the old lock into it.
    mod.setMasterEnabled(true);
    cfg = nlohmann::json();
    mod.saveConfig(cfg);
    cfg["m_holdMode"] = true;
    cfg["m_smoothReturn"] = false; // snap releases for a deterministic check
    mod.loadConfig(cfg);
    mod.onKeybindEvent("keybind", true);
    player.setRot(7.0f, -30.0f);
    mod.onPreTick(&player);
    checkRot(player, 7.0f, -30.0f, "engaged at the first player");

    FakePlayer other;
    other.setRot(50.0f, 100.0f);
    mod.onPreTick(&other);
    checkRot(other, 50.0f, 100.0f, "player switch relocks at the new player");
    mod.onTurnMeasured(0.0f, 20.0f, 0.0f, 20.0f);
    mod.onPostTick(&other);
    checkRot(other, 50.0f, 120.0f, "new player carries the camera");
    checkRot(player, 7.0f, -30.0f, "old player untouched after the switch");

    // Config round-trip and clamping.
    cfg = nlohmann::json();
    cfg["m_returnSpeed"] = 5.0f;
    cfg["m_maxYaw"] = 500;
    cfg["m_maxPitch"] = -5;
    cfg["m_holdMode"] = false;
    cfg["m_smoothReturn"] = false;
    cfg["m_overlayToggle"] = false;
    mod.loadConfig(cfg);
    nlohmann::json saved;
    mod.saveConfig(saved);
    check(saved["m_returnSpeed"].get<float>() == 1.0f, "return speed clamped to 1.0");
    check(saved["m_maxYaw"].get<int>() == 180, "max yaw clamped to 180");
    check(saved["m_maxPitch"].get<int>() == 15, "max pitch clamped to 15");
    check(saved["m_holdMode"].get<bool>() == false, "hold mode round-trips");
    check(saved["m_smoothReturn"].get<bool>() == false, "smooth return round-trips");
    check(saved["m_overlayToggle"].get<bool>() == false, "overlay toggle round-trips");
    check(saved.contains("keybind"), "base config keys round-trip");

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("all free look module checks passed\n");
        return 0;
    }
    std::printf("%d free look module check(s) failed\n", g_failures);
    return 1;
}
