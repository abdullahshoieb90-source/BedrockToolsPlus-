// Unit tests for the Hit Sound module's pure helpers (folder scanning, the
// launcher "radio" value format, the generated sample WAV, and the arrow hit
// detection that turns one tick's nearby actors into "your arrow landed").
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include tests/hitsound_test.cpp -o /tmp/hitsound_test
//     /tmp/hitsound_test

#include "modules/misc/hitsound_files.hpp"
#include "modules/misc/hitsound_projectile.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hs = hitsound;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

void checkEqual(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what + " -> \"" + got + "\" (want \"" + want + "\")");
}

void checkEqual(int got, int want, const std::string& what) {
    check(got == want, what + " -> " + std::to_string(got) + " (want " + std::to_string(want) + ")");
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += '|';
        out += items[i];
    }
    return out;
}

std::uint32_t readU32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) |
           (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t readU16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at]) |
           (static_cast<std::uint16_t>(b[at + 1]) << 8);
}

void testExtensions() {
    std::printf("extensions\n");
    check(hs::hasAudioExtension("ding.wav"), "accepts .wav");
    check(hs::hasAudioExtension("pling.ogg"), "accepts .ogg");
    check(hs::hasAudioExtension("punch.mp3"), "accepts .mp3");
    check(hs::hasAudioExtension("hit.m4a"), "accepts .m4a");
    check(hs::hasAudioExtension("boom.FLAC"), "accepts upper-case .FLAC");
    check(hs::hasAudioExtension("Hit.OgG"), "accepts mixed-case .OgG");
    check(!hs::hasAudioExtension("notes.txt"), "rejects .txt");
    check(!hs::hasAudioExtension("song.aac"), "rejects .aac");
    check(!hs::hasAudioExtension("noext"), "rejects file without extension");
    check(!hs::hasAudioExtension("wave.wavv"), "rejects .wavv lookalike");
    check(!hs::isUsableSoundFileName("bad,name.wav"), "skips comma file names");
    check(hs::isUsableSoundFileName("bad name.wav"), "keeps space file names");
}

void testScanning() {
    std::printf("folder scanning\n");
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "hitsound_test_scan";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ec.clear();
    std::filesystem::create_directories(dir / "subdir", ec);
    ec.clear();

    const char* entries[] = {
        "z_loud.ogg", "a_punch.wav", "Note Ding.mp3", "ignored.txt", "bad,name.wav",
        "UPPER.FLAC", "tail.m4a", "noext", "subdir/inner.ogg",
    };
    for (const char* name : entries) {
        std::ofstream(dir / name, std::ios::binary) << "x";
    }

    const std::vector<std::string> files = hs::scanSoundFiles(dir.string());
    const std::string expected = "Note Ding.mp3|UPPER.FLAC|a_punch.wav|tail.m4a|z_loud.ogg";
    checkEqual(join(files), expected, "scan lists usable audio sorted, skips the rest");

    check(hs::scanSoundFiles(dir.string() + "/does_not_exist").empty(), "missing dir -> empty");
    ec.clear();
    std::filesystem::remove_all(dir, ec);
}

void testRadioValue() {
    std::printf("radio (de)serialization\n");
    const std::vector<std::string> files{"a.wav", "b.ogg", "c.mp3"};

    checkEqual(hs::makeRadioValue(0, files), "0,None,a.wav,b.ogg,c.mp3", "index 0 serialization");
    checkEqual(hs::makeRadioValue(2, files), "2,None,a.wav,b.ogg,c.mp3", "index 2 serialization");
    checkEqual(hs::makeRadioValue(-3, files), "0,None,a.wav,b.ogg,c.mp3", "negative clamps to None");
    checkEqual(hs::makeRadioValue(99, files), "3,None,a.wav,b.ogg,c.mp3", "overflow clamps to last");

    int index = -1;
    std::string name = "unset";
    check(hs::parseRadioValue("2,None,a.wav,b.ogg,c.mp3", index, name), "parse full radio value");
    checkEqual(index, 2, "parsed index");
    checkEqual(name, "b.ogg", "parsed name from option list");

    check(hs::parseRadioValue("0,None,a.wav", index, name), "parse with None selected");
    checkEqual(index, 0, "None index");
    checkEqual(name, "None", "None name");

    check(hs::parseRadioValue("1", index, name), "parse bare numeric index");
    checkEqual(index, 1, "bare index value");
    checkEqual(name, "", "bare index carries no name");

    check(hs::parseRadioValue("a.wav", index, name), "parse bare file name");
    checkEqual(index, 0, "bare name starts at None");
    checkEqual(name, "a.wav", "bare name preserved");

    check(!hs::parseRadioValue("", index, name), "empty value rejected");

    checkEqual(hs::resolveSelectionIndex(1, "b.ogg", files), 2, "resolve by name (reordered)");
    checkEqual(hs::resolveSelectionIndex(1, "gone.ogg", files), 0, "missing file resolves to None");
    checkEqual(hs::resolveSelectionIndex(2, "", files), 2, "resolve by index");
    checkEqual(hs::resolveSelectionIndex(7, "", files), 0, "out-of-range index resolves to None");
    checkEqual(hs::resolveSelectionIndex(0, "", files), 0, "None stays None");
}

void testSampleWav() {
    std::printf("sample wav generation\n");
    const std::vector<std::uint8_t> wav = hs::makeSampleHitWav();

    checkEqual(static_cast<int>(wav.size() >= 44u ? 1 : 0), 1, "has header");
    check(wav.size() >= 44u, "size at least the header");
    if (wav.size() < 44u) return;

    checkEqual(std::string(reinterpret_cast<const char*>(wav.data()), 4), "RIFF", "RIFF tag");
    checkEqual(std::string(reinterpret_cast<const char*>(wav.data() + 8), 4), "WAVE", "WAVE tag");
    checkEqual(std::string(reinterpret_cast<const char*>(wav.data() + 12), 4), "fmt ", "fmt tag");
    checkEqual(std::string(reinterpret_cast<const char*>(wav.data() + 36), 4), "data", "data tag");

    const std::uint32_t dataSize = readU32(wav, 40);
    checkEqual(static_cast<int>(wav.size()), static_cast<int>(44 + dataSize),
               "file length matches data chunk");
    checkEqual(static_cast<int>(readU32(wav, 4)), static_cast<int>(36 + dataSize),
               "RIFF size field consistent");
    checkEqual(static_cast<int>(readU16(wav, 20)), 1, "PCM format");
    checkEqual(static_cast<int>(readU16(wav, 22)), 1, "mono");
    checkEqual(static_cast<int>(readU32(wav, 24)), 44100, "44.1 kHz sample rate");
    checkEqual(static_cast<int>(readU32(wav, 28)), 44100 * 2, "byte rate for mono 16-bit");
    checkEqual(static_cast<int>(readU16(wav, 32)), 2, "block align");
    checkEqual(static_cast<int>(readU16(wav, 34)), 16, "16 bits per sample");

    bool nonSilent = false;
    for (std::size_t i = 44; i < wav.size(); i += 2) {
        const std::int16_t sample = static_cast<std::int16_t>(readU16(wav, i));
        if (sample != 0) {
            nonSilent = true;
            break;
        }
    }
    check(nonSilent, "sample contains audio");
    checkEqual(static_cast<int>(readU16(wav, 44)), 0, "starts at zero (click-free)");
}

// ---------------------------------------------------------------------------
// Arrow (projectile) hit detection
// ---------------------------------------------------------------------------

namespace projectile {

constexpr std::uint32_t kMob = bedrocktools::sdk::offsets::ActorCategories::IsMob;
constexpr std::uint32_t kPlayer = bedrocktools::sdk::offsets::ActorCategories::IsPlayer;
constexpr std::uint32_t kItem = bedrocktools::sdk::offsets::ActorCategories::IsItem;
constexpr std::uint32_t kProjectile = bedrocktools::sdk::offsets::ActorCategories::IsProjectile;

// Stand-ins for real actors: the tracker only ever compares the pointer, it
// never dereferences it.
void* id(int n) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000 + n)); }

hs::ActorSnapshot actor(int n, hs::Point3 position, hs::Point3 delta, int hurtTime,
                        std::uint32_t categories) {
    hs::ActorSnapshot snapshot;
    snapshot.id = id(n);
    snapshot.position = position;
    snapshot.delta = delta;
    snapshot.hurtTime = hurtTime;
    snapshot.categories = categories;
    return snapshot;
}

hs::ActorSnapshot mob(int n, hs::Point3 position, int hurtTime) {
    return actor(n, position, hs::Point3{}, hurtTime, kMob);
}

// One tick's worth of nearby actors around a player standing at the origin.
struct Tick {
    std::vector<hs::ActorSnapshot> actors;

    Tick& arrow(int n, hs::Point3 position, hs::Point3 delta) {
        actors.push_back(actor(n, position, delta, 0, kProjectile));
        return *this;
    }
    Tick& mobAt(int n, hs::Point3 position, int hurtTime) {
        actors.push_back(mob(n, position, hurtTime));
        return *this;
    }
    Tick& item(int n, hs::Point3 position, int hurtTime) {
        actors.push_back(actor(n, position, hs::Point3{}, hurtTime, kItem));
        return *this;
    }
};

const hs::Point3 kPlayerPosition{0.0f, 64.0f, 0.0f};

// An arrow the local player just fired: it starts at the player and moves away.
hs::ActorSnapshot ownShot(int n, float distance) {
    return actor(n, hs::Point3{distance, 64.4f, 0.0f}, hs::Point3{2.5f, 0.0f, 0.0f}, 0, kProjectile);
}

} // namespace projectile

void testProjectileAttribution() {
    std::printf("arrow attribution\n");
    using namespace projectile;
    const hs::ProjectileConfig config;

    check(hs::looksLikeOwnProjectile(kPlayerPosition, ownShot(1, 1.0f), config),
          "arrow spawned at the player and flying away is ours");

    // The same arrow one tick later: still near the player, still moving away.
    check(hs::looksLikeOwnProjectile(kPlayerPosition, ownShot(1, 3.5f), config),
          "the next tick of the same arrow still looks like our shot");

    hs::ActorSnapshot incoming = actor(2, hs::Point3{4.0f, 64.0f, 0.0f}, hs::Point3{-3.0f, 0.0f, 0.0f},
                                       0, kProjectile);
    check(!hs::looksLikeOwnProjectile(kPlayerPosition, incoming, config),
          "arrow flying towards the player (a skeleton's) is not ours");

    hs::ActorSnapshot farAway = actor(3, hs::Point3{20.0f, 64.0f, 0.0f}, hs::Point3{2.5f, 0.0f, 0.0f},
                                      0, kProjectile);
    check(!hs::looksLikeOwnProjectile(kPlayerPosition, farAway, config),
          "projectile that spawned far from the player is not ours");

    hs::ActorSnapshot stuck = actor(4, hs::Point3{1.0f, 64.0f, 0.0f}, hs::Point3{}, 0, kProjectile);
    check(!hs::looksLikeOwnProjectile(kPlayerPosition, stuck, config),
          "arrow stuck in a block (no motion) is not a shot");

    // A stale previous-position read turns into an absurd per-tick motion.
    hs::ActorSnapshot garbage = actor(5, hs::Point3{1.0f, 64.0f, 0.0f}, hs::Point3{1200.0f, 64.0f, -900.0f},
                                      0, kProjectile);
    check(!hs::looksLikeOwnProjectile(kPlayerPosition, garbage, config),
          "impossible per-tick motion is rejected instead of trusting the read");
}

void testProjectileTracker() {
    std::printf("arrow hit tracking\n");
    using namespace projectile;

    {
        // Damage nobody shot for must stay silent — this is what keeps fall
        // damage, other players and fighting mobs from playing the sound.
        hs::ProjectileTracker tracker;
        check(tracker.update(id(9), kPlayerPosition, Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 0).actors.data(),
                             1, 0) == nullptr,
              "no hit before any shot");
        check(!tracker.shotArmed(50), "no shot is armed");
        check(tracker.update(id(9), kPlayerPosition, Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10).actors.data(),
                             1, 50) == nullptr,
              "damage without a shot in flight is ignored");
    }

    {
        // The happy path: shoot, the arrow flies, the mob's hurt-time jumps.
        hs::ProjectileTracker tracker;
        tracker.update(id(9), kPlayerPosition, Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 0).actors.data(), 1, 0);

        auto shotTick = Tick{}.arrow(2, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f})
                            .mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        check(tracker.update(id(9), kPlayerPosition, shotTick.actors.data(), shotTick.actors.size(), 100) == nullptr,
              "the shot itself is not a hit");
        check(tracker.shotArmed(150), "the shot arms the watch window");

        auto flightTick = Tick{}.arrow(2, {4.0f, 64.2f, 0.0f}, {3.0f, 0.0f, 0.0f})
                              .mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        check(tracker.update(id(9), kPlayerPosition, flightTick.actors.data(), flightTick.actors.size(), 200) == nullptr,
              "an arrow still in flight is not a hit");

        auto hitTick = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, hitTick.actors.data(), hitTick.actors.size(), 300) == id(1),
              "the arrow landing raises the victim's hurt-time and reports it");

        check(tracker.update(id(9), kPlayerPosition, hitTick.actors.data(), hitTick.actors.size(), 350) == nullptr,
              "one shot plays once");
        check(!tracker.shotArmed(360), "the window closes after the hit");
    }

    {
        // A second shot has to be detected afresh before it can play again.
        hs::ProjectileTracker tracker;
        auto shot = Tick{}.arrow(2, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, shot.actors.data(), shot.actors.size(), 0);
        auto hit = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, hit.actors.data(), hit.actors.size(), 100) == id(1),
              "first arrow lands");

        auto again = Tick{}.arrow(3, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 8);
        tracker.update(id(9), kPlayerPosition, again.actors.data(), again.actors.size(), 200);
        auto secondHit = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, secondHit.actors.data(), secondHit.actors.size(), 300) == id(1),
              "a fresh shot can land on the same victim again");
    }

    {
        // The hurt flash counting back down must not refire, and a real second
        // jump on the same victim must.
        hs::ProjectileTracker tracker;
        auto shot = Tick{}.arrow(2, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, shot.actors.data(), shot.actors.size(), 0);
        auto hit = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, hit.actors.data(), hit.actors.size(), 100) == id(1),
              "hit reports once");

        auto rearm = Tick{}.arrow(3, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        tracker.update(id(9), kPlayerPosition, rearm.actors.data(), rearm.actors.size(), 200);
        auto fading = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 8);
        check(tracker.update(id(9), kPlayerPosition, fading.actors.data(), fading.actors.size(), 300) == nullptr,
              "the hurt flash counting down does not refire");
        auto nextHit = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, nextHit.actors.data(), nextHit.actors.size(), 400) == id(1),
              "a jump back up on the same victim is a new hit");
    }

    {
        // An arrow that was not ours must never open the window.
        hs::ProjectileTracker tracker;
        tracker.update(id(9), kPlayerPosition, Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 0).actors.data(), 1, 0);
        auto incoming = Tick{}.arrow(2, {4.0f, 64.0f, 0.0f}, {-3.0f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        check(tracker.update(id(9), kPlayerPosition, incoming.actors.data(), incoming.actors.size(), 100) == nullptr,
              "an arrow shot at us is not a shot of ours");
        check(!tracker.shotArmed(150), "an incoming arrow does not arm the window");
        auto hurt = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, hurt.actors.data(), hurt.actors.size(), 200) == nullptr,
              "damage while no shot of ours is in flight stays silent");
    }

    {
        // The window expires on its own, and a projectile far from the player
        // never opens a new one.
        hs::ProjectileTracker tracker;
        auto passing = Tick{}.arrow(2, {2.0f, 64.0f, 0.0f}, {3.0f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, passing.actors.data(), passing.actors.size(), 0);
        check(tracker.shotArmed(50), "a close projectile moving away arms the window");

        auto idle = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        check(tracker.update(id(9), kPlayerPosition, idle.actors.data(), idle.actors.size(), 5000) == nullptr,
              "the window still expires");
        check(!tracker.shotArmed(5000), "expired window is gone");

        auto late = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, late.actors.data(), late.actors.size(), 5100) == nullptr,
              "a rise after the window closed is ignored");

        auto farAway = Tick{}.arrow(3, {20.0f, 64.0f, 0.0f}, {3.0f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, farAway.actors.data(), farAway.actors.size(), 5200);
        check(!tracker.shotArmed(5210), "a projectile far from the player does not arm the window");
    }

    {
        // The melee path already played this tick: update the state, stay quiet.
        hs::ProjectileTracker tracker;
        auto shot = Tick{}.arrow(2, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, shot.actors.data(), shot.actors.size(), 0);
        auto hit = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, hit.actors.data(), hit.actors.size(), 100, true) == nullptr,
              "a suppressed hit is not reported");
        check(tracker.update(id(9), kPlayerPosition, hit.actors.data(), hit.actors.size(), 150) == nullptr,
              "the suppressed damage does not replay on the next tick");
    }

    {
        // Getting hurt yourself, dropped items and unreadable actors are all
        // ignored rather than mistaken for a landed arrow.
        hs::ProjectileTracker tracker;
        auto shot = Tick{}.arrow(2, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, shot.actors.data(), shot.actors.size(), 0);

        auto selfHurt = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        selfHurt.actors.push_back(actor(9, kPlayerPosition, hs::Point3{}, 10, kPlayer));
        check(tracker.update(id(9), kPlayerPosition, selfHurt.actors.data(), selfHurt.actors.size(), 100) == nullptr,
              "taking damage yourself is not a hit");

        auto droppedItem = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 0).item(4, {3.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, droppedItem.actors.data(), droppedItem.actors.size(), 200) == nullptr,
              "item entities are not victims");

        auto unreadable = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 5000);
        check(tracker.update(id(9), kPlayerPosition, unreadable.actors.data(), unreadable.actors.size(), 300) == nullptr,
              "an implausible hurt-time is not trusted");
        auto recovered = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, recovered.actors.data(), recovered.actors.size(), 400) == nullptr,
              "the victim gets a fresh baseline after an unreadable tick");
        check(tracker.update(id(9), kPlayerPosition, recovered.actors.data(), recovered.actors.size(), 500) == nullptr,
              "the same value twice is not a hit");
    }

    {
        // A fresh arrow can spend its spawn tick with no motion recorded yet.
        // The shot must then be recognised on the following tick, which is a
        // full ~3 blocks away from the player.
        hs::ProjectileTracker tracker;
        auto spawnTick = Tick{}.arrow(2, {0.3f, 64.4f, 0.0f}, {0.0f, 0.0f, 0.0f})
                             .mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, spawnTick.actors.data(), spawnTick.actors.size(), 0);
        check(!tracker.shotArmed(10), "a spawn tick that shows no motion is not a shot yet");

        auto firstFlight = Tick{}.arrow(2, {3.5f, 64.4f, 0.0f}, {3.2f, 0.0f, 0.0f})
                               .mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, firstFlight.actors.data(), firstFlight.actors.size(), 50);
        check(tracker.shotArmed(60), "the same arrow is recognised a tick later");

        auto hitTick = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, hitTick.actors.data(), hitTick.actors.size(), 100) == id(1),
              "and its landing still plays");
    }

    {
        // Bookkeeping: victims that leave the area are dropped and the watched
        // list stays bounded.
        hs::ProjectileTracker tracker;
        auto shot = Tick{}.arrow(2, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f})
                        .mobAt(1, {10.0f, 64.0f, 0.0f}, 0)
                        .mobAt(3, {12.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, shot.actors.data(), shot.actors.size(), 0);
        check(tracker.watchedCount() == 2, "both nearby mobs are watched");

        auto oneLeft = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, oneLeft.actors.data(), oneLeft.actors.size(), 100);
        check(tracker.watchedCount() == 1, "a mob that left the area is no longer watched");

        hs::ProjectileConfig small;
        small.maxWatched = 2;
        hs::ProjectileTracker capped(small);
        auto crowd = Tick{}.arrow(2, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f})
                         .mobAt(1, {10.0f, 64.0f, 0.0f}, 0)
                         .mobAt(3, {12.0f, 64.0f, 0.0f}, 0)
                         .mobAt(4, {14.0f, 64.0f, 0.0f}, 0);
        capped.update(id(9), kPlayerPosition, crowd.actors.data(), crowd.actors.size(), 0);
        check(capped.watchedCount() == 2, "the watched list respects its cap");
    }

    {
        // Disabling the module mid-flight must not leave a live window behind.
        hs::ProjectileTracker tracker;
        auto shot = Tick{}.arrow(2, {1.0f, 64.4f, 0.0f}, {2.5f, 0.0f, 0.0f}).mobAt(1, {10.0f, 64.0f, 0.0f}, 0);
        tracker.update(id(9), kPlayerPosition, shot.actors.data(), shot.actors.size(), 0);
        tracker.reset();
        check(!tracker.shotArmed(50), "reset drops the armed window");
        check(tracker.watchedCount() == 0, "reset drops the watched victims");
        auto hit = Tick{}.mobAt(1, {10.0f, 64.0f, 0.0f}, 10);
        check(tracker.update(id(9), kPlayerPosition, hit.actors.data(), hit.actors.size(), 100) == nullptr,
              "damage right after a reset stays silent");
    }
}

} // namespace

int main() {
    testExtensions();
    testScanning();
    testRadioValue();
    testSampleWav();
    testProjectileAttribution();
    testProjectileTracker();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("all hitsound tests passed\n");
        return 0;
    }
    std::printf("%d hitsound test(s) failed\n", g_failures);
    return 1;
}
