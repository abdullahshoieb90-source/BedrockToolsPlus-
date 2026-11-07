#include "voicechat_engine.hpp"

#include <monocypher/monocypher.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

namespace bedrocktools::voice {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kReceivePoll = std::chrono::milliseconds(100);
constexpr auto kJoinRetry = std::chrono::seconds(1);
constexpr auto kPingInterval = std::chrono::seconds(2);
constexpr auto kServerTimeout = std::chrono::seconds(8);
constexpr auto kPeerExpiry = std::chrono::seconds(3);
constexpr std::size_t kJitterFrames = 3;
constexpr std::size_t kMaxBufferedFrames = 8;

bool secureRandom(std::uint8_t* data, std::size_t size) {
    // /dev/urandom is available on every supported Android version and does
    // not depend on NDK getrandom() declarations or a particular API level.
    std::size_t offset = 0;
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (offset < size) {
        const ssize_t count = read(fd, data + offset, size - offset);
        if (count > 0) offset += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
    return offset == size;
}

std::uint64_t randomId() {
    std::uint64_t value = 0;
    while (value == 0) {
        if (!secureRandom(reinterpret_cast<std::uint8_t*>(&value), sizeof(value))) return 0;
    }
    return value;
}

bool validConnectionSettings(const VoiceChatSettings& settings) {
    return !settings.server.empty() && settings.server.size() <= 253 && settings.port != 0 &&
           validTextField(settings.room, kMaxRoomBytes) && settings.roomKey.size() >= 16 &&
           settings.roomKey.size() <= 256 &&
           validTextField(settings.playerName.empty() ? std::string_view("Player")
                                                      : std::string_view(settings.playerName),
                          kMaxNameBytes);
}

bool sameConnection(const VoiceChatSettings& a, const VoiceChatSettings& b) {
    return a.server == b.server && a.port == b.port && a.room == b.room &&
           a.roomKey == b.roomKey && a.playerName == b.playerName;
}

void sleepInterruptible(std::chrono::milliseconds duration,
                        const std::atomic_bool& running) {
    const auto deadline = Clock::now() + duration;
    while (running.load(std::memory_order_acquire) && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

} // namespace

struct VoiceChatEngine::Impl {
    struct RemoteVoice {
        std::deque<VoicePacket> frames;
        std::array<std::uint8_t, 16> sessionPrefix{};
        std::uint64_t newestSequence = 0;
        Clock::time_point lastPacket = Clock::now();
        bool playing = false;
    };

    mutable std::mutex settingsMutex;
    VoiceChatSettings settings;

    AudioEngine audio;
    std::atomic_bool running{false};
    std::atomic_bool networkRunning{false};
    std::thread networkThread;

    std::mutex sendMutex;
    int socket = -1;
    Key key{};
    std::array<std::uint8_t, 16> noncePrefix{};
    std::uint64_t nextSequence = 1;

    std::atomic<std::uint64_t> clientId{0};
    std::atomic_bool talkActive{false};
    std::atomic<float> localX{0.0f};
    std::atomic<float> localY{0.0f};
    std::atomic<float> localZ{0.0f};

    std::mutex peersMutex;
    std::unordered_map<std::uint64_t, RemoteVoice> peers;
    std::atomic<std::uint16_t> activeSpeakers{0};
    std::atomic<std::uint16_t> peerCount{0};

    mutable std::mutex statusMutex;
    ConnectionState connectionState = ConnectionState::Disabled;
    std::string statusMessage = "Voice chat is disabled";

    void setStatus(ConnectionState state, std::string message) {
        std::lock_guard lock(statusMutex);
        connectionState = state;
        statusMessage = std::move(message);
    }

    Position localPosition() const {
        return Position{localX.load(std::memory_order_relaxed),
                        localY.load(std::memory_order_relaxed),
                        localZ.load(std::memory_order_relaxed)};
    }

    void updateCaptureState() {
        const bool active = running.load(std::memory_order_acquire) &&
                            talkActive.load(std::memory_order_acquire) &&
                            clientId.load(std::memory_order_acquire) != 0 && audio.running();
        audio.setCaptureActive(active);
    }

    void clearPeers() {
        std::lock_guard lock(peersMutex);
        peers.clear();
        activeSpeakers.store(0, std::memory_order_release);
    }

    bool sendVoice(const PcmFrame& samples) {
        std::lock_guard lock(sendMutex);
        const std::uint64_t id = clientId.load(std::memory_order_acquire);
        if (socket < 0 || id == 0 || !networkRunning.load(std::memory_order_acquire)) return false;
        const std::uint64_t sequence = nextSequence++;
        if (nextSequence == 0) nextSequence = 1; // practically unreachable, never reuse zero
        const Nonce nonce = makeNonce(noncePrefix, sequence);
        const auto packet = makeVoicePacket(id, key, nonce, sequence, localPosition(), samples);
        if (packet.empty()) return false;
        const ssize_t sent = send(socket, packet.data(), packet.size(), MSG_NOSIGNAL);
        return sent == static_cast<ssize_t>(packet.size());
    }

    void receiveVoice(const std::uint8_t* data, std::size_t size, const Key& voiceKey) {
        VoicePacket packet;
        if (!openVoicePacket(data, size, voiceKey, packet) ||
            packet.clientId == clientId.load(std::memory_order_acquire)) {
            return;
        }

        std::lock_guard lock(peersMutex);
        auto& remote = peers[packet.clientId];
        remote.lastPacket = Clock::now();
        if (remote.sessionPrefix != packet.sessionPrefix) {
            // A reconnect may reuse the relay client ID while restarting the
            // sequence at one. The random nonce prefix identifies the new
            // session, so reset replay bookkeeping instead of muting it.
            remote.frames.clear();
            remote.sessionPrefix = packet.sessionPrefix;
            remote.newestSequence = 0;
            remote.playing = false;
        }
        if (remote.newestSequence >= packet.sequence) return; // duplicate or reordered old frame
        remote.newestSequence = packet.sequence;
        if (remote.frames.size() >= kMaxBufferedFrames) remote.frames.pop_front();
        remote.frames.push_back(std::move(packet));
    }

    void mix(PcmFrame& output) {
        output.fill(0);
        VoiceChatSettings current;
        {
            std::lock_guard lock(settingsMutex);
            current = settings;
        }
        const Position listener = localPosition();
        const auto now = Clock::now();
        std::array<std::int32_t, kSamplesPerFrame> mixed{};
        unsigned speakers = 0;

        std::lock_guard lock(peersMutex);
        for (auto it = peers.begin(); it != peers.end();) {
            auto& remote = it->second;
            if (now - remote.lastPacket > kPeerExpiry) {
                it = peers.erase(it);
                continue;
            }
            if (!remote.playing && remote.frames.size() < kJitterFrames &&
                now - remote.lastPacket < std::chrono::milliseconds(80)) {
                ++it;
                continue;
            }
            if (remote.frames.empty()) {
                remote.playing = false;
                ++it;
                continue;
            }

            remote.playing = true;
            VoicePacket frame = std::move(remote.frames.front());
            remote.frames.pop_front();
            const float distanceGain = current.mode == VoiceMode::Proximity
                ? proximityGain(listener, frame.senderPosition, current.maxDistance) : 1.0f;
            const float gain = std::clamp(current.outputVolume, 0.0f, 1.0f) * distanceGain;
            if (gain > 0.001f) {
                ++speakers;
                for (std::size_t i = 0; i < output.size(); ++i)
                    mixed[i] += static_cast<std::int32_t>(std::lround(frame.samples[i] * gain));
            }
            ++it;
        }

        for (std::size_t i = 0; i < output.size(); ++i) {
            mixed[i] = std::clamp<std::int32_t>(mixed[i],
                std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max());
            output[i] = static_cast<std::int16_t>(mixed[i]);
        }
        activeSpeakers.store(static_cast<std::uint16_t>(std::min(speakers, 65535u)),
                             std::memory_order_release);
    }

    static bool sendToSocket(int fd, const std::vector<std::uint8_t>& packet) {
        if (fd < 0 || packet.empty() || packet.size() > kMaxDatagramBytes) return false;
        return send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(packet.size());
    }

    int connectRelay(const VoiceChatSettings& config) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* addresses = nullptr;
        const std::string port = std::to_string(config.port);
        if (getaddrinfo(config.server.c_str(), port.c_str(), &hints, &addresses) != 0) return -1;

        int connected = -1;
        for (addrinfo* address = addresses; address; address = address->ai_next) {
            if (!networkRunning.load(std::memory_order_acquire)) break;
            const int fd = ::socket(address->ai_family, SOCK_DGRAM | SOCK_CLOEXEC,
                                    address->ai_protocol);
            if (fd < 0) continue;
            if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
                const int flags = fcntl(fd, F_GETFL, 0);
                if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                int bufferSize = 256 * 1024;
                setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));
                connected = fd;
                break;
            }
            close(fd);
        }
        freeaddrinfo(addresses);
        return connected;
    }

    void networkLoop() {
        while (running.load(std::memory_order_acquire) &&
               networkRunning.load(std::memory_order_acquire)) {
            VoiceChatSettings config;
            {
                std::lock_guard lock(settingsMutex);
                config = settings;
            }
            setStatus(ConnectionState::Connecting, "Resolving voice relay...");
            const int fd = connectRelay(config);
            if (fd < 0) {
                setStatus(ConnectionState::Error, "Voice relay is unreachable; retrying...");
                sleepInterruptible(std::chrono::seconds(1), networkRunning);
                continue;
            }

            Key voiceKey = deriveRoomKey(config.room, config.roomKey);
            std::array<std::uint8_t, 16> sessionPrefix{};
            std::uint64_t requestId = randomId();
            if (requestId == 0 || !secureRandom(sessionPrefix.data(), sessionPrefix.size())) {
                close(fd);
                setStatus(ConnectionState::Error, "Secure random generator is unavailable");
                break;
            }
            {
                std::lock_guard lock(sendMutex);
                socket = fd;
                key = voiceKey;
                noncePrefix = sessionPrefix;
                nextSequence = 1;
                clientId.store(0, std::memory_order_release);
            }
            clearPeers();
            peerCount.store(0, std::memory_order_release);
            updateCaptureState();

            bool joined = false;
            bool fatalError = false;
            auto lastJoin = Clock::now() - kJoinRetry;
            auto lastPing = Clock::now();
            auto lastServerPacket = Clock::now();
            const auto joinPacket = makeJoinPacket(requestId, config.room, config.playerName);
            if (joinPacket.empty()) fatalError = true;

            while (!fatalError && running.load(std::memory_order_acquire) &&
                   networkRunning.load(std::memory_order_acquire)) {
                const auto now = Clock::now();
                if (!joined && now - lastJoin >= kJoinRetry) {
                    if (!sendToSocket(fd, joinPacket)) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            fatalError = true;
                    }
                    lastJoin = now;
                    setStatus(ConnectionState::Connecting, "Joining encrypted voice room...");
                }

                if (joined && now - lastPing >= kPingInterval) {
                    const auto ping = makePingPacket(clientId.load(std::memory_order_acquire));
                    sendToSocket(fd, ping);
                    lastPing = now;
                }
                if (joined && now - lastServerPacket > kServerTimeout) {
                    joined = false;
                    clientId.store(0, std::memory_order_release);
                    peerCount.store(0, std::memory_order_release);
                    updateCaptureState();
                    clearPeers();
                    setStatus(ConnectionState::Connecting, "Voice relay timed out; rejoining...");
                    // Keep the same join request so retransmitted Welcome
                    // packets remain valid; the relay is endpoint-affine.
                    lastServerPacket = now;
                    lastJoin = now - kJoinRetry;
                }

                pollfd pfd{fd, POLLIN, 0};
                const int ready = poll(&pfd, 1, static_cast<int>(kReceivePoll.count()));
                if (ready < 0) {
                    if (errno == EINTR) continue;
                    fatalError = true;
                    break;
                }
                if (ready == 0 || !(pfd.revents & POLLIN)) {
                    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) fatalError = true;
                    continue;
                }

                // Bound work per poll so a flooded socket cannot starve
                // keepalives or shutdown indefinitely.
                for (unsigned packetIndex = 0; packetIndex < 32; ++packetIndex) {
                    std::array<std::uint8_t, 2048> buffer{};
                    const ssize_t size = recv(fd, buffer.data(), buffer.size(), 0);
                    if (size < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        fatalError = true;
                        break;
                    }
                    if (size == 0 || static_cast<std::size_t>(size) > kMaxDatagramBytes) continue;
                    const auto* data = buffer.data();
                    const auto packetSize = static_cast<std::size_t>(size);

                    WelcomePacket welcome;
                    PongPacket pong;
                    if (parseWelcomePacket(data, packetSize, welcome)) {
                        if (welcome.requestId != requestId) continue;
                        clientId.store(welcome.clientId, std::memory_order_release);
                        peerCount.store(welcome.peerCount, std::memory_order_release);
                        joined = true;
                        lastServerPacket = Clock::now();
                        lastPing = lastServerPacket;
                        setStatus(ConnectionState::Connected, "Connected to encrypted voice room");
                        updateCaptureState();
                    } else if (parsePongPacket(data, packetSize, pong)) {
                        if (!joined || pong.clientId != clientId.load(std::memory_order_acquire))
                            continue;
                        peerCount.store(pong.peerCount, std::memory_order_release);
                        lastServerPacket = Clock::now();
                    } else if (joined) {
                        receiveVoice(data, packetSize, voiceKey);
                        lastServerPacket = Clock::now();
                    }
                }
            }

            if (joined) sendToSocket(fd, makeLeavePacket(clientId.load(std::memory_order_acquire)));
            {
                std::lock_guard lock(sendMutex);
                if (socket == fd) socket = -1;
                clientId.store(0, std::memory_order_release);
                crypto_wipe(key.data(), key.size());
                crypto_wipe(noncePrefix.data(), noncePrefix.size());
            }
            updateCaptureState();
            clearPeers();
            peerCount.store(0, std::memory_order_release);
            close(fd);
            crypto_wipe(voiceKey.data(), voiceKey.size());
            if (running.load(std::memory_order_acquire) &&
                networkRunning.load(std::memory_order_acquire)) {
                setStatus(ConnectionState::Connecting, "Reconnecting to voice relay...");
                sleepInterruptible(std::chrono::milliseconds(250), networkRunning);
            }
        }
    }

    void startNetwork() {
        stopNetwork();
        VoiceChatSettings config;
        {
            std::lock_guard lock(settingsMutex);
            config = settings;
        }
        if (!validConnectionSettings(config)) {
            setStatus(ConnectionState::Error,
                "Set a relay, room, player name and a room key of at least 16 characters");
            return;
        }
        networkRunning.store(true, std::memory_order_release);
        setStatus(ConnectionState::Connecting, "Starting voice connection...");
        networkThread = std::thread([this] { networkLoop(); });
    }

    void stopNetwork() {
        networkRunning.store(false, std::memory_order_release);
        clientId.store(0, std::memory_order_release);
        updateCaptureState();
        {
            std::lock_guard lock(sendMutex);
            socket = -1;
            crypto_wipe(key.data(), key.size());
            crypto_wipe(noncePrefix.data(), noncePrefix.size());
        }
        if (networkThread.joinable()) networkThread.join();
        clearPeers();
        peerCount.store(0, std::memory_order_release);
    }
};

VoiceChatEngine::VoiceChatEngine() : m_impl(std::make_unique<Impl>()) {}
VoiceChatEngine::~VoiceChatEngine() { stop(); }

void VoiceChatEngine::start(const VoiceChatSettings& value, void* javaVm) {
    if (m_impl->running.load(std::memory_order_acquire)) {
        updateSettings(value);
        return;
    }
    {
        std::lock_guard lock(m_impl->settingsMutex);
        m_impl->settings = value;
    }
    m_impl->running.store(true, std::memory_order_release);
    m_impl->audio.setInputGain(value.inputVolume);
    m_impl->audio.start(javaVm,
        [impl = m_impl.get()](const PcmFrame& frame) { impl->sendVoice(frame); },
        [impl = m_impl.get()](PcmFrame& frame) { impl->mix(frame); });
    m_impl->startNetwork();
}

void VoiceChatEngine::stop() {
    if (!m_impl->running.exchange(false, std::memory_order_acq_rel)) return;
    m_impl->talkActive.store(false, std::memory_order_release);
    m_impl->stopNetwork();
    m_impl->audio.stop();
    m_impl->clearPeers();
    m_impl->setStatus(ConnectionState::Disabled, "Voice chat is disabled");
}

void VoiceChatEngine::updateSettings(const VoiceChatSettings& value) {
    bool reconnect = false;
    {
        std::lock_guard lock(m_impl->settingsMutex);
        reconnect = !sameConnection(m_impl->settings, value);
        m_impl->settings = value;
    }
    m_impl->audio.setInputGain(value.inputVolume);
    if (m_impl->running.load(std::memory_order_acquire) && reconnect) m_impl->startNetwork();
}

void VoiceChatEngine::setLocalPosition(Position position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return;
    m_impl->localX.store(position.x, std::memory_order_relaxed);
    m_impl->localY.store(position.y, std::memory_order_relaxed);
    m_impl->localZ.store(position.z, std::memory_order_relaxed);
}

void VoiceChatEngine::setTalkActive(bool active) {
    m_impl->talkActive.store(active, std::memory_order_release);
    m_impl->updateCaptureState();
}

VoiceChatStatus VoiceChatEngine::status() const {
    VoiceChatStatus result;
    {
        std::lock_guard lock(m_impl->statusMutex);
        result.state = m_impl->connectionState;
        result.message = m_impl->statusMessage;
    }
    result.peerCount = m_impl->peerCount.load(std::memory_order_acquire);
    result.activeSpeakers = m_impl->activeSpeakers.load(std::memory_order_acquire);
    const std::string audioError = m_impl->audio.error();
    if (!audioError.empty() && m_impl->running.load(std::memory_order_acquire)) {
        result.state = ConnectionState::Error;
        result.message = audioError;
    }
    return result;
}

bool VoiceChatEngine::running() const {
    return m_impl->running.load(std::memory_order_acquire);
}

} // namespace bedrocktools::voice
