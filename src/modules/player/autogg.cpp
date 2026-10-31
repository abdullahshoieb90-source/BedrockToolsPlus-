#include "autogg.hpp"

#include "core/GameHooks.hpp"
#include "core/memory/Hooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <regex>
#include <string>
#include <utility>

namespace {

namespace Offsets = bedrocktools::sdk::offsets;
using bedrocktools::memory::SignatureId;
using bedrocktools::sdk::Actor;
using bedrocktools::sdk::Player;

using SendToServerFn = void* (*)(void*, void*);
using GetPacketSenderFn = void* (*)(void*);
using CreatePacketFn = std::shared_ptr<void> (*)(int);

AutoGG* g_autoGG = nullptr;

// Resolved once and reused: the packet path runs on the network thread for
// every title/text packet the server sends.
SendToServerFn g_sendToServer = nullptr;
GetPacketSenderFn g_getPacketSender = nullptr;
CreatePacketFn g_createPacket = nullptr;

// The vanilla TextPacket id in MinecraftPackets::createPacket.
constexpr int kTextPacketId = 9;
// Type 1 of a TextPacket means "chat", which the server echoes to everyone.
constexpr std::uint32_t kChatVariant = 1;
constexpr std::uint8_t kChatPacketType = 1;
// Several end-of-match packets usually arrive together (title, subtitle and a
// chat line). Only one reply is sent per game end.
constexpr auto kSendCooldown = std::chrono::seconds(3);
// Minimum gap between two kill/death reactions, so a fight cannot produce a
// message per hit.
constexpr auto kReactionCooldown = std::chrono::seconds(3);
// A reaction is dropped when a match-end packet was seen this recently: the
// proper GG reply already covers the moment.
constexpr auto kEndSignalGuard = std::chrono::seconds(2);
// How long a hit victim is watched for its health to reach zero.
constexpr auto kKillConfirmWindow = std::chrono::seconds(2);

// Title packet types that carry text worth inspecting: title and subtitle.
constexpr int kTitleType = 0;
constexpr int kSubtitleType = 1;

// Longest victim name substituted into a kill message, in bytes.
constexpr std::size_t kMaxVictimNameBytes = 32;
// Sanity bound on the synced-data item count we are willing to walk.
constexpr std::size_t kMaxDataItems = 256;

// End-screen strings, including the section-sign color codes the game embeds.
// The escapes are split ("\xC2\xA7" "f...") so the hex escape never swallows
// the following character.
constexpr const char* kTitleMarkers[] = {
    "\xC2\xA7" "f\xC2\xA7" "aYou won the game!",
    "\xC2\xA7" "f\xC2\xA7" "cYou lost the game!",
    "\xC2\xA7" "bHiders\xC2\xA7" "r\xC2\xA7" "f Win",
    "\xC2\xA7" "eSeekers\xC2\xA7" "r\xC2\xA7" "f Win",
    "\xC2\xA7" "aYou Win!",
    "\xC2\xA7" "cGame Over!",
};

bool resolvePacketFunctions() {
    if (!g_sendToServer) {
        g_sendToServer = reinterpret_cast<SendToServerFn>(
            bedrocktools::memory::resolve(SignatureId::LoopbackPacketSenderSendToServer));
    }
    if (!g_getPacketSender) {
        g_getPacketSender = reinterpret_cast<GetPacketSenderFn>(
            bedrocktools::memory::resolve(SignatureId::ClientInstanceGetPacketSender));
    }
    if (!g_createPacket) {
        g_createPacket = reinterpret_cast<CreatePacketFn>(
            bedrocktools::memory::resolve(SignatureId::MinecraftPacketsCreatePacket));
    }
    return g_sendToServer && g_getPacketSender && g_createPacket;
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// Reads a std::string the game already constructed inside the packet payload.
const std::string& packetString(void* packet, std::size_t offset) {
    const auto base = reinterpret_cast<std::uintptr_t>(packet) + Offsets::Packet::Size;
    return *reinterpret_cast<const std::string*>(base + offset);
}

// The text of a TextPacket, whichever variant the server used.
std::string textPacketMessage(void* packet) {
    const auto payload = reinterpret_cast<std::uintptr_t>(packet) + Offsets::Packet::Size;
    const auto variant = *reinterpret_cast<const std::uint32_t*>(
        payload + Offsets::TextPacketPayload::mVariantIndex);

    if (variant == kChatVariant) {
        return *reinterpret_cast<const std::string*>(
            payload + Offsets::TextPacketPayload::AuthorAndMessage::mMessage);
    }
    if (variant == 0 || variant == 2) {
        return *reinterpret_cast<const std::string*>(
            payload + Offsets::TextPacketPayload::MessageOnly::mMessage);
    }
    return {};
}

// Titles shown by The Hive, Galaxite and the vanilla game once a round ends.
bool isGameEndTitle(const std::string& text) {
    if (text.empty()) return false;

    if (text == "   ") return true;  // Hive "match over" spacer title
    if (text == "\xC2\xA7" "cYou died!") return true;
    if (text == "\xC2\xA7" "7You're spectating the \xC2\xA7" "as\xC2\xA7" "eh\xC2\xA7"
                "6o\xC2\xA7" "cw\xC2\xA7" "7!") return true;

    for (const char* marker : kTitleMarkers) {
        if (contains(text, marker)) return true;
    }

    if (contains(text, "Team\xC2\xA7" "r\xC2\xA7" "a won the game!")) return true;
    if (contains(text, "Finished")) return true;
    if (contains(text, "Out of Time!")) return true;

    static const std::regex galaxiteChampion(
        R"(Is The \xC2\xA76\xC2\xA7l(Chronos|Rush) (Champion|Champions)!)",
        std::regex::optimize);
    return std::regex_search(text, galaxiteChampion);
}

// Chat/system lines that only appear once a round is over.
bool isGameEndText(const std::string& text) {
    if (text.empty()) return false;

    if (text == "Game OVER!" || text == "You are out of lives!") return true;

    if (contains(text, "Finished")) return true;
    if (contains(text, "Out of Time!")) return true;
    if (contains(text, "You won the game!")) return true;
    if (contains(text, "You lost the game!")) return true;
    if (contains(text, "\xC2\xA7" "aYou Win!")) return true;
    if (contains(text, "\xC2\xA7" "cGame Over!")) return true;
    if (contains(text, "\xC2\xA7" "c\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "c\xC2\xA7"
                          "lGame OVER!")) return true;
    if (contains(text, "\xC2\xA7" "r\xC2\xA7" "c\xC2\xA7" "lGame OVER!")) return true;
    if (contains(text, "\xC2\xA7" "a won the game!")) return true;
    if (contains(text, "\xC2\xA7" "a has won the game!")) return true;

    return false;
}

// Reads the actor's health from the synced actor data (data id 1). Vanilla
// servers send HEALTH as an int and some send it as a float, so both DataItem
// types are accepted. Returns false when the actor has no synced data (for
// example because the pointer is stale) instead of guessing a value.
//
// The walk itself mirrors the upstream TNT Timer's synced-data reader: the
// actor holds a wrapper at Actor::mEntityData whose first pointer is the data
// component, and the component starts with the begin/end pointers of its item
// array, indexed by data id.
bool readHealth(Actor* actor, float& health) {
    if (!actor) return false;

    const auto wrapper = reinterpret_cast<std::uintptr_t>(actor) + Offsets::Actor::mEntityData;
    void* component = *reinterpret_cast<void**>(wrapper);
    if (!component) return false;

    auto** begin = *reinterpret_cast<void***>(component);
    auto** end = *reinterpret_cast<void***>(
        reinterpret_cast<std::uintptr_t>(component) + sizeof(void*));
    if (!begin || !end || end <= begin) return false;

    const auto count = static_cast<std::size_t>(end - begin);
    constexpr std::size_t id = Offsets::ActorDataIds::Health;
    if (count <= id || count > kMaxDataItems) return false;

    void* item = begin[id];
    if (!item) return false;

    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(address + Offsets::DataItem::mType);
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(address + Offsets::DataItem::mId);
    if (itemId != id) return false;

    if (type == Offsets::DataItem::IntType) {
        health = static_cast<float>(*reinterpret_cast<const int*>(
            address + Offsets::DataItem::mValue));
        return true;
    }
    if (type == Offsets::DataItem::FloatType) {
        health = *reinterpret_cast<const float*>(address + Offsets::DataItem::mValue);
        return true;
    }
    return false;
}

bool hasString(const nlohmann::json& j, const char* key) {
    return j.contains(key) && j[key].is_string();
}

bool readBool(const nlohmann::json& j, const char* key, bool fallback) {
    return j.contains(key) && j[key].is_boolean() ? j[key].get<bool>() : fallback;
}

std::string clampMessage(std::string value) {
    if (value.size() > AutoGG::MaxMessageLength) value.resize(AutoGG::MaxMessageLength);
    return value;
}

void (*g_titleOriginal)(void*, void*, void*) = nullptr;
void (*g_textOriginal)(void*, void*, void*) = nullptr;

} // namespace

AutoGG::AutoGG()
    : Module("AutoGG", "Says GG when a match ends, and optionally reacts to kills and deaths.") {
    g_autoGG = this;
    showInMenu = true;
    // The replies are sent from the network/game thread, never drawn here.
    hideInHudEditor = true;

    // Start with every cooldown already elapsed so the first match end, kill or
    // death after the mod loads replies immediately.
    const auto fresh = std::chrono::steady_clock::now();
    m_lastSend = fresh - kSendCooldown;
    m_lastReactionSend = fresh - kReactionCooldown;
    m_lastEndSignal = fresh - kEndSignalGuard;
}

AutoGG::~AutoGG() {
    if (g_autoGG == this) g_autoGG = nullptr;
    if (m_titleHook) bedrocktools::hooks::remove(m_titleHook);
    if (m_textHook) bedrocktools::hooks::remove(m_textHook);
    m_titleHook = nullptr;
    m_textHook = nullptr;
    m_initialized = false;
}

AutoGG* AutoGG::instance() {
    return g_autoGG;
}

void AutoGG::onInit() {
    if (m_initialized) return;
    m_initialized = true;

    // Both hooks stay installed for the whole session: the detours chain to the
    // game's handlers first and only inspect the packet when the module is on,
    // so toggling the module needs no re-hooking.
    const auto titleAddress =
        bedrocktools::memory::resolve(SignatureId::ClientNetworkHandlerHandleSetTitle);
    if (titleAddress != 0) {
        m_titleHook = bedrocktools::hooks::install(
            reinterpret_cast<void*>(titleAddress),
            reinterpret_cast<void*>(&AutoGG::titlePacketDetour),
            reinterpret_cast<void**>(&g_titleOriginal));
    }

    const auto textAddress =
        bedrocktools::memory::resolve(SignatureId::ClientNetworkHandlerHandleText);
    if (textAddress != 0) {
        m_textHook = bedrocktools::hooks::install(
            reinterpret_cast<void*>(textAddress),
            reinterpret_cast<void*>(&AutoGG::textPacketDetour),
            reinterpret_cast<void**>(&g_textOriginal));
    }

    // The reaction channels ride the same game-thread events Hit Sound uses:
    // AttackEvent reports the actor we just hit and LocalPlayerTickEvent lets
    // us watch the local/victim health settle.
    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>(
        [](auto& event) {
            if (g_autoGG) g_autoGG->onAttack(event.target);
        });
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_autoGG) g_autoGG->onLocalPlayerTick(event.player);
        });
}

void AutoGG::onEnable() {
    // Turning the module back on clears the cooldowns instead of inheriting
    // them from whatever match was being played when it was switched off.
    const auto fresh = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mMutex);
    m_lastSend = fresh - kSendCooldown;
    m_lastReactionSend = fresh - kReactionCooldown;
    m_lastEndSignal = fresh - kEndSignalGuard;
}

std::string AutoGG::message() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return m_message;
}

AutoGG::ReactionSettings AutoGG::reactionSettings() const {
    std::lock_guard<std::mutex> lock(mMutex);
    ReactionSettings settings;
    settings.killEnabled = m_killMessagesEnabled;
    settings.deathEnabled = m_deathMessagesEnabled;
    settings.playersOnly = m_killPlayersOnly;
    settings.killText = m_killMessageText;
    settings.deathText = m_deathMessageText;
    return settings;
}

void AutoGG::onAttack(Actor* target) {
    if (!enabled || !target) return;

    const auto settings = reactionSettings();
    if (!settings.killEnabled) return;
    if (settings.playersOnly && !target->hasCategory(Offsets::ActorCategories::IsPlayer)) return;

    const auto deadline = std::chrono::steady_clock::now() + kKillConfirmWindow;
    for (auto& pending : m_pendingKills) {
        if (pending.target == target) {
            pending.deadline = deadline;
            return;
        }
    }

    if (m_pendingKills.size() >= MaxPendingKills) m_pendingKills.erase(m_pendingKills.begin());
    m_pendingKills.push_back(PendingKill{target, deadline});
}

void AutoGG::onLocalPlayerTick(Player* player) {
    if (!enabled) return;

    const auto settings = reactionSettings();

    // Death channel: one reply per death, re-armed once the player is alive
    // again (respawn). The report flag is kept even while the reaction is off
    // so switching it on mid-death does not fire retroactively.
    float health = 0.0f;
    if (player && readHealth(player, health)) {
        if (health <= 0.0f) {
            if (!m_deathReported) {
                m_deathReported = true;
                if (settings.deathEnabled) sendReaction(settings.deathText);
            }
        } else {
            m_deathReported = false;
        }
    }

    // Kill channel: confirm that an actor we hit inside the window actually
    // reached zero health.
    if (!settings.killEnabled) {
        m_pendingKills.clear();
        return;
    }
    if (m_pendingKills.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_pendingKills.begin(); it != m_pendingKills.end();) {
        if (now > it->deadline) {
            it = m_pendingKills.erase(it);
            continue;
        }

        // Same stale-pointer guard Hit Sound uses: hurt-time outside its real
        // range means the actor object is gone and its memory was recycled.
        const int hurtTime = it->target->hurtTime();
        if (hurtTime < 0 || hurtTime > 100) {
            it = m_pendingKills.erase(it);
            continue;
        }
        if (settings.playersOnly &&
            !it->target->hasCategory(Offsets::ActorCategories::IsPlayer)) {
            it = m_pendingKills.erase(it);
            continue;
        }

        float victimHealth = 0.0f;
        if (!readHealth(it->target, victimHealth)) {
            // No readable data right now: keep the entry until the window
            // closes instead of trusting the pointer.
            ++it;
            continue;
        }
        if (victimHealth > 0.0f) {
            ++it;
            continue;
        }

        auto* victim = it->target;
        it = m_pendingKills.erase(it);
        sendReaction(formatKillMessage(settings.killText, victim));
        break;
    }
}

std::string AutoGG::formatKillMessage(const std::string& text, Actor* target) {
    std::string victim;
    if (target && target->hasCategory(Offsets::ActorCategories::IsPlayer)) {
        victim = static_cast<Player*>(target)->name();
        if (victim.size() > kMaxVictimNameBytes) victim.resize(kMaxVictimNameBytes);
    }

    std::string result = text;
    std::size_t pos = 0;
    while ((pos = result.find("{name}", pos)) != std::string::npos) {
        result.replace(pos, 6, victim);
        pos += victim.size();
    }
    return clampMessage(std::move(result));
}

void AutoGG::sendReaction(std::string text) {
    if (text.empty()) return;

    {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastReactionSend < kReactionCooldown) return;
        if (now - m_lastEndSignal < kEndSignalGuard) return;
        m_lastReactionSend = now;
    }

    sendChatMessage(text);
}

void AutoGG::sendReply() {
    // Copy the message and claim the cooldown slot in one lock so a reply can
    // never be sent twice for the same burst of end packets.
    std::string reply;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastSend < kSendCooldown) return;
        reply = m_message;
        if (reply.empty()) return;  // empty setting == stay silent
        m_lastSend = now;
    }

    sendChatMessage(reply);
}

void AutoGG::noteEndSignal() {
    std::lock_guard<std::mutex> lock(mMutex);
    m_lastEndSignal = std::chrono::steady_clock::now();
}

bool AutoGG::sendChatMessage(const std::string& text) {
    if (text.empty() || !resolvePacketFunctions()) return false;

    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) return false;

    std::shared_ptr<void> packet = g_createPacket(kTextPacketId);
    if (!packet) return false;

    const auto payload = reinterpret_cast<std::uintptr_t>(packet.get()) + Offsets::Packet::Size;

    // The packet arrives as a "message only" TextPacket, so swap its payload
    // over to the chat variant and write author/message in place.
    auto* messageOnly = reinterpret_cast<std::string*>(
        payload + Offsets::TextPacketPayload::MessageOnly::mMessage);
    messageOnly->~basic_string();

    *reinterpret_cast<std::uint32_t*>(
        payload + Offsets::TextPacketPayload::mVariantIndex) = kChatVariant;
    *reinterpret_cast<std::uint8_t*>(
        payload + Offsets::TextPacketPayload::AuthorAndMessage::mType) = kChatPacketType;
    new (reinterpret_cast<void*>(
        payload + Offsets::TextPacketPayload::AuthorAndMessage::mAuthor)) std::string();
    new (reinterpret_cast<void*>(
        payload + Offsets::TextPacketPayload::AuthorAndMessage::mMessage)) std::string(text);

    void* sender = g_getPacketSender(client);
    if (!sender) return false;
    g_sendToServer(sender, packet.get());
    return true;
}

void AutoGG::titlePacketDetour(void* handler, void* source, void* packet) {
    if (g_titleOriginal) g_titleOriginal(handler, source, packet);

    auto* module = AutoGG::instance();
    if (!module || !module->enabled) return;

    const auto payload = reinterpret_cast<std::uintptr_t>(packet) + Offsets::Packet::Size;
    const int type = *reinterpret_cast<const int*>(
        payload + Offsets::SetTitlePacketPayload::mType);
    if (type != kTitleType && type != kSubtitleType) return;

    const auto& text = packetString(packet, Offsets::SetTitlePacketPayload::mTitleText);
    if (!isGameEndTitle(text)) return;

    module->noteEndSignal();
    module->sendReply();
}

void AutoGG::textPacketDetour(void* handler, void* source, void* packet) {
    if (g_textOriginal) g_textOriginal(handler, source, packet);

    auto* module = AutoGG::instance();
    if (!module || !module->enabled) return;

    if (!isGameEndText(textPacketMessage(packet))) return;

    module->noteEndSignal();
    module->sendReply();
}

void AutoGG::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    // Serialized against the packet hooks reading the message. Module config
    // load may have called onEnable() above, which takes the same lock, so
    // this block runs after it.
    std::lock_guard<std::mutex> lock(mMutex);
    if (hasString(j, "ggMessage")) m_message = clampMessage(j["ggMessage"].get<std::string>());
    if (hasString(j, "killMessageText")) {
        m_killMessageText = clampMessage(j["killMessageText"].get<std::string>());
    }
    if (hasString(j, "deathMessageText")) {
        m_deathMessageText = clampMessage(j["deathMessageText"].get<std::string>());
    }
    m_killMessagesEnabled = readBool(j, "m_killMessage", m_killMessagesEnabled);
    m_deathMessagesEnabled = readBool(j, "m_deathMessage", m_deathMessagesEnabled);
    m_killPlayersOnly = readBool(j, "killPlayersOnly", m_killPlayersOnly);
}

void AutoGG::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    std::lock_guard<std::mutex> lock(mMutex);
    j["ggMessage"] = m_message;
    j["m_killMessage"] = m_killMessagesEnabled;
    j["killMessageText"] = m_killMessageText;
    j["killPlayersOnly"] = m_killPlayersOnly;
    j["m_deathMessage"] = m_deathMessagesEnabled;
    j["deathMessageText"] = m_deathMessageText;
}
