#pragma once

// Pure helpers for the Custom Capes module.
//
// Everything in this header is plain C++ with no Minecraft, launcher or
// mod-menu dependencies so it can be unit-tested on the host (see
// tests/customcapes_test.cpp). It covers the three "dumb data" problems of
// the module:
//
//   * scanning the capes directory for usable PNG files
//   * (de)serializing the launcher "radio" config value used for the picker
//   * resampling an arbitrary RGBA image into the visible classic-cape face
//     and the Elytra UV area of the 64x32 canvas Minecraft expects

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#if __has_include(<filesystem>)
#include <filesystem>
#define CUSTOMCAPES_HAS_FILESYSTEM 1
#else
#define CUSTOMCAPES_HAS_FILESYSTEM 0
#endif

namespace customcapes {

// Vanilla classic capes are rendered from a 64x32 RGBA8 texture. The cape is
// a 10x16x1 cuboid unwrapped at texture offset (0,0), which distributes its
// six faces over the canvas like this (standard box unwrap):
//
//     top    ( 1, 0) 10x 1      bottom (11, 0) 10x 1
//     right  ( 0, 1)  1x16      back   ( 1, 1) 10x16   <- outer face (design)
//     left   (11, 1)  1x16      front  (12, 1) 10x16   <- inner face (lining)
inline constexpr std::uint32_t kCapeWidth = 64;
inline constexpr std::uint32_t kCapeHeight = 32;

// The outer (worn, visible) face the image is painted onto — the region
// x=1..11, y=1..17 of the 64x32 canvas.
inline constexpr std::uint32_t kCapeBackX = 1;
inline constexpr std::uint32_t kCapeBackY = 1;
inline constexpr std::uint32_t kCapeBackWidth = 10;
inline constexpr std::uint32_t kCapeBackHeight = 16;

// The inner face on the other side of the cuboid (same 10x16 size). The
// image is never repeated here; it is filled with one flat lining color.
inline constexpr std::uint32_t kCapeFrontX = 12;
inline constexpr std::uint32_t kCapeFrontY = 1;

// The 1-voxel-thick edge strips of the cuboid; painting them with colors
// from the image's edges keeps the cape from looking paper-thin.
inline constexpr std::uint32_t kCapeTopX = 1;      // 10x1 strip on row y=0
inline constexpr std::uint32_t kCapeTopY = 0;
inline constexpr std::uint32_t kCapeBottomX = 11;  // 10x1 strip on row y=0
inline constexpr std::uint32_t kCapeBottomY = 0;
inline constexpr std::uint32_t kCapeSideRightX = 0; // 1x16 strip at x=0
inline constexpr std::uint32_t kCapeSideLeftX = 11; // 1x16 strip at x=11
inline constexpr std::uint32_t kCapeSideY = 1;

// When an Elytra is equipped, Bedrock renders its 10x20x2 wing cube from the
// same cape image, at UV (22,0). Both wings share this area (the other wing is
// mirrored by the model), so a cape image with a transparent right-hand side
// produces an untextured/default Elytra. These are the six box-unwrapped
// regions sampled by geometry.elytra.
inline constexpr std::uint32_t kElytraUvX = 22;
inline constexpr std::uint32_t kElytraUvY = 0;
inline constexpr std::uint32_t kElytraUvWidth = 24;
inline constexpr std::uint32_t kElytraUvHeight = 22;
inline constexpr std::uint32_t kElytraFaceWidth = 10;
inline constexpr std::uint32_t kElytraFaceHeight = 20;
inline constexpr std::uint32_t kElytraDepth = 2;
inline constexpr std::uint32_t kElytraTopX = 24;       // 10x2
inline constexpr std::uint32_t kElytraTopY = 0;
inline constexpr std::uint32_t kElytraBottomX = 34;    // 10x2
inline constexpr std::uint32_t kElytraBottomY = 0;
inline constexpr std::uint32_t kElytraSideRightX = 22; // 2x20
inline constexpr std::uint32_t kElytraFrontX = 24;     // 10x20
inline constexpr std::uint32_t kElytraSideLeftX = 34;  // 2x20
inline constexpr std::uint32_t kElytraBackX = 36;      // 10x20, visible design
inline constexpr std::uint32_t kElytraSideY = 2;

// Vanilla's wing cube is rectangular, but transparent texels cut it into the
// familiar tapered Elytra silhouette. Each bit below represents x=22..45 for
// one row y=0..21 of textures/models/armor/elytra.png. Reusing only its alpha
// silhouette lets a custom cape supply the colors without turning the Elytra
// into two opaque rectangular slabs.
inline constexpr std::array<std::uint32_t, kElytraUvHeight> kElytraAlphaMask = {
    0x03fe00u, 0x000c00u, 0x0ff000u, 0x1ff000u, 0x3fe000u, 0x3fe000u,
    0x3fe000u, 0x7fe000u, 0x7fe000u, 0x7fe000u, 0x7fe000u, 0xffc001u,
    0xffc001u, 0xffc001u, 0xffc001u, 0xffc001u, 0xff8001u, 0xff8001u,
    0xff8001u, 0xff0001u, 0xff0001u, 0xfe0001u,
};

// True for every texel actually addressed by the Elytra box unwrap. The two
// unused 2x2 corners on the first two rows are deliberately excluded.
inline constexpr bool isElytraUvPixel(std::uint32_t x, std::uint32_t y) {
    if (x < kElytraUvX || x >= kElytraUvX + kElytraUvWidth ||
        y < kElytraUvY || y >= kElytraUvY + kElytraUvHeight) {
        return false;
    }
    return y >= kElytraDepth ||
           (x >= kElytraTopX && x < kElytraBottomX + kElytraFaceWidth);
}

inline constexpr bool isElytraMaskPixel(std::uint32_t x, std::uint32_t y) {
    if (x < kElytraUvX || x >= kElytraUvX + kElytraUvWidth ||
        y >= kElytraUvHeight) {
        return false;
    }
    return (kElytraAlphaMask[y] & (1u << (x - kElytraUvX))) != 0;
}

// Safety cap so a hostile/corrupt PNG can never exhaust device memory.
inline constexpr std::uint32_t kMaxSourceDimension = 4096;

// Index 0 of the radio picker is always the "no custom cape" entry.
inline constexpr const char* kNoneLabel = "None";

inline bool hasPngExtension(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".png";
}

// A file name becomes a radio option, and options are comma-separated, so a
// file whose name contains a comma could never round-trip through the
// launcher menu. Such files are skipped instead of corrupting the list.
inline bool isUsableCapeFileName(const std::string& name) {
    if (!hasPngExtension(name)) return false;
    return name.find(',') == std::string::npos;
}

// Lists the capes directory (non-recursive), returning plain file names of
// PNG files, sorted alphabetically. Missing/inaccessible directories simply
// yield an empty list.
inline std::vector<std::string> scanCapeFiles(const std::string& directory) {
    std::vector<std::string> files;
#if CUSTOMCAPES_HAS_FILESYSTEM
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) return files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (isUsableCapeFileName(name)) files.push_back(name);
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
#endif
    return files;
}

// Serializes the picker value in the launcher's radio format:
// "<currentIndex>,<None>,<file1>,<file2>,..." — the menu renders the part
// after the first comma as the option list and treats the part before it as
// the selected index (same convention the Crosshair module uses).
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

// Same radio format for a plain label list, without the implicit "None"
// entry that makeRadioValue() prepends for file pickers. Used by the Cape
// Fit mode, whose options are "Fit,Fill,Crop" and whose index 0 is a real
// choice rather than "nothing selected".
inline std::string makeLabelRadioValue(int selectedIndex,
                                       const std::vector<std::string>& labels) {
    const int optionCount = static_cast<int>(labels.size());
    if (optionCount == 0) return "0";
    if (selectedIndex < 0) selectedIndex = 0;
    if (selectedIndex >= optionCount) selectedIndex = optionCount - 1;

    std::string value = std::to_string(selectedIndex);
    for (const std::string& label : labels) {
        value += ',';
        value += label;
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

    // A bare file name (no numeric index): treat it as that cape's selection.
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

// Returns true when a complete 64x32 cape canvas already contains visible
// artwork in any texel used by geometry.elytra. Checking the whole UV unwrap,
// rather than only the vanilla silhouette, preserves custom wing shapes.
inline bool hasElytraArtwork(const std::vector<std::uint8_t>& canvas) {
    const std::size_t expected = static_cast<std::size_t>(kCapeWidth) * kCapeHeight * 4u;
    if (canvas.size() < expected) return false;
    for (std::uint32_t y = 0; y < kElytraUvHeight; ++y) {
        for (std::uint32_t x = kElytraUvX; x < kElytraUvX + kElytraUvWidth; ++x) {
            if (!isElytraUvPixel(x, y)) continue;
            const std::size_t i = (static_cast<std::size_t>(y) * kCapeWidth + x) * 4u;
            if (canvas[i + 3] != 0) return true;
        }
    }
    return false;
}

// Paints the visible cape design onto the shared Elytra wing UV. This is a
// fallback for generated capes and complete 64x32 capes whose Elytra area is
// empty. The RGB and source alpha come from the cape's outer face; the final
// alpha is also clipped by the vanilla tapered-wing mask.
inline void paintElytraFromCape(std::vector<std::uint8_t>& canvas) {
    const std::size_t expected = static_cast<std::size_t>(kCapeWidth) * kCapeHeight * 4u;
    if (canvas.size() < expected) return;

    const auto pixel = [&](std::uint32_t x, std::uint32_t y) -> std::uint8_t* {
        return &canvas[(static_cast<std::size_t>(y) * kCapeWidth + x) * 4u];
    };

    // Remove invisible RGB left in an otherwise transparent input, then add
    // only the texels belonging to the vanilla wing silhouette.
    for (std::uint32_t y = 0; y < kElytraUvHeight; ++y) {
        for (std::uint32_t x = kElytraUvX; x < kElytraUvX + kElytraUvWidth; ++x) {
            if (!isElytraUvPixel(x, y)) continue;
            std::memset(pixel(x, y), 0, 4);
        }
    }

    for (std::uint32_t y = 0; y < kElytraUvHeight; ++y) {
        for (std::uint32_t x = kElytraUvX; x < kElytraUvX + kElytraUvWidth; ++x) {
            if (!isElytraMaskPixel(x, y)) continue;

            // Map each unwrapped face back to an adjacent column of the cape
            // design. The large visible face (x=36..45) gets all ten columns;
            // the thin side strips continue the corresponding edge color.
            std::uint32_t designX = 0;
            if (x < kElytraTopX) {
                designX = kCapeBackWidth - 1;
            } else if (x < kElytraTopX + kElytraFaceWidth) {
                designX = x - kElytraTopX;
            } else if (x < kElytraBackX) {
                designX = 0;
            } else {
                designX = std::min(x - kElytraBackX, kCapeBackWidth - 1);
            }

            const std::uint32_t wingY = y < kElytraSideY ? 0 : y - kElytraSideY;
            const std::uint32_t designY = std::min(
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(wingY) * kCapeBackHeight) /
                    kElytraFaceHeight),
                kCapeBackHeight - 1);

            std::memcpy(pixel(x, y),
                        pixel(kCapeBackX + designX, kCapeBackY + designY), 4);
        }
    }
}

// ---------------------------------------------------------------
// Resampler — Dynamic Resolution + Bilinear + Fit/Fill/Crop
// ---------------------------------------------------------------
// Any source resolution is accepted (22×23, 88×92, 176×184, 704×736,
// 736×797, 128×64, ...); W×H are read from the decoded image, never assumed.
//
// How the artwork is mapped onto the 10×16 cape face is selectable:
//
//   Fit  (default) — the WHOLE image is used, aspect preserved. It is scaled
//         into the largest centred rectangle that still keeps the source
//         aspect ratio, and the remaining bands are filled by continuing the
//         image's own edge colors (no black bars, no transparent gaps).
//         A 22×23 cape therefore lands as a 10×10 design centred on the
//         face with three rows of its own edge color above and below —
//         nothing is cut off and nothing is stretched.
//   Fill — the WHOLE image is stretched over the entire 10×16 face. No
//         cropping either, but the aspect ratio is not preserved.
//   Crop — aspect preserved, centered "cover" crop. No distortion, but the
//         part of the image that does not fit the 10:16 face is discarded
//         (35% of the pixels of a 22×23 cape, 69% of a 128×64 one).
//
// Two special cases are handled before any of that:
//   * an exact 64×32 input is copied pixel-for-pixel (a hand-authored cape
//     canvas, including its own Elytra pixels);
//   * an HD cape canvas (2:1 aspect, at least 128 px wide: 128×64, 256×128,
//     ...) is a scaled-up 64×32 layout rather than a logo, so it is
//     box-downscaled onto the whole 64×32 canvas instead of being painted
//     onto the back face only — otherwise its layout would be destroyed.
//
// Pipeline for the generic case:
//   1) Detect source W,H dynamically (parameters width, height).
//   2) Pick the source rect and the destination rect on the back face
//      according to the fit mode.
//   3) Bilinear-resample the source rect into the destination rect.
//   4) Letterbox bands (Fit) continue the outermost sampled row/column.
//   5) Inner FRONT face (12,1) gets a flat lining color (half-bright avg).
//   6) Thickness strips (top/bottom/side) copy adjacent edge pixels.
//   7) Elytra UV (22,0) generated from the same artwork with tapered mask.
// ---------------------------------------------------------------

// How an image of arbitrary resolution is placed on the cape face. The
// numeric values are the menu's radio indices (see kCapeFitLabels).
enum class CapeFitMode : int {
    Fit = 0,
    Fill = 1,
    Crop = 2,
};

inline constexpr const char* kCapeFitLabels[] = {"Fit", "Fill", "Crop"};
inline constexpr int kCapeFitModeCount = 3;

inline int clampCapeFitIndex(int index) {
    return (index < 0 || index >= kCapeFitModeCount) ? 0 : index;
}

inline CapeFitMode capeFitModeFromIndex(int index) {
    switch (clampCapeFitIndex(index)) {
        case 1:  return CapeFitMode::Fill;
        case 2:  return CapeFitMode::Crop;
        default: return CapeFitMode::Fit;
    }
}

// Case-insensitive label lookup, so a menu that reports the option's name
// instead of its index still selects the right mode. Returns -1 when the
// label is unknown.
inline int capeFitIndexFromLabel(const std::string& label) {
    for (int i = 0; i < kCapeFitModeCount; ++i) {
        const std::string candidate = kCapeFitLabels[i];
        if (label.size() != candidate.size()) continue;
        bool same = true;
        for (std::size_t c = 0; c < label.size(); ++c) {
            if (std::tolower(static_cast<unsigned char>(label[c])) !=
                std::tolower(static_cast<unsigned char>(candidate[c]))) {
                same = false;
                break;
            }
        }
        if (same) return i;
    }
    return -1;
}

// The radio option list shown in the mod menu ("Fit,Fill,Crop").
inline std::vector<std::string> capeFitLabelList() {
    return std::vector<std::string>(kCapeFitLabels, kCapeFitLabels + kCapeFitModeCount);
}

// True for a scaled-up 64×32 cape canvas (OptiFine-style HD capes): the
// canvas aspect ratio at a size where it can only be a canvas, not a logo.
inline bool isHdCapeCanvas(std::uint32_t width, std::uint32_t height) {
    return height != 0 && width == 2u * height && width >= 2u * kCapeWidth;
}

// Averages the source block of every destination texel (box filter), which
// is what an HD canvas needs to be scaled down without aliasing away thin
// features. RGBA channels are averaged independently.
inline void boxDownscaleToCanvas(const std::uint8_t* rgba, std::uint32_t width,
                                 std::uint32_t height, std::vector<std::uint8_t>& out) {
    const std::size_t expected = static_cast<std::size_t>(kCapeWidth) * kCapeHeight * 4u;
    if (!rgba || width == 0 || height == 0 || out.size() < expected) return;

    for (std::uint32_t y = 0; y < kCapeHeight; ++y) {
        const std::uint32_t y0 = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y) * height) / kCapeHeight);
        std::uint32_t y1 = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y + 1) * height) / kCapeHeight);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > height) y1 = height;

        for (std::uint32_t x = 0; x < kCapeWidth; ++x) {
            const std::uint32_t x0 = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * width) / kCapeWidth);
            std::uint32_t x1 = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x + 1) * width) / kCapeWidth);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > width) x1 = width;

            std::uint32_t sum[4] = {0, 0, 0, 0};
            std::uint32_t count = 0;
            for (std::uint32_t sy = y0; sy < y1; ++sy) {
                for (std::uint32_t sx = x0; sx < x1; ++sx) {
                    const std::uint8_t* s = rgba + (static_cast<std::size_t>(sy) * width + sx) * 4u;
                    for (int c = 0; c < 4; ++c) sum[c] += s[c];
                    ++count;
                }
            }
            std::uint8_t* d = &out[(static_cast<std::size_t>(y) * kCapeWidth + x) * 4u];
            if (count == 0) {
                std::memset(d, 0, 4);
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                d[c] = static_cast<std::uint8_t>((sum[c] + count / 2u) / count);
            }
        }
    }
}

inline void bilinearSample(const std::uint8_t* rgba, std::uint32_t W, std::uint32_t H,
                           double fx, double fy, std::uint8_t out[4]) {
    if (!rgba || W == 0 || H == 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    // Clamp to valid texel range for sampling
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > static_cast<double>(W - 1)) fx = static_cast<double>(W - 1);
    if (fy > static_cast<double>(H - 1)) fy = static_cast<double>(H - 1);

    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(fx));
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(fy));
    const std::uint32_t x1 = std::min(x0 + 1u, W - 1u);
    const std::uint32_t y1 = std::min(y0 + 1u, H - 1u);
    const double wx = fx - static_cast<double>(x0);
    const double wy = fy - static_cast<double>(y0);

    const std::size_t i00 = (static_cast<std::size_t>(y0) * W + x0) * 4u;
    const std::size_t i10 = (static_cast<std::size_t>(y0) * W + x1) * 4u;
    const std::size_t i01 = (static_cast<std::size_t>(y1) * W + x0) * 4u;
    const std::size_t i11 = (static_cast<std::size_t>(y1) * W + x1) * 4u;

    for (int c = 0; c < 4; ++c) {
        const double top = (1.0 - wx) * static_cast<double>(rgba[i00 + c]) +
                           wx * static_cast<double>(rgba[i10 + c]);
        const double bottom = (1.0 - wx) * static_cast<double>(rgba[i01 + c]) +
                              wx * static_cast<double>(rgba[i11 + c]);
        const double value = (1.0 - wy) * top + wy * bottom;
        // Round to nearest
        double clamped = value;
        if (clamped < 0) clamped = 0;
        if (clamped > 255) clamped = 255;
        out[c] = static_cast<std::uint8_t>(clamped + 0.5);
    }
}

inline std::vector<std::uint8_t> resampleToCape(const std::uint8_t* rgba, std::uint32_t width,
                                                std::uint32_t height,
                                                CapeFitMode mode = CapeFitMode::Fit) {
    std::vector<std::uint8_t> out;
    out.resize(static_cast<std::size_t>(kCapeWidth) * kCapeHeight * 4u, 0);
    if (!rgba || width == 0 || height == 0) return out;

    // Exact 64×32 canvas — keep pixel-perfect, preserve custom Elytra if present
    if (width == kCapeWidth && height == kCapeHeight) {
        out.assign(rgba, rgba + out.size());
        if (!hasElytraArtwork(out)) paintElytraFromCape(out);
        return out;
    }

    // HD cape canvas (128×64, 256×128, ...): a scaled-up 64×32 layout, so it
    // is scaled down onto the whole canvas instead of being treated as a
    // logo that only covers the back face.
    if (isHdCapeCanvas(width, height)) {
        boxDownscaleToCanvas(rgba, width, height, out);
        if (!hasElytraArtwork(out)) paintElytraFromCape(out);
        return out;
    }

    const auto px = [](std::vector<std::uint8_t>& canvas, std::uint32_t x,
                       std::uint32_t y) -> std::uint8_t* {
        return &canvas[(static_cast<std::size_t>(y) * kCapeWidth + x) * 4u];
    };

    // --- 1) Dynamic Resolution Detection ---------------------------------
    const double srcW = static_cast<double>(width);
    const double srcH = static_cast<double>(height);

    // Source rectangle that is sampled, and the destination rectangle on the
    // back face it is drawn into. Defaults describe Fill: the whole image
    // onto the whole face.
    double cropX = 0.0, cropY = 0.0, cropW = srcW, cropH = srcH;
    std::uint32_t dstX = 0, dstY = 0;
    std::uint32_t dstW = kCapeBackWidth, dstH = kCapeBackHeight;

    const auto roundClamped = [](double value, std::uint32_t minV,
                                 std::uint32_t maxV) -> std::uint32_t {
        const double rounded = std::floor(value + 0.5);
        if (!(rounded > 0.0)) return minV;
        if (rounded < static_cast<double>(minV)) return minV;
        if (rounded > static_cast<double>(maxV)) return maxV;
        return static_cast<std::uint32_t>(rounded);
    };

    if (mode == CapeFitMode::Crop) {
        // --- Smart Crop (cover): aspect preserved, centered ---------------
        // Target aspect is the cape back face (10×16)
        const double targetAspect = static_cast<double>(kCapeBackWidth) /
                                    static_cast<double>(kCapeBackHeight); // 0.625
        const double srcAspect = srcW / srcH;
        if (srcAspect > targetAspect) {
            // Source wider than target -> crop width, keep full height
            cropH = srcH;
            cropW = srcH * targetAspect;
            cropX = (srcW - cropW) * 0.5;
            cropY = 0.0;
        } else {
            // Source taller than target -> crop height, keep full width
            cropW = srcW;
            cropH = srcW / targetAspect;
            cropX = 0.0;
            cropY = (srcH - cropH) * 0.5;
        }
        // Guard against degenerate crops (tiny images)
        if (cropW < 1.0) {
            cropW = srcW;
            cropX = 0.0;
        }
        if (cropH < 1.0) {
            cropH = srcH;
            cropY = 0.0;
        }
    } else if (mode == CapeFitMode::Fit) {
        // --- Fit (contain): whole image, aspect preserved, centered -------
        // Largest integer rectangle on the 10×16 face with the source aspect.
        if (srcW * static_cast<double>(kCapeBackHeight) >=
            srcH * static_cast<double>(kCapeBackWidth)) {
            dstW = kCapeBackWidth;
            dstH = roundClamped(srcH * static_cast<double>(kCapeBackWidth) / srcW, 1u,
                                kCapeBackHeight);
        } else {
            dstH = kCapeBackHeight;
            dstW = roundClamped(srcW * static_cast<double>(kCapeBackHeight) / srcH, 1u,
                                kCapeBackWidth);
        }
        dstX = (kCapeBackWidth - dstW) / 2u;
        dstY = (kCapeBackHeight - dstH) / 2u;
    }

    // --- 2) Bilinear Resampling into the destination rect -----------------
    for (std::uint32_t y = 0; y < dstH; ++y) {
        for (std::uint32_t x = 0; x < dstW; ++x) {
            // Map destination pixel center to source crop space
            // Standard half-pixel center mapping: (dx+0.5)*crop / dst -0.5
            const double srcXf = cropX + (static_cast<double>(x) + 0.5) * cropW /
                                              static_cast<double>(dstW) -
                                          0.5;
            const double srcYf = cropY + (static_cast<double>(y) + 0.5) * cropH /
                                              static_cast<double>(dstH) -
                                          0.5;

            std::uint8_t sampled[4];
            bilinearSample(rgba, width, height, srcXf, srcYf, sampled);
            std::uint8_t* dst = px(out, kCapeBackX + dstX + x, kCapeBackY + dstY + y);
            dst[0] = sampled[0];
            dst[1] = sampled[1];
            dst[2] = sampled[2];
            dst[3] = sampled[3];
        }
    }

    // --- 3) Letterbox bands (Fit): continue the image's own edge colors ---
    // Never leave transparent or black bars around the artwork: the bands
    // repeat the outermost sampled row/column, which is the image's edge.
    if (dstY > 0 || dstY + dstH < kCapeBackHeight) {
        for (std::uint32_t x = 0; x < dstW; ++x) {
            const std::uint32_t column = kCapeBackX + dstX + x;
            const std::uint8_t* top = px(out, column, kCapeBackY + dstY);
            const std::uint8_t* bottom = px(out, column, kCapeBackY + dstY + dstH - 1);
            for (std::uint32_t y = 0; y < dstY; ++y) {
                std::memcpy(px(out, column, kCapeBackY + y), top, 4);
            }
            for (std::uint32_t y = dstY + dstH; y < kCapeBackHeight; ++y) {
                std::memcpy(px(out, column, kCapeBackY + y), bottom, 4);
            }
        }
    }
    if (dstX > 0 || dstX + dstW < kCapeBackWidth) {
        for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
            const std::uint32_t row = kCapeBackY + y;
            const std::uint8_t* left = px(out, kCapeBackX + dstX, row);
            const std::uint8_t* right = px(out, kCapeBackX + dstX + dstW - 1, row);
            for (std::uint32_t x = 0; x < dstX; ++x) {
                std::memcpy(px(out, kCapeBackX + x, row), left, 4);
            }
            for (std::uint32_t x = dstX + dstW; x < kCapeBackWidth; ++x) {
                std::memcpy(px(out, kCapeBackX + x, row), right, 4);
            }
        }
    }

    // --- 4) Inner front face: flat lining color (half-bright average) ---
    std::uint32_t sum[4] = {0, 0, 0, 0};
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
            const std::uint8_t* p = px(out, kCapeBackX + x, kCapeBackY + y);
            for (int c = 0; c < 4; ++c) sum[c] += p[c];
        }
    }
    const std::uint32_t area = kCapeBackWidth * kCapeBackHeight;
    std::uint8_t liner[4];
    for (int c = 0; c < 3; ++c) liner[c] = static_cast<std::uint8_t>((sum[c] / area) / 2u);
    liner[3] = static_cast<std::uint8_t>(sum[3] / area);
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
            std::uint8_t* p = px(out, kCapeFrontX + x, kCapeFrontY + y);
            for (int c = 0; c < 4; ++c) p[c] = liner[c];
        }
    }

    // --- 5) Thickness: edge strips auto-filled from image edge colors ---
    for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
        std::memcpy(px(out, kCapeTopX + x, kCapeTopY),
                    px(out, kCapeBackX + x, kCapeBackY), 4);
        std::memcpy(px(out, kCapeBottomX + x, kCapeBottomY),
                    px(out, kCapeBackX + x, kCapeBackY + kCapeBackHeight - 1), 4);
    }
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        std::memcpy(px(out, kCapeSideRightX, kCapeSideY + y),
                    px(out, kCapeBackX, kCapeBackY + y), 4);
        std::memcpy(px(out, kCapeSideLeftX, kCapeSideY + y),
                    px(out, kCapeBackX + kCapeBackWidth - 1, kCapeBackY + y), 4);
    }

    // --- 6) Elytra: reuse the cape design with tapered alpha mask ---
    paintElytraFromCape(out);

    return out;
}

} // namespace customcapes
