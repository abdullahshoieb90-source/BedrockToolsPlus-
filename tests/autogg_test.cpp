// Unit tests for the AutoGG module ported from upstream BedrockTools.
//
// Covers the parts that decide whether a "gg" packet is sent:
//   * the end-of-match title and chat patterns
//   * the TextPacket that is built for the reply (chat variant, author/message)
//   * the one-reply-per-match cooldown, and silence while the module is off
//   * config compatibility with the upstream "ggMessage" key
//
// The module is included directly so its anonymous-namespace helpers and its
// network detours are reachable. Tests/autogg_fakepl supplies a pl::memory hook
// fake that records the detours installed by onInit().
//
// Build and run standalone:
//
//     g++ -std=c++20 -I include -I src -I tests/autogg_fakepl -I tests/fakejson
//         tests/autogg_test.cpp -o /tmp/autogg_test
//     /tmp/autogg_test

#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Real declarations first, then the stubs the module links against.
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include "core/GameHooks.hpp"

namespace O = bedrocktools::sdk::offsets;

namespace {

constexpr std::uintptr_t kTitleAddress = 0x1000;
constexpr std::uintptr_t kTextAddress = 0x2000;

// The reply packet captured inside sendToServer, before the module's
// shared_ptr releases the buffer.
struct SentReply {
    std::uint32_t variant = 0;
    std::uint8_t type = 0;
    std::string message;
};

std::vector<SentReply> g_sent;
void* g_sender = reinterpret_cast<void*>(0x4000);

std::shared_ptr<void> testCreatePacket(int id) {
    (void)id;
    const std::size_t payloadSize = 0xC0;
    auto* raw = new unsigned char[O::Packet::Size + payloadSize]();

    // The real packet carries an empty message string; the module destructs it
    // and rewrites the variant. Construct both string slots so the deleter is
    // always safe even when the module bails out early.
    const auto payload = reinterpret_cast<std::uintptr_t>(raw) + O::Packet::Size;
    new (reinterpret_cast<void*>(payload + O::TextPacketPayload::AuthorAndMessage::mAuthor))
        std::string();
    new (reinterpret_cast<void*>(payload + O::TextPacketPayload::AuthorAndMessage::mMessage))
        std::string();

    return std::shared_ptr<void>(raw, [](void* pointer) {
        auto* base = static_cast<unsigned char*>(pointer);
        const auto payload = reinterpret_cast<std::uintptr_t>(base) + O::Packet::Size;
        reinterpret_cast<std::string*>(
            payload + O::TextPacketPayload::AuthorAndMessage::mAuthor)->~basic_string();
        reinterpret_cast<std::string*>(
            payload + O::TextPacketPayload::AuthorAndMessage::mMessage)->~basic_string();
        delete[] base;
    });
}

void* testSendToServer(void* sender, void* packet) {
    (void)sender;
    const auto payload = reinterpret_cast<std::uintptr_t>(packet) + O::Packet::Size;

    SentReply reply;
    reply.variant = *reinterpret_cast<const std::uint32_t*>(
        payload + O::TextPacketPayload::mVariantIndex);
    reply.type = *reinterpret_cast<const std::uint8_t*>(
        payload + O::TextPacketPayload::AuthorAndMessage::mType);
    reply.message = *reinterpret_cast<const std::string*>(
        payload + O::TextPacketPayload::AuthorAndMessage::mMessage);
    g_sent.push_back(std::move(reply));
    return nullptr;
}

void* testGetPacketSender(void*) {
    return g_sender;
}

void* testClientInstance() {
    return reinterpret_cast<void*>(0x3000);
}

} // namespace

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId id) {
    switch (id) {
        case SignatureId::ClientNetworkHandlerHandleSetTitle:
            return kTitleAddress;
        case SignatureId::ClientNetworkHandlerHandleText:
            return kTextAddress;
        case SignatureId::LoopbackPacketSenderSendToServer:
            return reinterpret_cast<std::uintptr_t>(&testSendToServer);
        case SignatureId::ClientInstanceGetPacketSender:
            return reinterpret_cast<std::uintptr_t>(&testGetPacketSender);
        case SignatureId::MinecraftPacketsCreatePacket:
            return reinterpret_cast<std::uintptr_t>(&testCreatePacket);
        default:
            return 0;
    }
}
} // namespace bedrocktools::memory

namespace bedrocktools::core::gamehooks {
void* clientInstance() {
    return testClientInstance();
}
} // namespace bedrocktools::core::gamehooks

// The production build defines the bus in src/core/events/Events.cpp; the host
// test provides its own instance so the module's event subscriptions and the
// tests below share it without linking the game runtime.
namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

// Module under test.
#include "modules/player/autogg.cpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

using Detour = void (*)(void*, void*, void*);

// Payload buffer large enough for the title and text packet layouts.
struct TestPacket {
    alignas(16) unsigned char storage[O::Packet::Size + 0x100]{};

    void* raw() { return storage; }
    std::uintptr_t payload() { return reinterpret_cast<std::uintptr_t>(storage) + O::Packet::Size; }
};

void makeTitlePacket(TestPacket& packet, int type, const std::string& text) {
    const auto payload = packet.payload();
    *reinterpret_cast<int*>(payload + O::SetTitlePacketPayload::mType) = type;
    new (reinterpret_cast<void*>(payload + O::SetTitlePacketPayload::mTitleText))
        std::string(text);
}

void makeTextPacket(TestPacket& packet, std::uint32_t variant, const std::string& text) {
    const auto payload = packet.payload();
    *reinterpret_cast<std::uint32_t*>(
        payload + O::TextPacketPayload::mVariantIndex) = variant;
    *reinterpret_cast<std::uint8_t*>(
        payload + O::TextPacketPayload::MessageOnly::mType) = 1;

    const auto messageOffset = variant == 1
        ? O::TextPacketPayload::AuthorAndMessage::mMessage
        : O::TextPacketPayload::MessageOnly::mMessage;
    new (reinterpret_cast<void*>(payload + messageOffset)) std::string(text);
}

const char* kWonTitle = "\xC2\xA7" "f\xC2\xA7" "aYou won the game!";
const char* kLostChat = "You lost the game!";
const char* kHiveChat = "\xC2\xA7" "a has won the game!";

// Minimal in-memory actor used to drive the kill/death channels. The synced
// data wrapper at Actor::mEntityData points at a two-entry item array whose
// health slot (data id 1) is writable, and the name slot holds a real
// std::string so the kill reply can read the victim's name.
class FakeActor {
public:
    FakeActor() {
        m_items.assign(O::ActorDataIds::Health + 1, nullptr);
        m_items[O::ActorDataIds::Health] = m_healthItem;
        // SynchedActorData layout the module walks: item vector begin/end at
        // +0x00/+0x08, then the dirty and present bit masks.
        *reinterpret_cast<void**>(m_component) = m_items.data();
        *reinterpret_cast<void**>(m_component + sizeof(void*)) =
            m_items.data() + m_items.size();

        *reinterpret_cast<void**>(m_storage + O::Actor::mEntityData) = m_component;
        *reinterpret_cast<int*>(m_storage + O::Actor::mHurtTime) = 0;
        new (m_storage + O::Player::mName) std::string("Victim");
        setPlayer(true);
        setHealth(20.0f);
        setHealthPresent(true);
    }

    ~FakeActor() {
        reinterpret_cast<std::string*>(m_storage + O::Player::mName)->~basic_string();
    }

    FakeActor(const FakeActor&) = delete;
    FakeActor& operator=(const FakeActor&) = delete;

    bedrocktools::sdk::Actor* actor() {
        return reinterpret_cast<bedrocktools::sdk::Actor*>(m_storage);
    }
    bedrocktools::sdk::Player* player() {
        return reinterpret_cast<bedrocktools::sdk::Player*>(m_storage);
    }

    void setPlayer(bool player) {
        *reinterpret_cast<std::uint32_t*>(m_storage + O::Actor::mCategories) =
            player ? O::ActorCategories::IsPlayer : O::ActorCategories::IsMob;
    }

    void setHealth(float value) {
        writeHeader(O::DataItem::FloatType);
        *reinterpret_cast<float*>(m_healthItem + O::DataItem::mValue) = value;
    }

    void setHealthInt(int value) {
        writeHeader(O::DataItem::IntType);
        *reinterpret_cast<int*>(m_healthItem + O::DataItem::mValue) = value;
    }

    void removeHealthItem() {
        m_items[O::ActorDataIds::Health] = nullptr;
    }

    void restoreHealthItem() {
        m_items[O::ActorDataIds::Health] = m_healthItem;
    }

    // Mirrors the present bit SynchedActorData keeps for every data id. A
    // server that never sends HEALTH leaves it clear even though the item
    // exists with a default value of zero.
    void setHealthPresent(bool present) {
        auto* word = reinterpret_cast<std::uint64_t*>(m_component + kSynchedPresentMask);
        const auto bit = std::uint64_t{1} << (O::ActorDataIds::Health % 64);
        if (present) {
            *word |= bit;
        } else {
            *word &= ~bit;
        }
    }

private:
    void writeHeader(std::uint8_t type) {
        *reinterpret_cast<std::uint8_t*>(m_healthItem + O::DataItem::mType) = type;
        *reinterpret_cast<std::uint16_t*>(m_healthItem + O::DataItem::mId) =
            static_cast<std::uint16_t>(O::ActorDataIds::Health);
    }

    alignas(16) unsigned char m_storage[0xC00]{};
    alignas(16) unsigned char m_healthItem[0x18]{};
    std::vector<void*> m_items;
    alignas(16) unsigned char m_component[0x48]{};
};


} // namespace

int main() {
    std::printf("AutoGG\n");

    // End-of-match detection, including the color-coded server strings.
    check(isGameEndTitle(kWonTitle), "color-coded vanilla win title detected");
    check(isGameEndTitle("   "), "Hive spacer title detected");
    check(isGameEndTitle("Out of Time!"), "Hive time-up title detected");
    check(!isGameEndTitle("Waiting for players"), "ordinary title ignored");
    check(isGameEndText("Finished"), "Finished chat line detected");
    check(isGameEndText(kLostChat), "vanilla lost line detected");
    check(isGameEndText(kHiveChat), "Hive winner line detected");
    check(!isGameEndText("gg"), "ordinary chat ignored");

    AutoGG module;
    check(std::string_view(module.moduleId) == "bedrocktoolsplus.AutoGG",
          "module id matches the upstream menu entry");
    check(module.message() == "gg", "default reply is gg");

    // The saved key is the one upstream used, so existing configs carry over.
    nlohmann::json config;
    module.saveConfig(config);
    check(config.contains("ggMessage"), "config keeps the upstream ggMessage key");
    check(config["ggMessage"].get<std::string>() == "gg", "saved reply round-trips");

    module.onInit();
    auto* titleDetour = reinterpret_cast<Detour>(
        pl::memory::detourForTarget(reinterpret_cast<void*>(kTitleAddress)));
    auto* textDetour = reinterpret_cast<Detour>(
        pl::memory::detourForTarget(reinterpret_cast<void*>(kTextAddress)));
    check(titleDetour != nullptr, "title handler hooked");
    check(textDetour != nullptr, "text handler hooked");

    module.setMasterEnabled(true);

    // A match-end title sends one chat reply.
    TestPacket title;
    makeTitlePacket(title, 0, kWonTitle);
    titleDetour(nullptr, nullptr, title.raw());
    check(g_sent.size() == 1, "end title sends exactly one reply");
    check(g_sent.size() == 1 && g_sent[0].message == "gg", "reply text is gg");
    check(g_sent.size() == 1 && g_sent[0].variant == 1,
          "reply uses the TextPacket chat variant");
    check(g_sent.size() == 1 && g_sent[0].type == 1, "reply is typed as a chat message");

    // The chat line that follows the same match end must not send again.
    TestPacket chat;
    makeTextPacket(chat, 0, "Finished");
    textDetour(nullptr, nullptr, chat.raw());
    check(g_sent.size() == 1, "cooldown suppresses the second end packet");

    // Disabled module never replies, even with the cooldown elapsed.
    module.setMasterEnabled(false);
    TestPacket disabledTitle;
    makeTitlePacket(disabledTitle, 0, kWonTitle);
    titleDetour(nullptr, nullptr, disabledTitle.raw());
    check(g_sent.size() == 1, "disabled module stays silent");

    // Custom message from the menu, then a chat-only match end.
    config = nlohmann::json{};
    module.saveConfig(config);
    config["ggMessage"] = std::string("Good game");
    module.loadConfig(config);
    check(module.message() == "Good game", "custom reply loaded");
    module.setMasterEnabled(true);  // also clears the cooldown

    TestPacket hiveChat;
    makeTextPacket(hiveChat, 0, kHiveChat);
    textDetour(nullptr, nullptr, hiveChat.raw());
    check(g_sent.size() == 2, "end chat line sends a reply");
    check(g_sent.size() == 2 && g_sent[1].message == "Good game",
          "custom reply text is sent");

    // A title that is not an end screen is ignored.
    module.setMasterEnabled(false);
    module.setMasterEnabled(true);
    TestPacket normalTitle;
    makeTitlePacket(normalTitle, 0, "Waiting for players");
    titleDetour(nullptr, nullptr, normalTitle.raw());
    check(g_sent.size() == 2, "non-end title ignored");

    // Actionbar/other title types are not end screens either.
    module.setMasterEnabled(false);
    module.setMasterEnabled(true);
    TestPacket actionBar;
    makeTitlePacket(actionBar, 2, kWonTitle);
    titleDetour(nullptr, nullptr, actionBar.raw());
    check(g_sent.size() == 2, "title types other than title/subtitle ignored");

    // The author-and-message variant of a TextPacket is read as well.
    module.setMasterEnabled(false);
    module.setMasterEnabled(true);
    TestPacket authored;
    makeTextPacket(authored, 1, kLostChat);
    textDetour(nullptr, nullptr, authored.raw());
    check(g_sent.size() == 3, "author-and-message variant detected");

    // Oversized replies are clamped to the menu limit.
    config["ggMessage"] = std::string(400, 'g');
    module.loadConfig(config);
    check(module.message().size() == AutoGG::MaxMessageLength, "long reply is clamped");

    // An empty reply disables sending without poking the packet path.
    config["ggMessage"] = std::string();
    module.loadConfig(config);
    module.setMasterEnabled(false);
    module.setMasterEnabled(true);
    TestPacket emptyTitle;
    makeTitlePacket(emptyTitle, 0, kWonTitle);
    titleDetour(nullptr, nullptr, emptyTitle.raw());
    check(g_sent.size() == 3, "empty reply sends nothing");


    // ------------------------------------------------------------------
    // Kill / death reaction channels
    // ------------------------------------------------------------------
    nlohmann::json reactions;
    module.saveConfig(reactions);
    check(reactions["m_killMessage"].get<bool>() == false,
          "kill messages are off by default");
    check(reactions["m_deathMessage"].get<bool>() == false,
          "death messages are off by default");
    check(reactions["killMessageText"].get<std::string>() == "gg",
          "default kill reply is gg");
    check(reactions["deathMessageText"].get<std::string>() == "F",
          "default death reply is F");
    check(reactions["killPlayersOnly"].get<bool>() == true,
          "players-only defaults to on");

    auto reactivate = [&module] {
        module.setMasterEnabled(false);
        module.setMasterEnabled(true);
    };

    reactions["ggMessage"] = std::string("gg");
    reactions["m_killMessage"] = true;
    reactions["killMessageText"] = std::string("gg {name}");
    reactions["killPlayersOnly"] = true;
    reactions["m_deathMessage"] = false;
    module.loadConfig(reactions);
    reactivate();

    FakeActor localPlayer;
    bedrocktools::events::LocalPlayerTickEvent tick{};
    tick.player = localPlayer.player();

    // A hit alone must not produce a reply: the victim has to die inside the
    // confirmation window.
    const std::size_t beforeHit = g_sent.size();
    FakeActor victim;
    bedrocktools::events::AttackEvent hit(
        bedrocktools::events::AttackKind::GameMode, nullptr, victim.actor(), nullptr, nullptr);
    bedrocktools::events::bus().publish(hit);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeHit, "a hit that does not kill stays silent");

    victim.setHealth(0.0f);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeHit + 1, "confirmed kill sends one reply");
    check(g_sent.size() == beforeHit + 1 && g_sent.back().message == "gg Victim",
          "kill reply substitutes the victim name");

    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeHit + 1, "a confirmed kill is reported once");

    // A hit on an already dead actor is never reported as a kill.
    reactivate();
    const std::size_t beforeCorpse = g_sent.size();
    FakeActor corpse;
    corpse.setHealth(0.0f);
    bedrocktools::events::AttackEvent corpseHit(
        bedrocktools::events::AttackKind::GameMode, nullptr, corpse.actor(), nullptr, nullptr);
    bedrocktools::events::bus().publish(corpseHit);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeCorpse, "a hit on an already dead player is not a kill");

    // Servers that never sync HEALTH: the item exists but its present bit is
    // clear and it reads as zero, which must not be treated as a kill.
    const std::size_t beforeUnsynced = g_sent.size();
    FakeActor unsynced;
    unsynced.setHealthPresent(false);
    bedrocktools::events::AttackEvent unsyncedHit(
        bedrocktools::events::AttackKind::GameMode, nullptr, unsynced.actor(), nullptr, nullptr);
    bedrocktools::events::bus().publish(unsyncedHit);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeUnsynced, "unsynced health defaults are ignored");

    // But a victim whose health only becomes readable after the swing still
    // counts once it was seen alive and then reaches zero.
    reactivate();
    const std::size_t beforeLate = g_sent.size();
    FakeActor lateVictim;
    lateVictim.removeHealthItem();
    bedrocktools::events::AttackEvent lateHit(
        bedrocktools::events::AttackKind::GameMode, nullptr, lateVictim.actor(), nullptr, nullptr);
    bedrocktools::events::bus().publish(lateHit);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeLate, "no reply while the victim health is unreadable");

    lateVictim.restoreHealthItem();
    bedrocktools::events::bus().publish(tick);
    lateVictim.setHealth(0.0f);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeLate + 1,
          "a victim first seen alive then dead counts as a kill");

    // players-only: a mob reaching zero health is not a player kill.
    reactivate();
    const std::size_t beforeMob = g_sent.size();
    FakeActor mob;
    mob.setPlayer(false);
    bedrocktools::events::AttackEvent mobHit(
        bedrocktools::events::AttackKind::GameMode, nullptr, mob.actor(), nullptr, nullptr);
    bedrocktools::events::bus().publish(mobHit);
    mob.setHealth(0.0f);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeMob, "mob kills are ignored while players-only is on");

    // With players-only off the same mob kill is reported.
    reactions["killPlayersOnly"] = false;
    reactions["killMessageText"] = std::string("gg");
    module.loadConfig(reactions);
    reactivate();
    FakeActor mob2;
    mob2.setPlayer(false);
    bedrocktools::events::AttackEvent mobHit2(
        bedrocktools::events::AttackKind::GameMode, nullptr, mob2.actor(), nullptr, nullptr);
    bedrocktools::events::bus().publish(mobHit2);
    mob2.setHealth(0.0f);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeMob + 1, "players-only off allows mob kills");

    // A victim whose synced health is unavailable is dropped, never guessed.
    reactivate();
    const std::size_t beforeUnreadable = g_sent.size();
    FakeActor unreadable;
    unreadable.removeHealthItem();
    bedrocktools::events::AttackEvent unreadableHit(
        bedrocktools::events::AttackKind::GameMode, nullptr, unreadable.actor(), nullptr, nullptr);
    bedrocktools::events::bus().publish(unreadableHit);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeUnreadable, "unreadable victim health is ignored");

    // Death channel: one reply per death, re-armed by the respawn.
    reactions["m_killMessage"] = false;
    reactions["m_deathMessage"] = true;
    reactions["deathMessageText"] = std::string("F");
    module.loadConfig(reactions);
    reactivate();

    localPlayer.setHealth(20.0f);
    bedrocktools::events::bus().publish(tick);
    const std::size_t beforeDeath = g_sent.size();
    localPlayer.setHealth(0.0f);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeDeath + 1, "death sends the configured reply");
    check(g_sent.size() == beforeDeath + 1 && g_sent.back().message == "F",
          "death reply text is used");

    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeDeath + 1, "one reply per death");

    // Servers that sync health as an int are handled too.
    localPlayer.setHealthInt(20);
    bedrocktools::events::bus().publish(tick);
    reactivate();
    localPlayer.setHealthInt(0);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == beforeDeath + 2, "int-typed synced health is read");

    // A death right after the match-end reply is swallowed: the GG already
    // covers the moment.
    module.loadConfig(reactions);
    reactivate();
    TestPacket finalTitle;
    makeTitlePacket(finalTitle, 0, kWonTitle);
    titleDetour(nullptr, nullptr, finalTitle.raw());
    const std::size_t afterGg = g_sent.size();
    check(g_sent.size() == beforeDeath + 3, "end title still replies");

    localPlayer.setHealth(20.0f);
    bedrocktools::events::bus().publish(tick);
    localPlayer.setHealth(0.0f);
    bedrocktools::events::bus().publish(tick);
    check(g_sent.size() == afterGg, "a death right after the GG stays silent");

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
