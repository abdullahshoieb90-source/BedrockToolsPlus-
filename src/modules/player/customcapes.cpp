#include "customcapes.hpp"
#include "customcapes_files.hpp"

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include "core/GameHooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <pl/ModMenu.hpp>
#include "../../config/ConfigManager.hpp"
#include "../../launcher/ExternalButtonRefresh.hpp"

#if defined(__ANDROID__)
#include <jni.h>
#endif

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

// The PNG writer must have exactly one definition in the shared object. Wings
// calls stbi_write_png() in the device build too (it regenerates wings.png when
// the asset is missing), so this cannot be dropped from Android: doing that
// left libBedrockToolsPlus.so with an undefined stbi_write_png, and the
// preloader's dlopen(RTLD_NOW) then failed - which made every module disappear
// from the launcher menu instead of just the one that needed the writer.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

CustomCapesModule* g_customCapes = nullptr;

namespace {

using namespace bedrocktools::sdk::offsets;

void freeBlobDeleter(unsigned char* data) {
    std::free(data);
}

// mce::ImageFormat::RGBA8Unorm (see mce/ImageFormat.h: Unknown=0, R8Unorm=1,
// RG8Unorm=2, RGB8Unorm=3, RGBA8Unorm=4). Skin and cape images are RGBA8.
constexpr std::uint32_t kCapeImageFormat = 4;

// mce::AlphaUsage::Transparent (see mce/AlphaUsage.h: Unknown=0, Data=1,
// Transparent=2). Written next to mUsage; see the patch site for why.
constexpr std::uint8_t kCapeAlphaUsageTransparent = 2;

constexpr int kLoadRetryTicks = 120;

// The (possibly prefiltered) source is kept in memory so switching
// Cover/Contain/Stretch re-resamples instantly. The prefilter caps the source
// at 1024 pixels on its longer side, i.e. at most 4 MB for a square image, so
// this budget effectively always fits and stays a hard guarantee for odd
// aspect ratios (a 1024x4096 source is still 16 MB and is not cached).
constexpr std::size_t kMaxCachedSourceBytes = 4u * 1024u * 1024u;
constexpr const char* kCapeIdBase = "bedrocktools";
constexpr const char* kSampleCapeName = "Sample Cape.png";

// libc++ std::string is 24 bytes with the short-string bit in the low bit of
// byte 0 (length << 1). 22 characters fit inline; longer ids would switch the
// object to the heap representation and require allocating a libc++ string,
// which this module refuses to do across the engine boundary.
constexpr std::size_t kShortStringCapacity = 22;

void writeShortStdString(uintptr_t addr, const char* text, std::size_t len) {
    if (len > kShortStringCapacity) return;
    unsigned char* p = reinterpret_cast<unsigned char*>(addr);
    std::memset(p, 0, 24);
    p[0] = static_cast<unsigned char>(len << 1);
    std::memcpy(p + 1, text, len);
}

bool shortStdStringEquals(uintptr_t addr, const char* text, std::size_t len) {
    if (len > kShortStringCapacity) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    return p[0] == static_cast<unsigned char>(len << 1) &&
           std::memcmp(p + 1, text, len) == 0;
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

// Reads the PNG header without decoding. Rejecting a non-PNG early keeps the
// decoder from being handed arbitrary data, and rejecting an oversized one
// before stbi_load() is what stops a 4096x4096 "cape" (64 MB decoded) from
// exhausting the phone's memory.
// NormalTick is hooked on Actor::normalTick, so the tick event fires for every
// actor in the level - mobs, other players, item entities. Writing our cape
// pixels into "whatever actor ticked last" is wrong twice over: it patches the
// wrong skin object (so the local player's cape never changes) and it hands an
// actor whose layout happens to match a malloc'd blob it does not own.
//
// The local player pointer is resolved through ClientInstance and compared with
// the actor that ticked; only a match is patched. When the client instance or
// its local player is not available yet, the tick is skipped instead of
// guessing: patching an arbitrary actor is never the right answer.
CustomCapesModule::LocalPlayerPredicate g_localPlayerPredicate = nullptr;

// The local player pointer, resolved once and remembered. Resolving it every
// tick would call into the game 20 times per second for no gain, and the
// identity only changes when the player leaves the world (the pointer is
// cleared on world exit).
void* g_cachedLocalPlayer = nullptr;

bool isLocalPlayer(const void* actor) {
    if (g_localPlayerPredicate) return g_localPlayerPredicate(actor);
    if (!actor) return false;

    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) {
        // The client instance is not available (menu/loading screen, or a build
        // where only the hook is live). Falling back to "accept the ticked
        // actor" is what the module did before the filter existed: it is
        // imprecise, but disabling the cape entirely would be worse than
        // treating the one actor that is being ticked as the local player.
        return true;
    }

    auto* instance = static_cast<bedrocktools::sdk::ClientInstance*>(client);
    if (g_cachedLocalPlayer) {
        return static_cast<void*>(g_cachedLocalPlayer) == actor;
    }

    auto* local = instance->localPlayer();
    if (!local) {
        // No local player yet (still connecting). Nothing can be patched
        // meaningfully, but the tick is harmless, so do not block it: the
        // skin resolution below will simply fail and report NoSkin.
        return true;
    }

    g_cachedLocalPlayer = static_cast<void*>(local);
    return g_cachedLocalPlayer == actor;
}

bool readPngFileHeader(const std::string& path, std::uint32_t& width, std::uint32_t& height) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    unsigned char header[33] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (file.gcount() < static_cast<std::streamsize>(sizeof(header))) return false;
    return customcapes::readPngHeader(header, sizeof(header), width, height);
}

} // namespace

CustomCapesModule::CustomCapesModule()
    : Module("Custom Capes",
             "Wear any PNG from the BedrockTools capes folder as your cape (local only).") {
    g_customCapes = this;
}

void CustomCapesModule::setLocalPlayerPredicateForTests(LocalPlayerPredicate predicate) {
    g_localPlayerPredicate = predicate;
}

const char* CustomCapesModule::applyStatusLabel(ApplyStatus status) {
    switch (status) {
        case ApplyStatus::Applied: return "applied";
        case ApplyStatus::NoPlayer: return "waiting for a world";
        case ApplyStatus::NoSkin: return "cannot read the player skin";
        case ApplyStatus::PersonaSkin: return "persona skin: classic capes are not rendered";
        case ApplyStatus::LayoutMismatch: return "skin layout mismatch (unsupported game build)";
        case ApplyStatus::EmptyPixels: return "no cape loaded";
        case ApplyStatus::LoadFailed: return "the selected PNG could not be loaded";
        case ApplyStatus::Idle: break;
    }
    return "off";
}

CustomCapesModule::~CustomCapesModule() {
    if (g_customCapes == this) g_customCapes = nullptr;
}

void CustomCapesModule::onInit() {
    m_capesDir = capeDirectoryForConfig();
    ensureCapesDirectory();
    m_files = customcapes::scanCapeFiles(m_capesDir);
    m_labels = customcapes::makeCapeLabels(m_files);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_customCapes) g_customCapes->onLocalPlayerTick(event.player);
        });
}

void CustomCapesModule::onEnable() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_selectedIndex > 0 && !m_capeLoaded && !m_loadFailed) loadSelectedCape();
    m_needsApply = true;
}

void CustomCapesModule::onDisable() {
    // The tick hook restores the vanilla cape on the next tick (the skin must
    // only be touched from the tick path, never from the menu thread).
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_needsApply = true;
    m_retryTicks = 0;
}

void CustomCapesModule::ensureCapesDirectory() {
    std::error_code ec;
    if (std::filesystem::is_directory(m_capesDir, ec)) return;
    if (!std::filesystem::create_directories(m_capesDir, ec) || ec) return;
    writeSamplePng(m_capesDir + "/" + kSampleCapeName);
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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Module::loadConfig(j);

    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    const customcapes::FitMode previousFit = m_fitMode;

    // Fit mode is read first: a config that changes both the fit and the
    // selection must decode the new file only once, with the new fit.
    if (j.contains("m_capeFit")) {
        int fit = static_cast<int>(m_fitMode);
        if (j["m_capeFit"].is_number_integer()) {
            fit = j["m_capeFit"].get<int>();
        } else if (j["m_capeFit"].is_string()) {
            int parsedIndex = fit;
            std::string parsedName;
            if (customcapes::parseRadioValue(j["m_capeFit"].get<std::string>(), parsedIndex, parsedName)) {
                fit = parsedIndex;
            }
            if (!parsedName.empty()) {
                for (int i = 0; i < customcapes::kFitModeCount; ++i) {
                    if (parsedName == customcapes::fitModeLabel(customcapes::fitModeFromInt(i))) {
                        fit = i;
                        break;
                    }
                }
            }
        }
        m_fitMode = customcapes::fitModeFromInt(fit);
    }

    if (j.contains("m_cape")) {
        int parsedIndex = m_selectedIndex;
        std::string parsedName;
        if (j["m_cape"].is_string()) {
            customcapes::parseRadioValue(j["m_cape"].get<std::string>(), parsedIndex, parsedName);
        } else if (j["m_cape"].is_number_integer()) {
            parsedIndex = j["m_cape"].get<int>();
        }
        m_files = customcapes::scanCapeFiles(m_capesDir);
        m_labels = customcapes::makeCapeLabels(m_files);
        m_selectedIndex = customcapes::resolveSelectionIndex(parsedIndex, parsedName, m_files, m_labels);
    }

    // Compare the *file name* against what is loaded, not the option index:
    // m_files was just re-listed, so an index can point at a different file
    // than the one whose pixels are in memory (a cape added or removed above
    // the selection shifts every index). That stale-image case is exactly the
    // "my cape did not change" bug.
    const std::string wanted =
        (m_selectedIndex > 0 && m_selectedIndex <= static_cast<int>(m_files.size()))
            ? m_files[static_cast<std::size_t>(m_selectedIndex - 1)]
            : std::string();
    const bool selectionChanged = wanted != m_loadedFile;
    const bool fitChanged = m_fitMode != previousFit;
    if (selectionChanged) {
        releaseLoadedCape();
        if (m_selectedIndex > 0) loadSelectedCape();
        m_needsApply = true;
    } else if (fitChanged) {
        applyFitChange();
    }
}

void CustomCapesModule::saveConfig(nlohmann::json& j) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Module::saveConfig(j);
    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    m_files = customcapes::scanCapeFiles(m_capesDir);
    m_labels = customcapes::makeCapeLabels(m_files);
    if (m_selectedIndex > static_cast<int>(m_files.size())) m_selectedIndex = 0;
    j["m_cape"] = customcapes::makeRadioValue(m_selectedIndex, m_files, m_labels);

    // Radio value for the fit picker: "<index>,Cover,Contain,Stretch".
    std::string fitValue = std::to_string(static_cast<int>(m_fitMode));
    for (int i = 0; i < customcapes::kFitModeCount; ++i) {
        fitValue += ',';
        fitValue += customcapes::fitModeLabel(customcapes::fitModeFromInt(i));
    }
    j["m_capeFit"] = fitValue;

    // Action entries. The launcher shows a boolean whose key contains "button"
    // as a Button and reports a press as the string "true" through
    // onMenuConfigChanged, which is handled below.
    j["m_reloadButton"] = false;
    j["m_copyPathButton"] = false;

    // Human readable status shown as a text row: how many capes were found and
    // which one is loaded. Written only while a save runs, which is when the
    // launcher reads the module's settings again.
    // The status row spells out both halves of the question the user actually
    // asks: which file is loaded, and what happened when it was applied.
    char status[260];
    std::snprintf(status, sizeof(status), "%zu capes - %s [%s] - local only (others cannot see it)",
                  m_files.size(),
                  m_loadedFile.empty() ? "none selected" : m_loadedFile.c_str(),
                  applyStatusLabel(m_applyStatus));
    j["m_status"] = std::string(status);
    // The folder path itself, so it can be read (or copied with the button)
    // without digging through a file manager.
    j["m_folderPath"] = m_capesDir;
}

bool CustomCapesModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_reloadButton") {
        if (value != "true") return true; // released / false: nothing to do
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            refreshFilesLocked();
            // Re-read the file from disk: it may have been edited in place,
            // which a pixel cache would otherwise hide.
            if (m_selectedIndex > 0) {
                loadSelectedCape();
                m_needsApply = true;
            }
        }
        return true;
    }
    if (key == "m_copyPathButton") {
        if (value != "true") return true;
        copyCapesPath();
        return true;
    }
    if (key == "m_status" || key == "m_folderPath") {
        // Read-only info rows. The launcher renders them as text fields, but
        // the values are owned by the module (they are regenerated on every
        // save), so edits are simply ignored instead of overwriting the folder
        // path the module actually uses.
        return true;
    }
    return false;
}

bool CustomCapesModule::refreshFiles() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return refreshFilesLocked();
}

bool CustomCapesModule::refreshFilesLocked() {
    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    const std::vector<std::string> before = m_files;
    std::vector<std::string> files = customcapes::scanCapeFiles(m_capesDir);
    std::vector<std::string> labels = customcapes::makeCapeLabels(files);
    m_files = std::move(files);
    m_labels = std::move(labels);

    // Keep the current cape selected by name; the index may have moved when a
    // file was added or removed.
    if (m_selectedIndex > 0 && !before.empty()) {
        const std::size_t previous = static_cast<std::size_t>(m_selectedIndex - 1);
        const std::string current = previous < before.size() ? before[previous] : std::string();
        m_selectedIndex = customcapes::resolveSelectionIndex(0, current, m_files, m_labels);
    }
    if (m_selectedIndex > static_cast<int>(m_files.size())) m_selectedIndex = 0;

    // The selected file itself may have been edited on disk, so drop the
    // cached pixels: the next load re-reads it.
    if (m_selectedIndex > 0) releaseLoadedCape();
    return before != m_files;
}

void CustomCapesModule::releaseLoadedCape() {
    m_pixels.clear();
    m_pixels.shrink_to_fit();
    m_sourcePixels.clear();
    m_sourcePixels.shrink_to_fit();
    m_sourceWidth = 0;
    m_sourceHeight = 0;
    m_loadedFile.clear();
    m_capeLoaded = false;
    m_loadFailed = false;
    m_retryTicks = 0;
}

bool CustomCapesModule::loadSelectedCape() {
    releaseLoadedCape();
    if (m_selectedIndex <= 0 || m_selectedIndex > static_cast<int>(m_files.size())) return false;

    const std::string path =
        m_capesDir + "/" + m_files[static_cast<std::size_t>(m_selectedIndex - 1)];

    // Validate before decoding: signature + IHDR + a sane size.
    std::uint32_t headerWidth = 0;
    std::uint32_t headerHeight = 0;
    if (!readPngFileHeader(path, headerWidth, headerHeight) ||
        !customcapes::isSupportedSourceSize(headerWidth, headerHeight)) {
        m_loadFailed = true;
        return false;
    }

    int width = 0, height = 0, channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0 ||
        !customcapes::isSupportedSourceSize(static_cast<std::uint32_t>(width),
                                            static_cast<std::uint32_t>(height))) {
        if (decoded) stbi_image_free(decoded);
        m_loadFailed = true;
        return false;
    }

    // Very large images are reduced once with an area downscale first: the
    // cape-face sampler is exact but walks every source texel, which is too
    // slow for a 16 MB image on the client tick.
    std::vector<std::uint8_t> reduced;
    const std::uint8_t* source = decoded;
    std::uint32_t sourceWidth = static_cast<std::uint32_t>(width);
    std::uint32_t sourceHeight = static_cast<std::uint32_t>(height);
    if (customcapes::sourceNeedsPrefilter(sourceWidth, sourceHeight)) {
        std::uint32_t targetWidth = 0;
        std::uint32_t targetHeight = 0;
        customcapes::prefilterSize(sourceWidth, sourceHeight, targetWidth, targetHeight);
        reduced = customcapes::downscaleArea(decoded, sourceWidth, sourceHeight,
                                             targetWidth, targetHeight);
        if (!reduced.empty()) {
            source = reduced.data();
            sourceWidth = targetWidth;
            sourceHeight = targetHeight;
        }
    }

    // The decoded source is cached (when it fits the budget) so switching
    // Cover/Contain/Stretch can re-resample without touching the disk again.
    const std::size_t byteCount =
        static_cast<std::size_t>(sourceWidth) * static_cast<std::size_t>(sourceHeight) * 4u;
    if (byteCount <= kMaxCachedSourceBytes) {
        m_sourcePixels.assign(source, source + byteCount);
        m_sourceWidth = sourceWidth;
        m_sourceHeight = sourceHeight;
    }
    m_pixels = customcapes::resampleToCapeEx(source, sourceWidth, sourceHeight, m_fitMode);
    stbi_image_free(decoded);
    m_loadedFit = m_fitMode;
    m_loadedFile = m_files[static_cast<std::size_t>(m_selectedIndex - 1)];
    m_capeLoaded = true;
    rebuildCapeId();
    return true;
}

// The engine caches the cape texture by mCapeId, so every change gets a fresh
// id. Anything that replaces the pixels (new file, edited file, new fit mode)
// must call this or the old cape keeps rendering.
void CustomCapesModule::rebuildCapeId() {
    ++m_capeIdSerial;
    m_activeCapeId = std::string(kCapeIdBase) + "-" + std::to_string(m_capeIdSerial);
    if (m_activeCapeId.size() > kShortStringCapacity) {
        m_activeCapeId = std::string(kCapeIdBase) + "-" +
                         std::to_string(m_capeIdSerial % 1000000u);
        if (m_activeCapeId.size() > kShortStringCapacity) {
            m_activeCapeId.resize(kShortStringCapacity);
        }
    }
}

// Re-resamples the already decoded source with the new fit mode, so changing
// Cover/Contain/Stretch is instant and does not hit the disk again.
void CustomCapesModule::applyFitChange() {
    if (!m_capeLoaded || m_loadedFit == m_fitMode) return;
    if (m_sourcePixels.empty() || m_sourceWidth == 0 || m_sourceHeight == 0) {
        // Nothing cached (enabled late / decode failed): fall back to a reload.
        if (m_selectedIndex > 0) loadSelectedCape();
        return;
    }
    m_pixels = customcapes::resampleToCapeEx(m_sourcePixels.data(), m_sourceWidth,
                                             m_sourceHeight, m_fitMode);
    m_loadedFit = m_fitMode;
    rebuildCapeId();
    m_needsApply = true;
}

// Copies the capes folder path to the system clipboard, so it can be pasted
// into a file manager or a download prompt on the device.
//
// This deliberately does NOT try to launch a file manager: handing another app
// a file:// URI extracted from a File (Uri.fromFile) throws
// FileUriExposedException on Android 7+ and would crash the game, while
// FileProvider needs manifest access this mod does not have. The clipboard
// only needs an Application context and cannot throw across apps. Any failure
// is swallowed — the folder path is also shown as a text row in the menu.
void CustomCapesModule::copyCapesPath() const {
#if defined(__ANDROID__)
    void* vm = bedrocktools::launcher::javaVm();
    if (!vm) return;
    auto* javaVm = static_cast<JavaVM*>(vm);
    JNIEnv* env = nullptr;
    bool attached = false;
    const jint state = javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        JavaVMAttachArgs args{JNI_VERSION_1_6,
                              const_cast<char*>("BedrockToolsPlus/CustomCapes"), nullptr};
        if (javaVm->AttachCurrentThread(&env, &args) != JNI_OK) return;
        attached = true;
    } else if (state != JNI_OK) {
        return;
    }
    if (!env) {
        if (attached) javaVm->DetachCurrentThread();
        return;
    }

    const auto clear = [&]() {
        if (env->ExceptionCheck()) env->ExceptionClear();
    };

    jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
    jclass contextClass = env->FindClass("android/content/Context");
    jclass clipDataClass = env->FindClass("android/content/ClipData");
    jclass clipboardClass = env->FindClass("android/content/ClipboardManager");
    if (activityThreadClass && contextClass && clipDataClass && clipboardClass && !env->ExceptionCheck()) {
        jmethodID currentApplication = env->GetStaticMethodID(
            activityThreadClass, "currentApplication", "()Landroid/app/Application;");
        jmethodID getSystemService = env->GetMethodID(
            contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jmethodID newPlainText = env->GetStaticMethodID(
            clipDataClass, "newPlainText",
            "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
        jmethodID setPrimaryClip = env->GetMethodID(
            clipboardClass, "setPrimaryClip", "(Landroid/content/ClipData;)V");
        clear();

        jobject application = currentApplication
                                  ? env->CallStaticObjectMethod(activityThreadClass,
                                                                currentApplication)
                                  : nullptr;
        jobject clipboard = nullptr;
        if (application && getSystemService && !env->ExceptionCheck()) {
            jstring service = env->NewStringUTF("clipboard");
            clipboard = env->CallObjectMethod(application, getSystemService, service);
            if (service) env->DeleteLocalRef(service);
            clear();
        }
        if (clipboard && newPlainText && setPrimaryClip && !env->ExceptionCheck()) {
            jstring label = env->NewStringUTF("BedrockToolsPlus capes folder");
            jstring text = env->NewStringUTF(m_capesDir.c_str());
            jobject clip = (label && text)
                               ? env->CallStaticObjectMethod(clipDataClass, newPlainText, label, text)
                               : nullptr;
            if (label) env->DeleteLocalRef(label);
            if (text) env->DeleteLocalRef(text);
            if (clip && !env->ExceptionCheck()) {
                env->CallVoidMethod(clipboard, setPrimaryClip, clip);
                clear(); // a locked clipboard is not worth a crash
            }
            if (clip) env->DeleteLocalRef(clip);
        }
        if (clipboard) env->DeleteLocalRef(clipboard);
        if (application) env->DeleteLocalRef(application);
    }
    if (activityThreadClass) env->DeleteLocalRef(activityThreadClass);
    if (contextClass) env->DeleteLocalRef(contextClass);
    if (clipDataClass) env->DeleteLocalRef(clipDataClass);
    if (clipboardClass) env->DeleteLocalRef(clipboardClass);
    clear();
    if (attached) javaVm->DetachCurrentThread();
#else
    // Host build: nothing to copy to; the path is reported in the status row.
#endif
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
    g_cachedLocalPlayer = nullptr; // re-resolved on the next world
    m_patchedSkin = nullptr;
    m_injectedBlob = nullptr; // ownership: engine (frees via the deleter tag)
    m_hasBackup = false;
    m_backup = CapeBackup{};
    m_needsApply = true; // next live skin must be (re)patched from scratch
}

void CustomCapesModule::onLocalPlayerTick(void* player) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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

    // Actor::normalTick fires for every actor, not just the local player.
    if (!isLocalPlayer(player)) return;

    const bool wantCape = enabled && m_selectedIndex > 0;
    if (wantCape && !m_capeLoaded) {
        if (m_retryTicks <= 0) {
            // Either the first attempt, or a file the user just fixed on disk.
            if (!loadSelectedCape()) m_retryTicks = kLoadRetryTicks;
            else m_needsApply = true;
        } else {
            --m_retryTicks;
        }
    }

    if (!wantCape) {
        m_applyStatus = ApplyStatus::Idle;
        restoreOriginalCape(resolvePlayerSkin(player));
        return;
    }
    if (!m_capeLoaded) {
        m_applyStatus = m_loadFailed ? ApplyStatus::LoadFailed : ApplyStatus::EmptyPixels;
        restoreOriginalCape(resolvePlayerSkin(player));
        return;
    }

    void* skin = resolvePlayerSkin(player);
    if (!skin) {
        // The player exists but the skin could not be reached: on the very
        // first ticks the skin object is not published yet, so this is normal
        // transient state rather than an error.
        m_applyStatus = ApplyStatus::NoSkin;
        return;
    }

    if (!applyCustomCape(skin)) {
        // applyCustomCape() records the specific reason (persona / layout).
        restoreOriginalCape(skin);
        return;
    }
    m_applyStatus = ApplyStatus::Applied;
}

bool CustomCapesModule::applyCustomCape(void* skin) {
    if (m_pixels.empty()) {
        m_applyStatus = ApplyStatus::EmptyPixels;
        return false;
    }

    if (*reinterpret_cast<bool*>(
            reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mIsPersona)) {
        m_applyStatus = ApplyStatus::PersonaSkin;
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
    if (!layoutOk) {
        m_applyStatus = ApplyStatus::LayoutMismatch;
        return false;
    }

    // The cape pixels go into mCapeImage and the synthetic id into mCapeId —
    // never into mSkinImage (that is the player's skin texture; touching it
    // makes the game fall back to the default Steve skin).
    const uintptr_t capeImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeImage;
    const uintptr_t capeIdAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeId;

    if (m_patchedSkin != skin) {
        // New skin object: back up its original cape so that "None"/disable
        // can bring the vanilla cape back before we patch anything. The
        // original pixel blob is only detached, never freed, so restoring
        // the raw pointer is safe.
        m_backup.format = *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat);
        m_backup.width = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth);
        m_backup.height = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight);
        m_backup.depth = *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth);
        // mUsage and mAlphaUsage are single bytes (mce::ImageUsage /
        // mce::AlphaUsage are `uchar`); reading them as a uint32 would also
        // capture the padding byte and writing that back later clobbers them.
        m_backup.usage = *reinterpret_cast<std::uint8_t*>(capeImage + Image::mUsage);
        m_backup.alphaUsage = *reinterpret_cast<std::uint8_t*>(capeImage + Image::mAlphaUsage);
        m_backup.blob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
        m_backup.deleter = *reinterpret_cast<void**>(capeImage + Image::mBlobDeleterOffset);
        m_backup.size = *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset);
        std::memcpy(m_backup.capeIdBytes, reinterpret_cast<const void*>(capeIdAddr),
                    sizeof(m_backup.capeIdBytes));

        // The engine owns the blob we injected into the previous skin object
        // and frees it through the deleter tag we set — do not free it here.
        m_patchedSkin = skin;
        m_injectedBlob = nullptr;
        m_hasBackup = true;
        m_needsApply = true;
    }

    // Patch already in place and untouched? Nothing to do this tick.
    const bool idIntact = shortStdStringEquals(capeIdAddr, m_activeCapeId.c_str(),
                                               m_activeCapeId.size());
    if (!m_needsApply && m_injectedBlob != nullptr &&
        *reinterpret_cast<void**>(capeImage + Image::mBytesOffset) == m_injectedBlob &&
        *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth) == customcapes::kCapeWidth &&
        *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight) == customcapes::kCapeHeight &&
        idIntact) {
        return true;
    }

    const std::size_t bytes = m_pixels.size();
    void* newBlob = std::malloc(bytes);
    if (!newBlob) return false;
    std::memcpy(newBlob, m_pixels.data(), bytes);

    // Point the cape image at the new blob before releasing the previous
    // one so the skin never references freed memory.
    void* previousBlob = m_injectedBlob;
    *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat) = kCapeImageFormat;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth) = customcapes::kCapeWidth;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight) = customcapes::kCapeHeight;
    // The engine derives the texture description (and the sampled byte count)
    // from these, so both must be explicit for a 64x32 RGBA8 cape.
    *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth) = 1;
    *reinterpret_cast<std::uint8_t*>(capeImage + Image::mUsage) =
        static_cast<std::uint8_t>(m_backup.usage);
    // mAlphaUsage (0x11) sits right after mUsage. A cape texture is RGBA with
    // a real alpha channel, so it must not be copied from whatever the backup
    // held (a vanilla cape commonly has it at 0/Unknown, which makes the
    // renderer treat the pixels as opaque data). 2 == Transparent.
    *reinterpret_cast<std::uint8_t*>(capeImage + Image::mAlphaUsage) = kCapeAlphaUsageTransparent;
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

    // Put the vanilla cape back: dimensions, format, blob metadata, the
    // original pixel pointer and the original cape id. The blob pointer is
    // restored before our injected blob is freed, so the skin never
    // references freed memory.
    *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat) = m_backup.format;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth) = m_backup.width;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight) = m_backup.height;
    *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth) = m_backup.depth;
    *reinterpret_cast<std::uint8_t*>(capeImage + Image::mUsage) =
        static_cast<std::uint8_t>(m_backup.usage);
    *reinterpret_cast<std::uint8_t*>(capeImage + Image::mAlphaUsage) =
        static_cast<std::uint8_t>(m_backup.alphaUsage);
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
