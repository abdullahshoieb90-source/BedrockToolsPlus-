#include "voicechat.hpp"

#include "../ModuleRegistry.hpp"
#include "../../launcher/ExternalButtonRefresh.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <span>
#include <string_view>
#include <vector>

VoiceChatModule* g_voiceChat = nullptr;

namespace {
using bedrocktools::voice::ConnectionState;
using bedrocktools::voice::Position;
using bedrocktools::voice::VoiceChatSettings;
using bedrocktools::voice::VoiceMode;
using Clock = std::chrono::steady_clock;

constexpr const char* kTalkButtonId = "bedrocktoolsplus.VoiceChat.Talk";
constexpr const char* kStatusElementId = "bedrocktoolsplus.VoiceChat.Status";

std::string trim(std::string value) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::string truncateUtf8(std::string value, std::size_t maxBytes) {
    if (value.size() <= maxBytes) return value;
    std::size_t end = maxBytes;
    std::size_t continuation = end;
    while (continuation > 0 &&
           (static_cast<unsigned char>(value[continuation - 1]) & 0xc0u) == 0x80u) {
        --continuation;
    }
    if (continuation > 0) {
        const auto lead = static_cast<unsigned char>(value[continuation - 1]);
        std::size_t expected = 1;
        if ((lead & 0xe0u) == 0xc0u) expected = 2;
        else if ((lead & 0xf0u) == 0xe0u) expected = 3;
        else if ((lead & 0xf8u) == 0xf0u) expected = 4;
        if (expected > 1 && end - (continuation - 1) < expected)
            end = continuation - 1;
    }
    value.resize(end);
    return value;
}

std::pair<std::string, std::uint16_t> parseServerAddress(std::string value) {
    value = trim(std::move(value));
    if (value.empty()) return {"", 0};

    std::string host;
    std::string portText;
    bool explicitPort = false;
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string::npos) return {value, 0};
        host = value.substr(1, close - 1);
        if (close + 1 < value.size()) {
            if (value[close + 1] != ':') return {host, 0};
            explicitPort = true;
            portText = value.substr(close + 2);
        }
    } else {
        const auto firstColon = value.find(':');
        const auto lastColon = value.rfind(':');
        if (firstColon != std::string::npos && firstColon == lastColon) {
            host = value.substr(0, firstColon);
            explicitPort = true;
            portText = value.substr(firstColon + 1);
        } else {
            // A bare IPv6 address is accepted with the default port.
            host = value;
        }
    }

    std::uint16_t port = 39741;
    if (!portText.empty()) {
        unsigned parsed = 0;
        const auto result = std::from_chars(portText.data(), portText.data() + portText.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != portText.data() + portText.size() ||
            parsed == 0 || parsed > 65535) {
            return {host, 0};
        }
        port = static_cast<std::uint16_t>(parsed);
    } else if (explicitPort) {
        return {host, 0};
    }
    return {trim(host), port};
}

int parseMode(const nlohmann::json& value, int fallback) {
    if (value.is_number_integer()) return std::clamp(value.get<int>(), 0, 1);
    if (!value.is_string()) return fallback;
    std::string text = value.get<std::string>();
    const auto comma = text.find(',');
    if (comma != std::string::npos) text.resize(comma);
    if (text == "Proximity" || text == "proximity" || text == "1") return 1;
    if (text == "Room" || text == "room" || text == "0") return 0;
    return fallback;
}

float clampedFloat(const nlohmann::json& parent, const char* key, float fallback,
                   float minimum, float maximum) {
    if (!parent.contains(key) || !parent[key].is_number()) return fallback;
    return std::clamp(parent[key].get<float>(), minimum, maximum);
}

} // namespace

VoiceChatModule::VoiceChatModule()
    : Module("Voice Chat",
             "Encrypted room and proximity voice chat through a self-hosted relay. Hold Talk or press V; microphone permission is required.") {
    g_voiceChat = this;
    hideInHudEditor = false;
}

VoiceChatModule::~VoiceChatModule() {
    m_engine.stop();
    pl::modmenu::unregisterButton(kTalkButtonId);
    clearOverlay();
    if (g_voiceChat == this) g_voiceChat = nullptr;
}

void VoiceChatModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (!g_voiceChat || !g_voiceChat->enabled || !event.player) return;
            const auto position = event.player->position();
            g_voiceChat->m_engine.setLocalPosition(Position{position.x, position.y, position.z});

            bool nameChanged = false;
            {
                std::lock_guard lock(g_voiceChat->m_configMutex);
                const std::string name = truncateUtf8(event.player->name(),
                    bedrocktools::voice::kMaxNameBytes);
                if (!name.empty() && name != g_voiceChat->m_gameName) {
                    g_voiceChat->m_gameName = name;
                    nameChanged = g_voiceChat->m_playerName.empty();
                }
            }
            if (nameChanged) g_voiceChat->scheduleSettingsUpdate();
        });
}

void VoiceChatModule::onEnable() {
    m_keyboardTalk.store(false, std::memory_order_release);
    m_buttonTalk.store(false, std::memory_order_release);
    m_engine.start(settingsSnapshot(), bedrocktools::launcher::javaVm());
    updateTalkState();
    syncTalkButton();
}

void VoiceChatModule::onDisable() {
    m_keyboardTalk.store(false, std::memory_order_release);
    m_buttonTalk.store(false, std::memory_order_release);
    m_engine.setTalkActive(false);
    m_engine.stop();
    pl::modmenu::unregisterButton(kTalkButtonId);
    clearOverlay();
}

VoiceChatSettings VoiceChatModule::settingsSnapshot() {
    std::lock_guard lock(m_configMutex);
    VoiceChatSettings settings;
    const auto endpoint = parseServerAddress(m_server);
    settings.server = endpoint.first;
    settings.port = endpoint.second;
    settings.room = m_room;
    settings.roomKey = m_roomKey;
    settings.playerName = !m_playerName.empty() ? m_playerName
        : (!m_gameName.empty() ? m_gameName : "Player");
    settings.playerName = truncateUtf8(settings.playerName, bedrocktools::voice::kMaxNameBytes);
    settings.mode = m_mode == 1 ? VoiceMode::Proximity : VoiceMode::Room;
    settings.maxDistance = m_maxDistance;
    settings.inputVolume = m_inputVolume;
    settings.outputVolume = m_outputVolume;
    return settings;
}

void VoiceChatModule::scheduleSettingsUpdate() {
    std::lock_guard lock(m_configMutex);
    m_restartPending = true;
    m_restartAt = Clock::now() + std::chrono::milliseconds(600);
}

void VoiceChatModule::updateTalkState() {
    bool pushToTalk = false;
    {
        std::lock_guard lock(m_configMutex);
        pushToTalk = m_pushToTalk;
    }
    const bool active = enabled && (!pushToTalk ||
        m_keyboardTalk.load(std::memory_order_acquire) ||
        m_buttonTalk.load(std::memory_order_acquire));
    m_engine.setTalkActive(active);
}

void VoiceChatModule::syncTalkButton() {
    pl::modmenu::unregisterButton(kTalkButtonId);
    bool pushToTalk = false;
    {
        std::lock_guard lock(m_configMutex);
        pushToTalk = m_pushToTalk;
    }
    if (!enabled || !pushToTalk) return;

    pl::modmenu::ButtonBuilder builder(kTalkButtonId, "Voice Talk");
    builder.moduleId(moduleId)
        .label("TALK")
        .behavior(pl::modmenu::ButtonBehavior::Hold)
        .defaultVisible(true)
        .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
        .styleColors(0xFF303030u, 0xFF2E8B57u, 0xFFFFFFFFu)
        .textColor(0xFFFFFFFFu)
        .activeTextColor(0xFFFFFFFFu)
        .sizeScale(1.25f, 1.25f)
        .onEvent([this](std::string_view, pl::modmenu::ButtonEvent event, float) {
            if (event == pl::modmenu::ButtonEvent::Down) {
                if (enabled) m_buttonTalk.store(true, std::memory_order_release);
            } else if (event == pl::modmenu::ButtonEvent::Up) {
                m_buttonTalk.store(false, std::memory_order_release);
            } else {
                return;
            }
            updateTalkState();
        });
    (void)builder.registerButton();
}

bool VoiceChatModule::onKeyEvent(int key, bool isDown) {
    int talkKey = 0;
    bool pushToTalk = false;
    {
        std::lock_guard lock(m_configMutex);
        talkKey = m_talkKeybind;
        pushToTalk = m_pushToTalk;
    }
    if (!enabled || !pushToTalk || talkKey <= 0 || key != talkKey) return false;
    if (isDown && ModuleRegistry::get().keybindBlocked()) return false;
    m_keyboardTalk.store(isDown, std::memory_order_release);
    updateTalkState();
    return true;
}

void VoiceChatModule::clearOverlay() {
    pl::modmenu::submitDrawCommands(moduleId,
        std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId,
        std::span<const pl::modmenu::HudEditorElement>{});
}

void VoiceChatModule::onMenuRegistered() {
    syncTalkButton();
    if (!enabled) clearOverlay();
}

void VoiceChatModule::onFrame() {
    if (!enabled) return;

    bool applySettings = false;
    {
        std::lock_guard lock(m_configMutex);
        if (m_restartPending && Clock::now() >= m_restartAt) {
            m_restartPending = false;
            applySettings = true;
        }
    }
    if (applySettings) {
        m_engine.updateSettings(settingsSnapshot());
        updateTalkState();
    }

    bool showStatus = false;
    float x = 0.0f;
    float y = 0.0f;
    {
        std::lock_guard lock(m_configMutex);
        showStatus = m_showStatus;
        x = m_hudPosX;
        y = m_hudPosY;
    }
    if (!showStatus) {
        clearOverlay();
        return;
    }

    const auto status = m_engine.status();

    std::string title;
    std::uint32_t color = 0xFFFFC107u;
    if (status.state == ConnectionState::Connected) {
        title = "Voice: " + std::to_string(status.peerCount) + " online";
        if (status.activeSpeakers > 0)
            title += " | " + std::to_string(status.activeSpeakers) + " speaking";
        const bool micLive = m_buttonTalk.load(std::memory_order_acquire) ||
                             m_keyboardTalk.load(std::memory_order_acquire);
        bool ptt = true;
        {
            std::lock_guard lock(m_configMutex);
            ptt = m_pushToTalk;
        }
        if (!ptt || micLive) title += " | MIC LIVE";
        color = 0xFF66D19Eu;
    } else if (status.state == ConnectionState::Error) {
        title = "Voice: Error";
        color = 0xFFFF6B6Bu;
    } else {
        title = "Voice: Connecting...";
    }

    const std::string detail = truncateUtf8(status.message, 72);
    const float width = 260.0f;
    const float height = detail.empty() ? 26.0f : 46.0f;
    std::vector<pl::modmenu::DrawCommand> commands;
    pl::modmenu::DrawCommand background;
    background.type = pl::modmenu::DrawCommandType::RectFilled;
    background.x = x;
    background.y = y;
    background.w = width;
    background.h = height;
    background.x3 = 4.0f;
    background.color = 0xB0000000u;
    commands.push_back(background);

    pl::modmenu::DrawCommand text;
    text.type = pl::modmenu::DrawCommandType::Text;
    text.x = x + 8.0f;
    text.y = y + 5.0f;
    text.size = 13.0f;
    text.color = color;
    text.text = title;
    commands.push_back(text);
    if (!detail.empty()) {
        text.x = x + 8.0f;
        text.y = y + 25.0f;
        text.size = 10.0f;
        text.color = 0xFFDDDDDDu;
        text.text = detail;
        commands.push_back(text);
    }
    pl::modmenu::submitDrawCommands(moduleId, commands);

    pl::modmenu::HudEditorElement element;
    element.elementId = kStatusElementId;
    element.displayName = "Voice Chat Status";
    element.positionKeyX = "hudPosX";
    element.positionKeyY = "hudPosY";
    element.x = x;
    element.y = y;
    element.width = width;
    element.height = height;
    element.gridSize = 4.0f;
    element.snapFlags = pl::modmenu::HudSnapGrid | pl::modmenu::HudSnapElements;
    pl::modmenu::submitHudEditorElements(moduleId,
        std::span<const pl::modmenu::HudEditorElement>(&element, 1));
}

void VoiceChatModule::loadConfig(const nlohmann::json& j) {
    const bool wasEnabled = enabled;
    bool previousPtt = false;
    int previousTalkKey = 0;
    {
        std::lock_guard lock(m_configMutex);
        previousPtt = m_pushToTalk;
        previousTalkKey = m_talkKeybind;
    }

    // Parse feature settings before the base class can invoke onEnable(), so
    // an enabled profile never briefly connects with default credentials.
    {
        std::lock_guard lock(m_configMutex);
        if (j.contains("m_server") && j["m_server"].is_string())
            m_server = j["m_server"].get<std::string>();
        if (j.contains("m_room") && j["m_room"].is_string())
            m_room = j["m_room"].get<std::string>();
        if (j.contains("m_roomKey") && j["m_roomKey"].is_string())
            m_roomKey = j["m_roomKey"].get<std::string>();
        if (j.contains("m_playerName") && j["m_playerName"].is_string())
            m_playerName = j["m_playerName"].get<std::string>();
        if (j.contains("m_mode")) m_mode = parseMode(j["m_mode"], m_mode);
        if (j.contains("m_pushToTalk") && j["m_pushToTalk"].is_boolean())
            m_pushToTalk = j["m_pushToTalk"].get<bool>();
        if (j.contains("m_talkKeybind") && j["m_talkKeybind"].is_number_integer())
            m_talkKeybind = std::max(0, j["m_talkKeybind"].get<int>());
        m_maxDistance = clampedFloat(j, "m_maxDistance", m_maxDistance, 4.0f, 128.0f);
        m_inputVolume = clampedFloat(j, "m_inputVolume", m_inputVolume, 0.0f, 1.0f);
        m_outputVolume = clampedFloat(j, "m_outputVolume", m_outputVolume, 0.0f, 1.0f);
        if (j.contains("m_showStatus") && j["m_showStatus"].is_boolean())
            m_showStatus = j["m_showStatus"].get<bool>();
        m_hudPosX = clampedFloat(j, "hudPosX", m_hudPosX, 0.0f, 4000.0f);
        m_hudPosY = clampedFloat(j, "hudPosY", m_hudPosY, 0.0f, 4000.0f);
    }

    Module::loadConfig(j);
    if (enabled && wasEnabled) scheduleSettingsUpdate();

    bool pttChanged = false;
    bool talkKeyChanged = false;
    {
        std::lock_guard lock(m_configMutex);
        pttChanged = previousPtt != m_pushToTalk;
        talkKeyChanged = previousTalkKey != m_talkKeybind;
    }
    if (pttChanged || talkKeyChanged) {
        m_keyboardTalk.store(false, std::memory_order_release);
        m_buttonTalk.store(false, std::memory_order_release);
        if (pttChanged) syncTalkButton();
        updateTalkState();
    }
}

void VoiceChatModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);
    j["m_server"] = m_server;
    j["m_room"] = m_room;
    j["m_roomKey"] = m_roomKey;
    j["m_playerName"] = m_playerName;
    j["m_mode"] = std::to_string(m_mode) + ",Room,Proximity";
    j["m_pushToTalk"] = m_pushToTalk;
    j["m_talkKeybind"] = m_talkKeybind;
    j["m_maxDistance"] = m_maxDistance;
    j["m_inputVolume"] = m_inputVolume;
    j["m_outputVolume"] = m_outputVolume;
    j["m_showStatus"] = m_showStatus;
    j["hudPosX"] = m_hudPosX;
    j["hudPosY"] = m_hudPosY;
}
