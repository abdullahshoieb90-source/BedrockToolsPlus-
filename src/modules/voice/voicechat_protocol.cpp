#include "voicechat_protocol.hpp"

#include <monocypher/monocypher.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace bedrocktools::voice {
namespace {

constexpr std::size_t kJoinFixedSize = kPacketPrefixSize + 8 + 2 + 1;
constexpr std::size_t kWelcomeSize = kPacketPrefixSize + 8 + 8 + 2;
constexpr std::size_t kPongSize = kPacketPrefixSize + 8 + 2;
constexpr std::size_t kClientPacketSize = kPacketPrefixSize + 8;
constexpr std::size_t kVoiceAadSize = kPacketPrefixSize + 8 + 24;
constexpr std::size_t kVoiceHeaderSize = kVoiceAadSize + 16;
constexpr std::size_t kVoicePlaintextSize = 8 + 12 + kPcmBytesPerFrame;

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint16_t readU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint64_t readU64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(data[shift / 8]) << shift;
    return value;
}

void writeU64(std::uint8_t* data, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        data[shift / 8] = static_cast<std::uint8_t>(value >> shift);
}

void writeFloat(std::uint8_t* data, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8)
        data[shift / 8] = static_cast<std::uint8_t>(bits >> shift);
}

float readFloat(const std::uint8_t* data) {
    std::uint32_t bits = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        bits |= static_cast<std::uint32_t>(data[shift / 8]) << shift;
    return std::bit_cast<float>(bits);
}

void appendPrefix(std::vector<std::uint8_t>& out, PacketType type) {
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    out.push_back(kProtocolVersion);
    out.push_back(static_cast<std::uint8_t>(type));
}

std::vector<std::uint8_t> makeClientPacket(PacketType type, std::uint64_t clientId) {
    std::vector<std::uint8_t> out;
    out.reserve(kClientPacketSize);
    appendPrefix(out, type);
    appendU64(out, clientId);
    return out;
}

std::string_view truncateUtf8(std::string_view value, std::size_t maxBytes) {
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
    return value.substr(0, end);
}

} // namespace

bool validPacketPrefix(const std::uint8_t* data, std::size_t size, PacketType type) {
    return data && size >= kPacketPrefixSize &&
           std::equal(kMagic.begin(), kMagic.end(), data) &&
           data[4] == kProtocolVersion && data[5] == static_cast<std::uint8_t>(type);
}

bool validTextField(std::string_view value, std::size_t maxBytes) {
    if (value.empty() || value.size() > maxBytes) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

Key deriveRoomKey(std::string_view room, std::string_view roomKey) {
    // Length prefixes prevent ambiguous concatenations. This is deliberately
    // a fast KDF: the UI and documentation therefore require a long, random
    // room key rather than a human-memorable password.
    static constexpr std::string_view domain = "BedrockToolsPlus Voice Chat v1";
    std::vector<std::uint8_t> material;
    material.reserve(domain.size() + room.size() + roomKey.size() + 16);
    material.insert(material.end(), domain.begin(), domain.end());
    material.push_back(0);
    appendU64(material, room.size());
    material.insert(material.end(), room.begin(), room.end());
    appendU64(material, roomKey.size());
    material.insert(material.end(), roomKey.begin(), roomKey.end());

    Key key{};
    crypto_blake2b(key.data(), key.size(), material.data(), material.size());
    crypto_wipe(material.data(), material.size());
    return key;
}

Nonce makeNonce(const std::array<std::uint8_t, 16>& sessionPrefix, std::uint64_t sequence) {
    Nonce nonce{};
    std::copy(sessionPrefix.begin(), sessionPrefix.end(), nonce.begin());
    writeU64(nonce.data() + sessionPrefix.size(), sequence);
    return nonce;
}

std::vector<std::uint8_t> makeJoinPacket(std::uint64_t requestId,
                                         std::string_view room,
                                         std::string_view playerName) {
    if (!validTextField(room, kMaxRoomBytes)) return {};
    if (playerName.empty()) playerName = "Player";
    if (!validTextField(playerName, kMaxNameBytes)) {
        playerName = truncateUtf8(playerName, kMaxNameBytes);
        if (!validTextField(playerName, kMaxNameBytes)) playerName = "Player";
    }

    std::vector<std::uint8_t> out;
    out.reserve(kJoinFixedSize + room.size() + playerName.size());
    appendPrefix(out, PacketType::Join);
    appendU64(out, requestId);
    appendU16(out, static_cast<std::uint16_t>(room.size()));
    out.push_back(static_cast<std::uint8_t>(playerName.size()));
    out.insert(out.end(), room.begin(), room.end());
    out.insert(out.end(), playerName.begin(), playerName.end());
    return out;
}

std::vector<std::uint8_t> makePingPacket(std::uint64_t clientId) {
    return makeClientPacket(PacketType::Ping, clientId);
}

std::vector<std::uint8_t> makeLeavePacket(std::uint64_t clientId) {
    return makeClientPacket(PacketType::Leave, clientId);
}

std::vector<std::uint8_t> makeVoicePacket(std::uint64_t clientId,
                                          const Key& key,
                                          const Nonce& nonce,
                                          std::uint64_t sequence,
                                          Position senderPosition,
                                          const PcmFrame& samples) {
    if (clientId == 0 || sequence == 0 || readU64(nonce.data() + 16) != sequence ||
        !std::isfinite(senderPosition.x) || !std::isfinite(senderPosition.y) ||
        !std::isfinite(senderPosition.z)) {
        return {};
    }

    std::array<std::uint8_t, kVoicePlaintextSize> plain{};
    writeU64(plain.data(), sequence);
    writeFloat(plain.data() + 8, senderPosition.x);
    writeFloat(plain.data() + 12, senderPosition.y);
    writeFloat(plain.data() + 16, senderPosition.z);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto value = static_cast<std::uint16_t>(samples[i]);
        plain[20 + i * 2] = static_cast<std::uint8_t>(value);
        plain[21 + i * 2] = static_cast<std::uint8_t>(value >> 8);
    }

    std::vector<std::uint8_t> out(kVoiceHeaderSize + plain.size());
    out[0] = kMagic[0];
    out[1] = kMagic[1];
    out[2] = kMagic[2];
    out[3] = kMagic[3];
    out[4] = kProtocolVersion;
    out[5] = static_cast<std::uint8_t>(PacketType::Voice);
    writeU64(out.data() + kPacketPrefixSize, clientId);
    std::copy(nonce.begin(), nonce.end(), out.begin() + kPacketPrefixSize + 8);

    auto* mac = out.data() + kVoiceAadSize;
    auto* ciphertext = out.data() + kVoiceHeaderSize;
    crypto_aead_lock(ciphertext, mac, key.data(), nonce.data(), out.data(), kVoiceAadSize,
                     plain.data(), plain.size());
    crypto_wipe(plain.data(), plain.size());
    return out;
}

bool parseWelcomePacket(const std::uint8_t* data, std::size_t size, WelcomePacket& out) {
    if (!validPacketPrefix(data, size, PacketType::Welcome) || size != kWelcomeSize)
        return false;
    WelcomePacket parsed;
    parsed.clientId = readU64(data + 6);
    parsed.requestId = readU64(data + 14);
    parsed.peerCount = readU16(data + 22);
    if (parsed.clientId == 0 || parsed.requestId == 0) return false;
    out = parsed;
    return true;
}

bool parsePongPacket(const std::uint8_t* data, std::size_t size, PongPacket& out) {
    if (!validPacketPrefix(data, size, PacketType::Pong) || size != kPongSize)
        return false;
    PongPacket parsed;
    parsed.clientId = readU64(data + 6);
    parsed.peerCount = readU16(data + 14);
    if (parsed.clientId == 0) return false;
    out = parsed;
    return true;
}

bool openVoicePacket(const std::uint8_t* data, std::size_t size, const Key& key,
                     VoicePacket& out) {
    if (!validPacketPrefix(data, size, PacketType::Voice) ||
        size != kVoiceHeaderSize + kVoicePlaintextSize) {
        return false;
    }

    VoicePacket parsed;
    parsed.clientId = readU64(data + 6);
    if (parsed.clientId == 0) return false;
    std::copy_n(data + 14, parsed.sessionPrefix.size(), parsed.sessionPrefix.begin());

    std::array<std::uint8_t, kVoicePlaintextSize> plain{};
    const auto* nonce = data + 14;
    const auto* mac = data + kVoiceAadSize;
    const auto* ciphertext = data + kVoiceHeaderSize;
    if (crypto_aead_unlock(plain.data(), mac, key.data(), nonce, data, kVoiceAadSize,
                           ciphertext, kVoicePlaintextSize) != 0) {
        crypto_wipe(plain.data(), plain.size());
        return false;
    }

    parsed.sequence = readU64(plain.data());
    if (parsed.sequence == 0 || parsed.sequence != readU64(nonce + 16)) {
        crypto_wipe(plain.data(), plain.size());
        return false;
    }
    parsed.senderPosition = Position{readFloat(plain.data() + 8),
                                     readFloat(plain.data() + 12),
                                     readFloat(plain.data() + 16)};
    if (!std::isfinite(parsed.senderPosition.x) || !std::isfinite(parsed.senderPosition.y) ||
        !std::isfinite(parsed.senderPosition.z)) {
        crypto_wipe(plain.data(), plain.size());
        return false;
    }
    for (std::size_t i = 0; i < parsed.samples.size(); ++i) {
        const auto value = static_cast<std::uint16_t>(plain[20 + i * 2]) |
                           static_cast<std::uint16_t>(
                               static_cast<std::uint16_t>(plain[21 + i * 2]) << 8);
        parsed.samples[i] = static_cast<std::int16_t>(value);
    }
    crypto_wipe(plain.data(), plain.size());
    out = parsed;
    return true;
}

float proximityGain(Position listener, Position speaker, float maxDistance) {
    if (!std::isfinite(maxDistance) || maxDistance <= 0.0f) return 0.0f;
    const float dx = listener.x - speaker.x;
    const float dy = listener.y - speaker.y;
    const float dz = listener.z - speaker.z;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) return 0.0f;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(distance) || distance >= maxDistance) return 0.0f;
    if (distance <= 2.0f) return 1.0f;
    return std::clamp(1.0f - ((distance - 2.0f) / (maxDistance - 2.0f)), 0.0f, 1.0f);
}

void applyPcmGain(PcmFrame& samples, float gain) {
    if (!std::isfinite(gain) || gain <= 0.0f) {
        samples.fill(0);
        return;
    }
    gain = std::min(gain, 4.0f);
    for (auto& sample : samples) {
        const long scaled = std::lround(static_cast<float>(sample) * gain);
        sample = static_cast<std::int16_t>(std::clamp(
            scaled, static_cast<long>(std::numeric_limits<std::int16_t>::min()),
            static_cast<long>(std::numeric_limits<std::int16_t>::max())));
    }
}

} // namespace bedrocktools::voice
