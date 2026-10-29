// Behavior test for Kill Aura (src/modules/combat/killaura.cpp).
//
// Drives the production tick handler with fake actor memory and a fake
// GameMode whose vtable slot 16 points at a recorder - the exact
// fingerprint the module validates before attacking. This covers target
// classification/reach (with killaura_logic.hpp doing the math), the CPS
// gate, Multi Attack, client rotation, the arm swing, the pause while a
// container/chat screen is open, Only Visible Targets, and - most
// importantly - the GameMode resolution: the derived Player::mGameMode slot
// is only ever used when its vtable fingerprint matches one of the resolved
// attack functions, and the event-captured pointer takes over when it does
// not.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson -pthread
//            tests/killaura_patch_test.cpp -o /tmp/killaura_patch_test
// Run:   /tmp/killaura_patch_test

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

#include "bedrocktools/events/EventBus.hpp"
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/sdk/Offsets.hpp"
#include "bedrocktools/sdk/Types.hpp"

// Host stubs. No signature resolves, so onInit leaves every function pointer
// null and the test installs its fakes into the module's statics directly.
namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
} // namespace bedrocktools::memory

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

#include "modules/combat/killaura.cpp"

namespace {

using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;
namespace offs = bedrocktools::sdk::offsets;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

// --- fake game objects -------------------------------------------------------

struct AttackCall {
    void* gameMode;
    void* target;
    Vec3 hit;
    int kind; // 0 = GameMode::attack, 1 = SurvivalMode::attack
};
std::vector<AttackCall> g_attacks;
bool g_attackResult = true;

bool fakeAttackGameMode(void* gm, void* target, const Vec3* hit) {
    g_attacks.push_back(AttackCall{gm, target, hit ? *hit : Vec3{}, 0});
    return g_attackResult;
}
bool fakeAttackSurvival(void* gm, void* target, const Vec3* hit) {
    g_attacks.push_back(AttackCall{gm, target, hit ? *hit : Vec3{}, 1});
    return g_attackResult;
}
bool fakeAttackOther(void* gm, void* target, const Vec3* hit) {
    g_attacks.push_back(AttackCall{gm, target, hit ? *hit : Vec3{}, 2});
    return true;
}

int g_swingCalls = 0;
std::uint8_t g_lastSwingSource = 0xFF;
bool fakeSwing(void*, std::uint8_t source, std::uint32_t) {
    ++g_swingCalls;
    g_lastSwingSource = source;
    return true;
}

// A GameMode stand-in: first word points at a vtable whose slot 16 is the
// given attack function, exactly what looksLikeGameMode() fingerprints.
struct FakeGameMode {
    explicit FakeGameMode(void* attackFn) {
        table[kAttackVtableSlot] = attackFn;
        vptr = table;
    }
    void* handle() { return &vptr; }

    void** vptr = nullptr;
    void* table[24] = {};
};

// Actor memory laid out at the offsets the module reads. Sized past
// Player::mSkin so the local player can also carry the derived mGameMode
// slot at kPlayerGameModeOffset.
struct FakeActor {
    struct Shape {
        AABB aabb{{0, 0, 0}, {0, 0, 0}};
    };

    FakeActor() {
        const auto stateAddr = reinterpret_cast<std::uintptr_t>(&state);
        const auto shapeAddr = reinterpret_cast<std::uintptr_t>(&shape);
        const auto rotAddr = reinterpret_cast<std::uintptr_t>(&rot);
        writeAt(bytes, offs::Actor::mStateVectorComponent, stateAddr);
        writeAt(bytes, offs::Actor::mStateVectorComponent + offs::BuiltInActorComponents::mAABBShapeComponent, shapeAddr);
        writeAt(bytes, offs::Actor::mActorRotationComponent, rotAddr);
    }

    void* handle() { return bytes.data(); }

    void setBox(float x, float y, float z, float w = 0.6f, float h = 1.8f) {
        shape.aabb = {{x, y, z}, {x + w, y + h, z + w}};
    }
    void setCategories(std::uint32_t categories) {
        writeAt(bytes, offs::Actor::mCategories, categories);
    }
    void setGameModeSlot(void* gm) {
        const auto addr = reinterpret_cast<std::uintptr_t>(gm);
        writeAt(bytes, kPlayerGameModeOffset, addr);
    }
    void setDimension(void* dimension) {
        const auto addr = reinterpret_cast<std::uintptr_t>(dimension);
        writeAt(bytes, offs::Actor::mDimension, addr);
    }

    alignas(16) std::array<std::byte, 0xA20> bytes{};
    Vec3 state{0, 0, 0};
    Shape shape{};
    // Aligned like a heap-allocated component: the module refuses to read
    // or write a rotation component at an implausible address.
    alignas(16) Vec2 rot{0, 0};
};

struct FakeDimension {
    FakeDimension() {
        const auto region = reinterpret_cast<std::uintptr_t>(&dummy);
        writeAt(bytes, offs::Dimension::mBlockSource, region);
    }
    void* handle() { return bytes.data(); }

    alignas(16) std::array<std::byte, 0x100> bytes{};
    alignas(16) std::byte dummy{};
};

bool g_solidAll = false;
bool fakeSolid(void*, const BlockPosI&) { return g_solidAll; }

std::unordered_set<void*> g_players;
std::unordered_set<void*> g_invisible;
bool fakeIsPlayer(void* actor) { return g_players.count(actor) != 0; }
bool fakeIsInvisible(void* actor) { return g_invisible.count(actor) != 0; }

std::vector<DistanceSortedActor> g_fetchList;
ActorVec fakeFetch(void*, void*, int) {
    if (g_fetchList.empty()) return ActorVec{};
    return ActorVec{g_fetchList.data(), g_fetchList.data() + g_fetchList.size(),
                    g_fetchList.data() + g_fetchList.size()};
}

void setFetch(std::initializer_list<void*> actors) {
    g_fetchList.clear();
    for (void* actor : actors) g_fetchList.push_back(DistanceSortedActor{actor, 0.0f, 0.0f});
}

} // namespace

int main() {
    KillAuraModule mod;
    mod.onInit(); // resolve() == 0 everywhere; install fakes instead.
    s_fetchNearby = &fakeFetch;
    s_actorIsPlayer = &fakeIsPlayer;
    s_actorIsInvisible = &fakeIsInvisible;
    s_playerSwing = &fakeSwing;
    s_isSolidBlockingBlock = &fakeSolid;
    s_gameModeAttackAddr = reinterpret_cast<std::uintptr_t>(&fakeAttackGameMode);
    s_survivalModeAttackAddr = reinterpret_cast<std::uintptr_t>(&fakeAttackSurvival);
    mod.setMasterEnabled(true);

    FakeGameMode survivalMode(reinterpret_cast<void*>(&fakeAttackSurvival));
    FakeGameMode badMode(reinterpret_cast<void*>(&fakeAttackOther));

    auto player = std::make_unique<FakeActor>();
    player->setBox(0.0f, 64.0f, 0.0f); // eye at (0.3, 65.62, 0.3)
    player->setGameModeSlot(survivalMode.handle());

    // Every actor stays alive for the whole run: the fake isPlayer /
    // isInvisible sets key on raw addresses, and a freed actor whose heap
    // address is recycled by the next allocation would inherit its
    // predecessor's identity.
    std::vector<std::unique_ptr<FakeActor>> keepAlive;
    auto keep = [&](std::unique_ptr<FakeActor> actor) {
        keepAlive.push_back(std::move(actor));
        return keepAlive.back().get();
    };
    auto makePlayer = [&](float x, float z = 0.0f) {
        auto actor = std::make_unique<FakeActor>();
        actor->setBox(x, 64.0f, z);
        actor->setCategories(offs::ActorCategories::IsPlayer | offs::ActorCategories::IsMob);
        g_players.insert(actor->handle());
        return keep(std::move(actor));
    };
    auto makeMob = [&](float x) {
        auto actor = std::make_unique<FakeActor>();
        actor->setBox(x, 64.0f, 0.0f);
        actor->setCategories(offs::ActorCategories::IsMob);
        return keep(std::move(actor));
    };

    auto tick = [&]() {
        g_attacks.clear();
        g_swingCalls = 0;
        mod.onDisable(); // resets the attack timer without toggling `enabled`
        mod.onLocalPlayerTick(player->handle());
    };

    std::printf("killaura targeting\n");
    {
        auto near_ = makePlayer(2.3f); // reach 2.0
        auto far_ = makePlayer(2.9f);  // reach 2.6
        setFetch({far_->handle(), near_->handle()});
        tick();
        check(g_attacks.size() == 1, "single mode attacks exactly one target");
        check(!g_attacks.empty() && g_attacks[0].target == near_->handle(),
              "the nearest target is picked regardless of fetch order");
        check(!g_attacks.empty() && g_attacks[0].gameMode == survivalMode.handle() &&
                  g_attacks[0].kind == 1,
              "attack goes through the derived GameMode slot's survival attack");
        check(!g_attacks.empty() && near(g_attacks[0].hit.x, 2.3f) &&
                  near(g_attacks[0].hit.y, 65.62f) && near(g_attacks[0].hit.z, 0.3f),
              "hit position is the closest point of the target box");
        check(g_swingCalls == 1 && g_lastSwingSource == 4,
              "arm swing played once with the Attack swing source");
    }
    {
        auto outOfRange = makePlayer(3.9f); // reach 3.6 > 3.0
        setFetch({outOfRange->handle()});
        tick();
        check(g_attacks.empty(), "target past the attack range is ignored");

        auto atLimit = makePlayer(3.3f); // reach exactly 3.0
        setFetch({atLimit->handle()});
        tick();
        check(g_attacks.size() == 1, "target exactly at the range limit is attacked");
    }
    {
        auto aPlayer = makePlayer(2.3f);
        auto aMob = makeMob(2.3f);
        mod.attackPlayers = false;
        setFetch({aPlayer->handle(), aMob->handle()});
        tick();
        check(g_attacks.size() == 1 && g_attacks[0].target == aMob->handle(),
              "players are skipped when Attack Players is off");
        mod.attackPlayers = true;
        mod.attackMobs = false;
        tick();
        check(g_attacks.size() == 1 && g_attacks[0].target == aPlayer->handle(),
              "mobs are skipped when Attack Mobs is off");
        mod.attackMobs = true;
    }
    {
        auto itemOwned = std::make_unique<FakeActor>();
        itemOwned->setBox(1.0f, 64.0f, 0.0f, 0.25f, 0.25f);
        itemOwned->setCategories(offs::ActorCategories::IsItem);
        auto* item = keep(std::move(itemOwned));
        setFetch({item->handle(), player->handle(), nullptr});
        tick();
        check(g_attacks.empty(), "items, the local player and null entries are never attacked");

        auto zeroBox = makePlayer(1.0f);
        zeroBox->shape.aabb = {{0, 0, 0}, {0, 0, 0}};
        setFetch({zeroBox->handle()});
        tick();
        check(g_attacks.empty(), "actors with an empty box are never attacked");
    }
    {
        auto ghost = makePlayer(2.3f);
        g_invisible.insert(ghost->handle());
        setFetch({ghost->handle()});
        tick();
        check(g_attacks.empty(), "invisible targets are skipped by default");
        mod.attackInvisible = true;
        tick();
        check(g_attacks.size() == 1, "invisible targets are attacked when enabled");
        mod.attackInvisible = false;
        g_invisible.clear();
    }

    std::printf("killaura multi attack and CPS gate\n");
    {
        auto a = makePlayer(2.3f);
        auto b = makePlayer(2.9f);
        auto c = makeMob(2.6f);
        mod.multiAttack = true;
        setFetch({b->handle(), c->handle(), a->handle(), player->handle(), nullptr});
        tick();
        check(g_attacks.size() == 3, "multi attack strikes every target in range once");
        check(g_attacks.size() == 3 && g_attacks[0].target == a->handle() &&
                  g_attacks[2].target == b->handle(),
              "multi attack hits nearest first");
        check(g_swingCalls == 1, "multi attack still swings only once per interval");
        mod.multiAttack = false;
    }
    {
        auto target = makePlayer(2.3f);
        setFetch({target->handle()});
        g_attacks.clear();
        mod.onDisable(); // reset timer once; the following ticks are not reset
        mod.onLocalPlayerTick(player->handle());
        mod.onLocalPlayerTick(player->handle());
        check(g_attacks.size() == 1, "CPS gate blocks an immediate second attack");
        std::this_thread::sleep_for(std::chrono::milliseconds(120)); // > 100 ms at 10 CPS
        mod.onLocalPlayerTick(player->handle());
        check(g_attacks.size() == 2, "attack fires again after the CPS interval");
    }

    std::printf("killaura rotation\n");
    {
        auto target = makePlayer(-2.9f); // closest point is 2.6 blocks at -X
        mod.rotateToTarget = true;
        setFetch({target->handle()});
        tick();
        check(g_attacks.size() == 1, "rotation mode still attacks");
        check(near(player->rot.y, 90.0f, 1e-2f) && near(player->rot.x, 0.0f, 1e-2f),
              "view snapped onto the target (yaw 90 toward -X)");
        mod.rotateToTarget = false;
        player->rot = Vec2{0, 0};
        tick();
        check(player->rot.y == 0.0f && player->rot.x == 0.0f,
              "view is untouched when rotation is off");
    }

    std::printf("killaura GameMode resolution\n");
    {
        auto target = makePlayer(2.3f);
        setFetch({target->handle()});

        // Wrong object in the derived slot: its slot-16 function is not a
        // known attack implementation, so the fingerprint rejects it and -
        // with nothing captured yet - no attack may happen at all.
        player->setGameModeSlot(badMode.handle());
        s_capturedGameMode = nullptr;
        s_capturedGameModePlayer = nullptr;
        tick();
        check(g_attacks.empty(),
              "a GameMode slot that fails the vtable fingerprint is never called");

        // Capture the real GameMode from the game's own action events.
        mod.enabled = false; // keep the setup ticks from attacking
        bedrocktools::events::LocalPlayerTickEvent tickEvent{
            reinterpret_cast<bedrocktools::sdk::Player*>(player->handle())};
        bedrocktools::events::bus().publish(tickEvent);
        bedrocktools::events::GameModeActionEvent actionEvent{
            bedrocktools::events::GameModeType::SurvivalMode,
            bedrocktools::events::GameModeAction::Attack,
            survivalMode.handle()};
        bedrocktools::events::bus().publish(actionEvent);
        mod.enabled = true;
        tick();
        check(g_attacks.size() == 1 && g_attacks[0].gameMode == survivalMode.handle(),
              "event-captured GameMode is used when the derived slot does not validate");

        // Derived slot valid again: it wins even with nothing captured.
        player->setGameModeSlot(survivalMode.handle());
        s_capturedGameMode = nullptr;
        s_capturedGameModePlayer = nullptr;
        tick();
        check(g_attacks.size() == 1, "derived slot works without any captured pointer");
    }

    std::printf("killaura screens, visibility and failed attacks\n");
    {
        auto target = makePlayer(2.3f);
        setFetch({target->handle()});

        bedrocktools::events::ScreenStateEvent opened{
            bedrocktools::events::ScreenKind::Container,
            bedrocktools::events::ScreenPhase::Opened, nullptr};
        bedrocktools::events::bus().publish(opened);
        tick();
        check(g_attacks.empty(), "no attacks while a container screen is open");
        bedrocktools::events::ScreenStateEvent closed{
            bedrocktools::events::ScreenKind::Container,
            bedrocktools::events::ScreenPhase::Closed, nullptr};
        bedrocktools::events::bus().publish(closed);
        tick();
        check(g_attacks.size() == 1, "attacks resume after the screen closes");
    }
    {
        auto target = makePlayer(2.3f);
        FakeDimension dimension;
        player->setDimension(dimension.handle());
        setFetch({target->handle()});

        mod.onlyVisibleTargets = true;
        g_solidAll = true; // wall of solid voxels between eye and target
        tick();
        check(g_attacks.empty(), "Only Visible Targets skips actors behind solid blocks");
        g_solidAll = false;
        tick();
        check(g_attacks.size() == 1, "visible targets are attacked with the option on");
        mod.onlyVisibleTargets = false;
        player->setDimension(nullptr);
    }
    {
        auto target = makePlayer(2.3f);
        setFetch({target->handle()});
        g_attackResult = false; // game rejects the swing (cooldown, blocked, ...)
        g_attacks.clear();
        g_swingCalls = 0;
        mod.onDisable();
        mod.onLocalPlayerTick(player->handle());
        check(g_attacks.size() == 1 && g_swingCalls == 0,
              "a rejected attack plays no swing");
        mod.onLocalPlayerTick(player->handle());
        check(g_attacks.size() == 1,
              "a rejected attack still consumes the CPS interval (no retry spam)");
        g_attackResult = true;
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all killaura behavior checks passed\n");
    return 0;
}
