#pragma once

// Pure helpers for the Hit Sound module.
//
// Everything in this header is plain C++ with no Minecraft, launcher,
// mod-menu, JNI or audio dependency so it can be unit-tested on the host (see
// tests/hitsound_test.cpp). It covers the "dumb data" problems of the module:
//
//   * scanning the hitsounds directory for usable audio files
//   * (de)serializing the launcher "radio" config value used for the picker
//   * generating the sample hit sound (a short RIFF/WAV "ding") that is
//     written into the folder on first launch so players can hear the module
//     work before they add their own files

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#if __has_include(<filesystem>)
#include <filesystem>
#define HITSOUND_HAS_FILESYSTEM 1
#else
#define HITSOUND_HAS_FILESYSTEM 0
#endif

namespace hitsound {

// Index 0 of the radio picker is always the "no custom sound" entry. The
// vanilla hit/hurt sounds are not cancelled, so picking None simply leaves the
// game's default behaviour.
inline constexpr const char* kNoneLabel = "None";

// Audio formats Android's MediaPlayer can decode from a plain file path.
// Files are matched case-insensitively. Two-letter "aac" is excluded on
// purpose (it needs an ADTS container to be reliably detectable) and midi is
// excluded because many devices cannot play it.
inline constexpr const char* kAudioExtensions[] = {".wav", ".ogg", ".mp3", ".m4a", ".flac"};

inline bool hasAudioExtension(const std::string& name) {
    if (name.size() < 5) return false;
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* ext : kAudioExtensions) {
        if (lower.size() >= std::string(ext).size() &&
            lower.compare(lower.size() - std::string(ext).size(), std::string(ext).size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

// A file name becomes a radio option, and options are comma-separated, so a
// file whose name contains a comma could never round-trip through the launcher
// menu. Such files are skipped instead of corrupting the list.
inline bool isUsableSoundFileName(const std::string& name) {
    if (!hasAudioExtension(name)) return false;
    return name.find(',') == std::string::npos;
}

// Lists the hitsounds directory (non-recursive), returning plain file names of
// audio files, sorted alphabetically. Missing/inaccessible directories simply
// yield an empty list.
inline std::vector<std::string> scanSoundFiles(const std::string& directory) {
    std::vector<std::string> files;
#if HITSOUND_HAS_FILESYSTEM
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) return files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (isUsableSoundFileName(name)) files.push_back(name);
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
#endif
    return files;
}

// Serializes the picker value in the launcher's radio format:
// "<currentIndex>,<None>,<file1>,<file2>,..." — the menu renders the part
// after the first comma as the option list and treats the part before it as
// the selected index (same convention the Custom Capes and Crosshair modules
// use).
inline std::string makeRadioValue(int selectedIndex, const std::vector<std::string>& files) {
    const int optionCount = 1 + static_cast<int>(files.size());
    if (selectedIndex < 0) selectedIndex = 0;
    if (selectedIndex >= optionCount) selectedIndex = optionCount - 1;

    std::string value = std::to_string(selectedIndex);
    value += ',';
    value += kNoneLabel;
    for (const std::string& file : files) {
        value += ',';
        value += file;
    }
    return value;
}

// Parses a radio value coming from the config file or from the launcher
// (which reports just the index when the selection changes). Returns the
// selected index and, when the option list is embedded in the value, the
// selected option's name so the caller can recover the chosen file even if
// the on-disk listing changed since the value was written.
inline bool parseRadioValue(const std::string& value, int& outIndex, std::string& outName) {
    outIndex = 0;
    outName.clear();
    if (value.empty()) return false;

    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = value.find(',', start);
        tokens.push_back(value.substr(start, comma == std::string::npos ? comma : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    bool numericIndex = !tokens[0].empty();
    for (char c : tokens[0]) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '+') {
            numericIndex = false;
            break;
        }
    }

    if (numericIndex) {
        try {
            outIndex = std::stoi(tokens[0]);
        } catch (...) {
            outIndex = 0;
        }
        const int option = outIndex; // option list includes "None" as entry 0
        if (option > 0 && option < static_cast<int>(tokens.size()) - 1) {
            outName = tokens[option + 1];
        } else if (option == 0 && tokens.size() > 1) {
            outName = tokens[1]; // normally "None"
        }
        return true;
    }

    // A bare file name (no numeric index): treat it as that sound's selection.
    outIndex = 0;
    outName = tokens[0];
    return !outName.empty();
}

// Maps a parsed radio selection back onto the current on-disk listing.
// files[0] never exists (index 0 == None). Returns the index within the
// "None + files" option space: 0 when nothing is selected/found.
inline int resolveSelectionIndex(int parsedIndex, const std::string& parsedName,
                                 const std::vector<std::string>& files) {
    // Prefer the recovered file name; it survives list reordering.
    if (!parsedName.empty() && parsedName != kNoneLabel) {
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i] == parsedName) return static_cast<int>(i) + 1;
        }
        // The file was deleted/renamed since the value was written.
        return 0;
    }
    if (parsedIndex <= 0) return 0;
    if (parsedIndex <= static_cast<int>(files.size())) return parsedIndex;
    return 0;
}

// Builds a short RIFF/WAVE "ding" the module writes as "Sample Hit.wav" the
// first time the hitsounds folder is created. 44.1 kHz, mono, 16-bit PCM,
// roughly 0.4 seconds — a two-tone pling with an exponential decay and
// click-free fade in/out, so the sample demonstrates the feature without
// needing any bundled binary asset.
inline std::vector<std::uint8_t> makeSampleHitWav() {
    constexpr std::uint32_t kSampleRate = 44100;
    constexpr std::uint16_t kChannels = 1;
    constexpr std::uint16_t kBitsPerSample = 16;
    constexpr double kDuration = 0.40;
    constexpr std::uint32_t kNumSamples = static_cast<std::uint32_t>(kSampleRate * kDuration);
    constexpr std::uint32_t kBytesPerSample = kChannels * (kBitsPerSample / 8);
    constexpr std::uint32_t kDataSize = kNumSamples * kBytesPerSample;

    std::vector<std::uint8_t> out;
    out.reserve(44 + kDataSize);

    auto pushBytes = [&out](std::uint32_t value, int count) {
        for (int i = 0; i < count; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    };
    auto pushTag = [&out](const char* tag) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(tag[i]));
    };

    pushTag("RIFF");
    pushBytes(36 + kDataSize, 4);
    pushTag("WAVE");
    pushTag("fmt ");
    pushBytes(16, 4);                                   // fmt chunk size
    pushBytes(1, 2);                                    // PCM
    pushBytes(kChannels, 2);
    pushBytes(kSampleRate, 4);
    pushBytes(kSampleRate * kChannels * (kBitsPerSample / 8), 4); // byte rate
    pushBytes(kBytesPerSample, 2);                      // block align
    pushBytes(kBitsPerSample, 2);
    pushTag("data");
    pushBytes(kDataSize, 4);

    constexpr double kFadeInSeconds = 0.004;
    constexpr double kFadeOutSeconds = 0.035;
    for (std::uint32_t n = 0; n < kNumSamples; ++n) {
        const double t = static_cast<double>(n) / static_cast<double>(kSampleRate);
        const double fadeIn = std::min(1.0, t / kFadeInSeconds);
        const double fadeOut = std::max(0.0, std::min(1.0, (kDuration - t) / kFadeOutSeconds));
        const double envelope = std::exp(-9.0 * t) * fadeIn * fadeOut;

        // Bright, friendly pling: A5 with a fifth above and an octave shimmer.
        const double signal =
            0.85 * std::sin(2.0 * 3.14159265358979 * 880.0 * t) +
            0.45 * std::sin(2.0 * 3.14159265358979 * 1320.0 * t + 1.1) +
            0.20 * std::sin(2.0 * 3.14159265358979 * 1760.0 * t + 2.3);

        double sample = 0.55 * envelope * signal;
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        const std::int16_t pcm = static_cast<std::int16_t>(std::lround(sample * 32767.0));
        pushBytes(static_cast<std::uint32_t>(static_cast<std::uint16_t>(pcm)), 2);
    }
    return out;
}

} // namespace hitsound
