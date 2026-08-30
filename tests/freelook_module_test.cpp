// Host-side integration test for the Free Look module.
//
// Builds the real module (src/modules/visual/freelook.cpp) as a second
// translation unit against the host fakes, then drives it through the exact
// entry points the game uses: the keybind handler, the applyTurnDelta hook
// path (shouldInterceptTurn + onTurnDelta) and the pre/post tick rotation
// writes. A fake player object carries a real ActorRotationComponent pointer
// at the offset the module reads, so the test observes exactly what the
// module writes into the body — and a real MoveInputComponent in the same
// entity registry the game uses, so the movement-scheme lock on flags 9/10
// is observable too.
//
// The camera is the game's to own — the module only steers it with deltas
// through applyTurnDelta — so the camera side is asserted on the deltas the
// module hands back (onTurnDelta's return value) and on the compensating
// deltas it emits during the release (lastCameraDelta / cameraDeltaCount).
// The fake hook hands back a null trampoline, which is what the real module
// null-checks before calling the original.
//
// The module includes bedrocktools/sdk/input/MoveInput.hpp, so the entt
// headers are needed (xmake normally provides entt; point ENTT_INCLUDE at
// the package's include dir — the same package effectdisplay_test uses):
//
//     ENTT=$(echo ~/.xmake/packages/e/entt/v3.16.0/*/include)
//     g++ -std=c++20 -I src -I include -I "$ENTT" -I tests/fakepl
//         -I tests/fakejson tests/freelook_module_test.cpp
//         src/modules/visual/freelook.cpp -o /tmp/freelook_module_test
//     /tmp/freelook_module_test

#include "modules/visual/freelook.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
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
    // hook path (the fake pl::memory::hook "succeeds", with a null original).
    return id == SignatureId::LocalPlayerApplyTurnDelta ? 0x10000 : 0;
}
} // namespace bedrocktools::memory

// ---------------------------------------------------------------------------
// Fake LocalPlayer: enough storage for the ActorRotationComponent pointer at
// Actor::mActorRotationComponent (0x218) plus the component itself, and an
// embedded EntityContext at Actor::mEntityContext (0x8) backed by a real
// registry holding a MoveInputComponent — the same shape the game uses.
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kRotationComponentOffset = 0x218;

// One registry shared by every fake player; each fake owns its entity.
entt::basic_registry<EntityId> g_inputRegistry;

struct FakePlayer {
    alignas(8) unsigned char bytes[kRotationComponentOffset + 8];
    alignas(8) float rotation[4];
    EntityId entity;

    FakePlayer() {
        std::memset(bytes, 0, sizeof(bytes));
        std::memset(rotation, 0, sizeof(rotation));
        const uintptr_t component = reinterpret_cast<std::uintptr_t>(&rotation[0]);
        std::memcpy(bytes + kRotationComponentOffset, &component, sizeof(component));

        // The engine embeds the EntityContext inside the actor (it is not a
        // pointer), exactly like Actor::entityContext() walks it.
        entity = g_inputRegistry.create();
        g_inputRegistry.emplace<MoveInputComponent>(entity);
        new (bytes + bedrocktools::sdk::offsets::Actor::mEntityContext) EntityContext{
            *static_cast<EntityRegistry*>(nullptr), g_inputRegistry, entity};
    }

    ~FakePlayer() {
        g_inputRegistry.destroy(entity);
    }

    MoveInputComponent* input() {
        return bedrocktools::sdk::moveInputComponent(bytes);
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

void checkTurn(const freelook::Turn& turn, float pitch, float yaw, const std::string& what) {
    check(std::fabs(turn.pitch - pitch) < 0.001f && std::fabs(turn.yaw - yaw) < 0.001f,
          what + " (got {" + std::to_string(turn.pitch) + ", " + std::to_string(turn.yaw) +
              "}, want {" + std::to_string(pitch) + ", " + std::to_string(yaw) + "})");
}

// Drives the real applyTurnDelta detour body with one delta and returns what
// would be handed to the game's original function — i.e. what reaches the
// camera. The body must never move as a side effect of this.
freelook::Turn feedTurn(FreeLookModule& mod, FakePlayer& player, float pitch, float yaw) {
    const auto out = mod.filterTurnDelta(&player, bedrocktools::sdk::Vec2{pitch, yaw});
    return freelook::Turn{out.x, out.y};
}

// Config helper: round-trips the module's own settings and overrides some.
nlohmann::json settings(FreeLookModule& mod) {
    nlohmann::json cfg;
    mod.saveConfig(cfg);
    return cfg;
}

} // namespace

int main() {
    std::printf("free look module\n");

    FreeLookModule mod;
    check(mod.moduleId == "bedrocktoolsplus.Free Look", "module id");
    mod.onInit(); // hooks + event subscriptions + overlay button

    // -----------------------------------------------------------------
    // Module off: nothing is intercepted and no rotation is written.
    // -----------------------------------------------------------------
    FakePlayer player;
    player.setRot(10.0f, 20.0f);
    mod.onPreTick(&player);
    mod.onPostTick(&player);
    checkRot(player, 10.0f, 20.0f, "disabled module leaves the body alone");
    check(!mod.shouldInterceptTurn(&player), "disabled module does not intercept turns");
    checkTurn(feedTurn(mod, player, 5.0f, 5.0f), 5.0f, 5.0f, "disabled module passes turns through");

    // -----------------------------------------------------------------
    // Engage: the body locks, the camera keeps receiving the full input and
    // the movement input scheme stops following the camera.
    // -----------------------------------------------------------------
    mod.setMasterEnabled(true);
    check(mod.enabled, "module enabled");

    // Pretend the engine has the camera-relative scheme active (flags 9+10),
    // plus two unrelated flags the movement lock must leave alone.
    auto* moveInput = player.input();
    check(moveInput != nullptr, "fake player exposes a move input component");
    const std::uint16_t cameraScheme = static_cast<std::uint16_t>((1u << 9) | (1u << 10) | 0x5);
    moveInput->mFlagValues.value = cameraScheme;

    mod.onKeybindEvent("keybind", true);
    mod.onPreTick(&player);
    checkRot(player, 10.0f, 20.0f, "engaging locks at the current rotation");
    check(mod.shouldInterceptTurn(&player), "engaged module intercepts turns");
    check((moveInput->mFlagValues.value & (freelook::MovementFrameLock::LockMask)) == 0,
          "engaged free look clears the camera-relative movement scheme");
    check((moveInput->mFlagValues.value & 0x5) == 0x5,
          "unrelated input flags survive the movement lock");

    // The turn reaches the camera unchanged (Free Look must not reshape the
    // look input) and the body does not follow it.
    checkTurn(feedTurn(mod, player, 5.0f, -40.0f), 5.0f, -40.0f, "turn passes through to the camera");
    checkTurn(mod.cameraSwing(), 5.0f, -40.0f, "camera swing tracks the turn");
    checkRot(player, 10.0f, 20.0f, "the turn does not move the body");

    // applyTurnDelta may be shared with other actors: a turn that is not the
    // tracked local player's is passed on untouched and ignored entirely.
    FakePlayer stranger;
    stranger.setRot(0.0f, 0.0f);
    checkTurn(feedTurn(mod, stranger, 3.0f, 4.0f), 3.0f, 4.0f, "a foreign actor's turn passes through");
    checkRot(stranger, 0.0f, 0.0f, "a foreign actor's body is not touched");
    checkTurn(mod.cameraSwing(), 5.0f, -40.0f, "a foreign actor's turn does not move the swing");

    // Post-tick re-asserts the lock so the player model renders locked too.
    player.setRot(99.0f, 99.0f); // pretend the tick moved the rotation
    mod.onPostTick(&player);
    checkRot(player, 10.0f, 20.0f, "post-tick re-asserts the locked body");
    check(mod.cameraDeltaCount() == 0, "no compensating deltas while active");

    // Next tick: the lock must be back in place *before* the tick runs —
    // that write is what movement and the MovePlayerPacket read. Something
    // else moving the rotation between ticks must not survive into the tick.
    player.setRot(-42.0f, 123.0f);
    mod.onPreTick(&player);
    checkRot(player, 10.0f, 20.0f, "pre-tick restores the lock before the tick runs");
    checkTurn(feedTurn(mod, player, 0.0f, 10.0f), 0.0f, 10.0f, "further turns keep flowing");
    checkTurn(mod.cameraSwing(), 5.0f, -30.0f, "swing accumulates");
    mod.onPostTick(&player);
    checkRot(player, 10.0f, 20.0f, "body still locked after the tick");

    // -----------------------------------------------------------------
    // Release with Smooth Return: compensating deltas walk the camera home
    // and the body is unlocked only once it arrives.
    // -----------------------------------------------------------------
    mod.resetCameraDeltaLog();
    mod.onKeybindEvent("keybind", false);
    mod.onPreTick(&player);
    checkRot(player, 10.0f, 20.0f, "release keeps the body locked");
    check(mod.shouldInterceptTurn(&player), "the return still owns the camera");
    checkTurn(feedTurn(mod, player, 30.0f, 30.0f), 0.0f, 0.0f, "input is zeroed during the return");

    freelook::Turn compensated{0.0f, 0.0f};
    mod.onPostTick(&player);
    check(mod.cameraDeltaCount() == 1, "the release sends a compensating delta");
    compensated.pitch += mod.lastCameraDelta().pitch;
    compensated.yaw += mod.lastCameraDelta().yaw;
    check(mod.lastCameraDelta().pitch < 0.0f && mod.lastCameraDelta().yaw > 0.0f,
          "compensation runs opposite to the swing");
    check(std::fabs(mod.cameraSwing().yaw) < 30.0f, "the swing shrinks with every step");
    check(std::fabs(mod.cameraSwing().yaw) > 0.0f, "smooth return does not snap");

    int steps = 1;
    while (mod.shouldInterceptTurn(&player) && steps < 200) {
        mod.onPreTick(&player);
        checkRot(player, 10.0f, 20.0f, "the body stays locked until the camera lands");
        const int before = mod.cameraDeltaCount();
        mod.onPostTick(&player);
        if (mod.cameraDeltaCount() != before) {
            compensated.pitch += mod.lastCameraDelta().pitch;
            compensated.yaw += mod.lastCameraDelta().yaw;
        }
        ++steps;
    }
    check(steps > 1 && steps < 200, "smooth return settles over several ticks");
    checkTurn(compensated, -5.0f, 30.0f, "compensating deltas undo the whole swing");
    checkTurn(mod.cameraSwing(), 0.0f, 0.0f, "no swing left after the return");
    check(!mod.shouldInterceptTurn(&player), "the body is unlocked once the camera arrives");
    checkTurn(feedTurn(mod, player, 7.0f, 7.0f), 7.0f, 7.0f, "turns pass through again");
    mod.onPreTick(&player); // settled tick: writes nothing, restores the scheme
    check(moveInput->mFlagValues.value == cameraScheme,
          "release restores the saved movement scheme");

    // Settled: ticks no longer touch the rotation.
    player.setRot(1.0f, 1.0f);
    mod.onPreTick(&player);
    mod.onPostTick(&player);
    checkRot(player, 1.0f, 1.0f, "settled module writes nothing");

    // -----------------------------------------------------------------
    // Smooth Return off: one full compensating step.
    // -----------------------------------------------------------------
    nlohmann::json cfg = settings(mod);
    cfg["m_smoothReturn"] = false;
    mod.loadConfig(cfg);
    mod.resetCameraDeltaLog();
    mod.onKeybindEvent("keybind", true);
    player.setRot(0.0f, 0.0f);
    mod.onPreTick(&player);
    (void)feedTurn(mod, player, 12.0f, 50.0f);
    mod.onPostTick(&player);
    mod.onKeybindEvent("keybind", false);
    mod.onPreTick(&player);
    mod.onPostTick(&player);
    check(mod.cameraDeltaCount() == 1, "snap return sends exactly one delta");
    checkTurn(mod.lastCameraDelta(), -12.0f, -50.0f, "snap return undoes the swing in one step");
    check(!mod.shouldInterceptTurn(&player), "snap return unlocks the body immediately");
    checkRot(player, 0.0f, 0.0f, "snap return leaves the body on the lock");

    // -----------------------------------------------------------------
    // Swing limits: the hook trims the delta handed to the camera.
    // -----------------------------------------------------------------
    cfg = settings(mod);
    cfg["m_maxYaw"] = 45;
    cfg["m_maxPitch"] = 20;
    mod.loadConfig(cfg);
    mod.onKeybindEvent("keybind", true);
    player.setRot(0.0f, 0.0f);
    mod.onPreTick(&player);
    checkTurn(feedTurn(mod, player, 90.0f, 90.0f), 20.0f, 45.0f, "the delta is trimmed to the limits");
    checkTurn(feedTurn(mod, player, 10.0f, 10.0f), 0.0f, 0.0f, "input at the limit is blocked");
    checkTurn(feedTurn(mod, player, -10.0f, -10.0f), -10.0f, -10.0f, "turning back is immediate");
    checkRot(player, 0.0f, 0.0f, "the body never followed the trimmed turns");
    mod.onKeybindEvent("keybind", false);
    mod.onPreTick(&player);
    mod.onPostTick(&player); // snap return is still configured
    check(!mod.shouldInterceptTurn(&player), "released after the limited swing");

    // -----------------------------------------------------------------
    // Toggle mode.
    // -----------------------------------------------------------------
    cfg = settings(mod);
    cfg["m_holdMode"] = false;
    cfg["m_maxYaw"] = 180; // back to unlimited for the remaining cases
    cfg["m_maxPitch"] = 90;
    mod.loadConfig(cfg);
    mod.onKeybindEvent("keybind", true);
    mod.onKeybindEvent("keybind", false); // release of the same press
    player.setRot(-5.0f, 30.0f);
    mod.onPreTick(&player);
    checkRot(player, -5.0f, 30.0f, "toggle mode engages on press");
    check(mod.shouldInterceptTurn(&player), "toggle mode stays engaged after the release");
    (void)feedTurn(mod, player, 0.0f, 25.0f);
    player.setRot(60.0f, -170.0f); // something else nudged the rotation
    mod.onPreTick(&player);
    checkRot(player, -5.0f, 30.0f, "toggle mode re-locks the body before the tick");
    mod.onPostTick(&player);
    checkRot(player, -5.0f, 30.0f, "body locked in toggle mode");
    mod.onKeybindEvent("keybind", true); // toggle off
    mod.onKeybindEvent("keybind", false);
    mod.onPreTick(&player);
    mod.onPostTick(&player);
    check(!mod.shouldInterceptTurn(&player), "toggle off releases");
    checkRot(player, -5.0f, 30.0f, "toggle off leaves the body on the lock");

    // -----------------------------------------------------------------
    // Disabling the module mid-swing: the camera is snapped back in one
    // compensating delta and the body is released.
    // -----------------------------------------------------------------
    mod.onKeybindEvent("keybind", true); // toggle back on
    player.setRot(0.0f, 0.0f);
    mod.onPreTick(&player);
    (void)feedTurn(mod, player, 10.0f, 90.0f);
    mod.onPostTick(&player);
    checkRot(player, 0.0f, 0.0f, "body locked before the disable");
    mod.resetCameraDeltaLog();
    mod.setMasterEnabled(false);
    check(mod.cameraDeltaCount() == 1, "onDisable sends the catch-up delta");
    checkTurn(mod.lastCameraDelta(), -10.0f, -90.0f, "onDisable snaps the camera home");
    checkRot(player, 0.0f, 0.0f, "onDisable leaves the body on the lock");
    check(!mod.shouldInterceptTurn(&player), "disabled module stops intercepting");
    player.setRot(3.0f, 3.0f);
    mod.onPreTick(&player);
    mod.onPostTick(&player);
    checkRot(player, 3.0f, 3.0f, "disabled module writes nothing");

    // -----------------------------------------------------------------
    // Player switch (respawn / dimension change): re-lock at the new player
    // and never write the old lock into it.
    // -----------------------------------------------------------------
    mod.setMasterEnabled(true);
    cfg = settings(mod);
    cfg["m_holdMode"] = true;
    cfg["m_smoothReturn"] = false;
    cfg["m_maxYaw"] = 180;
    cfg["m_maxPitch"] = 90;
    mod.loadConfig(cfg);
    mod.onKeybindEvent("keybind", true);
    player.setRot(7.0f, -30.0f);
    mod.onPreTick(&player);
    checkRot(player, 7.0f, -30.0f, "engaged at the first player");
    (void)feedTurn(mod, player, 0.0f, 40.0f);

    FakePlayer other;
    other.setRot(50.0f, 100.0f);
    mod.resetCameraDeltaLog();
    mod.onPreTick(&other);
    checkRot(other, 50.0f, 100.0f, "player switch re-locks at the new player");
    checkRot(player, 7.0f, -30.0f, "old player untouched after the switch");
    check(mod.cameraDeltaCount() == 0, "no compensation into a replaced camera");
    checkTurn(mod.cameraSwing(), 0.0f, 0.0f, "the swing is dropped with the old player");
    check(!mod.shouldInterceptTurn(&player), "the old player is no longer intercepted");
    checkTurn(feedTurn(mod, other, 0.0f, 20.0f), 0.0f, 20.0f, "the new player's turns flow");
    mod.onPostTick(&other);
    checkRot(other, 50.0f, 100.0f, "new player's body is locked");

    // -----------------------------------------------------------------
    // Config round-trip and clamping.
    // -----------------------------------------------------------------
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
    check(saved.contains("m_returnSpeed"), "return speed round-trips");
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
