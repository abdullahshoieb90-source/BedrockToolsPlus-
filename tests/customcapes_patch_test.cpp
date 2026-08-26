// Regression test for the Custom Capes in-game skin patch.
//
// Builds the real module (src/modules/player/customcapes.cpp, compiled by
// scripts/run_tests.sh as a second translation unit) and drives it against a
// fake SerializedSkinImpl byte buffer laid out exactly as
// include/bedrocktools/sdk/offsets/Skin.hpp documents it for the Android
// target, connected through the fake Player -> SkinRef -> ThreadOwner pointer
// chain that resolvePlayerSkin() walks.
//
// Covered:
//   * cape pixels are written to mCapeImage (168) and the synthetic cape id
//     to mCapeId (336) — and NEVER to mSkinImage (120). Overwriting the skin
//     image is what made the game fall back to the default Steve skin.
//   * the injected blob gets matching width/height/format/deleter/size.
//   * a second tick does not churn the blob (patch stays intact, no
//     per-tick malloc/free of memory the renderer may still sample).
//   * disabling restores the original cape image and cape id bit-for-bit
//     before our injected blob is freed.
//   * persona skins and unexpected skin-image layouts are left untouched.
//   * Leave World: with the level link (Actor::mLevel) gone the tick hook
//     writes nothing to the skin (no use-after-free), survives a null
//     player, never double-frees the blob the engine already released via
//     the deleter tag, and re-applies a fresh blob after rejoin.
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I third_party
//         -I tests/fakejson -I tests/fakepl
//         tests/customcapes_patch_test.cpp src/modules/player/customcapes.cpp
//         -o /tmp/customcapes_patch_test
//     /tmp/customcapes_patch_test

#include "modules/player/customcapes.hpp"
#include "modules/player/customcapes_files.hpp"
#include "config/ConfigManager.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace off = bedrocktools::sdk::offsets;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

std::string g_testConfigPath;

// Link stubs for the config backend the module queries for its folder.
} // namespace

namespace bedrocktools::config {
ConfigManager::~ConfigManager() = default;
std::string ConfigManager::getConfigPath() const { return g_testConfigPath; }
// The module persists grid selections through ConfigManager::save(); the
// host test has no config writer, so just satisfy the linker.
void ConfigManager::save() {}
} // namespace bedrocktools::config

namespace bedrocktools::events {
// Link stub: onInit() subscribes through the event bus, which the host test
// never drives. The EventBus class itself is fully inline.
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace {

void fakeDeleter(unsigned char*) {}

// A fake mce::Image: format@0, width@4, height@8, depth@0xC, usage@0x10,
// blob{pixels@0x18, deleter@0x20, size@0x28}; 48 bytes total (see Skin.hpp).
void writeImage(std::uint8_t* base, std::uint32_t format, std::uint32_t width,
                std::uint32_t height, void* pixels, void* deleter, std::size_t size) {
    std::memcpy(base + off::Image::mImageFormat, &format, 4);
    std::memcpy(base + off::SkinImage::mWidth, &width, 4);
    std::memcpy(base + off::SkinImage::mHeight, &height, 4);
    std::uint32_t depth = 1;
    std::memcpy(base + off::Image::mDepth, &depth, 4);
    std::uint32_t usage = 0;
    std::memcpy(base + off::Image::mUsage, &usage, 4);
    std::memcpy(base + off::Image::mBytesOffset, &pixels, sizeof(void*));
    std::memcpy(base + off::Image::mBlobDeleterOffset, &deleter, sizeof(void*));
    std::memcpy(base + off::Image::mBlobSizeOffset, &size, sizeof(size_t));
}

bool sameBytes(const std::uint8_t* a, const std::uint8_t* b, std::size_t count) {
    return std::memcmp(a, b, count) == 0;
}

std::uint32_t readU32(const std::uint8_t* base, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, 4);
    return value;
}

void* readPtr(std::uint8_t* base, std::size_t offset) {
    void* value = nullptr;
    std::memcpy(&value, base + offset, sizeof(void*));
    return value;
}

std::size_t readSize(std::uint8_t* base, std::size_t offset) {
    std::size_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

} // namespace

int main() {
    // ------------------------------------------------------------------
    // Temporary config/capes folders with one small red cape PNG.
    // ------------------------------------------------------------------
    const std::string root = "/tmp/bt-customcapes-patch-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/capes");
    g_testConfigPath = root + "/config.json";

    constexpr std::uint32_t kSrcW = 10, kSrcH = 20;
    std::vector<std::uint8_t> red(kSrcW * kSrcH * 4);
    for (std::size_t i = 0; i < red.size(); i += 4) {
        red[i + 0] = 200; red[i + 1] = 40; red[i + 2] = 30; red[i + 3] = 255;
    }
    stbi_write_png((root + "/capes/Red.png").c_str(), kSrcW, kSrcH, 4, red.data(), kSrcW * 4);
    const std::vector<std::uint8_t> expectedCape = customcapes::resampleToCape(red.data(), kSrcW, kSrcH);

    // ------------------------------------------------------------------
    // Fake skin (offsets per Skin.hpp) and the Player -> skin chain.
    // ------------------------------------------------------------------
    // Sized past mIsPersonaCapeOnClassicSkin (443) so persona checks stay in bounds.
    std::vector<std::uint8_t> skin(off::SerializedSkinImpl::mIsPersonaCapeOnClassicSkin + 32, 0);
    std::uint8_t* skinBase = skin.data();

    std::vector<std::uint8_t> skinPixels(64 * 64 * 4, 0xAB);
    std::vector<std::uint8_t> capePixels(64 * 32 * 4, 0xCD);
    void* const skinBlob = skinPixels.data();
    void* const capeBlob = capePixels.data();
    void* const originalDeleter = reinterpret_cast<void*>(&fakeDeleter);

    writeImage(skinBase + off::SerializedSkinImpl::mSkinImage, 4, 64, 64,
               skinBlob, originalDeleter, skinPixels.size());
    writeImage(skinBase + off::SerializedSkinImpl::mCapeImage, 4, 64, 32,
               capeBlob, originalDeleter, capePixels.size());
    // mCapeId (336) starts as an empty short std::string (all zero bytes).

    const std::vector<std::uint8_t> skinSnapshot = skin;

    // resolvePlayerSkin(): player+Player::mSkin -> *ptr -> +0 -> *ptr -> +0,
    // so player+mSkin must hold the address of a pointer that holds the skin.
    std::vector<std::uint8_t> player(off::Player::mSkin + sizeof(void*), 0);
    void* skinSlot = skinBase;          // its value is the skin pointer
    void* skinSlotAddress = &skinSlot;  // what Player::mSkin points at
    std::memcpy(player.data() + off::Player::mSkin, &skinSlotAddress, sizeof(void*));

    // The tick hook requires a live level link (Actor::mLevel) before it
    // will touch the skin — give the fake player one, exactly like a
    // player attached to a world.
    void* liveLevel = (void*)0x1;
    std::memcpy(player.data() + off::Actor::mLevel, &liveLevel, sizeof(void*));

    // ------------------------------------------------------------------
    // Load the cape through the config path, enable, and tick.
    // ------------------------------------------------------------------
    CustomCapesModule mod;
    nlohmann::json cfg;
    cfg["m_cape"] = 1;
    mod.loadConfig(cfg);
    mod.enabled = true;
    mod.onLocalPlayerTick(player.data());

    std::printf("patch applies to the cape fields only\n");
    check(sameBytes(skinBase + off::SerializedSkinImpl::mSkinImage,
                    skinSnapshot.data() + off::SerializedSkinImpl::mSkinImage,
                    off::Image::Size),
          "mSkinImage (120) is untouched — skin must not turn into Steve");
    check(readU32(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mImageFormat) == 4,
          "mCapeImage format is RGBA8");
    check(readU32(skinBase + off::SerializedSkinImpl::mCapeImage, off::SkinImage::mWidth) == 64 &&
              readU32(skinBase + off::SerializedSkinImpl::mCapeImage, off::SkinImage::mHeight) == 32,
          "mCapeImage dimensions are 64x32");

    void* injected = readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset);
    check(injected != nullptr && injected != capeBlob,
          "mCapeImage points at the injected cape pixels");
    check(readSize(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBlobSizeOffset) ==
              expectedCape.size(),
          "blob size matches 64*32*4");
    check(readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBlobDeleterOffset) !=
              originalDeleter,
          "blob deleter was retagged to free()");
    check(injected != nullptr &&
              sameBytes(static_cast<const std::uint8_t*>(injected), expectedCape.data(),
                        expectedCape.size()),
          "injected pixels match the resampled cape");

    const std::uint8_t* capeId = skinBase + off::SerializedSkinImpl::mCapeId;
    check(capeId[0] == static_cast<std::uint8_t>(14u << 1) &&
              std::memcmp(capeId + 1, "bedrocktools-1", 14) == 0,
          "mCapeId (336) holds the short-string id \"bedrocktools-1\"");
    check(skin[off::SerializedSkinImpl::mIsPersona] == 0,
          "persona flag untouched");

    // ------------------------------------------------------------------
    // A second tick must not churn the blob.
    // ------------------------------------------------------------------
    std::printf("second tick stays idle\n");
    mod.onLocalPlayerTick(player.data());
    check(readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset) == injected,
          "cape blob pointer unchanged on the next tick");
    check(sameBytes(skinBase + off::SerializedSkinImpl::mSkinImage,
                    skinSnapshot.data() + off::SerializedSkinImpl::mSkinImage,
                    off::Image::Size),
          "mSkinImage still untouched after two ticks");

    // ------------------------------------------------------------------
    // Disable restores the original cape exactly.
    // ------------------------------------------------------------------
    std::printf("disable restores the vanilla cape\n");
    mod.enabled = false;
    mod.onLocalPlayerTick(player.data());
    check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeImage,
                    skinSnapshot.data() + off::SerializedSkinImpl::mCapeImage,
                    off::Image::Size),
          "mCapeImage (168) fully restored");
    check(sameBytes(capeId, skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24),
          "mCapeId fully restored");
    check(readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset) == capeBlob,
          "original cape blob pointer restored");

    // ------------------------------------------------------------------
    // Persona skins are never patched.
    // ------------------------------------------------------------------
    std::printf("persona skin is skipped\n");
    skin[off::SerializedSkinImpl::mIsPersona] = 1;
    mod.enabled = true;
    mod.onLocalPlayerTick(player.data());
    check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeImage,
                    skinSnapshot.data() + off::SerializedSkinImpl::mCapeImage,
                    off::Image::Size),
          "mCapeImage untouched for persona skins");
    skin[off::SerializedSkinImpl::mIsPersona] = 0;
    mod.enabled = false;
    mod.onLocalPlayerTick(player.data());

    // ------------------------------------------------------------------
    // A skin-image layout we do not recognize aborts the patch.
    // ------------------------------------------------------------------
    std::printf("unexpected skin layout aborts the patch\n");
    std::uint32_t brokenWidth = 32;
    std::memcpy(skinBase + off::SerializedSkinImpl::mSkinImage + off::SkinImage::mWidth,
                &brokenWidth, 4);
    mod.enabled = true;
    mod.onLocalPlayerTick(player.data());
    check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeImage,
                    skinSnapshot.data() + off::SerializedSkinImpl::mCapeImage,
                    off::Image::Size),
          "mCapeImage untouched when the skin layout is unexpected");
    check(readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset) == capeBlob,
          "original cape blob still in place");
    mod.enabled = false;

    // ------------------------------------------------------------------
    // World exit (Leave World): the engine detaches the player from its
    // level, then destroys the skin and frees our injected blob through
    // the deleter tag we wrote. The module must (a) not write to the
    // skin once the level link is gone (use-after-free), (b) not free the
    // injected blob a second time after the engine already did (double
    // free — proven by running this test under ASan), and (c) re-apply
    // cleanly to a fresh skin object after rejoin.
    // ------------------------------------------------------------------
    std::printf("world exit: safe detach, no skin write, no double free\n");
    std::memcpy(skin.data(), skinSnapshot.data(), skin.size()); // heal the layout break
    mod.enabled = true;
    mod.onLocalPlayerTick(player.data());
    void* exitInjected = readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset);
    check(exitInjected != nullptr && exitInjected != capeBlob,
          "patch is in place before Leave World");

    // Step 1: the engine detaches the player from its level. A tick that
    // still reaches the hook during teardown must not touch the skin —
    // it may already be freed.
    void* noLevel = nullptr;
    std::memcpy(player.data() + off::Actor::mLevel, &noLevel, sizeof(void*));
    const std::vector<std::uint8_t> skinAtExit = skin;
    mod.onLocalPlayerTick(player.data());
    check(sameBytes(skinBase, skinAtExit.data(), skin.size()),
          "level gone (module on) -> no writes to the skin");

    // Step 2: the engine destroys the skin and frees our injected blob
    // through the deleter tag we wrote into mCapeImage — exactly what the
    // real skin destructor does.
    auto* engineDeleter = reinterpret_cast<void (*)(unsigned char*)>(
        readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBlobDeleterOffset));
    check(engineDeleter != nullptr && engineDeleter != originalDeleter,
          "skin image carries our free() deleter tag for the engine");
    engineDeleter(static_cast<unsigned char*>(exitInjected));

    // Step 3: worst case — the user had just disabled the module (restore
    // pending) when the world went away. A teardown tick must neither
    // restore the vanilla cape into the dead skin nor free the blob the
    // engine already released.
    mod.enabled = false;
    mod.onLocalPlayerTick(player.data());
    check(sameBytes(skinBase, skinAtExit.data(), skin.size()),
          "level gone (module off) -> no skin write, no double-free");

    // The hook also survives a null player pointer (defensive null-check).
    mod.onLocalPlayerTick(nullptr);
    check(sameBytes(skinBase, skinAtExit.data(), skin.size()),
          "null player -> still no writes to the skin");

    // Step 4: rejoin — fresh skin object, level link restored.
    std::memcpy(skin.data(), skinSnapshot.data(), skin.size());
    void* rejoinLevel = (void*)0x2;
    std::memcpy(player.data() + off::Actor::mLevel, &rejoinLevel, sizeof(void*));
    mod.enabled = true;
    mod.onLocalPlayerTick(player.data());
    void* rejoinInjected = readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset);
    check(rejoinInjected != nullptr && rejoinInjected != capeBlob,
          "cape re-applied to the fresh skin after rejoin");
    check(sameBytes(static_cast<const std::uint8_t*>(rejoinInjected), expectedCape.data(),
                    expectedCape.size()),
          "rejoin pixels are still the resampled cape");
    check(readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBlobDeleterOffset) !=
              originalDeleter,
          "rejoin blob is retagged so the engine can free it safely");

    // Tidy up: disable restores the vanilla cape and frees the fresh blob.
    mod.enabled = false;
    mod.onLocalPlayerTick(player.data());
    check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeImage,
                    skinSnapshot.data() + off::SerializedSkinImpl::mCapeImage,
                    off::Image::Size),
          "mCapeImage restored after rejoin/disable");

    std::printf("\n%s\n", g_failures == 0 ? "all custom capes patch tests passed"
                                          : "SOME PATCH TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
