#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bedrocktools::voice {

// BedrockToolsPlus Voice Chat wire protocol, version 1.
//
// The relay only sees room names, client IDs and encrypted voice datagrams.
// Voice payloads use XChaCha20-Poly1305 with a key derived from the room name
// and a user-supplied high-entropy room key. Packet headers are authenticated
// as additional data. The relay never receives the room key.
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::array<std::uint8_t, 4> kMagic{'B', 'T', 'V', 'C'};
inline constexpr std::size_t kPacketPrefixSize = 6; // magic + version + type

inline constexpr std::size_t kSampleRate = 16000;
inline constexpr std::size_t kSamplesPerFrame = 320; // 20 ms, mono
inline constexpr std::size_t kPcmBytesPerFrame = kSamplesPerFrame * sizeof(std::int16_t);
inline constexpr std::size_t kMaxRoomBytes = 64;
inline constexpr std::size_t kMaxNameBytes = 32;
inline constexpr std::size_t kMaxDatagramBytes = 1200;

using Key = std::array<std::uint8_t, 32>;
using Nonce = std::array<std::uint8_t, 24>;
using PcmFrame = std::array<std::int16_t, kSamplesPerFrame>;

struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class PacketType : std::uint8_t {
    Join = 1,
    Welcome = 2,
    Voice = 3,
    Ping = 4,
    Pong = 5,
    Leave = 6,
};

struct WelcomePacket {
    std::uint64_t clientId = 0;
    std::uint64_t requestId = 0;
    std::uint16_t peerCount = 0;
};

struct PongPacket {
    std::uint64_t clientId = 0;
    std::uint16_t peerCount = 0;
};

struct VoicePacket {
    std::uint64_t clientId = 0;
    std::uint64_t sequence = 0;
    std::array<std::uint8_t, 16> sessionPrefix{};
    Position senderPosition{};
    PcmFrame samples{};
};

using ServerPacket = std::variant<WelcomePacket, PongPacket>;

bool validPacketPrefix(const std::uint8_t* data, std::size_t size, PacketType type);
bool validTextField(std::string_view value, std::size_t maxBytes);

Key deriveRoomKey(std::string_view room, std::string_view roomKey);
Nonce makeNonce(const std::array<std::uint8_t, 16>& sessionPrefix, std::uint64_t sequence);

std::vector<std::uint8_t> makeJoinPacket(std::uint64_t requestId,
                                         std::string_view room,
                                         std::string_view playerName);
std::vector<std::uint8_t> makePingPacket(std::uint64_t clientId);
std::vector<std::uint8_t> makeLeavePacket(std::uint64_t clientId);

std::vector<std::uint8_t> makeVoicePacket(std::uint64_t clientId,
                                          const Key& key,
                                          const Nonce& nonce,
                                          std::uint64_t sequence,
                                          Position senderPosition,
                                          const PcmFrame& samples);

bool parseWelcomePacket(const std::uint8_t* data, std::size_t size, WelcomePacket& out);
bool parsePongPacket(const std::uint8_t* data, std::size_t size, PongPacket& out);
bool openVoicePacket(const std::uint8_t* data, std::size_t size, const Key& key,
                     VoicePacket& out);

// Linear attenuation with full volume at two blocks or less and silence at the
// configured limit. Invalid distances/ranges fail closed for privacy.
float proximityGain(Position listener, Position speaker, float maxDistance);
void applyPcmGain(PcmFrame& samples, float gain);

} // namespace bedrocktools::voice
