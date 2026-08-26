// Unit tests for the Player Health module.
//
// Part one checks the pure formatting helpers in
// src/modules/visual/playerhealth_format.hpp (hearts / bar / numbers lines,
// ratio colors, radio serialization).
//
// Part two builds the real module (src/modules/visual/playerhealth.cpp,
// compiled by scripts/run_tests.sh as a second translation unit) and drives
// PlayerHealthModule::onLocalPlayerTick against fake actor memory laid out
// exactly as include/bedrocktools/sdk/offsets/World.hpp documents it:
//
//   * the health line is appended under the name ("Alex\n<line>") and is
//     refreshed from the synced Health data item (int, float, and short layouts)
//   * unchanged health does not rewrite the nametag
//   * a nametag renamed under us becomes the new base name
//   * players without a synced health item are left untouched
//   * the range limit restores far players and skips them
//   * the local player is skipped unless Show Self is on
//   * despawned players are dropped without touching stale memory
//   * disabling restores every live nametag and the always-show data item
//   * a server-set always-show item is preserved, not reset
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I tests/fakejson -I tests/fakepl
//         tests/playerhealth_test.cpp src/modules/visual/playerhealth.cpp
//         -o /tmp/playerhealth_test
//     /tmp/playerhealth_test

#include "modules/visual/playerhealth.hpp"
#include "modules/visual/playerhealth_format.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ph = bedrocktools::visual::playerhealth;
namespace off = bedrocktools::sdk::offsets;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

std::string cc(char code) { return ph::cc(code); }
std::string hearts(int n) { std::string s; for (int i = 0; i < n; ++i) s += ph::kHeart; return s; }
std::string bars(int n) { return std::string(static_cast<std::size_t>(n), '|'); }

// --------------------------------------------------------------------------
// Fake game functions backing bedrocktools::memory::resolve().
// --------------------------------------------------------------------------

std::unordered_map<void*, std::string> g_names;
std::unordered_set<void*> g_players;
std::vector<void*> g_actors;
std::unordered_map<void*, float> g_funcHealth;
int g_setNameTagCalls = 0;
int g_alwaysShowUpdateCalls = 0;

std::string fakeGetNameTag(void* actor) {
    const auto it = g_names.find(actor);
    return it == g_names.end() ? std::string{} : it->second;
}

void fakeSetNameTag(void* actor, const std::string& name) {
    g_names[actor] = name;
    ++g_setNameTagCalls;
}

bool fakeIsPlayer(void* actor) { return g_players.count(actor) != 0; }
std::vector<void*> fakeGetRuntimeActorList(void*) { return g_actors; }
void fakeEnsureIndex(void*, std::uint16_t) {}
void fakeUpdateAlwaysShowNameTag(void*, const void*) { ++g_alwaysShowUpdateCalls; }

float fakeMobGetHealth(void* actor) {
    const auto it = g_funcHealth.find(actor);
    return it == g_funcHealth.end() ? -1.0f : it->second;
}

}  // namespace

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId id) {
    switch (id) {
        case SignatureId::ActorManagerList: return reinterpret_cast<std::uintptr_t>(&fakeGetRuntimeActorList);
        case SignatureId::ActorIsPlayer: return reinterpret_cast<std::uintptr_t>(&fakeIsPlayer);
        case SignatureId::ActorGetNameTag: return reinterpret_cast<std::uintptr_t>(&fakeGetNameTag);
        case SignatureId::ActorSetNameTag: return reinterpret_cast<std::uintptr_t>(&fakeSetNameTag);
        case SignatureId::SynchedActorDataEnsureIndex: return reinterpret_cast<std::uintptr_t>(&fakeEnsureIndex);
        case SignatureId::ActorSynchedDataUpdateAlwaysShowNameTag: return reinterpret_cast<std::uintptr_t>(&fakeUpdateAlwaysShowNameTag);
        case SignatureId::MobGetHealth: return reinterpret_cast<std::uintptr_t>(&fakeMobGetHealth);
        default: return 0;
    }
}
}  // namespace bedrocktools::memory

namespace bedrocktools::events {
// Link stub: onInit() subscribes through the event bus, which the host test
// never drives. The EventBus class itself is fully inline.
EventBus& bus() {
    static EventBus instance;
    return instance;
}
}  // namespace bedrocktools::events

namespace {

// --------------------------------------------------------------------------
// Fake actor memory, mirroring the documented offsets.
// --------------------------------------------------------------------------

struct FakeDataItem {
    void* vtable = nullptr;            // +0x0
    std::uint8_t type = 0;             // +0x8
    std::uint8_t pad = 0;              // +0x9
    std::uint16_t id = 0;              // +0xA
    std::uint32_t header = 1;          // +0xC (dirty/header, not the payload)
    std::int32_t value = 0;            // +0x10 (int / float / byte views)
};

static_assert(offsetof(FakeDataItem, value) == off::DataItem::mValue);
static_assert(sizeof(FakeDataItem) >= off::DataItem::mMinimumSize);

struct FakeComponent {
    void** begin = nullptr;            // +0x0
    void** end = nullptr;              // +0x8
    void** cap = nullptr;              // +0x10
    std::uint64_t dirty[3] = {};       // +0x18
    std::uint64_t present[3] = {};     // +0x30
};

struct FakeStateVector {
    bedrocktools::sdk::Vec3 pos{};
    bedrocktools::sdk::Vec3 prev{};
};

struct FakeAttributeInstance {
    void* vtable = nullptr;
    float currentValue = 0.0f;
    float maxValue = 20.0f;
};

struct FakeActor {
    static constexpr std::size_t kActorSize = 1024;      // covers mLevel (464)
    static constexpr std::size_t kLevelSize = 0x470 + 16; // covers mActorManager

    std::vector<std::uint8_t> actor = std::vector<std::uint8_t>(kActorSize, 0);
    std::vector<std::uint8_t> level = std::vector<std::uint8_t>(kLevelSize, 0);
    FakeComponent component{};
    std::array<void*, 96> items{};
    FakeDataItem healthItem;   // id 1, int
    FakeDataItem floatHealthItem;  // id 1, float
    FakeDataItem colorItem;    // id 3, byte (vtable donor)
    FakeDataItem alwaysShowItem; // id 81, byte
    FakeStateVector stateVector{};
    FakeAttributeInstance healthAttribute;

    void* ptr() { return actor.data(); }

    void wire(const char* name, bool isPlayer, float x, float y, float z,
              bool withHealth, bool floatHealth = false, int health = 20,
              bool withAlwaysShow = false, std::int8_t alwaysShow = 0,
              bool withAttr = false, float attrHealth = 20.0f) {
        std::memset(actor.data(), 0, actor.size());
        std::memset(level.data(), 0, level.size());

        colorItem = FakeDataItem{};
        colorItem.vtable = reinterpret_cast<void*>(0x1000);  // vtable donor for created items
        colorItem.type = 0;
        colorItem.id = 3;
        items[3] = &colorItem;

        if (withHealth) {
            if (floatHealth) {
                floatHealthItem = FakeDataItem{};
                floatHealthItem.type = static_cast<std::uint8_t>(off::DataItem::FloatType);
                floatHealthItem.id = 1;
                float v = static_cast<float>(health);
                std::memcpy(&floatHealthItem.value, &v, sizeof(v));
                items[1] = &floatHealthItem;
            } else {
                healthItem = FakeDataItem{};
                healthItem.type = static_cast<std::uint8_t>(off::DataItem::IntType);
                healthItem.id = 1;
                healthItem.value = health;
                items[1] = &healthItem;
            }
        }

        if (withAlwaysShow) {
            alwaysShowItem = FakeDataItem{};
            alwaysShowItem.type = 0;
            alwaysShowItem.id = 81;
            alwaysShowItem.value = alwaysShow;
            items[81] = &alwaysShowItem;
        }

        component.begin = items.data();
        component.end = items.data() + items.size();
        component.cap = items.data() + items.size();

        *reinterpret_cast<void**>(actor.data() + off::Actor::mEntityData) = &component;
        stateVector.pos = {x, y, z};
        *reinterpret_cast<void**>(actor.data() + off::Actor::mStateVectorComponent) = &stateVector;
        *reinterpret_cast<void**>(actor.data() + off::Actor::mLevel) = level.data();
        *reinterpret_cast<void**>(level.data() + off::Level::mActorManager) = reinterpret_cast<void*>(0x1234);

        if (withAttr) {
            healthAttribute.currentValue = attrHealth;
            healthAttribute.maxValue = 20.0f;
            *reinterpret_cast<void**>(actor.data() + off::Mob::mHealthAttribute) = &healthAttribute;
        } else {
            *reinterpret_cast<void**>(actor.data() + off::Mob::mHealthAttribute) = nullptr;
        }

        g_names[ptr()] = name;
        if (isPlayer) g_players.insert(ptr());
    }

    void setHealth(int health) {
        healthItem.value = health;
    }

    void setAttributeHealth(float hp) {
        healthAttribute.currentValue = hp;
    }

    void useShortHealth(std::int16_t health) {
        healthItem.type = static_cast<std::uint8_t>(off::DataItem::ShortType);
        healthItem.value = 0;
        std::memcpy(&healthItem.value, &health, sizeof(health));
    }

    std::uint8_t itemByte(std::size_t id, std::size_t at) const {
        void* item = items[id];
        if (!item) return 0xFF;
        return *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(item) + at);
    }

    bool itemExists(std::size_t id) const { return items[id] != nullptr; }
};

void runTicks(PlayerHealthModule& mod, FakeActor& local, int count = 2) {
    for (int i = 0; i < count; ++i) mod.onLocalPlayerTick(local.ptr());
}

void formatTests() {
    std::printf("format helpers\n");

    const std::string fullHearts = cc('c') + hearts(10);
    check(ph::composeHearts(20.0f, 20.0f) == fullHearts, "full hearts at 20/20");
    check(ph::composeHearts(17.0f, 20.0f) == cc('c') + hearts(9) + cc('8') + hearts(1), "17/20 rounds to 9 red hearts");
    check(ph::composeHearts(0.0f, 20.0f) == cc('8') + hearts(10), "dead player is all dark hearts");
    check(ph::composeHearts(1.0f, 20.0f) == cc('c') + hearts(1) + cc('8') + hearts(9), "1 hp still shows one red heart");
    check(ph::composeHearts(30.0f, 40.0f) == cc('c') + hearts(8) + cc('8') + hearts(2), "boosted 30/40 squeezes onto 10 hearts");
    check(ph::composeHearts(5.0f, 0.0f) == cc('c') + hearts(3) + cc('8') + hearts(7), "max health 0 falls back to 20");

    check(ph::composeBar(20.0f, 20.0f) == cc('a') + bars(10), "full bar at 20/20");
    check(ph::composeBar(7.0f, 20.0f) == cc('a') + bars(4) + cc('7') + bars(6), "7/20 rounds to 4 green segments");
    check(ph::composeBar(1.0f, 20.0f) == cc('a') + bars(1) + cc('7') + bars(9), "1 hp shows one green segment");
    check(ph::composeBar(0.0f, 20.0f) == cc('7') + bars(10), "dead player is an empty bar");

    check(ph::composeNumbers(17.0f, 20.0f) == cc('a') + ph::kHeart + std::string(" 17/20"), "17/20 numbers, green ratio");
    check(ph::composeNumbers(7.0f, 20.0f) == cc('e') + ph::kHeart + std::string(" 7/20"), "7/20 numbers, yellow ratio");
    check(ph::composeNumbers(3.0f, 20.0f) == cc('c') + ph::kHeart + std::string(" 3/20"), "3/20 numbers, red ratio");
    check(ph::composeNumbers(0.0f, 20.0f) == cc('8') + ph::kHeart + std::string(" 0/20"), "0/20 numbers, dark gray ratio");
    check(ph::composeNumbers(9.5f, 20.0f).compare(0, 3, cc('e')) == 0, "9.5/20 uses the yellow threshold");

    check(ph::composeHealthLine(ph::StyleBar, 20.0f, 20.0f) == ph::composeBar(20.0f, 20.0f), "dispatch: bar");
    check(ph::composeHealthLine(ph::StyleNumbers, 20.0f, 20.0f) == ph::composeNumbers(20.0f, 20.0f), "dispatch: numbers");
    check(ph::composeHealthLine(99, 20.0f, 20.0f) == ph::composeHearts(20.0f, 20.0f), "dispatch: unknown style falls back to hearts");

    std::printf("style radio serialization\n");
    check(ph::styleRadioValue(0) == "0,hearts,bar,numbers", "radio value for hearts");
    check(ph::styleRadioValue(2) == "2,hearts,bar,numbers", "radio value for numbers");
    check(ph::styleRadioValue(99) == "0,hearts,bar,numbers", "out-of-range index falls back to hearts");
    check(ph::resolveStyleIndex("0,hearts,bar,numbers") == 0, "parse full radio value (hearts)");
    check(ph::resolveStyleIndex("2,hearts,bar,numbers") == 2, "parse full radio value (numbers)");
    check(ph::resolveStyleIndex("1") == 1, "parse bare launcher index");
    check(ph::resolveStyleIndex("bar") == 1, "parse bare style id");
    check(ph::resolveStyleIndex("") == 0, "empty value defaults to hearts");
    check(ph::resolveStyleIndex("bogus") == 0, "unknown id defaults to hearts");
    check(ph::resolveStyleIndex("9,hearts,bar,numbers") == 0, "out-of-range index defaults to hearts");
}

void moduleTests() {
    std::printf("module behavior\n");

    g_names.clear();
    g_players.clear();
    g_actors.clear();
    g_setNameTagCalls = 0;
    g_alwaysShowUpdateCalls = 0;

    FakeActor local, alex, blair, evan, carl, dana, mob;
    local.wire("Steve", true, 0, 64, 0, true, false, 20);
    alex.wire("Alex", true, 3, 64, 0, true, false, 17);
    blair.wire("Blair", true, -2, 64, 4, true, true, 7 /* float 7.0 */);
    evan.wire("Evan", true, 4, 64, 4, true, false, 20);
    evan.useShortHealth(20);
    carl.wire("Carl", true, 5, 64, 5, false);           // no synced health item
    dana.wire("Dana", true, 6, 64, 6, true, false, 12, true, 1); // server-forced always-show
    mob.wire("Zombie", false, 1, 64, 1, true, false, 20);

    g_actors = {local.ptr(), alex.ptr(), blair.ptr(), evan.ptr(), carl.ptr(), dana.ptr(), mob.ptr()};

    PlayerHealthModule mod;
    mod.onInit();
    mod.setMasterEnabled(true);

    runTicks(mod, local);
    std::string expectedAlex = std::string("Alex\n") + ph::composeHearts(17.0f, 20.0f);
    check(g_names[alex.ptr()] == expectedAlex, "Alex gets the health line under the name");
    check(g_names[blair.ptr()] == "Blair\n" + ph::composeHearts(7.0f, 20.0f), "Blair reads the float health item");
    check(g_names[evan.ptr()] == "Evan\n" + ph::composeHearts(20.0f, 20.0f), "Evan reads the short health item as full health");
    check(g_names[carl.ptr()] == "Carl", "Carl (no health item) is untouched");
    check(g_names[mob.ptr()] == "Zombie", "non-player actors are untouched");
    check(g_names[local.ptr()] == "Steve", "local player skipped by default (Show Self off)");
    check(alex.itemExists(81) && alex.itemByte(81, off::DataItem::mValue) == 1, "always-show item forced to 1 for Alex");
    check(dana.itemByte(81, off::DataItem::mValue) == 1, "Dana keeps her server-set always-show value");
    check(g_alwaysShowUpdateCalls >= 1, "the always-show update callback ran");

    std::printf("idempotent refresh\n");
    const int callsBefore = g_setNameTagCalls;
    runTicks(mod, local, 4);
    check(g_setNameTagCalls == callsBefore, "unchanged health does not rewrite nametags");

    std::printf("health change and external rename\n");
    alex.setHealth(3);
    runTicks(mod, local);
    check(g_names[alex.ptr()] == "Alex\n" + ph::composeHearts(3.0f, 20.0f), "Alex's line follows the new health");

    g_names[alex.ptr()] = "Renamed";
    runTicks(mod, local);
    check(g_names[alex.ptr()] == "Renamed\n" + ph::composeHearts(3.0f, 20.0f), "external rename becomes the new base name");

    std::printf("range limit\n");
    mod.m_range = 5;
    blair.stateVector.pos = {1000.0f, 64.0f, 1000.0f};  // now far outside the range
    runTicks(mod, local);
    check(g_names[blair.ptr()] == "Blair", "Blair farther than the range is restored");
    // Bring Blair back in range.
    blair.stateVector.pos = {-2.0f, 64.0f, 4.0f};
    mod.m_range = 64;
    runTicks(mod, local);
    check(g_names[blair.ptr()] == "Blair\n" + ph::composeHearts(7.0f, 20.0f), "Blair is re-tagged inside the range");

    std::printf("show self\n");
    mod.m_showSelf = true;
    runTicks(mod, local);
    check(g_names[local.ptr()] == "Steve\n" + ph::composeHearts(20.0f, 20.0f), "Show Self tags the local player too");
    mod.m_showSelf = false;
    runTicks(mod, local);
    check(g_names[local.ptr()] == "Steve", "turning Show Self off restores the local player");

    std::printf("despawned players\n");
    g_actors.erase(std::remove(g_actors.begin(), g_actors.end(), dana.ptr()), g_actors.end());
    g_names[dana.ptr()] = "poisoned";  // would be caught by a restore attempt
    runTicks(mod, local);
    check(g_names[dana.ptr()] == "poisoned", "despawned player memory is never touched");
    check(dana.itemByte(81, off::DataItem::mValue) == 1, "despawned player keeps her server-set always-show");
    g_actors.push_back(dana.ptr());
    g_names[dana.ptr()] = "Dana";
    runTicks(mod, local);
    check(g_names[dana.ptr()] == "Dana\n" + ph::composeHearts(12.0f, 20.0f), "respawned player is tracked again");

    std::printf("style change through the config\n");
    nlohmann::json j;
    mod.saveConfig(j);
    check(j["m_style"].get<std::string>() == "0,hearts,bar,numbers", "config saves the style radio value");
    j["m_style"] = "1,hearts,bar,numbers";
    mod.loadConfig(j);
    check(mod.m_styleIndex == ph::StyleBar, "config loads the bar style");
    runTicks(mod, local);
    check(g_names[alex.ptr()] == "Renamed\n" + ph::composeBar(3.0f, 20.0f), "the line switches to the bar style");

    std::printf("disable restores everything\n");
    mod.setMasterEnabled(false);
    runTicks(mod, local, 1);
    check(g_names[alex.ptr()] == "Renamed", "Alex restored on disable");
    check(g_names[blair.ptr()] == "Blair", "Blair restored on disable");
    check(g_names[evan.ptr()] == "Evan", "Evan restored on disable");
    check(g_names[carl.ptr()] == "Carl", "Carl was never touched");
    check(g_names[dana.ptr()] == "Dana", "Dana restored on disable");
    check(g_names[local.ptr()] == "Steve", "local player still clean");
    check(alex.itemExists(81) && alex.itemByte(81, off::DataItem::mValue) == 0, "Alex's forced always-show reset to 0");
    check(dana.itemByte(81, off::DataItem::mValue) == 1, "Dana's server-set always-show survives the restore");

    std::printf("re-enable after restore\n");
    mod.setMasterEnabled(true);
    runTicks(mod, local);
    check(g_names[alex.ptr()] == "Renamed\n" + ph::composeBar(3.0f, 20.0f), "re-enable re-tags from the clean base name");
}

void damageAndRegenTests() {
    std::printf("damage and regeneration testing (Attribute and Metadata Fallback)\n");

    g_names.clear();
    g_players.clear();
    g_actors.clear();
    g_funcHealth.clear();
    g_setNameTagCalls = 0;
    g_alwaysShowUpdateCalls = 0;

    FakeActor local, attrPlayer, metaPlayer, priorityPlayer;
    local.wire("Local", true, 0, 64, 0, true, false, 20);

    // Player 1: Uses Mob/Health Attribute directly
    attrPlayer.wire("AttrPlayer", true, 2, 64, 0, false, false, 0, false, 0, true, 20.0f);

    // Player 2: Uses metadata fallback (no Mob/Health Attribute)
    metaPlayer.wire("MetaPlayer", true, 4, 64, 0, true, false, 20);

    // Player 3: Has BOTH Mob/Health Attribute (15 HP) and metadata (20 HP) to verify priority
    priorityPlayer.wire("PriorityPlayer", true, 6, 64, 0, true, false, 20, false, 0, true, 15.0f);

    g_actors = {local.ptr(), attrPlayer.ptr(), metaPlayer.ptr(), priorityPlayer.ptr()};

    PlayerHealthModule mod;
    mod.onInit();
    mod.setMasterEnabled(true);

    // Initial tick
    runTicks(mod, local);
    check(g_names[attrPlayer.ptr()] == "AttrPlayer\n" + ph::composeHearts(20.0f, 20.0f), "AttrPlayer initial full health (20/20)");
    check(g_names[metaPlayer.ptr()] == "MetaPlayer\n" + ph::composeHearts(20.0f, 20.0f), "MetaPlayer initial full health via metadata fallback (20/20)");
    check(g_names[priorityPlayer.ptr()] == "PriorityPlayer\n" + ph::composeHearts(15.0f, 20.0f), "PriorityPlayer reads Mob/Health Attribute (15) over metadata (20)");

    std::printf("damage tracking\n");
    // AttrPlayer takes damage: 20 -> 12 -> 4
    attrPlayer.setAttributeHealth(12.0f);
    metaPlayer.setHealth(12);
    priorityPlayer.setAttributeHealth(7.0f); // attribute drops to 7, metadata stays 20
    runTicks(mod, local);

    check(g_names[attrPlayer.ptr()] == "AttrPlayer\n" + ph::composeHearts(12.0f, 20.0f), "AttrPlayer health line updated after damage (12/20)");
    check(g_names[metaPlayer.ptr()] == "MetaPlayer\n" + ph::composeHearts(12.0f, 20.0f), "MetaPlayer health line updated after damage via metadata (12/20)");
    check(g_names[priorityPlayer.ptr()] == "PriorityPlayer\n" + ph::composeHearts(7.0f, 20.0f), "PriorityPlayer health line updated after attribute damage (7/20)");

    attrPlayer.setAttributeHealth(3.0f);
    metaPlayer.setHealth(3);
    runTicks(mod, local);

    check(g_names[attrPlayer.ptr()] == "AttrPlayer\n" + ph::composeHearts(3.0f, 20.0f), "AttrPlayer low health after critical damage (3/20)");
    check(g_names[metaPlayer.ptr()] == "MetaPlayer\n" + ph::composeHearts(3.0f, 20.0f), "MetaPlayer low health after critical damage via metadata (3/20)");

    std::printf("regeneration tracking\n");
    // Regeneration: 3 -> 11 -> 20
    attrPlayer.setAttributeHealth(11.0f);
    metaPlayer.setHealth(11);
    priorityPlayer.setAttributeHealth(18.0f);
    runTicks(mod, local);

    check(g_names[attrPlayer.ptr()] == "AttrPlayer\n" + ph::composeHearts(11.0f, 20.0f), "AttrPlayer health line updated after regeneration (11/20)");
    check(g_names[metaPlayer.ptr()] == "MetaPlayer\n" + ph::composeHearts(11.0f, 20.0f), "MetaPlayer health line updated after regeneration via metadata (11/20)");
    check(g_names[priorityPlayer.ptr()] == "PriorityPlayer\n" + ph::composeHearts(18.0f, 20.0f), "PriorityPlayer health line updated after attribute regeneration (18/20)");

    attrPlayer.setAttributeHealth(20.0f);
    metaPlayer.setHealth(20);
    runTicks(mod, local);

    check(g_names[attrPlayer.ptr()] == "AttrPlayer\n" + ph::composeHearts(20.0f, 20.0f), "AttrPlayer fully regenerated (20/20)");
    check(g_names[metaPlayer.ptr()] == "MetaPlayer\n" + ph::composeHearts(20.0f, 20.0f), "MetaPlayer fully regenerated via metadata (20/20)");

    std::printf("signature function Mob/Health Attribute\n");
    // Test MobGetHealth signature function return value overriding
    g_funcHealth[attrPlayer.ptr()] = 8.5f;
    runTicks(mod, local);
    check(g_names[attrPlayer.ptr()] == "AttrPlayer\n" + ph::composeHearts(8.5f, 20.0f), "MobGetHealth signature function returns live health (8.5/20)");

    // Damage via function signature: 8.5 -> 2.0
    g_funcHealth[attrPlayer.ptr()] = 2.0f;
    runTicks(mod, local);
    check(g_names[attrPlayer.ptr()] == "AttrPlayer\n" + ph::composeHearts(2.0f, 20.0f), "MobGetHealth damage updated (2/20)");

    // Regeneration via function signature: 2.0 -> 16.0
    g_funcHealth[attrPlayer.ptr()] = 16.0f;
    runTicks(mod, local);
    check(g_names[attrPlayer.ptr()] == "AttrPlayer\n" + ph::composeHearts(16.0f, 20.0f), "MobGetHealth regeneration updated (16/20)");
}

}  // namespace

int main() {
    std::printf("player health formatting\n");
    formatTests();

    std::printf("player health module\n");
    moduleTests();

    std::printf("damage and regeneration\n");
    damageAndRegenTests();

    std::printf("\n%s\n", g_failures == 0 ? "all player health tests passed" : "player health tests FAILED");
    return g_failures == 0 ? 0 : 1;
}
