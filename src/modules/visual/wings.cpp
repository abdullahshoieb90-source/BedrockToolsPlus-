#include <bedrocktools/modules/visual/wings.hpp>

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include "../../config/ConfigManager.hpp"

// STB image implementation is provided by customcapes.cpp in this target; we
// only use the loader declarations here.
#include <stb/stb_image.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace {

using namespace bedrocktools::sdk::offsets;

constexpr std::uint32_t kSkinImageFormat = 4; // mce::ImageFormat::RGBA8Unorm
constexpr int kLoadRetryTicks = 120;
constexpr const char* kFallbackGeometryName = "geometry.wings";
constexpr std::uint32_t kMaxSourceDimension = 4096;

void freeBlobDeleter(unsigned char* data) {
    std::free(data);
}

std::string wingsDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    std::string dir = (lastSlash != std::string::npos) ? configPath.substr(0, lastSlash)
                                                       : "/sdcard/games/BedrockTools";
    return dir + "/wings";
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size <= 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    in.read(out.data(), size);
    return in.good() || in.eof();
}

// Extracts the first geometry identifier out of a modern
// ("minecraft:geometry") or legacy ("geometry") Bedrock skin-pack JSON. The
// lookup is deliberately lightweight (no JSON parser): every skin-pack geometry
// describes its model with a quoted "identifier" key, and that value is all the
// engine needs to select the custom model.
std::string parseGeometryIdentifier(const std::string& geometryData) {
    const std::string key = "\"identifier\"";
    const std::size_t keyPos = geometryData.find(key);
    if (keyPos == std::string::npos) return kFallbackGeometryName;

    std::size_t pos = keyPos + key.size();
    while (pos < geometryData.size() && std::isspace(static_cast<unsigned char>(geometryData[pos]))) {
        ++pos;
    }
    if (pos >= geometryData.size() || geometryData[pos] != ':') return kFallbackGeometryName;
    ++pos;
    while (pos < geometryData.size() && std::isspace(static_cast<unsigned char>(geometryData[pos]))) {
        ++pos;
    }
    if (pos >= geometryData.size() || geometryData[pos] != '"') return kFallbackGeometryName;
    ++pos;

    const std::size_t begin = pos;
    while (pos < geometryData.size() && geometryData[pos] != '"') ++pos;
    if (pos >= geometryData.size()) return kFallbackGeometryName;

    const std::string identifier = geometryData.substr(begin, pos - begin);
    return identifier.empty() ? kFallbackGeometryName : identifier;
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

// The SerializedSkinImpl geometry fields are genuine libc++ std::string
// objects (24 bytes on Android arm64). Rather than relying on the module's
// own std::string ABI matching the engine's, we read and write the exact
// libc++ layout by hand — the same technique CustomCapes uses for the
// short-string cape id.
//
//   short: byte0 = (size << 1) | 0, data[1..size], rest zero (SSO, 22 bytes).
//   long : size_t cap = (cap | 1) @0, size_t size @8, char* data @16.
constexpr std::size_t kSsoCapacity = 22;

bool isLongStdString(uintptr_t addr) {
    return (*reinterpret_cast<const std::uint8_t*>(addr) & 0x01u) != 0;
}

void freeStdString(uintptr_t addr) {
    if (isLongStdString(addr)) {
        void* data = *reinterpret_cast<void**>(addr + 16);
        std::free(data);
    }
}

void writeStdString(uintptr_t addr, const std::string& value) {
    // Free whatever (heap-owning) string the engine currently has in this
    // field so the slot never leaks when we overwrite it.
    freeStdString(addr);

    if (value.size() <= kSsoCapacity) {
        std::memset(reinterpret_cast<void*>(addr), 0, 24);
        *reinterpret_cast<std::uint8_t*>(addr) =
            static_cast<std::uint8_t>(value.size() << 1);
        if (!value.empty()) {
            std::memcpy(reinterpret_cast<void*>(addr + 1), value.data(), value.size());
        }
        return;
    }

    const std::size_t cap = value.size() + 1;
    char* buffer = static_cast<char*>(std::malloc(cap));
    if (!buffer) {
        // Fall back to an empty short string rather than leaving a dangling
        // pointer in the skin. The next tick retries the patch.
        std::memset(reinterpret_cast<void*>(addr), 0, 24);
        return;
    }
    std::memcpy(buffer, value.data(), value.size());
    buffer[value.size()] = '\0';

    *reinterpret_cast<std::size_t*>(addr) = cap | 0x01u;
    *reinterpret_cast<std::size_t*>(addr + 8) = value.size();
    *reinterpret_cast<void**>(addr + 16) = buffer;
}

std::string readStdString(uintptr_t addr) {
    if (!isLongStdString(addr)) {
        const std::size_t len =
            static_cast<std::size_t>(*reinterpret_cast<const std::uint8_t*>(addr) >> 1);
        return std::string(reinterpret_cast<const char*>(addr + 1), len);
    }
    const std::size_t len = *reinterpret_cast<const std::size_t*>(addr + 8);
    const char* data = *reinterpret_cast<const char* const*>(addr + 16);
    return data ? std::string(data, len) : std::string();
}

} // namespace

WingsModule* g_wings = nullptr;

WingsModule::WingsModule()
    : Module("Wings", "Wear the custom wings_geometry.json + wings.png skin pack in the BedrockTools wings folder (local only).") {
    g_wings = this;
}

WingsModule::~WingsModule() {
    if (g_wings == this) g_wings = nullptr;
}

void WingsModule::onInit() {
    m_wingsDir = wingsDirectoryForConfig();

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_wings) g_wings->onLocalPlayerTick(event.player);
        });
}

void WingsModule::onEnable() {
    // Reload whenever the assets are not loaded yet, including after a
    // previous failed attempt — the user may have dropped the files in since.
    m_loadFailed = false;
    m_retryTicks = 0;
    if (!m_assetsLoaded) loadWingsAssets();
    m_needsApply = true;
}

void WingsModule::onDisable() {
    // The skin is restored on the next local-player tick, exactly like
    // CustomCapes, so the restore always runs against a live skin object.
}

void WingsModule::releaseWingsAssets() {
    m_geometryData.clear();
    m_geometryData.shrink_to_fit();
    m_defaultGeometryName.clear();
    m_defaultGeometryName.shrink_to_fit();
    m_texturePixels.clear();
    m_texturePixels.shrink_to_fit();
    m_textureWidth = 0;
    m_textureHeight = 0;
    m_assetsLoaded = false;
    m_loadFailed = false;
    m_retryTicks = 0;
}

void WingsModule::loadWingsAssets() {
    releaseWingsAssets();

    std::error_code ec;
    if (!std::filesystem::is_directory(m_wingsDir, ec)) {
        m_loadFailed = true;
        return;
    }

    const std::string geometryPath = m_wingsDir + "/wings_geometry.json";
    const std::string texturePath = m_wingsDir + "/wings.png";

    if (!readFile(geometryPath, m_geometryData) || m_geometryData.empty()) {
        m_loadFailed = true;
        return;
    }
    m_defaultGeometryName = parseGeometryIdentifier(m_geometryData);
    if (m_defaultGeometryName.empty()) {
        m_loadFailed = true;
        return;
    }

    int width = 0, height = 0, channels = 0;
    stbi_uc* decoded = stbi_load(texturePath.c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0 ||
        width > static_cast<int>(kMaxSourceDimension) ||
        height > static_cast<int>(kMaxSourceDimension)) {
        if (decoded) stbi_image_free(decoded);
        m_loadFailed = true;
        return;
    }

    const std::size_t bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    m_texturePixels.assign(decoded, decoded + bytes);
    stbi_image_free(decoded);

    m_textureWidth = width;
    m_textureHeight = height;
    m_assetsLoaded = true;
    m_needsApply = true;
}

void WingsModule::clearPatchState() {
    m_patchedSkin = nullptr;
    m_injectedBlob = nullptr;
    m_hasBackup = false;
    m_backup = SkinBackup{};
}

void WingsModule::onLocalPlayerTick(void* player) {
    if (!player) return;

    if (enabled && !m_assetsLoaded) {
        if (m_retryTicks <= 0) {
            loadWingsAssets();
            if (!m_assetsLoaded) m_retryTicks = kLoadRetryTicks;
        } else {
            --m_retryTicks;
        }
    }

    void* skin = resolvePlayerSkin(player);

    if (!enabled || !m_assetsLoaded || !skin) {
        restoreOriginalSkin(skin);
        return;
    }

    applyWings(skin);
}

bool WingsModule::applyWings(void* skin) {
    // Persona skins render through the animated-image pipeline and would not
    // pick up the classic skin image we patch here.
    if (*reinterpret_cast<bool*>(
            reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mIsPersona)) {
        return false;
    }

    // Sanity check the live skin image before touching any derived offset. If
    // the layout has shifted, all of the offsets below are unreliable too.
    const uintptr_t skinImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mSkinImage;
    const uint32_t skinW = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth);
    const uint32_t skinH = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight);
    void* skinPx = *reinterpret_cast<void**>(skinImage + Image::mBytesOffset);
    const bool layoutOk = skinPx != nullptr && skinW > 0 && skinH > 0 &&
                          skinW <= 1024 && skinH <= 1024;
    if (!layoutOk) return false;

    const uintptr_t geometryDataAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mGeometryData;
    const uintptr_t geometryNameAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mDefaultGeometryName;

    if (m_patchedSkin != skin) {
        // New skin object: back up its vanilla skin so disable/restore can put
        // everything back before we patch anything. The original pixel blob is
        // only detached, never freed, so restoring the raw pointer is safe.
        m_backup.format = *reinterpret_cast<uint32_t*>(skinImage + Image::mImageFormat);
        m_backup.width = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth);
        m_backup.height = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight);
        m_backup.depth = *reinterpret_cast<uint32_t*>(skinImage + Image::mDepth);
        m_backup.usage = *reinterpret_cast<uint32_t*>(skinImage + Image::mUsage);
        m_backup.blob = *reinterpret_cast<void**>(skinImage + Image::mBytesOffset);
        m_backup.deleter = *reinterpret_cast<void**>(skinImage + Image::mBlobDeleterOffset);
        m_backup.size = *reinterpret_cast<std::size_t*>(skinImage + Image::mBlobSizeOffset);
        m_backup.geometryData = readStdString(geometryDataAddr);
        m_backup.defaultGeometryName = readStdString(geometryNameAddr);

        // The engine owns the blob injected into the previous skin object and
        // frees it through the deleter we tagged; never free it here.
        m_patchedSkin = skin;
        m_injectedBlob = nullptr;
        m_hasBackup = true;
        m_needsApply = true;
    }

    // Patch already in place and untouched? Nothing to do this tick.
    const bool geometryIntact =
        readStdString(geometryDataAddr) == m_geometryData &&
        readStdString(geometryNameAddr) == m_defaultGeometryName;
    if (!m_needsApply && m_injectedBlob != nullptr &&
        *reinterpret_cast<void**>(skinImage + Image::mBytesOffset) == m_injectedBlob &&
        *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth) ==
            static_cast<uint32_t>(m_textureWidth) &&
        *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight) ==
            static_cast<uint32_t>(m_textureHeight) &&
        geometryIntact) {
        return true;
    }

    const std::size_t bytes = m_texturePixels.size();
    void* newBlob = std::malloc(bytes);
    if (!newBlob) return false;
    std::memcpy(newBlob, m_texturePixels.data(), bytes);

    // Point the skin image at the new blob before releasing the previous one so
    // the skin never references freed memory.
    void* previousBlob = m_injectedBlob;
    *reinterpret_cast<uint32_t*>(skinImage + Image::mImageFormat) = kSkinImageFormat;
    *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth) =
        static_cast<uint32_t>(m_textureWidth);
    *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight) =
        static_cast<uint32_t>(m_textureHeight);
    *reinterpret_cast<void**>(skinImage + Image::mBytesOffset) = newBlob;
    *reinterpret_cast<void**>(skinImage + Image::mBlobDeleterOffset) =
        reinterpret_cast<void*>(&freeBlobDeleter);
    *reinterpret_cast<std::size_t*>(skinImage + Image::mBlobSizeOffset) = bytes;
    m_injectedBlob = newBlob;
    if (previousBlob != nullptr) std::free(previousBlob);

    // Make the engine render the custom model instead of the default humanoid.
    writeStdString(geometryDataAddr, m_geometryData);
    writeStdString(geometryNameAddr, m_defaultGeometryName);

    m_needsApply = false;
    return true;
}

void WingsModule::restoreOriginalSkin(void* skin) {
    if (!m_hasBackup || skin == nullptr || skin != m_patchedSkin) {
        // Nothing to restore, or the patched skin object is gone (the engine
        // destroyed it together with our injected blob). Either way the patch
        // state is stale.
        clearPatchState();
        return;
    }

    const uintptr_t skinImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mSkinImage;
    const uintptr_t geometryDataAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mGeometryData;
    const uintptr_t geometryNameAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mDefaultGeometryName;

    // Put the vanilla skin back: dimensions, format, blob metadata and the
    // original pixel pointer before our injected blob is freed, so the skin
    // never references freed memory. The geometry strings are restored through
    // libc++ assignment, which frees our injected heap string if it had one.
    *reinterpret_cast<uint32_t*>(skinImage + Image::mImageFormat) = m_backup.format;
    *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth) = m_backup.width;
    *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight) = m_backup.height;
    *reinterpret_cast<uint32_t*>(skinImage + Image::mDepth) = m_backup.depth;
    *reinterpret_cast<uint32_t*>(skinImage + Image::mUsage) = m_backup.usage;
    *reinterpret_cast<void**>(skinImage + Image::mBlobDeleterOffset) = m_backup.deleter;
    *reinterpret_cast<std::size_t*>(skinImage + Image::mBlobSizeOffset) = m_backup.size;
    *reinterpret_cast<void**>(skinImage + Image::mBytesOffset) = m_backup.blob;

    writeStdString(geometryDataAddr, m_backup.geometryData);
    writeStdString(geometryNameAddr, m_backup.defaultGeometryName);

    if (m_injectedBlob != nullptr) {
        std::free(m_injectedBlob);
        m_injectedBlob = nullptr;
    }

    clearPatchState();
}

void WingsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
}

void WingsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
}
