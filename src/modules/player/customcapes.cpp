#include "customcapes.hpp"
#include "customcapes_files.hpp"

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include "../../config/ConfigManager.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

CustomCapesModule* g_customCapes = nullptr;

namespace {

using namespace bedrocktools::sdk::offsets;

void freeBlobDeleter(unsigned char* data) {
    std::free(data);
}

constexpr std::uint32_t kCapeImageFormat = 4;
constexpr int kLoadRetryTicks = 120;
constexpr const char* kCapeIdBase = "bedrocktools";
constexpr std::size_t kCapeIdBaseLen = 12;

void writeShortStdString(uintptr_t addr, const char* text, std::size_t len) {
    unsigned char* p = reinterpret_cast<unsigned char*>(addr);
    std::memset(p, 0, 24);
    p[0] = static_cast<unsigned char>(len << 1);
    std::memcpy(p + 1, text, len);
}

std::string capeDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    std::string dir = (lastSlash != std::string::npos) ? configPath.substr(0, lastSlash)
                                                       : "/sdcard/games/BedrockTools";
    return dir + "/capes";
}

void* resolvePlayerSkin(void* player) {
    void* skinRefPtr = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(player) + Player::mSkin);
    if (!skinRefPtr) return nullptr;

    void* sharedBase = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(skinRefPtr) + SerializedSkinRef::mSkinImpl);
    if (!sharedBase) return nullptr;

    void* threadOwner = *reinterpret_cast<void**>(sharedBase);
    if (!threadOwner) return nullptr;

    return reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(threadOwner) + ThreadOwner::mObject);
}

} // namespace

CustomCapesModule::CustomCapesModule()
    : Module("Custom Capes", "Wear any PNG from the BedrockTools capes folder as your cape (local only).") {
    g_customCapes = this;
}

CustomCapesModule::~CustomCapesModule() {
    if (g_customCapes == this) g_customCapes = nullptr;
}

void CustomCapesModule::onInit() {
    m_capesDir = capeDirectoryForConfig();
    ensureCapesDirectory();
    m_files = customcapes::scanCapeFiles(m_capesDir);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_customCapes) g_customCapes->onLocalPlayerTick(event.player);
        });
}

void CustomCapesModule::onEnable() {
    if (m_selectedIndex > 0 && !m_capeLoaded && !m_loadFailed) loadSelectedCape();
    m_needsApply = true;
}

void CustomCapesModule::onDisable() {
}

void CustomCapesModule::ensureCapesDirectory() {
    std::error_code ec;
    if (std::filesystem::is_directory(m_capesDir, ec)) return;
    if (!std::filesystem::create_directories(m_capesDir, ec) || ec) return;
    writeSamplePng(m_capesDir + "/Sample Cape.png");
}

void CustomCapesModule::writeSamplePng(const std::string& path) const {
    std::vector<std::uint8_t> src(customcapes::kCapeBackWidth * customcapes::kCapeBackHeight * 4u);
    for (std::uint32_t y = 0; y < customcapes::kCapeBackHeight; ++y) {
        for (std::uint32_t x = 0; x < customcapes::kCapeBackWidth; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * customcapes::kCapeBackWidth + x) * 4u;
            const float t = static_cast<float>(y) / static_cast<float>(customcapes::kCapeBackHeight - 1);
            src[i + 0] = static_cast<std::uint8_t>(70 + 120 * t);
            src[i + 1] = static_cast<std::uint8_t>(30 + 30 * t);
            src[i + 2] = static_cast<std::uint8_t>(160 + 60 * t);
            src[i + 3] = 255;
            const bool border = x < 2 || y < 2 ||
                                x >= customcapes::kCapeBackWidth - 2 ||
                                y >= customcapes::kCapeBackHeight - 2;
            const bool stripe = x == 4 || x == 5;
            if (border || stripe) {
                src[i + 0] = 255; src[i + 1] = 220; src[i + 2] = 60; src[i + 3] = 255;
            }
        }
    }
    const std::vector<std::uint8_t> px = customcapes::resampleToCape(
        src.data(), customcapes::kCapeBackWidth, customcapes::kCapeBackHeight);
    stbi_write_png(path.c_str(), customcapes::kCapeWidth, customcapes::kCapeHeight, 4,
                   px.data(), customcapes::kCapeWidth * 4);
}

void CustomCapesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    const int previousIndex = m_selectedIndex;

    if (j.contains("m_cape")) {
        int parsedIndex = m_selectedIndex;
        std::string parsedName;
        if (j["m_cape"].is_string()) {
            customcapes::parseRadioValue(j["m_cape"].get<std::string>(), parsedIndex, parsedName);
        } else if (j["m_cape"].is_number_integer()) {
            parsedIndex = j["m_cape"].get<int>();
        }
        m_files = customcapes::scanCapeFiles(m_capesDir);
        m_selectedIndex = customcapes::resolveSelectionIndex(parsedIndex, parsedName, m_files);
    }

    if (m_selectedIndex != previousIndex || (m_selectedIndex > 0 && !m_capeLoaded)) {
        releaseLoadedCape();
        if (m_selectedIndex > 0) loadSelectedCape();
        m_needsApply = true;
    }
}

void CustomCapesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    m_files = customcapes::scanCapeFiles(m_capesDir);
    if (m_selectedIndex > static_cast<int>(m_files.size())) m_selectedIndex = 0;
    j["m_cape"] = customcapes::makeRadioValue(m_selectedIndex, m_files);
}

void CustomCapesModule::releaseLoadedCape() {
    m_pixels.clear();
    m_pixels.shrink_to_fit();
    m_capeLoaded = false;
    m_loadFailed = false;
    m_retryTicks = 0;
}

void CustomCapesModule::loadSelectedCape() {
    releaseLoadedCape();
    if (m_selectedIndex <= 0 || m_selectedIndex > static_cast<int>(m_files.size())) return;

    const std::string path = m_capesDir + "/" + m_files[static_cast<std::size_t>(m_selectedIndex - 1)];

    int width = 0, height = 0, channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0 ||
        width > static_cast<int>(customcapes::kMaxSourceDimension) ||
        height > static_cast<int>(customcapes::kMaxSourceDimension)) {
        if (decoded) stbi_image_free(decoded);
        m_loadFailed = true;
        return;
    }

    m_pixels = customcapes::resampleToCape(decoded, static_cast<std::uint32_t>(width),
                                           static_cast<std::uint32_t>(height));
    stbi_image_free(decoded);
    m_capeLoaded = true;

    ++m_capeIdSerial;
    m_activeCapeId = std::string(kCapeIdBase) + "-" + std::to_string(m_capeIdSerial);
    if (m_activeCapeId.size() > 22) {
        m_activeCapeId = std::string(kCapeIdBase) + "-" + std::to_string(m_capeIdSerial % 1000000);
        if (m_activeCapeId.size() > 22) m_activeCapeId.resize(22);
    }
}

void CustomCapesModule::clearPatchState() {
    m_patchedSkin = nullptr;
    m_injectedBlob = nullptr;
    m_hasBackup = false;
    m_backup.pixels.clear();
}

void CustomCapesModule::onLocalPlayerTick(void* player) {
    if (!player) return;

    if (m_selectedIndex > 0 && !m_capeLoaded && enabled) {
        if (m_retryTicks <= 0) {
            loadSelectedCape();
            if (!m_capeLoaded) m_retryTicks = kLoadRetryTicks;
        } else {
            --m_retryTicks;
        }
    }

    void* skin = resolvePlayerSkin(player);

    if (!enabled || m_selectedIndex <= 0 || !m_capeLoaded || !skin) {
        restoreOriginalCape(skin);
        return;
    }

    applyCustomCape(skin);
}

bool CustomCapesModule::applyCustomCape(void* skin) {
    if (*reinterpret_cast<bool*>(
            reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mIsPersona)) {
        return false;
    }

    const uintptr_t skinImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mSkinImage;
    const uint32_t skinW = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth);
    const uint32_t skinH = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight);
    void* skinPx = *reinterpret_cast<void**>(skinImage + Image::mBytesOffset);
    const bool layoutOk = skinPx != nullptr &&
                          (skinW == 64 || skinW == 128) && (skinH == 64 || skinH == 128);
    if (!layoutOk) return false;

    if (m_patchedSkin != skin) {
        m_patchedSkin = skin;
        m_injectedBlob = nullptr;
        m_needsApply = true;
    }
    m_hasBackup = true;

    const std::size_t bytes = m_pixels.size();
    void* newBlob = std::malloc(bytes);
    if (!newBlob) return false;
    std::memcpy(newBlob, m_pixels.data(), bytes);

    if (m_injectedBlob != nullptr) {
        std::free(m_injectedBlob);
    }

    *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth) = customcapes::kCapeWidth;
    *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight) = customcapes::kCapeHeight;
    *reinterpret_cast<void**>(skinImage + Image::mBytesOffset) = newBlob;

    m_injectedBlob = newBlob;

    writeShortStdString(reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mSkinImage,
                        m_activeCapeId.c_str(), m_activeCapeId.size());

    m_needsApply = false;
    return true;
}

void CustomCapesModule::restoreOriginalCape(void* skin) {
    if (!m_hasBackup || skin != m_patchedSkin || skin == nullptr) {
        clearPatchState();
        return;
    }

    if (m_injectedBlob != nullptr) {
        std::free(m_injectedBlob);
        m_injectedBlob = nullptr;
    }

    clearPatchState();
}

