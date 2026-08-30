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
// mce::Image::mDepth is 1 for every 2D texture. The engine derives the
// texture description and the pixel-byte count (width * height * depth *
// bytesPerPixel) from this field, so a cape image left at depth 0 is
// rejected by the texture factory and never reaches the screen.
constexpr std::uint32_t kCapeImageDepth = 1;
constexpr int kLoadRetryTicks = 120;
constexpr const char* kCapeIdBase = "bedrocktoolsplus";
constexpr std::size_t kCapeIdBaseLen = 16;

void writeShortStdString(uintptr_t addr, const char* text, std::size_t len) {
    if (len > 22) return; // libc++ short-string capacity inside 24 bytes
    unsigned char* p = reinterpret_cast<unsigned char*>(addr);
    std::memset(p, 0, 24);
    p[0] = static_cast<unsigned char>(len << 1);
    std::memcpy(p + 1, text, len);
}

bool shortStdStringEquals(uintptr_t addr, const char* text, std::size_t len) {
    if (len > 22) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    return p[0] == static_cast<unsigned char>(len << 1) &&
           std::memcmp(p + 1, text, len) == 0;
}

bool shortStdStringHasPrefix(uintptr_t addr, const char* prefix, std::size_t prefixLen) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    // We only write short strings (<= 22 bytes). If the low bit is set this is
    // a libc++ long string, which is not one of our synthetic ids.
    if ((p[0] & 1u) != 0u) return false;
    const std::size_t len = static_cast<std::size_t>(p[0] >> 1);
    return len >= prefixLen && len <= 22 && std::memcmp(p + 1, prefix, prefixLen) == 0;
}

bool isPlausibleCapeImage(uintptr_t capeImage) {
    const std::uint32_t format = *reinterpret_cast<std::uint32_t*>(capeImage + Image::mImageFormat);
    const std::uint32_t width = *reinterpret_cast<std::uint32_t*>(capeImage + SkinImage::mWidth);
    const std::uint32_t height = *reinterpret_cast<std::uint32_t*>(capeImage + SkinImage::mHeight);
    const std::uint32_t depth = *reinterpret_cast<std::uint32_t*>(capeImage + Image::mDepth);
    const std::uint32_t usage = *reinterpret_cast<std::uint32_t*>(capeImage + Image::mUsage);
    void* blob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    const std::size_t size = *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset);

    const bool emptyCape = width == 0 && height == 0 && blob == nullptr && size == 0;
    const bool classicCape = ((width == customcapes::kCapeWidth && height == customcapes::kCapeHeight) ||
                              (width == customcapes::kCapeWidth * 2 && height == customcapes::kCapeHeight * 2)) &&
                             blob != nullptr && size >= static_cast<std::size_t>(width) * height * 4u;

    // ImageUsage is an enum stored in the low byte. Values above this small
    // range usually mean the offset is no longer an mce::Image and touching it
    // would corrupt the skin (observed as a Steve fallback or a skin-change
    // crash on shifted game layouts).
    return (emptyCape || classicCape) && (format == 0 || format == kCapeImageFormat) &&
           (depth == 0 || depth == 1) && usage <= 4;
}

std::string capeDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    std::string dir = (lastSlash != std::string::npos) ? configPath.substr(0, lastSlash)
                                                       : "/sdcard/games/BedrockToolsPlus";
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
    : Module("Custom Capes", "Wear any PNG from the BedrockToolsPlus capes folder as your cape (local only).") {
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

void CustomCapesModule::backupOriginalCape(std::uintptr_t capeImage, std::uintptr_t capeIdAddr) {
    m_backup.format = *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat);
    m_backup.width = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth);
    m_backup.height = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight);
    m_backup.depth = *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth);
    m_backup.usage = *reinterpret_cast<uint32_t*>(capeImage + Image::mUsage);
    m_backup.blob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    m_backup.deleter = *reinterpret_cast<void**>(capeImage + Image::mBlobDeleterOffset);
    m_backup.size = *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset);
    std::memcpy(m_backup.capeIdBytes, reinterpret_cast<const void*>(capeIdAddr),
                sizeof(m_backup.capeIdBytes));

    // If the game rebuilt the skin in-place after our previous patch, it can
    // temporarily leave the synthetic cape id behind while replacing the image
    // blob. Treat that id as ours, not as the new skin's vanilla cape id, so
    // disabling the module will not restore an invalid custom id and force the
    // client back to Steve.
    if (shortStdStringHasPrefix(capeIdAddr, kCapeIdBase, kCapeIdBaseLen)) {
        std::memset(m_backup.capeIdBytes, 0, sizeof(m_backup.capeIdBytes));
    }

    m_hasBackup = true;
}

void CustomCapesModule::clearPatchState() {
    m_patchedSkin = nullptr;
    m_injectedBlob = nullptr;
    m_hasBackup = false;
    m_backup = CapeBackup{};
}

bool CustomCapesModule::playerHasLiveLevel(const void* player) {
    if (!player) return false;
    // Actor::mLevel is the first link the engine severs on Leave World,
    // before the player and its skin objects are destroyed. A live level
    // link means the skin object this player owns is still live.
    const void* level = *reinterpret_cast<const void* const*>(
        reinterpret_cast<std::uintptr_t>(player) + Actor::mLevel);
    return level != nullptr;
}

void CustomCapesModule::onWorldExit() {
    // The level is gone, so the player and its skin are gone or on their
    // way out. Two hard rules:
    //   1. Never write to the skin here. Restoring the vanilla cape into a
    //      freed SerializedSkinImpl is the use-after-free that crashed the
    //      game on Leave World; restoreOriginalCape() only runs while the
    //      level is live (see onLocalPlayerTick).
    //   2. Never free m_injectedBlob here. The blob was handed to the skin
    //      tagged with freeBlobDeleter, so the engine frees it while it
    //      destroys the skin — freeing it again would double-free.
    // Dropping our references is all that is needed: m_pixels (the cape
    // file) is module-owned and stays loaded, so the cape is re-applied to
    // the fresh skin object when the player joins a world again.
    m_patchedSkin = nullptr;
    m_injectedBlob = nullptr; // ownership: engine (frees via the deleter tag)
    m_hasBackup = false;
    m_backup = CapeBackup{};
    m_needsApply = true; // next live skin must be (re)patched from scratch
}

void CustomCapesModule::onLocalPlayerTick(void* player) {
    // --- World-exit guard (Leave World crash fix) -----------------------
    // While the engine tears down the world it (1) detaches the player
    // from its level and then (2) destroys the player and its skin. Any
    // tick that reaches us after step 1 must not read or write the skin
    // object: it is freed memory, and writing the vanilla cape back into
    // it — or freeing our injected blob a second time — is the access
    // violation / double-free that crashed the game on Leave World.
    if (!player || !playerHasLiveLevel(player)) {
        onWorldExit();
        return;
    }

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

    // Layout sanity check on the verified skin-image offset: if the live
    // skin texture is not where we expect it, every derived offset
    // (mCapeImage, mCapeId) is unreliable too, so abort the patch.
    const uintptr_t skinImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mSkinImage;
    const uint32_t skinW = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth);
    const uint32_t skinH = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight);
    void* skinPx = *reinterpret_cast<void**>(skinImage + Image::mBytesOffset);
    const bool layoutOk = skinPx != nullptr &&
                          (skinW == 64 || skinW == 128) && (skinH == 64 || skinH == 128);
    if (!layoutOk) return false;

    // The cape pixels go into mCapeImage and the synthetic id into mCapeId —
    // never into mSkinImage (that is the player's skin texture; touching it
    // makes the game fall back to the default Steve skin).
    const uintptr_t capeImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeImage;
    const uintptr_t capeIdAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeId;

    if (!isPlausibleCapeImage(capeImage)) {
        return false;
    }

    // Patch already in place and untouched? Nothing to do this tick.
    const bool idIntact = shortStdStringEquals(capeIdAddr, m_activeCapeId.c_str(),
                                               m_activeCapeId.size());
    void* const liveBlob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    const bool liveUsesOurBlob = m_injectedBlob != nullptr && liveBlob == m_injectedBlob;

    if (m_patchedSkin == skin && m_hasBackup) {
        if (!m_needsApply && liveUsesOurBlob &&
            *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth) == customcapes::kCapeWidth &&
            idIntact) {
            return true;
        }

        if (m_injectedBlob != nullptr && !liveUsesOurBlob) {
            // Skin changes can rebuild SerializedSkinImpl in-place: the pointer
            // remains equal, but the game has already detached or freed our old
            // blob and installed a new vanilla cape. Do not free the stale
            // pointer, and take a fresh backup of the new skin before patching.
            m_injectedBlob = nullptr;
            backupOriginalCape(capeImage, capeIdAddr);
            m_needsApply = true;
        }
    } else {
        // New skin object: back up its original cape so that "None"/disable
        // can bring the vanilla cape back before we patch anything. The
        // original pixel blob is only detached, never freed, so restoring
        // the raw pointer is safe.
        //
        // The engine owns the blob we injected into the previous skin object
        // and frees it through the deleter tag we set — do not free it here.
        m_patchedSkin = skin;
        m_injectedBlob = nullptr;
        backupOriginalCape(capeImage, capeIdAddr);
        m_needsApply = true;
    }

    const std::size_t bytes = m_pixels.size();
    void* newBlob = std::malloc(bytes);
    if (!newBlob) return false;
    std::memcpy(newBlob, m_pixels.data(), bytes);

    // Point the cape image at the new blob before releasing the previous
    // one so the skin never references freed memory. Only free the old blob
    // when the live skin still points at it; if the game changed skins in
    // place it may already have freed that pointer.
    void* previousBlob = liveUsesOurBlob ? m_injectedBlob : nullptr;

    // Describe a texture the engine can actually upload. It builds the cape
    // texture from this image's own fields, so every one of them has to say
    // "64x32 RGBA8, depth 1": a player without any cape carries a
    // default-constructed mCapeImage whose format/depth/usage are all 0, and
    // a depth-0 image has a computed size of w*h*0*4 = 0 bytes — the texture
    // factory drops it and the cape is silently never drawn. Depth is always
    // 1 for a 2D texture; the image usage is inherited from the player's own
    // skin texture (an image the engine already renders) whenever the cape
    // image does not carry one of its own.
    const std::uint32_t skinUsage =
        *reinterpret_cast<const uint32_t*>(skinImage + Image::mUsage);
    const std::uint32_t capeUsage =
        *reinterpret_cast<const uint32_t*>(capeImage + Image::mUsage);

    *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat) = kCapeImageFormat;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth) = customcapes::kCapeWidth;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight) = customcapes::kCapeHeight;
    *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth) = kCapeImageDepth;
    if (capeUsage == 0 && skinUsage <= 4) {
        *reinterpret_cast<uint32_t*>(capeImage + Image::mUsage) = skinUsage;
    }
    *reinterpret_cast<void**>(capeImage + Image::mBytesOffset) = newBlob;
    *reinterpret_cast<void**>(capeImage + Image::mBlobDeleterOffset) =
        reinterpret_cast<void*>(&freeBlobDeleter);
    *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset) = bytes;
    m_injectedBlob = newBlob;
    if (previousBlob != nullptr) std::free(previousBlob);

    writeShortStdString(capeIdAddr, m_activeCapeId.c_str(), m_activeCapeId.size());

    m_needsApply = false;
    return true;
}

void CustomCapesModule::restoreOriginalCape(void* skin) {
    if (!m_hasBackup || skin == nullptr || skin != m_patchedSkin) {
        // Either nothing to restore or the patched skin object is gone (the
        // engine destroyed it together with our injected blob).
        clearPatchState();
        return;
    }

    const uintptr_t capeImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeImage;
    const uintptr_t capeIdAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeId;

    void* const liveBlob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    if (m_injectedBlob != nullptr && liveBlob != m_injectedBlob) {
        // The game rebuilt the skin in-place before our disable/None restore
        // tick. Our backup belongs to the old appearance and our blob pointer
        // may already have been released by the engine, so restoring/freeing it
        // here would corrupt the new skin or double-free. Just detach state.
        clearPatchState();
        return;
    }

    // Put the vanilla cape back: dimensions, format, blob metadata, the
    // original pixel pointer and the original cape id. The blob pointer is
    // restored before our injected blob is freed, so the skin never
    // references freed memory.
    *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat) = m_backup.format;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth) = m_backup.width;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight) = m_backup.height;
    *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth) = m_backup.depth;
    *reinterpret_cast<uint32_t*>(capeImage + Image::mUsage) = m_backup.usage;
    *reinterpret_cast<void**>(capeImage + Image::mBlobDeleterOffset) = m_backup.deleter;
    *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset) = m_backup.size;
    *reinterpret_cast<void**>(capeImage + Image::mBytesOffset) = m_backup.blob;
    std::memcpy(reinterpret_cast<void*>(capeIdAddr), m_backup.capeIdBytes,
                sizeof(m_backup.capeIdBytes));

    if (m_injectedBlob != nullptr) {
        std::free(m_injectedBlob);
        m_injectedBlob = nullptr;
    }

    clearPatchState();
}

