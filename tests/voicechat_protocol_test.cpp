// Host tests for the authenticated voice-wire format and proximity attenuation.
// Built by scripts/run_tests.sh together with protocol.cpp and Monocypher.
#include "modules/voice/voicechat_protocol.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace bedrocktools::voice;

namespace {

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::vector<std::uint8_t> serverPacket(PacketType type, std::uint64_t clientId,
                                       std::uint16_t peers, std::uint64_t request = 0) {
    std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
    out.push_back(kProtocolVersion);
    out.push_back(static_cast<std::uint8_t>(type));
    put64(out, clientId);
    if (type == PacketType::Welcome) put64(out, request);
    put16(out, peers);
    return out;
}

void testJoinValidation() {
    const auto join = makeJoinPacket(0x0102030405060708ULL, "friends", "Steve");
    assert(join.size() == 6 + 8 + 2 + 1 + 7 + 5);
    assert(validPacketPrefix(join.data(), join.size(), PacketType::Join));
    assert(join[14] == 7 && join[15] == 0 && join[16] == 5);
    assert(std::string(join.begin() + 17, join.begin() + 24) == "friends");
    assert(std::string(join.begin() + 24, join.end()) == "Steve");

    assert(makeJoinPacket(1, "", "Steve").empty());
    assert(makeJoinPacket(1, "bad\nroom", "Steve").empty());
    assert(makeJoinPacket(1, std::string(65, 'r'), "Steve").empty());
    const auto defaultName = makeJoinPacket(1, "friends", "");
    assert(defaultName.back() == 'r'); // Player

    const auto truncatedUtf8 = makeJoinPacket(1, "friends", std::string(30, 'a') + "\xF0\x9F\x98\x80");
    assert(!truncatedUtf8.empty());
    assert(truncatedUtf8[16] == 30); // do not split the four-byte emoji
    const auto exactUtf8 = makeJoinPacket(1, "friends", std::string(28, 'a') + "\xF0\x9F\x98\x80");
    assert(!exactUtf8.empty());
    assert(exactUtf8[16] == 32);
}

void testWelcomeAndPong() {
    const auto welcome = serverPacket(PacketType::Welcome, 42, 3, 99);
    WelcomePacket parsedWelcome;
    assert(parseWelcomePacket(welcome.data(), welcome.size(), parsedWelcome));
    assert(parsedWelcome.clientId == 42);
    assert(parsedWelcome.requestId == 99);
    assert(parsedWelcome.peerCount == 3);

    auto truncated = welcome;
    truncated.pop_back();
    assert(!parseWelcomePacket(truncated.data(), truncated.size(), parsedWelcome));
    auto wrongType = welcome;
    wrongType[5] = static_cast<std::uint8_t>(PacketType::Pong);
    assert(!parseWelcomePacket(wrongType.data(), wrongType.size(), parsedWelcome));

    const auto pong = serverPacket(PacketType::Pong, 42, 7);
    PongPacket parsedPong;
    assert(parsePongPacket(pong.data(), pong.size(), parsedPong));
    assert(parsedPong.clientId == 42 && parsedPong.peerCount == 7);
}

void testVoiceRoundTripAndAuthentication() {
    const Key key = deriveRoomKey("friends", "correct horse battery staple");
    const Key otherKey = deriveRoomKey("friends", "a different secret key");
    static_assert(key.size() == 32);
    const std::array<std::uint8_t, 16> prefix{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    constexpr std::uint64_t sequence = 0x0102030405060708ULL;
    const Nonce nonce = makeNonce(prefix, sequence);

    PcmFrame samples{};
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<std::int16_t>(static_cast<int>(i) - 160);
    samples[0] = std::numeric_limits<std::int16_t>::min();
    samples[1] = std::numeric_limits<std::int16_t>::max();

    const Position position{12.5f, -63.25f, 900.0f};
    auto packet = makeVoicePacket(77, key, nonce, sequence, position, samples);
    assert(packet.size() < kMaxDatagramBytes);
    assert(validPacketPrefix(packet.data(), packet.size(), PacketType::Voice));

    VoicePacket decoded;
    assert(openVoicePacket(packet.data(), packet.size(), key, decoded));
    assert(decoded.clientId == 77);
    assert(decoded.sequence == sequence);
    assert(decoded.sessionPrefix == prefix);
    assert(decoded.senderPosition.x == position.x);
    assert(decoded.senderPosition.y == position.y);
    assert(decoded.senderPosition.z == position.z);
    assert(decoded.samples == samples);

    assert(!openVoicePacket(packet.data(), packet.size(), otherKey, decoded));

    auto changedClient = packet;
    changedClient[6] ^= 1;
    assert(!openVoicePacket(changedClient.data(), changedClient.size(), key, decoded));

    auto changedNonce = packet;
    changedNonce[20] ^= 0x80;
    assert(!openVoicePacket(changedNonce.data(), changedNonce.size(), key, decoded));

    auto changedMac = packet;
    changedMac[40] ^= 1;
    assert(!openVoicePacket(changedMac.data(), changedMac.size(), key, decoded));

    auto changedAudio = packet;
    changedAudio.back() ^= 1;
    assert(!openVoicePacket(changedAudio.data(), changedAudio.size(), key, decoded));

    auto changedSequence = makeVoicePacket(77, key, nonce, sequence, position, samples);
    // The nonce's final eight bytes are authenticated additional data and must
    // match the encrypted sequence, preventing ambiguous replay bookkeeping.
    changedSequence[30] ^= 1;
    assert(!openVoicePacket(changedSequence.data(), changedSequence.size(), key, decoded));

    assert(makeVoicePacket(0, key, nonce, sequence, position, samples).empty());
    assert(makeVoicePacket(77, key, nonce, 0, position, samples).empty());
    assert(makeVoicePacket(77, key, nonce, sequence + 1, position, samples).empty());
    assert(makeVoicePacket(77, key, nonce, sequence,
        Position{std::numeric_limits<float>::quiet_NaN(), 0, 0}, samples).empty());
}

void testProximityAndGain() {
    const Position listener{0, 64, 0};
    assert(proximityGain(listener, Position{0, 64, 0}, 32) == 1.0f);
    assert(proximityGain(listener, Position{2, 64, 0}, 32) == 1.0f);
    assert(proximityGain(listener, Position{32, 64, 0}, 32) == 0.0f);
    assert(proximityGain(listener, Position{33, 64, 0}, 32) == 0.0f);
    const float halfway = proximityGain(listener, Position{17, 64, 0}, 32);
    assert(std::fabs(halfway - 0.5f) < 0.0001f);
    assert(proximityGain(listener, Position{3, 64, 4}, 32) <= 1.0f);
    assert(proximityGain(listener, listener, 0) == 0.0f);
    assert(proximityGain(listener, listener,
        std::numeric_limits<float>::quiet_NaN()) == 0.0f);

    PcmFrame pcm{};
    pcm[0] = 1000;
    pcm[1] = -1000;
    pcm[2] = 32767;
    pcm[3] = -32768;
    applyPcmGain(pcm, 2.0f);
    assert(pcm[0] == 2000 && pcm[1] == -2000);
    assert(pcm[2] == 32767 && pcm[3] == -32768);
    applyPcmGain(pcm, 0.0f);
    assert(std::all_of(pcm.begin(), pcm.end(), [](std::int16_t sample) { return sample == 0; }));
}

} // namespace

int main() {
    testJoinValidation();
    testWelcomeAndPong();
    testVoiceRoundTripAndAuthentication();
    testProximityAndGain();
    std::cout << "voice chat protocol tests passed\n";
}
