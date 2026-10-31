#include "autogg.hpp"

#include "core/GameHooks.hpp"
#include "core/memory/Hooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <regex>
#include <string>

namespace {

namespace PacketOffsets = bedrocktools::sdk::offsets;
using bedrocktools::memory::SignatureId;

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

// Title packet types that carry text worth inspecting: title and subtitle.
constexpr int kTitleType = 0;
constexpr int kSubtitleType = 1;

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
    const auto base = reinterpret_cast<std::uintptr_t>(packet) + PacketOffsets::Packet::Size;
    return *reinterpret_cast<const std::string*>(base + offset);
}

// The text of a TextPacket, whichever variant the server used.
std::string textPacketMessage(void* packet) {
    const auto payload = reinterpret_cast<std::uintptr_t>(packet) + PacketOffsets::Packet::Size;
    const auto variant = *reinterpret_cast<const std::uint32_t*>(
        payload + PacketOffsets::TextPacketPayload::mVariantIndex);

    if (variant == kChatVariant) {
        return *reinterpret_cast<const std::string*>(
            payload + PacketOffsets::TextPacketPayload::AuthorAndMessage::mMessage);
    }
    if (variant == 0 || variant == 2) {
        return *reinterpret_cast<const std::string*>(
            payload + PacketOffsets::TextPacketPayload::MessageOnly::mMessage);
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

void (*g_titleOriginal)(void*, void*, void*) = nullptr;
void (*g_textOriginal)(void*, void*, void*) = nullptr;

} // namespace

AutoGG::AutoGG()
    : Module("AutoGG", "Says GG automatically when a match ends.") {
    g_autoGG = this;
    showInMenu = true;
    // The reply is sent from the network thread, never drawn here.
    hideInHudEditor = true;
    // Start with the cooldown already elapsed so the first match end replies
    // immediately after the mod is loaded.
    m_lastSend = std::chrono::steady_clock::now() - kSendCooldown;
}

AutoGG::~AutoGG() {
    if (g_autoGG == this) g_autoGG = nullptr;
    if (m_titleHook) bedrocktools::hooks::remove(m_titleHook);
    if (m_textHook) bedrocktools::hooks::remove(m_textHook);
    m_titleHook = nullptr;
    m_textHook = nullptr;
    m_hooksInstalled = false;
}

AutoGG* AutoGG::instance() {
    return g_autoGG;
}

void AutoGG::onInit() {
    // Both hooks stay installed for the whole session: the detours chain to the
    // game's handlers first and only inspect the packet when the module is on,
    // so toggling the module needs no re-hooking.
    if (m_hooksInstalled) return;

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

    // Either handler is enough to detect a match end, so the module works on
    // builds where only one of the two signatures resolves.
    m_hooksInstalled = m_titleHook != nullptr || m_textHook != nullptr;
}

void AutoGG::onEnable() {
    // Turning the module back on clears the cooldown instead of inheriting one
    // from whatever match was being played when it was switched off.
    std::lock_guard<std::mutex> lock(mMutex);
    m_lastSend = std::chrono::steady_clock::now() - kSendCooldown;
}

std::string AutoGG::message() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return m_message;
}

void AutoGG::sendReply() {
    if (!resolvePacketFunctions()) return;

    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) return;

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

    std::shared_ptr<void> packet = g_createPacket(kTextPacketId);
    if (!packet) return;

    const auto payload = reinterpret_cast<std::uintptr_t>(packet.get()) +
                         PacketOffsets::Packet::Size;

    // The packet arrives as a "message only" TextPacket, so swap its payload
    // over to the chat variant and write author/message in place.
    auto* messageOnly = reinterpret_cast<std::string*>(
        payload + PacketOffsets::TextPacketPayload::MessageOnly::mMessage);
    messageOnly->~basic_string();

    *reinterpret_cast<std::uint32_t*>(
        payload + PacketOffsets::TextPacketPayload::mVariantIndex) = kChatVariant;
    *reinterpret_cast<std::uint8_t*>(
        payload + PacketOffsets::TextPacketPayload::AuthorAndMessage::mType) = kChatPacketType;
    new (reinterpret_cast<void*>(
        payload + PacketOffsets::TextPacketPayload::AuthorAndMessage::mAuthor)) std::string();
    new (reinterpret_cast<void*>(
        payload + PacketOffsets::TextPacketPayload::AuthorAndMessage::mMessage)) std::string(reply);

    void* sender = g_getPacketSender(client);
    if (sender) g_sendToServer(sender, packet.get());
}

void AutoGG::titlePacketDetour(void* handler, void* source, void* packet) {
    if (g_titleOriginal) g_titleOriginal(handler, source, packet);

    auto* module = AutoGG::instance();
    if (!module || !module->enabled || !module->m_hooksInstalled) return;

    const auto payload = reinterpret_cast<std::uintptr_t>(packet) + PacketOffsets::Packet::Size;
    const int type = *reinterpret_cast<const int*>(
        payload + PacketOffsets::SetTitlePacketPayload::mType);
    if (type != kTitleType && type != kSubtitleType) return;

    const auto& text = packetString(
        packet, PacketOffsets::SetTitlePacketPayload::mTitleText);
    if (isGameEndTitle(text)) module->sendReply();
}

void AutoGG::textPacketDetour(void* handler, void* source, void* packet) {
    if (g_textOriginal) g_textOriginal(handler, source, packet);

    auto* module = AutoGG::instance();
    if (!module || !module->enabled || !module->m_hooksInstalled) return;

    if (isGameEndText(textPacketMessage(packet))) module->sendReply();
}

void AutoGG::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("ggMessage") && j["ggMessage"].is_string()) {
        std::string value = j["ggMessage"].get<std::string>();
        if (value.size() > MaxMessageLength) value.resize(MaxMessageLength);
        std::lock_guard<std::mutex> lock(mMutex);
        m_message = std::move(value);
    }
}

void AutoGG::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["ggMessage"] = message();
}
