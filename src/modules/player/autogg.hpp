#pragma once

#include <modules/Module.hpp>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>

namespace bedrocktools::hooks { struct State; }

// Watches the title and chat packets the server sends and answers with a
// configurable message once a match has clearly ended (Hive, Galaxite and the
// vanilla "You won/lost the game!" screens). Ported from the upstream
// BedrockTools AutoGG module and kept config-compatible with it: the saved key
// is still "ggMessage" inside the "AutoGG" module section.
class AutoGG final : public Module {
public:
    // Reply length cap, in bytes. Chat lines are short; this only stops a
    // pasted wall of text from being sent as a packet every match.
    static constexpr std::size_t MaxMessageLength = 256;

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

private:
    // Network-handler detours. Both chain to the game's original handler first
    // so the end screen still renders exactly as usual.
    static void titlePacketDetour(void* handler, void* source, void* packet);
    static void textPacketDetour(void* handler, void* source, void* packet);

    // Sends the reply when the module is on, the message is not empty and the
    // cooldown has elapsed. Called from the network thread.
    void sendReply();

    mutable std::mutex mMutex;
    std::string m_message = "gg";
    std::chrono::steady_clock::time_point m_lastSend{};

    bool m_hooksInstalled = false;
    bedrocktools::hooks::State* m_titleHook = nullptr;
    bedrocktools::hooks::State* m_textHook = nullptr;
};
