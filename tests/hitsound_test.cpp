// Unit tests for the Hit Sound module's pure helpers (folder scanning, the
// launcher "radio" value format, and the generated sample WAV).
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src tests/hitsound_test.cpp -o /tmp/hitsound_test
//     /tmp/hitsound_test

#include "modules/misc/hitsound_files.hpp"

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

} // namespace

int main() {
    testExtensions();
    testScanning();
    testRadioValue();
    testSampleWav();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("all hitsound tests passed\n");
        return 0;
    }
    std::printf("%d hitsound test(s) failed\n", g_failures);
    return 1;
}
