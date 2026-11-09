#pragma once

// Pure helpers for the Custom Capes module.
//
// Everything in this header is plain C++ with no Minecraft, launcher or
// mod-menu dependencies so it can be unit-tested on the host (see
// tests/customcapes_test.cpp). It covers the "dumb data" problems of the
// module:
//
//   * scanning the capes directory for usable PNG files, and validating a
//     file's PNG header before the decoder ever sees it
//   * (de)serializing the launcher "radio" config value used for the picker,
//     including friendly and collision-free display labels
//   * fitting and resampling an arbitrary RGBA image into the visible
//     classic-cape face, the lining, the thickness strips and the Elytra UV
//     area of the 64x32 canvas Minecraft expects
//   * reducing very large sources before they reach the cape-face sampler

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

// --- PNG validation -------------------------------------------------------
// The picker lists files by extension, but an extension proves nothing: a
// .png that is really a JPEG, a truncated download or a 0-byte placeholder
// would still show up as an option and then silently fail to load. Every file
// is therefore checked against the PNG signature and its IHDR before it is
// offered. Reading the header first also means an oversized image is rejected
// *before* the decoder allocates w*h*4 bytes, which is what used to let a
// 4096x4096 PNG (64 MB decoded) exhaust memory on a phone.

inline constexpr std::uint32_t kMaxSourcePixels = 2048u * 2048u;

// Bit depth / colour type combinations a PNG may legally use. Anything else is
// a corrupt header, not a file worth handing to the decoder.
inline bool isLegalPngDepthAndColorType(std::uint8_t bitDepth, std::uint8_t colorType) {
    switch (colorType) {
        case 0: // greyscale
        case 3: // palette
            return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8;
        case 2: // truecolour
        case 4: // greyscale + alpha
        case 6: // truecolour + alpha
            return bitDepth == 8 || bitDepth == 16;
        default:
            return false;
    }
}

// Width/height of a PNG file read from its IHDR chunk, without decoding it.
inline bool readPngHeader(const std::uint8_t* data, std::size_t size,
                          std::uint32_t& outWidth, std::uint32_t& outHeight) {
    static constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    outWidth = 0;
    outHeight = 0;
    if (!data || size < 33) return false;
    if (std::memcmp(data, kSignature, sizeof(kSignature)) != 0) return false;
    // 8 signature + 4 chunk length + 4 "IHDR" + 13 data bytes.
    if (std::memcmp(data + 12, "IHDR", 4) != 0) return false;
    const std::uint32_t width = (static_cast<std::uint32_t>(data[16]) << 24) |
                                (static_cast<std::uint32_t>(data[17]) << 16) |
                                (static_cast<std::uint32_t>(data[18]) << 8) |
                                static_cast<std::uint32_t>(data[19]);
    const std::uint32_t height = (static_cast<std::uint32_t>(data[20]) << 24) |
                                 (static_cast<std::uint32_t>(data[21]) << 16) |
                                 (static_cast<std::uint32_t>(data[22]) << 8) |
                                 static_cast<std::uint32_t>(data[23]);
    const std::uint8_t bitDepth = data[24];
    const std::uint8_t colorType = data[25];
    if (width == 0 || height == 0) return false;
    if (!isLegalPngDepthAndColorType(bitDepth, colorType)) return false;
    outWidth = width;
    outHeight = height;
    return true;
}

// True when an image of this size is small enough to decode safely and worth
// decoding at all (the cape face is 10x16 texels).
inline bool isSupportedSourceSize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (width > kMaxSourceDimension || height > kMaxSourceDimension) return false;
    return static_cast<std::uint64_t>(width) * height <= kMaxSourcePixels;
}

// Declared early: the prefilter below and sampleFace() later both route
// through it, and it is defined with the rest of the sampling code.
inline void boxSample(const std::uint8_t* rgba, std::uint32_t W, std::uint32_t H,
                      double x0, double y0, double x1, double y1, std::uint8_t out[4]);

// --- large-image prefilter ------------------------------------------------
// The area-average sampler walks every source texel that overlaps a
// destination texel, so it is linear in the source size for each of the 160
// destination texels. On a 2048x2048 source (16 MB) that is a few hundred
// million texel reads, which visibly stalls the client tick on a phone.
// Sources above kDirectSampleDimension are therefore reduced once with the
// exact area downscale below, and the cape face is then sampled from the (at
// most 1024-pixel) reduced image. On the host that pass takes ~34 ms for
// 2048x2048 with -O2, versus several hundred ms in one tick without it.

inline constexpr std::uint32_t kDirectSampleDimension = 1024;

inline bool sourceNeedsPrefilter(std::uint32_t width, std::uint32_t height) {
    return width > kDirectSampleDimension || height > kDirectSampleDimension;
}

// Largest size with the same aspect ratio, capped on its longer side.
inline void prefilterSize(std::uint32_t width, std::uint32_t height,
                          std::uint32_t& outWidth, std::uint32_t& outHeight) {
    if (width == 0 || height == 0) {
        outWidth = outHeight = 1;
        return;
    }
    if (width >= height) {
        outWidth = std::min(width, kDirectSampleDimension);
        outHeight = std::max<std::uint32_t>(
            1u, static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(height) * outWidth + width / 2) / width));
    } else {
        outHeight = std::min(height, kDirectSampleDimension);
        outWidth = std::max<std::uint32_t>(
            1u, static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(width) * outHeight + height / 2) / height));
    }
}

// Exact area-average downscale (alpha-weighted, same math as boxSample).
// Returns an empty vector when the request is degenerate, so the caller keeps
// using the original image.
inline std::vector<std::uint8_t> downscaleArea(const std::uint8_t* rgba,
                                               std::uint32_t width, std::uint32_t height,
                                               std::uint32_t targetWidth,
                                               std::uint32_t targetHeight) {
    std::vector<std::uint8_t> out;
    if (!rgba || width == 0 || height == 0 || targetWidth == 0 || targetHeight == 0) return out;
    out.resize(static_cast<std::size_t>(targetWidth) * targetHeight * 4u, 0);

    const double scaleX = static_cast<double>(width) / static_cast<double>(targetWidth);
    const double scaleY = static_cast<double>(height) / static_cast<double>(targetHeight);
    for (std::uint32_t y = 0; y < targetHeight; ++y) {
        const double sy0 = static_cast<double>(y) * scaleY;
        const double sy1 = static_cast<double>(y + 1) * scaleY;
        for (std::uint32_t x = 0; x < targetWidth; ++x) {
            const double sx0 = static_cast<double>(x) * scaleX;
            const double sx1 = static_cast<double>(x + 1) * scaleX;
            boxSample(rgba, width, height, sx0, sy0, sx1, sy1,
                      &out[(static_cast<std::size_t>(y) * targetWidth + x) * 4u]);
        }
    }
    return out;
}

// --- picker labels --------------------------------------------------------
// The picker shows the file name without its ".png" suffix, which is what the
// user typed in their file manager. Two files whose names differ only by the
// extension case ("Cape.png" / "Cape.PNG") would collide, so a colliding label
// falls back to the exact file name.

inline std::string stripPngExtension(const std::string& name) {
    if (!hasPngExtension(name)) return name;
    return name.substr(0, name.size() - 4);
}

// Builds one display label per file, in the same order as `files`, keeping
// every label unique (the launcher radio list is comma separated and a
// duplicate label would make the selection ambiguous).
inline std::vector<std::string> makeCapeLabels(const std::vector<std::string>& files) {
    std::vector<std::string> labels;
    labels.reserve(files.size());
    for (const std::string& file : files) {
        std::string label = stripPngExtension(file);
        if (label.empty()) label = file;
        bool duplicate = false;
        for (const std::string& used : labels) {
            if (used == label) { duplicate = true; break; }
        }
        labels.push_back(duplicate ? file : label);
    }
    return labels;
}

// Serializes the picker value in the launcher's radio format:
// "<currentIndex>,<None>,<file1>,<file2>,..." — the menu renders the part
// after the first comma as the option list and treats the part before it as
// the selected index (same convention the Crosshair module uses).
inline std::string makeRadioValue(int selectedIndex, const std::vector<std::string>& files,
                                  const std::vector<std::string>& labels);

inline std::string makeRadioValue(int selectedIndex, const std::vector<std::string>& files) {
    return makeRadioValue(selectedIndex, files, makeCapeLabels(files));
}

// Same, with explicit display labels (one per file). A label list of the wrong
// size is ignored and the file names are used instead.
inline std::string makeRadioValue(int selectedIndex, const std::vector<std::string>& files,
                                  const std::vector<std::string>& labels) {
    const bool useLabels = labels.size() == files.size();
    const int optionCount = 1 + static_cast<int>(files.size());
    if (selectedIndex < 0) selectedIndex = 0;
    if (selectedIndex >= optionCount) selectedIndex = optionCount - 1;

    std::string value = std::to_string(selectedIndex);
    value += ',';
    value += kNoneLabel;
    for (std::size_t i = 0; i < files.size(); ++i) {
        value += ',';
        value += useLabels ? labels[i] : files[i];
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
                                 const std::vector<std::string>& files,
                                 const std::vector<std::string>& labels);

inline int resolveSelectionIndex(int parsedIndex, const std::string& parsedName,
                                 const std::vector<std::string>& files) {
    return resolveSelectionIndex(parsedIndex, parsedName, files, makeCapeLabels(files));
}

// As above, but the recovered option text may be either a display label
// ("My Cape") or a raw file name ("My Cape.png"), so both are matched. The
// name always wins over the numeric index: the list is re-scanned on every
// save, and a file added or removed above the selection would otherwise
// silently shift the user onto a different cape.
inline int resolveSelectionIndex(int parsedIndex, const std::string& parsedName,
                                 const std::vector<std::string>& files,
                                 const std::vector<std::string>& labels) {
    const bool useLabels = labels.size() == files.size();
    if (!parsedName.empty() && parsedName != kNoneLabel) {
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i] == parsedName) return static_cast<int>(i) + 1;
        }
        if (useLabels) {
            for (std::size_t i = 0; i < labels.size(); ++i) {
                if (labels[i] == parsedName) return static_cast<int>(i) + 1;
            }
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
// Fit modes — how an arbitrary image is laid onto the 10x16 back face
// ---------------------------------------------------------------
// The cape back face is a 10:16 (0.625) portrait, while most artwork is
// square, landscape or a different portrait ratio. Scaling both axes
// independently (the old behavior) distorts faces and text, so the fit mode
// decides what happens instead:
//
//   Cover    fill the face completely and crop the overflow. No distortion,
//            but the image's outer edges are lost — the module default,
//            because a cape with empty bars looks broken in game.
//   Contain  keep the whole design at its own aspect ratio and pad the
//            leftover with the image's edge pixels.
//   Stretch  scale both axes independently (distortion, legacy behavior).
enum class FitMode : int {
    Cover = 0,
    Contain = 1,
    Stretch = 2,
};

inline constexpr int kFitModeCount = 3;

inline FitMode fitModeFromInt(int value) {
    if (value < 0 || value >= kFitModeCount) return FitMode::Cover;
    return static_cast<FitMode>(value);
}

inline const char* fitModeLabel(FitMode mode) {
    switch (mode) {
        case FitMode::Contain: return "Contain";
        case FitMode::Stretch: return "Stretch";
        case FitMode::Cover: break;
    }
    return "Cover";
}

// The source region mapped onto the destination face, in source pixel
// coordinates. For Contain the region is *larger* than the image on one axis;
// the sampler clamps outside coordinates to the nearest edge texel, which is
// exactly the padding.
struct CropRect {
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
};

inline CropRect computeCrop(std::uint32_t width, std::uint32_t height,
                            std::uint32_t targetW, std::uint32_t targetH,
                            FitMode mode) {
    CropRect crop;
    if (width == 0 || height == 0 || targetW == 0 || targetH == 0) return crop;

    const double srcW = static_cast<double>(width);
    const double srcH = static_cast<double>(height);
    const double targetAspect = static_cast<double>(targetW) / static_cast<double>(targetH);
    const double srcAspect = srcW / srcH;

    if (mode == FitMode::Stretch) {
        crop.width = srcW;
        crop.height = srcH;
        return crop;
    }

    if (mode == FitMode::Contain) {
        // The whole image must stay visible, so the sampled region is the
        // source grown until it has the target aspect ratio.
        if (srcAspect > targetAspect) {
            crop.width = srcW;
            crop.height = srcW / targetAspect;
        } else {
            crop.width = srcH * targetAspect;
            crop.height = srcH;
        }
        crop.x = (srcW - crop.width) * 0.5;
        crop.y = (srcH - crop.height) * 0.5;
        return crop;
    }

    // Cover: the largest centred region that already has the target aspect.
    if (srcAspect > targetAspect) {
        crop.height = srcH;
        crop.width = srcH * targetAspect;
        crop.x = (srcW - crop.width) * 0.5;
    } else {
        crop.width = srcW;
        crop.height = srcW / targetAspect;
        crop.y = (srcH - crop.height) * 0.5;
    }

    // A crop thinner than one texel would sample a sliver of the artwork and
    // throw the rest away; for tiny images keep the full axis instead (the
    // sampler upsamples it, which looks far better than a 0.4-pixel strip).
    if (crop.width < 1.0 || crop.width <= 0.0) {
        crop.width = srcW;
        crop.x = 0.0;
    }
    if (crop.height < 1.0 || crop.height <= 0.0) {
        crop.height = srcH;
        crop.y = 0.0;
    }
    return crop;
}

// ---------------------------------------------------------------
// Image sampling
// ---------------------------------------------------------------
// The cape face is only 10x16 texels, so nearly every source is shrunk.
// Reading a single bilinear sample per destination pixel aliased badly: a
// 736x797 photo turned into noise, and thin details (text, logo outlines)
// either vanished or shimmered. boxSample() averages every source texel that
// overlaps the destination pixel, which is an exact area average when
// downscaling and a smooth blend when upscaling. Colors are averaged
// premultiplied by alpha, so a transparent border cannot bleed black into a
// logo.

inline void bilinearSample(const std::uint8_t* rgba, std::uint32_t W, std::uint32_t H,
                           double fx, double fy, std::uint8_t out[4]) {
    if (!rgba || W == 0 || H == 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
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
        double value = (1.0 - wy) * top + wy * bottom;
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        out[c] = static_cast<std::uint8_t>(value + 0.5);
    }
}

// Area-averages the source region [x0,x1) x [y0,y1) into one RGBA texel.
// Region edges that only partly cover a source texel weight it proportionally
// (so a 0.4-texel strip is not rounded up to a full texel), and coordinates
// outside the image are treated as the nearest edge texel.
inline void boxSample(const std::uint8_t* rgba, std::uint32_t W, std::uint32_t H,
                      double x0, double y0, double x1, double y1,
                      std::uint8_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!rgba || W == 0 || H == 0) return;
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);

    const auto clampRect = [](double& a0, double& a1, std::uint32_t limit) {
        const double hi = static_cast<double>(limit);
        if (a0 < 0.0) a0 = 0.0;
        if (a0 > hi) a0 = hi;
        if (a1 < 0.0) a1 = 0.0;
        if (a1 > hi) a1 = hi;
    };
    clampRect(x0, x1, W);
    clampRect(y0, y1, H);

    // Region entirely outside the image (Contain padding) or too thin to have
    // area: clamp to the nearest edge texel.
    if (x1 - x0 <= 1e-9 || y1 - y0 <= 1e-9) {
        bilinearSample(rgba, W, H, (x0 + x1) * 0.5 - 0.5, (y0 + y1) * 0.5 - 0.5, out);
        return;
    }

    const std::uint32_t ix0 = static_cast<std::uint32_t>(std::floor(x0));
    const std::uint32_t iy0 = static_cast<std::uint32_t>(std::floor(y0));
    const std::uint32_t ix1 = std::min(W, static_cast<std::uint32_t>(std::ceil(x1)));
    const std::uint32_t iy1 = std::min(H, static_cast<std::uint32_t>(std::ceil(y1)));

    double acc[4] = {0.0, 0.0, 0.0, 0.0};
    double totalWeight = 0.0;
    for (std::uint32_t y = iy0; y < iy1; ++y) {
        const double cy0 = std::max(y0, static_cast<double>(y));
        const double cy1 = std::min(y1, static_cast<double>(y) + 1.0);
        const double wy = cy1 - cy0;
        if (wy <= 0.0) continue;
        for (std::uint32_t x = ix0; x < ix1; ++x) {
            const double cx0 = std::max(x0, static_cast<double>(x));
            const double cx1 = std::min(x1, static_cast<double>(x) + 1.0);
            const double wx = cx1 - cx0;
            if (wx <= 0.0) continue;
            const double weight = wx * wy;
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4u;
            const double alpha = static_cast<double>(rgba[i + 3]) / 255.0;
            acc[0] += rgba[i + 0] * alpha * weight;
            acc[1] += rgba[i + 1] * alpha * weight;
            acc[2] += rgba[i + 2] * alpha * weight;
            acc[3] += rgba[i + 3] * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight <= 0.0) {
        bilinearSample(rgba, W, H, (x0 + x1) * 0.5 - 0.5, (y0 + y1) * 0.5 - 0.5, out);
        return;
    }

    const double alphaAvg = acc[3] / totalWeight;
    const double alphaNorm = alphaAvg / 255.0;
    for (int c = 0; c < 3; ++c) {
        double value = 0.0;
        if (alphaNorm > 1e-6) value = (acc[c] / totalWeight) / alphaNorm;
        if (value < 0.0) value = 0.0;
        if (value > 255.0) value = 255.0;
        out[c] = static_cast<std::uint8_t>(value + 0.5);
    }
    out[3] = static_cast<std::uint8_t>(alphaAvg + 0.5);
}

// Samples one destination texel that covers [x0,x1) x [y0,y1) of the source.
//
// Minification (the common case: a photo onto a 10x16 face) uses the area
// average, which is what keeps large sources from turning into aliasing noise.
// Magnification (a tiny 2x2 or 10x16 source) has no area to average, and box
// averaging would just repeat texels; a bilinear tap at the coverage centre
// keeps small images smooth instead of blocky.
inline void sampleFace(const std::uint8_t* rgba, std::uint32_t W, std::uint32_t H,
                       double x0, double y0, double x1, double y1,
                       std::uint8_t out[4]) {
    const bool magnifying = (x1 - x0) <= 1.0 && (y1 - y0) <= 1.0;
    if (magnifying) {
        // Centre of the destination texel in source *texel* space. Clamping
        // to the image keeps the bilinear tap (and Contain padding) sane.
        const double fx = (x0 + x1) * 0.5 - 0.5;
        const double fy = (y0 + y1) * 0.5 - 0.5;
        bilinearSample(rgba, W, H, fx, fy, out);
        return;
    }
    boxSample(rgba, W, H, x0, y0, x1, y1, out);
}

// ---------------------------------------------------------------
// Resampler — any image -> the 64x32 classic-cape canvas
// ---------------------------------------------------------------
// Pipeline (identical for every source size, so an image looks the same
// whether it was authored at 22x23 or 736x797):
//
//   1) Map the source onto the outer BACK face (1,1) 10x16 using the fit
//      mode and the area-average sampler.
//   2) Fill the inner FRONT face (12,1) with one flat lining colour derived
//      from the back face — this is what is seen as the cape swings away.
//   3) Continue the image's edge colours onto the top/bottom/side strips so
//      the one-voxel-thick cuboid does not read as a paper-thin sheet.
//   4) Generate the Elytra wing UV (22,0) from the same design, clipped to
//      the vanilla tapered silhouette.
//
// A complete, manually authored 64x32 canvas is passed through untouched
// (pixel-perfect) unless its Elytra strip is empty and needs a fallback.

// Lining brightness relative to the average design colour: the inner face is
// the design's average colour at half brightness, which reads as a lining
// instead of a second design.
inline constexpr double kLiningScale = 0.5;

inline std::vector<std::uint8_t> resampleToCapeEx(const std::uint8_t* rgba,
                                                  std::uint32_t width,
                                                  std::uint32_t height,
                                                  FitMode mode = FitMode::Cover) {
    std::vector<std::uint8_t> out(
        static_cast<std::size_t>(kCapeWidth) * kCapeHeight * 4u, 0);
    if (!rgba || width == 0 || height == 0) return out;

    // Exact 64x32 canvas: keep it pixel-perfect, and only synthesize wings
    // when the author did not draw any.
    if (width == kCapeWidth && height == kCapeHeight) {
        out.assign(rgba, rgba + out.size());
        if (!hasElytraArtwork(out)) paintElytraFromCape(out);
        return out;
    }

    const auto px = [](std::vector<std::uint8_t>& canvas, std::uint32_t x,
                       std::uint32_t y) -> std::uint8_t* {
        return &canvas[(static_cast<std::size_t>(y) * kCapeWidth + x) * 4u];
    };

    const CropRect crop = computeCrop(width, height, kCapeBackWidth, kCapeBackHeight, mode);

    // --- 1) Outer back face ---
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        const double sy0 = crop.y + static_cast<double>(y) * crop.height /
                                        static_cast<double>(kCapeBackHeight);
        const double sy1 = crop.y + static_cast<double>(y + 1) * crop.height /
                                        static_cast<double>(kCapeBackHeight);
        for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
            const double sx0 = crop.x + static_cast<double>(x) * crop.width /
                                            static_cast<double>(kCapeBackWidth);
            const double sx1 = crop.x + static_cast<double>(x + 1) * crop.width /
                                            static_cast<double>(kCapeBackWidth);
            sampleFace(rgba, width, height, sx0, sy0, sx1, sy1,
                       px(out, kCapeBackX + x, kCapeBackY + y));
        }
    }

    // --- 2) Inner front face: one flat lining colour ---
    double sum[3] = {0.0, 0.0, 0.0};
    double weightSum = 0.0;
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
            const std::uint8_t* p = px(out, kCapeBackX + x, kCapeBackY + y);
            const double alpha = static_cast<double>(p[3]) / 255.0;
            for (int c = 0; c < 3; ++c) sum[c] += static_cast<double>(p[c]) * alpha;
            weightSum += alpha;
        }
    }
    std::uint8_t liner[4] = {0, 0, 0, 255};
    if (weightSum > 1e-6) {
        for (int c = 0; c < 3; ++c) {
            const double value = std::min(255.0, std::max(0.0, (sum[c] / weightSum) * kLiningScale));
            liner[c] = static_cast<std::uint8_t>(value + 0.5);
        }
    }
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
            std::uint8_t* p = px(out, kCapeFrontX + x, kCapeFrontY + y);
            std::memcpy(p, liner, 4);
        }
    }

    // --- 3) Thickness: edge strips continue the image's edge colours ---
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

    // --- 4) Elytra wings ---
    paintElytraFromCape(out);

    return out;
}

// Backwards-compatible entry point (Cover fit), used by the module and tests.
inline std::vector<std::uint8_t> resampleToCape(const std::uint8_t* rgba, std::uint32_t width,
                                                std::uint32_t height) {
    return resampleToCapeEx(rgba, width, height, FitMode::Cover);
}

} // namespace customcapes
