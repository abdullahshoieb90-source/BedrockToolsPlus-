#include <bedrocktools/modules/visual/wings.hpp>
#include <bedrocktools/modules/visual/wings_default.hpp>

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include "../../config/ConfigManager.hpp"

// STB image implementation is provided by customcapes.cpp in this target; we
// only use the loader declarations here.
#include <stb/stb_image.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
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

// ---------------------------------------------------------------------------
// Geometry bone-rotation helper.
//
// Skin-pack geometry carries each bone's resting transform as a static JSON
// array, e.g. `"rotation": [0.0, 0.0, 20.0]` (euler degrees, X/Y/Z). The wing
// animation rewrites that array for wingRight / wingLeft every tick, then
// re-injects the resulting mGeometryData into the skin. The rewrite is plain
// text so it works on the embedded default pack as well as any user-supplied
// wings_geometry.json, without depending on a JSON runtime.
// ---------------------------------------------------------------------------

constexpr const char* kWingRightBone = "wingRight";
constexpr const char* kWingLeftBone = "wingLeft";
constexpr float kFlapAmplitudeDegrees = 35.0f;
constexpr float kFlapBaseRate = 6.0f; // radians/second at m_flapSpeed == 1.0

std::size_t skipWhitespace(const std::string& s, std::size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    return pos;
}

bool readQuotedString(const std::string& s, std::size_t quotePos, std::string& out) {
    if (quotePos >= s.size() || s[quotePos] != '"') return false;
    std::size_t pos = quotePos + 1;
    const std::size_t begin = pos;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\') ++pos;
        if (pos < s.size()) ++pos;
    }
    if (pos >= s.size()) return false;
    out = s.substr(begin, pos - begin);
    return true;
}

std::size_t findClosingBracket(const std::string& s, std::size_t openPos) {
    if (openPos >= s.size() || (s[openPos] != '[' && s[openPos] != '{')) return std::string::npos;
    const char open = s[openPos];
    const char close = open == '[' ? ']' : '}';
    int depth = 0;
    for (std::size_t i = openPos; i < s.size(); ++i) {
        if (s[i] == open) ++depth;
        else if (s[i] == close) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

std::size_t findEnclosingObjectStart(const std::string& s, std::size_t beforePos) {
    // Walk backwards from beforePos matching braces so nested arrays/objects
    // before the bone's "name" key cannot confuse the search.
    int depth = 0;
    for (std::size_t i = beforePos; i > 0; --i) {
        const char c = s[i - 1];
        if (c == '}') {
            ++depth;
        } else if (c == '{') {
            if (depth == 0) return i - 1;
            --depth;
        }
    }
    return std::string::npos;
}

std::string formatRotation(float x, float y, float z) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "[%.3f,%.3f,%.3f]", x, y, z);
    return std::string(buffer);
}

// Finds the first bone object whose "name" value equals boneName and rewrites
// its "rotation" array (inserting one if the bone has none). Returns false if
// the bone cannot be located — the caller then keeps the base geometry intact.
bool setBoneRotation(std::string& geometry, const std::string& boneName,
                     float x, float y, float z) {
    const std::string nameKey = "\"name\"";
    std::size_t searchFrom = 0;
    while (searchFrom < geometry.size()) {
        const std::size_t keyPos = geometry.find(nameKey, searchFrom);
        if (keyPos == std::string::npos) break;
        std::size_t colon = skipWhitespace(geometry, keyPos + nameKey.size());
        if (colon < geometry.size() && geometry[colon] == ':') {
            const std::size_t valueQuote = skipWhitespace(geometry, colon + 1);
            std::string found;
            if (valueQuote < geometry.size() && readQuotedString(geometry, valueQuote, found) &&
                found == boneName) {
                const std::size_t objStart = findEnclosingObjectStart(geometry, keyPos);
                const std::size_t objEnd = findClosingBracket(geometry, objStart);
                if (objStart == std::string::npos || objEnd == std::string::npos) return false;

                const std::string rotationKey = "\"rotation\"";
                const std::size_t rotKey = geometry.find(rotationKey, objStart);
                if (rotKey != std::string::npos && rotKey < objEnd) {
                    const std::size_t rotColon = skipWhitespace(geometry, rotKey + rotationKey.size());
                    const std::size_t bracketOpen = skipWhitespace(geometry, rotColon + 1);
                    const std::size_t bracketClose = findClosingBracket(geometry, bracketOpen);
                    if (bracketOpen < geometry.size() && geometry[bracketOpen] == '[' &&
                        bracketClose != std::string::npos) {
                        geometry.replace(bracketOpen, bracketClose - bracketOpen + 1,
                                         formatRotation(x, y, z));
                        return true;
                    }
                    return false; // malformed rotation value — do not corrupt JSON
                }

                const bool objectEmpty = geometry[objEnd - 1] == '{';
                geometry.insert(objEnd, std::string(objectEmpty ? "" : ",") +
                                            "\"rotation\":" + formatRotation(x, y, z));
                return true;
            }
        }
        searchFrom = keyPos + nameKey.size();
    }
    return false;
}

} // namespace

WingsModule* g_wings = nullptr;

WingsModule::WingsModule()
    : Module("Wings", "Wear the built-in wings skin pack (or your wings_geometry.json + wings.png in the BedrockTools wings folder).") {
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
    // Re-resolve on every enable: external files in the wings folder take
    // priority, and the built-in defaults are the fallback when they are not
    // present. This also picks up files the user dropped in while the module
    // was running the default pack. The flap clock restarts from zero so the
    // wings start in the neutral pose.
    m_loadFailed = false;
    m_retryTicks = 0;
    loadWingsAssets();
    m_flapTime = 0.0f;
    m_flapClockStarted = false;
    m_needsApply = true;
}

void WingsModule::onDisable() {
    // The skin is restored on the next local-player tick, exactly like
    // CustomCapes, so the restore always runs against a live skin object.
}

void WingsModule::releaseWingsAssets() {
    m_geometryData.clear();
    m_geometryData.shrink_to_fit();
    m_geometryTemplate.clear();
    m_geometryTemplate.shrink_to_fit();
    m_defaultGeometryName.clear();
    m_defaultGeometryName.shrink_to_fit();
    m_texturePixels.clear();
    m_texturePixels.shrink_to_fit();
    m_textureWidth = 0;
    m_textureHeight = 0;
    m_assetsLoaded = false;
    m_useDefaults = false;
    m_loadFailed = false;
    m_retryTicks = 0;
    m_flapTime = 0.0f;
    m_flapClockStarted = false;
}

// Loads the embedded default pack into the module-owned buffers.
void WingsModule::loadDefaultAssets() {
    m_geometryTemplate = wings_default::GeometryJson;
    m_geometryData = m_geometryTemplate;
    m_defaultGeometryName = wings_default::GeometryIdentifier;

    const std::size_t bytes = wings_default::TextureWidth * wings_default::TextureHeight * 4u;
    m_texturePixels.assign(wings_default::TexturePixels,
                           wings_default::TexturePixels + bytes);
    m_textureWidth = static_cast<int>(wings_default::TextureWidth);
    m_textureHeight = static_cast<int>(wings_default::TextureHeight);
    m_assetsLoaded = true;
    m_useDefaults = true;
    m_loadFailed = false;
    m_needsApply = true;
}

void WingsModule::loadWingsAssets() {
    releaseWingsAssets();

    std::error_code ec;
    if (!std::filesystem::is_directory(m_wingsDir, ec)) {
        // No user-supplied wings folder: use the built-in pack straight away,
        // so the feature works immediately after install.
        loadDefaultAssets();
        return;
    }

    const std::string geometryPath = m_wingsDir + "/wings_geometry.json";
    const std::string texturePath = m_wingsDir + "/wings.png";

    std::string externalGeometry;
    if (!readFile(geometryPath, externalGeometry) || externalGeometry.empty()) {
        loadDefaultAssets();
        return;
    }
    m_defaultGeometryName = parseGeometryIdentifier(externalGeometry);
    if (m_defaultGeometryName.empty()) {
        loadDefaultAssets();
        return;
    }
    m_geometryTemplate = std::move(externalGeometry);
    m_geometryData = m_geometryTemplate;

    int width = 0, height = 0, channels = 0;
    stbi_uc* decoded = stbi_load(texturePath.c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0 ||
        width > static_cast<int>(kMaxSourceDimension) ||
        height > static_cast<int>(kMaxSourceDimension)) {
        if (decoded) stbi_image_free(decoded);
        loadDefaultAssets();
        return;
    }

    const std::size_t bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    m_texturePixels.assign(decoded, decoded + bytes);
    stbi_image_free(decoded);

    m_textureWidth = width;
    m_textureHeight = height;
    m_assetsLoaded = true;
    m_useDefaults = false;
    m_loadFailed = false;
    m_needsApply = true;
}

// Rebuilds m_geometryData from m_geometryTemplate with the current flap phase
// applied to the wing bones. Bones that are missing keep the geometry as-is.
void WingsModule::buildAnimatedGeometry() {
    if (m_geometryTemplate.empty()) return;

    const float phase = m_flapTime * (kFlapBaseRate * m_flapSpeed);
    const float angle = kFlapAmplitudeDegrees * std::sin(phase);

    std::string geometry = m_geometryTemplate;
    setBoneRotation(geometry, kWingRightBone, 0.0f, 0.0f, angle);
    setBoneRotation(geometry, kWingLeftBone, 0.0f, 0.0f, -angle);
    m_geometryData = std::move(geometry);
    m_needsApply = true;
}

void WingsModule::advanceFlapAnimation(float dtSeconds) {
    if (dtSeconds <= 0.0f || !m_assetsLoaded) return;
    m_flapTime += dtSeconds;
    buildAnimatedGeometry();
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

    // Advance the flap clock with real elapsed time, so the flapping stays
    // smooth across standing and moving (which both run the same tick).
    if (enabled && m_assetsLoaded) {
        const auto now = std::chrono::steady_clock::now();
        float dt = 0.0f;
        if (m_flapClockStarted) {
            dt = std::chrono::duration<float>(now - m_lastFlapTick).count();
            if (dt > 0.25f) dt = 0.25f; // clamped after a hitch/join
        }
        m_lastFlapTick = now;
        m_flapClockStarted = true;
        advanceFlapAnimation(dt);
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

    // Texture patch: replace the pixel blob only when it is not ours yet or
    // when the asset dimensions changed. The flap animation rewrites geometry
    // every tick, so this must not churn the texture blob on every tick.
    const bool dimensionsMatch =
        *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth) ==
            static_cast<uint32_t>(m_textureWidth) &&
        *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight) ==
            static_cast<uint32_t>(m_textureHeight);
    if (m_injectedBlob == nullptr ||
        *reinterpret_cast<void**>(skinImage + Image::mBytesOffset) != m_injectedBlob ||
        !dimensionsMatch) {
        const std::size_t bytes = m_texturePixels.size();
        void* newBlob = std::malloc(bytes);
        if (!newBlob) return false;
        std::memcpy(newBlob, m_texturePixels.data(), bytes);

        // Point the skin image at the new blob before releasing the previous
        // one so the skin never references freed memory.
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
    }

    // Geometry animation: write the (possibly newly flapped) geometry and the
    // model name into the skin whenever they differ from what is already there.
    const bool geometrySame =
        readStdString(geometryDataAddr) == m_geometryData &&
        readStdString(geometryNameAddr) == m_defaultGeometryName;
    if (!geometrySame) {
        writeStdString(geometryDataAddr, m_geometryData);
        writeStdString(geometryNameAddr, m_defaultGeometryName);
    }

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
    if (j.contains("m_flapSpeed")) {
        const float speed = j["m_flapSpeed"].get<float>();
        m_flapSpeed = std::clamp(speed, 0.1f, 10.0f);
    }
}

void WingsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    const float flapSpeed = std::clamp(m_flapSpeed, 0.1f, 10.0f);
    j["m_flapSpeed"] = flapSpeed;
}
