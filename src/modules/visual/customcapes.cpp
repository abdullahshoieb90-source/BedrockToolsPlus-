#include "customcapes.hpp"

#include "config/ConfigManager.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb/stb_image.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_set>

namespace {

CustomCapesModule* g_customCapes = nullptr;
std::mutex g_blobMutex;
std::unordered_set<void*> g_liveBlobs;

std::string capesDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t slash = configPath.find_last_of('/');
    const std::string parent = slash == std::string::npos
        ? "/sdcard/games/BedrockToolsPlus" : configPath.substr(0, slash);
    return parent + "/capes";
}

void freeImageBlob(unsigned char* pixels) {
    // The game owns the Blob after injection and may destroy it while
    // replacing a skin. Remove it from our retired-allocation list first so
    // module teardown can never free the same buffer a second time.
    {
        std::lock_guard lock(g_blobMutex);
        g_liveBlobs.erase(pixels);
    }
    std::free(pixels);
}

void writeEmptyImage(unsigned char* image) {
    std::memset(image, 0, bedrocktools::sdk::offsets::Image::Size);
}

void writeRgbaImage(unsigned char* image, int width, int height, void* pixels, std::size_t byteCount) {
    using namespace bedrocktools::sdk::offsets;
    writeEmptyImage(image);
    *reinterpret_cast<std::uint32_t*>(image + Image::mImageFormat) = 3; // RGBA8Unorm
    *reinterpret_cast<std::uint32_t*>(image + Image::mWidth) = static_cast<std::uint32_t>(width);
    *reinterpret_cast<std::uint32_t*>(image + Image::mHeight) = static_cast<std::uint32_t>(height);
    *reinterpret_cast<std::uint32_t*>(image + Image::mDepth) = 1;
    *reinterpret_cast<unsigned char*>(image + Image::mUsage) = 1; // SRGB
    *reinterpret_cast<void**>(image + Image::mBytesOffset) = pixels;
    *reinterpret_cast<void (**)(unsigned char*)>(image + Image::mBlobDeleterOffset) = freeImageBlob;
    *reinterpret_cast<std::size_t*>(image + Image::mBlobSizeOffset) = byteCount;
}

void* skinImplForPlayer(void* player) {
    if (reinterpret_cast<std::uintptr_t>(player) < 0x1000) return nullptr;
    void* serializedSkin = reinterpret_cast<bedrocktools::sdk::Player*>(player)->skin();
    if (reinterpret_cast<std::uintptr_t>(serializedSkin) < 0x1000) return nullptr;
    void* impl = *reinterpret_cast<void**>(serializedSkin);
    return reinterpret_cast<std::uintptr_t>(impl) >= 0x1000 ? impl : nullptr;
}

constexpr const char* kReadme =
    "BedrockToolsPlus - Custom Capes\n"
    "================================\n\n"
    "Put PNG cape images in this folder, then restart the game.\n"
    "Open Mod Menu -> Custom Capes and choose the file in the Cape selector.\n\n"
    "Default Cape: restores the cape supplied by your current Minecraft skin.\n"
    "No Cape: hides your cape while the module is enabled.\n\n"
    "Recommended image size: 64x32 pixels (or a larger 2:1 image such as 128x64).\n"
    "PNG transparency is supported. Other image sizes are accepted, but may look stretched.\n"
    "File names containing commas are ignored because the menu cannot represent them.\n"
    "The cape is local-only: other players do not receive your custom image.\n";

} // namespace

CustomCapesModule::CustomCapesModule()
    : Module("Custom Capes", "Choose a PNG cape from the capes folder next to config.json. Put 64x32 PNG files in the folder and restart the game; every file appears in the Cape selector. Default Cape restores your skin's cape, No Cape hides it. Local-only.") {
    g_customCapes = this;
    showInMenu = true;
    hideInHudEditor = true;
}

CustomCapesModule::~CustomCapesModule() {
    restoreAppliedCape();
    // Buffers replaced by us stay alive until teardown because Bedrock may
    // still have an asynchronous texture upload referring to one. Buffers
    // already destroyed by the game were removed by freeImageBlob above.
    std::lock_guard lock(g_blobMutex);
    for (void* pixels : g_liveBlobs) std::free(pixels);
    g_liveBlobs.clear();
    if (g_customCapes == this) g_customCapes = nullptr;
}

void CustomCapesModule::onInit() {
    m_dir = capesDirectoryForConfig();
    ensureCapesDirectory();
    refreshFiles();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_customCapes) g_customCapes->onLocalPlayerTick(event.player);
        });
}

void CustomCapesModule::ensureCapesDirectory() {
    if (m_dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    if (ec) return;
    const std::string readme = m_dir + "/README.txt";
    if (!std::filesystem::exists(readme, ec)) {
        std::ofstream out(readme, std::ios::binary);
        out << kReadme;
    }
}

void CustomCapesModule::refreshFiles() {
    m_files = customcapes::scanCapeFiles(m_dir);
    if (m_selectedId != customcapes::kDefaultId && m_selectedId != customcapes::kNoneId &&
        std::find(m_files.begin(), m_files.end(), m_selectedId) == m_files.end()) {
        m_selectedId = customcapes::kDefaultId;
        m_selectedIndex = 0;
        m_dirty = true;
    }
}

void CustomCapesModule::select(const customcapes::Selection& selection) {
    m_selectedIndex = selection.index;
    m_selectedId = selection.id;
    m_dirty = true;
}

std::string CustomCapesModule::selectedPath() const {
    if (m_selectedIndex < 2 || m_selectedId.empty()) return {};
    if (std::find(m_files.begin(), m_files.end(), m_selectedId) == m_files.end()) return {};
    return m_dir + "/" + m_selectedId;
}

bool CustomCapesModule::loadSelectedCape() {
    if (m_loadedId == m_selectedId) return m_loadedPixels.valid();
    m_loadedId = m_selectedId;
    m_loadedPixels = {};

    const std::string path = selectedPath();
    if (path.empty()) return false;

    int width = 0, height = 0, channels = 0;
    if (!stbi_info(path.c_str(), &width, &height, &channels) ||
        width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        return false;
    }
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pixelCount > std::numeric_limits<std::size_t>::max() / 4) return false;

    unsigned char* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!decoded) return false;
    m_loadedPixels.width = width;
    m_loadedPixels.height = height;
    m_loadedPixels.rgba.assign(decoded, decoded + pixelCount * 4);
    stbi_image_free(decoded);
    return m_loadedPixels.valid();
}

void CustomCapesModule::onEnable() {
    refreshFiles();
    m_dirty = true;
}

void CustomCapesModule::onDisable() {
    restoreAppliedCape();
    clearTrackedCape();
}

void CustomCapesModule::clearTrackedCape() {
    m_appliedSkinImpl = nullptr;
    m_installedBlob = nullptr;
    m_originalImage.fill(0);
    m_originalCapeId.clear();
}

void CustomCapesModule::restoreAppliedCape() {
    using namespace bedrocktools::sdk::offsets;
    if (reinterpret_cast<std::uintptr_t>(m_appliedSkinImpl) < 0x1000) return;
    auto* impl = static_cast<unsigned char*>(m_appliedSkinImpl);
    std::memcpy(impl + SerializedSkinImpl::mCapeImage, m_originalImage.data(), Image::Size);
    auto* capeId = reinterpret_cast<std::string*>(impl + SerializedSkinImpl::mCapeId);
    *capeId = m_originalCapeId;
}

void CustomCapesModule::applyImage(void* skinImpl) {
    using namespace bedrocktools::sdk::offsets;
    auto* impl = static_cast<unsigned char*>(skinImpl);
    auto* image = impl + SerializedSkinImpl::mCapeImage;
    auto* capeId = reinterpret_cast<std::string*>(impl + SerializedSkinImpl::mCapeId);

    if (m_selectedIndex == 1) {
        writeEmptyImage(image);
        capeId->clear();
        m_installedBlob = nullptr;
        return;
    }

    if (!loadSelectedCape()) {
        // A missing/corrupt PNG must not leave the player with a blank or
        // half-written mce::Image. Keep the skin's original cape instead.
        restoreAppliedCape();
        m_installedBlob = nullptr;
        return;
    }

    const std::size_t byteCount = m_loadedPixels.rgba.size();
    void* pixels = std::malloc(byteCount);
    if (!pixels) {
        restoreAppliedCape();
        m_installedBlob = nullptr;
        return;
    }
    std::memcpy(pixels, m_loadedPixels.rgba.data(), byteCount);
    {
        std::lock_guard lock(g_blobMutex);
        g_liveBlobs.insert(pixels);
    }
    writeRgbaImage(image, m_loadedPixels.width, m_loadedPixels.height, pixels, byteCount);
    *capeId = "bedrocktoolsplus.custom_cape." + m_selectedId;
    m_installedBlob = pixels;
}

void CustomCapesModule::applyToPlayer(void* player) {
    using namespace bedrocktools::sdk::offsets;
    // The default option is a true no-op until a custom cape was applied.
    // In particular, do not dereference skin pointers during world loading
    // merely to restore a cape that this module never replaced.
    if (m_selectedIndex == 0 && !m_appliedSkinImpl) return;
    void* impl = skinImplForPlayer(player);
    if (!impl) return;

    if (impl != m_appliedSkinImpl) {
        // A world/skin change gives us a new owner. Do not write through the
        // previous pointer: it may already have been destroyed by the game.
        m_appliedSkinImpl = impl;
        auto* bytes = static_cast<unsigned char*>(impl);
        std::memcpy(m_originalImage.data(), bytes + SerializedSkinImpl::mCapeImage, Image::Size);
        m_originalCapeId = *reinterpret_cast<std::string*>(bytes + SerializedSkinImpl::mCapeId);
        m_installedBlob = nullptr;
        m_dirty = true;
    }

    auto* bytes = static_cast<unsigned char*>(impl);
    void* currentBlob = *reinterpret_cast<void**>(bytes + SerializedSkinImpl::mCapeImage + Image::mBytesOffset);
    const auto* currentCapeId = reinterpret_cast<const std::string*>(bytes + SerializedSkinImpl::mCapeId);
    const bool customWasReplaced = m_selectedIndex >= 2 && m_installedBlob && currentBlob != m_installedBlob;
    const bool hiddenCapeWasRestored = m_selectedIndex == 1 && (currentBlob || !currentCapeId->empty());
    if (!m_dirty && (customWasReplaced || hiddenCapeWasRestored)) {
        // Minecraft refreshed the SerializedSkin under us (for example after
        // a dimension change). Capture the new vanilla cape before replacing
        // it again, rather than restoring bytes from the previous skin.
        std::memcpy(m_originalImage.data(), bytes + SerializedSkinImpl::mCapeImage, Image::Size);
        m_originalCapeId = *currentCapeId;
        m_dirty = true;
    }

    if (!m_dirty) return;
    if (m_selectedIndex == 0) restoreAppliedCape();
    else applyImage(impl);
    m_installedBlob = m_selectedIndex == 0 ? nullptr : m_installedBlob;
    m_dirty = false;
}

void CustomCapesModule::onLocalPlayerTick(void* player) {
    if (!enabled) return;
    applyToPlayer(player);
}

void CustomCapesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_cape") && j["m_cape"].is_string()) {
        select(customcapes::parseSelection(j["m_cape"].get<std::string>(), m_files));
    } else if (j.contains("cape") && j["cape"].is_string()) {
        select(customcapes::parseSelection(j["cape"].get<std::string>(), m_files));
    }
}

void CustomCapesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    refreshFiles();
    const customcapes::Selection selection = customcapes::selectionForId(m_selectedId, m_files);
    m_selectedIndex = selection.index;
    m_selectedId = selection.id;
    j["m_cape"] = customcapes::makeRadioValue(m_selectedIndex, m_files);
}
