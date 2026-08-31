#include "customcapes.hpp"

#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include "../../config/ConfigManager.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace bedrocktools::sdk::offsets;

// Deleter handed to the engine through the mce::Blob deleter slot. The engine
// calls it (exactly once) when it destroys the image we installed, releasing
// the malloc'd pixel buffer. It must stay address-stable, so it lives at
// namespace scope.
void capePixelsDeleter(unsigned char* pixels) {
    std::free(pixels);
}

// Simple two-tone default cape (64x32) written into an empty capes folder so
// the selector always has at least one entry. Users can delete it.
void writeDefaultCape(const std::string& path) {
    constexpr int kWidth = 64;
    constexpr int kHeight = 32;
    std::vector<unsigned char> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4, 255);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const bool stripe = ((x / 8) + (y / 8)) % 2 == 0;
            unsigned char* p = &rgba[(static_cast<std::size_t>(y) * kWidth + x) * 4];
            if (stripe) {
                p[0] = 52; p[1] = 56; p[2] = 78;   // slate blue
            } else {
                p[0] = 34; p[1] = 40; p[2] = 56;   // darker slate
            }
            p[3] = 255;
        }
    }
    stbi_write_png(path.c_str(), kWidth, kHeight, 4, rgba.data(), kWidth * 4);
}

const char* kCapesReadme =
    "Custom Capes - put your cape PNGs in this folder.\n"
    "\n"
    "Any .png file here becomes selectable in the Custom Capes module\n"
    "(BedrockToolsPlus -> Custom Capes -> Cape).\n"
    "\n"
    "Tips:\n"
    "  * Classic (non-persona) skins only - persona skins ignore mCapeImage.\n"
    "  * 64x32 or 128x64 PNGs with full alpha work best.\n"
    "  * After adding a new cape use the \"Refresh Capes\" button in the\n"
    "    module, or toggle the module off and on again. New entries show up\n"
    "    in the selector after restarting the launcher.\n"
    "  * The cape is client-side: other players still see your original cape.\n"
    "\n"
    "The default.png in this folder is generated on first run and can be\n"
    "deleted (it is only written when the folder is empty).\n";

} // namespace

CustomCapesModule::CustomCapesModule()
    : Module("Custom Capes",
             "Swap your cape for your own PNG capes. Drop PNG files into the "
             "capes folder next to config.json (created automatically), pick "
             "one in the Cape selector and it replaces your character's cape. "
             "Classic skins only; the cape is client-side.") {
    // The module has no HUD surface of its own; all settings live in the
    // module menu (Cape selector + Refresh Capes button).
    hideInHudEditor = true;
}

CustomCapesModule::~CustomCapesModule() {
    if (m_originalPixels) std::free(m_originalPixels);
    m_originalPixels = nullptr;
}

void CustomCapesModule::onInit() {
    scanCapesDirectory();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onLocalPlayerTick(event.player); });
}

void CustomCapesModule::onEnable() {
    // Re-scan so capes dropped while the module was off are picked up without
    // a restart (the new entries also become visible after the next restart).
    scanCapesDirectory();
    m_needApply = true;
}

void CustomCapesModule::onDisable() {
    restoreOriginalCape(m_lastPlayer);
    m_lastInstalledBlob = nullptr;
    m_needApply = true; // re-apply on next enable
}

void CustomCapesModule::onLocalPlayerTick(void* player) {
    if (!enabled || !player) return;
    applyCapeToPlayer(player);
}

void CustomCapesModule::scanCapesDirectory() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    const std::string configDir = (lastSlash != std::string::npos)
        ? configPath.substr(0, lastSlash)
        : "/sdcard/games/BedrockToolsPlus";
    m_capesDir = configDir + "/capes";

    std::error_code ec;
    std::filesystem::create_directories(m_capesDir, ec);
    if (ec) {
        m_capeNames.clear();
        m_selectedIndex = -1;
        m_selectedName.clear();
        return;
    }

    std::vector<std::string> ids;
    for (std::filesystem::directory_iterator it(m_capesDir, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".png") continue;

        std::string stem = it->path().stem().string();
        if (stem.empty() || stem[0] == '.') continue;
        // Commas would break the launcher's radio format ("idx,opt1,opt2,...")
        // and the id -> file lookup ("<id>.png"), so such files are skipped.
        if (stem.find(',') != std::string::npos) continue;
        ids.push_back(std::move(stem));
    }
    std::sort(ids.begin(), ids.end());

    // Seed an empty folder so the selector always has at least one cape.
    const std::string readmePath = m_capesDir + "/README.txt";
    if (!std::filesystem::exists(readmePath, ec)) {
        std::ofstream out(readmePath, std::ios::binary | std::ios::trunc);
        if (out) out.write(kCapesReadme, static_cast<std::streamsize>(std::strlen(kCapesReadme)));
    }
    if (ids.empty()) {
        const std::string defaultPath = m_capesDir + "/default.png";
        if (!std::filesystem::exists(defaultPath, ec)) writeDefaultCape(defaultPath);
        if (std::filesystem::exists(defaultPath, ec)) ids.push_back("default");
    }

    m_capeNames = std::move(ids);

    // Keep the current selection when it still exists; otherwise fall back.
    if (m_capeNames.empty()) {
        m_selectedIndex = -1;
        m_selectedName.clear();
        return;
    }
    const auto it = std::find(m_capeNames.begin(), m_capeNames.end(), m_selectedName);
    if (it != m_capeNames.end()) {
        m_selectedIndex = static_cast<int>(it - m_capeNames.begin());
    } else {
        m_selectedIndex = 0;
        m_selectedName = m_capeNames[0];
    }
}

void CustomCapesModule::parseCapeValue(const std::string& value) {
    if (value.empty()) return;

    std::string head = value;
    const std::size_t comma = value.find(',');
    if (comma != std::string::npos) head = value.substr(0, comma);

    bool numeric = !head.empty();
    for (char c : head) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            numeric = false;
            break;
        }
    }

    if (numeric) {
        int idx = 0;
        try {
            idx = std::stoi(head);
        } catch (...) {
            idx = -1;
        }
        if (idx < 0 || idx >= static_cast<int>(m_capeNames.size())) {
            idx = m_capeNames.empty() ? -1 : 0;
        }
        m_selectedIndex = idx;
        m_selectedName = (idx >= 0) ? m_capeNames[idx] : "";
    } else {
        const auto it = std::find(m_capeNames.begin(), m_capeNames.end(), head);
        if (it != m_capeNames.end()) {
            m_selectedIndex = static_cast<int>(it - m_capeNames.begin());
            m_selectedName = *it;
        } else {
            m_selectedIndex = m_capeNames.empty() ? -1 : 0;
            m_selectedName = m_capeNames.empty() ? "" : m_capeNames[0];
        }
    }
    m_needApply = true;
}

void CustomCapesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_cape") && j["m_cape"].is_string()) {
        parseCapeValue(j["m_cape"].get<std::string>());
    } else if (j.contains("cape") && j["cape"].is_string()) {
        parseCapeValue(j["cape"].get<std::string>());
    }
    if (j.contains("refreshButton") && j["refreshButton"].is_boolean() &&
        j["refreshButton"].get<bool>()) {
        // The launcher fires this when the "Refresh Capes" button is tapped.
        scanCapesDirectory();
        m_needApply = true;
    }
}

void CustomCapesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    // Launcher radio format: "<selectedIndex>,<id1>,<id2>,...".
    std::string value = std::to_string(std::max(m_selectedIndex, 0));
    for (const auto& name : m_capeNames) {
        value += ',';
        value += name;
    }
    j["m_cape"] = value;
    // Boolean key whose name contains "button" -> launcher renders a Button
    // and sends "true" when tapped (handled in loadConfig above).
    j["refreshButton"] = false;
}

bool CustomCapesModule::loadCapePixels(const std::string& id, unsigned char** outPixels,
                                       int* outWidth, int* outHeight) {
    if (!outPixels || !outWidth || !outHeight || id.empty()) return false;
    const std::string path = m_capesDir + "/" + id + ".png";
    int components = 0;
    *outPixels = stbi_load(path.c_str(), outWidth, outHeight, &components, 4);
    return *outPixels != nullptr;
}

void CustomCapesModule::backupOriginalImage(void* skinImpl) {
    if (m_hasOriginal || !skinImpl) return;
    const std::uintptr_t image =
        reinterpret_cast<std::uintptr_t>(skinImpl) + SerializedSkinImpl::mCapeImage;

    void* blob = *reinterpret_cast<void**>(image + Image::mBytesOffset);
    const std::size_t size = *reinterpret_cast<std::size_t*>(image + Image::mBlobSizeOffset);

    m_originalWidth = static_cast<int>(*reinterpret_cast<std::uint32_t*>(image + SkinImage::mWidth));
    m_originalHeight = static_cast<int>(*reinterpret_cast<std::uint32_t*>(image + SkinImage::mHeight));
    m_originalSize = static_cast<int>(size);
    m_originalFormat = *reinterpret_cast<std::uint32_t*>(image + Image::mImageFormat);
    m_originalDepth = *reinterpret_cast<std::uint32_t*>(image + Image::mDepth);
    m_originalUsage = *reinterpret_cast<unsigned char*>(image + Image::mUsage);

    if (blob && size > 0) {
        m_originalPixels = static_cast<unsigned char*>(std::malloc(size));
        if (m_originalPixels) std::memcpy(m_originalPixels, blob, size);
    }
    m_hasOriginal = true;
}

void CustomCapesModule::installCapeImage(void* skinImpl, unsigned char* pixels,
                                         int width, int height) {
    const std::uintptr_t image =
        reinterpret_cast<std::uintptr_t>(skinImpl) + SerializedSkinImpl::mCapeImage;

    // Release the image the engine currently owns before overwriting it.
    void* oldBlob = *reinterpret_cast<void**>(image + Image::mBytesOffset);
    void (*oldDeleter)(unsigned char*) =
        *reinterpret_cast<void (**)(unsigned char*)>(image + Image::mBlobDeleterOffset);
    if (oldBlob && oldDeleter) oldDeleter(static_cast<unsigned char*>(oldBlob));

    // RGBA8Unorm, sRGB, depth 1: identical to what vanilla cape images carry.
    *reinterpret_cast<std::uint32_t*>(image + Image::mImageFormat) = 3;
    *reinterpret_cast<std::uint32_t*>(image + SkinImage::mWidth) = static_cast<std::uint32_t>(width);
    *reinterpret_cast<std::uint32_t*>(image + SkinImage::mHeight) = static_cast<std::uint32_t>(height);
    *reinterpret_cast<std::uint32_t*>(image + Image::mDepth) = 1; // 0 images are rejected
    *reinterpret_cast<unsigned char*>(image + Image::mUsage) = 1;  // SRGB
    *reinterpret_cast<void**>(image + Image::mBytesOffset) = pixels;
    *reinterpret_cast<void (**)(unsigned char*)>(image + Image::mBlobDeleterOffset) =
        &capePixelsDeleter;
    *reinterpret_cast<std::size_t*>(image + Image::mBlobSizeOffset) =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
}

bool CustomCapesModule::applyCapeToPlayer(void* player) {
    if (!player || m_selectedName.empty() || m_capeNames.empty()) return false;
    m_lastPlayer = player;

    void* skinRef = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(player) + Player::mSkin);
    if (!skinRef) return false;
    void* skinImpl = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(skinRef) + SerializedSkinRef::mSkinImpl);
    if (!skinImpl) return false;

    const std::uintptr_t image =
        reinterpret_cast<std::uintptr_t>(skinImpl) + SerializedSkinImpl::mCapeImage;
    void* currentBlob = *reinterpret_cast<void**>(image + Image::mBytesOffset);
    // Fast path: our cape is already installed on this skin and nothing
    // changed (covers every respawn tick; the engine only replaces the blob
    // when the skin itself is rebuilt).
    if (!m_needApply && currentBlob == m_lastInstalledBlob) return false;

    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    if (!loadCapePixels(m_selectedName, &pixels, &width, &height)) return false;
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }

    // Back up the original cape before the engine's blob is released.
    if (!m_hasOriginal) backupOriginalImage(skinImpl);

    const std::size_t size = static_cast<std::size_t>(width) * height * 4u;
    unsigned char* owned = static_cast<unsigned char*>(std::malloc(size));
    if (!owned) {
        stbi_image_free(pixels);
        return false;
    }
    std::memcpy(owned, pixels, size);
    stbi_image_free(pixels);

    installCapeImage(skinImpl, owned, width, height);
    m_lastInstalledBlob = owned;
    m_needApply = false;
    return true;
}

void CustomCapesModule::restoreOriginalCape(void* player) {
    if (!player || !m_hasOriginal) return;

    void* skinRef = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(player) + Player::mSkin);
    if (!skinRef) return;
    void* skinImpl = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(skinRef) + SerializedSkinRef::mSkinImpl);
    if (!skinImpl) return;

    const std::uintptr_t image =
        reinterpret_cast<std::uintptr_t>(skinImpl) + SerializedSkinImpl::mCapeImage;

    void* oldBlob = *reinterpret_cast<void**>(image + Image::mBytesOffset);
    // Safety: if the skin no longer holds the blob we installed (the game
    // rebuilt the skin since our last apply), touching anything here would
    // free memory owned by the engine. Leave the skin alone.
    if (m_lastInstalledBlob && oldBlob != m_lastInstalledBlob) return;
    void (*oldDeleter)(unsigned char*) =
        *reinterpret_cast<void (**)(unsigned char*)>(image + Image::mBlobDeleterOffset);
    if (oldBlob && oldDeleter) oldDeleter(static_cast<unsigned char*>(oldBlob));

    unsigned char* pixels = nullptr;
    if (m_originalSize > 0 && m_originalPixels) {
        pixels = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(m_originalSize)));
        if (pixels) std::memcpy(pixels, m_originalPixels, static_cast<std::size_t>(m_originalSize));
    }

    *reinterpret_cast<std::uint32_t*>(image + Image::mImageFormat) = m_originalFormat;
    *reinterpret_cast<std::uint32_t*>(image + SkinImage::mWidth) = static_cast<std::uint32_t>(m_originalWidth);
    *reinterpret_cast<std::uint32_t*>(image + SkinImage::mHeight) = static_cast<std::uint32_t>(m_originalHeight);
    *reinterpret_cast<std::uint32_t*>(image + Image::mDepth) = m_originalDepth;
    *reinterpret_cast<unsigned char*>(image + Image::mUsage) = m_originalUsage;
    *reinterpret_cast<void**>(image + Image::mBytesOffset) = pixels;
    *reinterpret_cast<void (**)(unsigned char*)>(image + Image::mBlobDeleterOffset) =
        pixels ? &capePixelsDeleter : nullptr;
    *reinterpret_cast<std::size_t*>(image + Image::mBlobSizeOffset) =
        pixels ? static_cast<std::size_t>(m_originalSize) : 0;

    m_lastInstalledBlob = nullptr;
}

void CustomCapesModule::freeCapePixels(unsigned char* pixels) {
    std::free(pixels);
}
