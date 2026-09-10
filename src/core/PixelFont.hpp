#pragma once

// The one TrueType font this package ships, and the id the launcher draws HUD
// text with it.
//
// resources/minecraft.ttf -- the game's own pixel font -- is packed into the
// .levipack (scripts/package_levipack.py) and dropped next to the module as a
// launcher resource, but the launcher does not use it unless a module hands it
// over through pl::modmenu::registerFont. Modules that want their text to look
// like the game's text (and, more importantly, that want to *measure* that
// text) therefore have to register it themselves, and every module that does
// so must not do it twice: this header makes it one call per process.
//
// Why a module that only *draws* text cares about registration at all: the
// width of a label is the one thing the launcher will not report back, and a
// label centered on a guessed width sits off the anchor it belongs to. A module
// can only measure a font whose metrics it knows, so the rule the HUD modules
// share is:
//
//   * text inside the pixel font's coverage -> fontId "minecraft", measured on
//     that font's own advances (the table in esp_geometry.hpp was read out of
//     the shipped file, the way scripts/gen_effect_translations.py derives the
//     per-language glyph-coverage flags Effect Display picks its font with);
//   * everything else (Arabic, Hebrew, CJK, Thai, emoji, ...) -> the launcher's
//     default font, i.e. an empty fontId. That font has the glyphs and does the
//     bidi shaping the pixel font cannot, which is the other half of why the
//     fallback matters: a script a font does not carry comes out as a row of
//     replacement boxes, which reads as extra characters beside the name.
//
// It degrades safely: when the packaged file is not there (a host test, or a
// launcher that took the library but not the resources) the registration fails,
// pixelFontAvailable() reports false, and every module keeps drawing with the
// launcher's default face.
//
// Header-only, with no state of its own beyond the one-shot latch, so the
// modules that use it (Effect Display, Esp) do not need a translation unit.

#include "Runtime.hpp"

#include <pl/ModMenu.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace bedrocktools::core {

// The fontId the launcher was given the packaged font under.
inline constexpr const char* kPixelFontId = "minecraft";

namespace detail {

// Reads resources/minecraft.ttf out of the package's own resource directory and
// registers it. Returns false when the file is missing, unreadable or empty;
// the caller latches on success only, so a module that initializes before the
// runtime knows its resource directory can still pick the font up later.
inline bool registerPixelFont() {
    const auto& directory = Runtime::get().resourceDirectory();
    if (directory.empty()) return false;

    std::ifstream file(directory / "minecraft.ttf", std::ios::binary);
    if (!file) return false;

    std::vector<unsigned char> bytes{(std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>()};
    if (bytes.empty()) return false;

    return pl::modmenu::registerFont(kPixelFontId, bytes);
}

} // namespace detail

// True once the launcher has the font. Every HUD text command that wants the
// pixel font has to check this, because a fontId the launcher never got does
// not fail loudly -- it just draws something else.
//
// A failed attempt is not latched, so a module that initialized before the
// runtime knew its resource directory can still pick the font up on a later
// call. That makes this a *startup* query, not a per-frame one: the modules
// sample it once (in onInit) and keep the answer, because a frame that asks
// again would open the file again.
inline bool pixelFontAvailable() {
    static std::atomic<bool> registered{false};
    if (registered.load(std::memory_order_relaxed)) return true;
    if (!detail::registerPixelFont()) return false;
    registered.store(true, std::memory_order_relaxed);
    return true;
}

} // namespace bedrocktools::core
