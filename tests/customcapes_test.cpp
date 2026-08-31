// Host test for the Custom Capes module.
//
// The module replaces the local player's cape by writing a decoded PNG into
// SerializedSkinImpl::mCapeImage (an mce::Image) inside the player's skin.
// This test verifies that logic on a fake skin built from the exact offsets
// the module uses (Player::mSkin -> SerializedSkinRef::mSkinImpl ->
// SerializedSkinImpl::mCapeImage -> mce::Image), without needing Minecraft:
//
//   * scanCapesDirectory creates the capes folder, writes README.txt, and
//     seeds default.png when the folder is empty
//   * PNG capes in the folder are listed sorted by name (no extension)
//   * saveConfig emits the launcher radio format "<index>,<id1>,<id2>,..."
//   * loadConfig accepts a full radio value, a bare index, or a cape id and
//     clamps invalid indices
//   * applyCapeToPlayer installs the selected PNG into the fake skin: image
//     format 3 / sRGB / depth 1, matching size and pixel bytes, the old blob
//     is released through its deleter, and repeated ticks are no-ops
//   * switching cape selection forces a re-apply
//   * a fresh player (respawn with a new skin) gets the cape re-applied
//   * restoreOriginalCape puts the original cape bytes back on disable, but
//     leaves a skin that the game rebuilt (foreign blob) untouched
//   * the "Refresh Capes" button reloads newly dropped PNGs
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I third_party
//         -I tests/fakejson -I tests/fakepl
//         tests/customcapes_test.cpp src/modules/player/customcapes.cpp
//         -o /tmp/customcapes_test
//     /tmp/customcapes_test

#include "modules/player/customcapes.hpp"
#include "config/ConfigManager.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <stb/stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace off = bedrocktools::sdk::offsets;

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::config {

std::string g_testConfigPath = "/tmp/bt-capes-test/config.json";

ConfigManager::~ConfigManager() = default;
std::string ConfigManager::getConfigPath() const { return g_testConfigPath; }

} // namespace bedrocktools::config

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// Fake skin fixtures
// ---------------------------------------------------------------------------

// Counts how many times an original blob deleter was invoked, so the test can
// verify the module releases the engine-owned buffer on every swap.
int g_originalBlobFrees = 0;
void originalCapeDeleter(unsigned char*) { ++g_originalBlobFrees; }

struct FakePlayer {
    std::vector<unsigned char> playerBuf;
    std::vector<unsigned char> skinRefBuf;
    std::vector<unsigned char> skinImplBuf;
    unsigned char* originalBlob = nullptr; // owned by the fake, released via deleter

    explicit FakePlayer(int capeWidth = 64, int capeHeight = 32) {
        playerBuf.assign(3200, 0);
        skinRefBuf.assign(64, 0);
        skinImplBuf.assign(512, 0);

        // Player::mSkin -> SerializedSkinRef*
        *reinterpret_cast<void**>(playerBuf.data() + off::Player::mSkin) = skinRefBuf.data();
        // SerializedSkinRef::mSkinImpl -> SerializedSkinImpl*
        *reinterpret_cast<void**>(skinRefBuf.data() + off::SerializedSkinRef::mSkinImpl) =
            skinImplBuf.data();

        // Give the fake skin a vanilla-looking cape image so the module has
        // something to back up and restore.
        const std::size_t size = static_cast<std::size_t>(capeWidth) * capeHeight * 4u;
        originalBlob = static_cast<unsigned char*>(std::malloc(size ? size : 1));
        for (std::size_t i = 0; i < size; ++i) originalBlob[i] = static_cast<unsigned char>(i & 0xFF);

        unsigned char* image = skinImplBuf.data() + off::SerializedSkinImpl::mCapeImage;
        *reinterpret_cast<std::uint32_t*>(image + off::Image::mImageFormat) = 3;
        *reinterpret_cast<std::uint32_t*>(image + off::SkinImage::mWidth) = static_cast<std::uint32_t>(capeWidth);
        *reinterpret_cast<std::uint32_t*>(image + off::SkinImage::mHeight) = static_cast<std::uint32_t>(capeHeight);
        *reinterpret_cast<std::uint32_t*>(image + off::Image::mDepth) = 1;
        *reinterpret_cast<unsigned char*>(image + off::Image::mUsage) = 1;
        *reinterpret_cast<void**>(image + off::Image::mBytesOffset) = originalBlob;
        *reinterpret_cast<void (**)(unsigned char*)>(image + off::Image::mBlobDeleterOffset) =
            &originalCapeDeleter;
        *reinterpret_cast<std::size_t*>(image + off::Image::mBlobSizeOffset) = size;
    }

    ~FakePlayer() {
        if (originalBlob) std::free(originalBlob);
    }

    unsigned char* capeImage() { return skinImplBuf.data() + off::SerializedSkinImpl::mCapeImage; }
    const unsigned char* capeImage() const { return skinImplBuf.data() + off::SerializedSkinImpl::mCapeImage; }
    void* blob() { return *reinterpret_cast<void**>(capeImage() + off::Image::mBytesOffset); }
    std::uint32_t width() { return *reinterpret_cast<const std::uint32_t*>(capeImage() + off::SkinImage::mWidth); }
    std::uint32_t height() { return *reinterpret_cast<const std::uint32_t*>(capeImage() + off::SkinImage::mHeight); }
    std::uint32_t format() { return *reinterpret_cast<std::uint32_t*>(capeImage() + off::Image::mImageFormat); }
    std::uint32_t depth() { return *reinterpret_cast<std::uint32_t*>(capeImage() + off::Image::mDepth); }
    unsigned char usage() { return *reinterpret_cast<unsigned char*>(capeImage() + off::Image::mUsage); }
    std::size_t blobSize() { return *reinterpret_cast<std::size_t*>(capeImage() + off::Image::mBlobSizeOffset); }
    void (*&deleter())(unsigned char*) {
        return *reinterpret_cast<void (**)(unsigned char*)>(capeImage() + off::Image::mBlobDeleterOffset);
    }
};

// Writes a deterministic RGBA PNG (each pixel = x ^ (y * 3) pattern) so the
// test can verify the exact bytes the module installs.
void writeTestCape(const std::string& path, int width, int height) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
            p[0] = static_cast<unsigned char>(x * 3);
            p[1] = static_cast<unsigned char>(y * 5);
            p[2] = static_cast<unsigned char>((x ^ (y * 3)) & 0xFF);
            p[3] = static_cast<unsigned char>(128 + (x + y) % 64);
        }
    }
    stbi_write_png(path.c_str(), width, height, 4, rgba.data(), width * 4);
}

void setupTestDir(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    bedrocktools::config::g_testConfigPath = dir + "/config.json";
}

std::string capesDir() {
    return std::filesystem::path(bedrocktools::config::g_testConfigPath).parent_path().string() + "/capes";
}

void runTestScan() {
    std::printf("--- scanCapesDirectory ---\n");
    const std::string dir = "/tmp/bt-capes-test/scan";
    setupTestDir(dir);

    CustomCapesModule module;
    module.onInit(); // scans + subscribes (EventBus host stub)

    check(!module.capesDirectory().empty(), "capes directory is non-empty");
    check(std::filesystem::exists(capesDir() + "/README.txt"), "README.txt was written");
    check(std::filesystem::exists(capesDir() + "/default.png"), "default.png was seeded in an empty folder");
    check(module.capeNames().size() == 1 && module.capeNames()[0] == "default",
          "empty folder exposes exactly the default cape");

    // Drop two real capes (unsorted on purpose) and rescan.
    writeTestCape(capesDir() + "/zeta.png", 64, 32);
    writeTestCape(capesDir() + "/alpha.png", 32, 32);
    writeTestCape(capesDir() + "/notacape.txt", 64, 32);
    writeTestCape(capesDir() + "/bad,name.png", 64, 32); // commas break the radio format
    module.scanCapesDirectory();

    check(module.capeNames().size() == 3, "three PNGs are listed (txt ignored, comma names skipped)");
    check(module.capeNames()[0] == "alpha" && module.capeNames()[1] == "default" &&
              module.capeNames()[2] == "zeta",
          "capes are sorted alphabetically");
    check(module.selectedName() == "default" && module.selectedIndex() == 1,
          "a still-existing selection is preserved across rescans");

    // Removing the selected cape falls back to the first available one.
    std::filesystem::remove(capesDir() + "/default.png");
    module.scanCapesDirectory();
    check(module.selectedName() == "alpha" && module.selectedIndex() == 0,
          "a removed selection falls back to the first cape");
}

void runTestConfigRoundtrip() {
    std::printf("--- config load/save ---\n");
    const std::string dir = "/tmp/bt-capes-test/config";
    setupTestDir(dir);
    writeTestCape(capesDir() + "/beta.png", 64, 32);
    writeTestCape(capesDir() + "/delta.png", 64, 32);

    CustomCapesModule module;
    module.onInit();

    nlohmann::json j;
    module.saveConfig(j);
    check(j.contains("m_cape") && j["m_cape"].is_string(), "saveConfig emits m_cape radio value");
    check(j["m_cape"].get<std::string>() == "0,beta,delta", "radio value is \"<index>,<id1>,<id2>\"");
    check(j.contains("refreshButton") && !j["refreshButton"].get<bool>(),
          "saveConfig emits the Refresh Capes button");

    // Full radio value (what the config file stores).
    nlohmann::json in;
    in["m_cape"] = "1,beta,delta";
    module.loadConfig(in);
    check(module.selectedIndex() == 1 && module.selectedName() == "delta",
          "loadConfig parses a full radio value");

    // Bare numeric index (what the launcher sends when the user picks).
    in["m_cape"] = "0";
    module.loadConfig(in);
    check(module.selectedIndex() == 0 && module.selectedName() == "beta",
          "loadConfig parses a bare index");

    // Cape id (robustness).
    in["m_cape"] = "delta";
    module.loadConfig(in);
    check(module.selectedName() == "delta", "loadConfig parses a cape id");

    // Out of range index clamps to a valid cape.
    in["m_cape"] = "99,beta,delta";
    module.loadConfig(in);
    check(module.selectedIndex() == 0 && module.selectedName() == "beta",
          "out-of-range index clamps to the first cape");
}

void runTestApplyAndRestore() {
    std::printf("--- apply / restore ---\n");
    const std::string dir = "/tmp/bt-capes-test/apply";
    setupTestDir(dir);
    writeTestCape(capesDir() + "/red.png", 64, 32);

    CustomCapesModule module;
    module.onInit();
    nlohmann::json j;
    j["m_cape"] = "red";
    module.loadConfig(j);

    FakePlayer player(64, 32);
    const unsigned char* originalPixels = player.originalBlob;
    const std::size_t originalSize = player.blobSize();
    const int freesBefore = g_originalBlobFrees;

    check(module.applyCapeToPlayer(player.playerBuf.data()) == true, "first apply installs the cape");
    check(player.blob() != player.originalBlob, "cape image blob was swapped");
    check(player.format() == 3, "image format is RGBA8Unorm (3)");
    check(player.depth() == 1, "image depth is 1 (non-zero images are accepted)");
    check(player.usage() == 1, "image usage is sRGB (1)");
    check(player.width() == 64 && player.height() == 32, "cape dimensions match the PNG");
    check(player.blobSize() == 64u * 32u * 4u, "blob size matches width*height*4");

    // The installed bytes must equal the PNG content.
    bool pixelsMatch = true;
    std::vector<unsigned char> expected(64u * 32u * 4u);
    // Rebuild the same pattern writeTestCape used.
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            unsigned char* p = &expected[(static_cast<std::size_t>(y) * 64 + x) * 4];
            p[0] = static_cast<unsigned char>(x * 3);
            p[1] = static_cast<unsigned char>(y * 5);
            p[2] = static_cast<unsigned char>((x ^ (y * 3)) & 0xFF);
            p[3] = static_cast<unsigned char>(128 + (x + y) % 64);
        }
    }
    if (std::memcmp(player.blob(), expected.data(), expected.size()) != 0) pixelsMatch = false;
    check(pixelsMatch, "installed pixels equal the PNG bytes");
    check(g_originalBlobFrees == freesBefore + 1, "original engine blob was released through its deleter");

    // No-op on the next tick (blob already ours).
    check(module.applyCapeToPlayer(player.playerBuf.data()) == false, "repeated ticks are no-ops");
    check(g_originalBlobFrees == freesBefore + 1, "no-op apply does not touch the blob");

    // Disabling restores the original cape bytes.
    module.restoreOriginalCape(player.playerBuf.data());
    check(player.blob() != nullptr, "restored cape has a blob again");
    check(player.blobSize() == originalSize, "restored blob size matches the original");
    bool restored = player.blob() != player.originalBlob &&
                    std::memcmp(player.blob(), originalPixels, originalSize) == 0;
    check(restored, "restored pixels equal the original cape");
    check(player.format() == 3 && player.depth() == 1 && player.usage() == 1,
          "restored image header keeps the original values");
}

void runTestRestoreGuard() {
    std::printf("--- restore safety guard ---\n");
    const std::string dir = "/tmp/bt-capes-test/guard";
    setupTestDir(dir);
    writeTestCape(capesDir() + "/red.png", 64, 32);

    CustomCapesModule module;
    module.onInit();
    nlohmann::json j;
    j["m_cape"] = "red";
    module.loadConfig(j);

    FakePlayer player(64, 32);
    check(module.applyCapeToPlayer(player.playerBuf.data()) == true, "cape installed");

    // Simulate the game rebuilding the skin after our apply: the cape image is
    // replaced with a fresh engine-owned blob that is NOT ours.
    const int freesBefore = g_originalBlobFrees;
    unsigned char* gameBlob = static_cast<unsigned char*>(std::malloc(64u * 32u * 4u));
    player.deleter() = &originalCapeDeleter; // engine-style deleter
    *reinterpret_cast<void**>(player.capeImage() + off::Image::mBytesOffset) = gameBlob;
    *reinterpret_cast<std::size_t*>(player.capeImage() + off::Image::mBlobSizeOffset) = 64u * 32u * 4u;

    module.restoreOriginalCape(player.playerBuf.data());
    check(player.blob() == gameBlob, "restore leaves a foreign (game-owned) blob untouched");
    check(g_originalBlobFrees == freesBefore, "foreign blob was not freed through our deleter");

    std::free(gameBlob);
}

void runTestSelectionChangeAndRespawn() {
    std::printf("--- selection change / respawn ---\n");
    const std::string dir = "/tmp/bt-capes-test/switch";
    setupTestDir(dir);
    writeTestCape(capesDir() + "/small.png", 32, 32);
    writeTestCape(capesDir() + "/wide.png", 128, 64);

    CustomCapesModule module;
    module.onInit();
    nlohmann::json j;
    j["m_cape"] = "small";
    module.loadConfig(j);

    FakePlayer player(64, 32);
    check(module.applyCapeToPlayer(player.playerBuf.data()) == true, "first cape applied");
    const void* firstBlob = player.blob();

    // Switch selection -> the module must re-install different pixels.
    j["m_cape"] = "wide";
    module.loadConfig(j);
    check(module.applyCapeToPlayer(player.playerBuf.data()) == true, "cape switch re-applies");
    check(player.blob() != firstBlob, "new cape installed a new blob");
    check(player.width() == 128 && player.height() == 64, "new cape dimensions applied");

    // Respawn: fresh skin with its own original blob -> re-apply.
    FakePlayer freshPlayer(64, 32);
    check(module.applyCapeToPlayer(freshPlayer.playerBuf.data()) == true,
          "fresh skin (respawn) gets the cape re-applied");
    check(freshPlayer.width() == 128 && freshPlayer.height() == 64,
          "fresh skin carries the selected cape");
}

void runTestRefreshButton() {
    std::printf("--- Refresh Capes button ---\n");
    const std::string dir = "/tmp/bt-capes-test/refresh";
    setupTestDir(dir);

    CustomCapesModule module;
    module.onInit();
    check(module.capeNames() == std::vector<std::string>{"default"},
          "starts with only the default cape");

    // User drops a new cape, then taps Refresh Capes.
    writeTestCape(capesDir() + "/newcape.png", 64, 32);
    nlohmann::json j;
    j["refreshButton"] = true;
    module.loadConfig(j);
    check(std::find(module.capeNames().begin(), module.capeNames().end(), "newcape") !=
              module.capeNames().end(),
          "Refresh Capes picks up newly dropped PNGs");
}

void runTestDisablePath() {
    std::printf("--- disable (onDisable) ---\n");
    const std::string dir = "/tmp/bt-capes-test/disable";
    setupTestDir(dir);
    writeTestCape(capesDir() + "/red.png", 64, 32);

    CustomCapesModule module;
    module.onInit();
    nlohmann::json j;
    j["m_cape"] = "red";
    module.loadConfig(j);

    FakePlayer player(64, 32);
    const std::size_t originalSize = player.blobSize();
    std::vector<unsigned char> originalPixels(player.originalBlob, player.originalBlob + originalSize);

    module.setMasterEnabled(true);
    module.onLocalPlayerTick(player.playerBuf.data());
    check(player.blob() != player.originalBlob, "enabled: cape installed via tick");

    module.setMasterEnabled(false); // triggers onDisable
    check(player.blob() != nullptr && player.blobSize() == originalSize, "disable restores the cape");
    bool restored = std::memcmp(player.blob(), originalPixels.data(), originalSize) == 0;
    check(restored, "disable restores the original cape pixels");
}

} // namespace

int main() {
    runTestScan();
    runTestConfigRoundtrip();
    runTestApplyAndRestore();
    runTestRestoreGuard();
    runTestSelectionChangeAndRespawn();
    runTestRefreshButton();
    runTestDisablePath();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("all customcapes tests passed\n");
        return 0;
    }
    std::printf("%d customcapes test(s) failed\n", g_failures);
    return 1;
}
