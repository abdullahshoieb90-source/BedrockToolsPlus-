#pragma once

#include <modules/Module.hpp>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace bedrocktools::hooks { struct State; }
namespace bedrocktools::sdk { class Actor; class Player; }

// Watches the title and chat packets the server sends and answers with a
// configurable message once a match has clearly ended (Hive, Galaxite and the
// vanilla "You won/lost the game!" screens). Ported from the upstream
// BedrockTools AutoGG module and kept config-compatible with it: the saved key
// is still "ggMessage" inside the "AutoGG" module section.
//
// On top of the upstream behaviour it can also react to the two events players
// actually type "gg" for: their own death and a kill they scored. Both are
// optional toggles (off by default) with their own reply text, and they share a
// cooldown so a fight cannot turn into a wall of chat messages.
class AutoGG final : public Module {
public:
    // Reply length cap, in bytes. Chat lines are short; this only stops a
    // pasted wall of text from being sent as a packet every match.
    static constexpr std::size_t MaxMessageLength = 256;
    // At most this many attacked actors are watched for a kill at once.
    static constexpr std::size_t MaxPendingKills = 8;

    AutoGG();
    ~AutoGG() override;

    void onInit() override;
    void onEnable() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    static AutoGG* instance();

    // Thread-safe copy of the configured reply. The settings menu edits the
    // message from the UI thread while the packet hooks read it from the
    // network thread, so the value is never handed out by reference.
    std::string message() const;

    // Game-thread entry points for the reaction channels. They are public so
    // the event subscriptions installed in onInit() can reach them, and so the
    // host tests can drive them without a running game.
    void onAttack(bedrocktools::sdk::Actor* target);
    void onLocalPlayerTick(bedrocktools::sdk::Player* player);

private:
    // A hit whose victim is watched for a short window; a kill is confirmed
    // when the victim's synced health reaches zero inside that window.
    struct PendingKill {
        bedrocktools::sdk::Actor* target = nullptr;
        std::chrono::steady_clock::time_point deadline{};
    };

    // Consistent snapshot of the reaction settings for one game-thread pass.
    struct ReactionSettings {
        bool killEnabled = false;
        bool deathEnabled = false;
        bool playersOnly = true;
        std::string killText;
        std::string deathText;
    };

    // Network-handler detours. Both chain to the game's original handler first
    // so the end screen still renders exactly as usual.
    static void titlePacketDetour(void* handler, void* source, void* packet);
    static void textPacketDetour(void* handler, void* source, void* packet);

    // Sends the reply when the module is on, the message is not empty and the
    // cooldown has elapsed. Called from the network thread.
    void sendReply();

    // Sends a kill/death reply, honouring the reaction cooldown and the
    // "a match is ending right now" guard.
    void sendReaction(std::string text);

    // Builds and sends one chat TextPacket. Returns false when a signature did
    // not resolve or the game is not ready to send.
    bool sendChatMessage(const std::string& text);

    // Marks that an end-of-match packet was just seen, so the reaction channels
    // can stay quiet while the proper GG reply is going out.
    void noteEndSignal();

    ReactionSettings reactionSettings() const;

    // Replaces "{name}" with the victim's name and clamps the result.
    static std::string formatKillMessage(const std::string& text,
                                         bedrocktools::sdk::Actor* target);

    mutable std::mutex mMutex;

    // Settings and timestamps: written by the settings menu (UI thread) and
    // read by the packet hooks and the tick (game thread), all through mMutex.
    std::string m_message = "gg";
    std::string m_killMessageText = "gg";
    std::string m_deathMessageText = "F";
    bool m_killMessagesEnabled = false;
    bool m_deathMessagesEnabled = false;
    bool m_killPlayersOnly = true;
    std::chrono::steady_clock::time_point m_lastSend{};
    std::chrono::steady_clock::time_point m_lastReactionSend{};
    std::chrono::steady_clock::time_point m_lastEndSignal{};

    // Game-thread state: only touched from the tick/attack hooks, which both
    // run on the game thread.
    std::vector<PendingKill> m_pendingKills;
    bool m_deathReported = false;

    bool m_initialized = false;
    bedrocktools::hooks::State* m_titleHook = nullptr;
    bedrocktools::hooks::State* m_textHook = nullptr;
};
