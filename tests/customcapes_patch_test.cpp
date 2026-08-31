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
#include <bedrocktools/memory/Signatures.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
// Decoding only: the stb_image implementation is compiled by
// src/modules/player/customcapes.cpp in this same binary.
#include <stb/stb_image.h>

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

// Local player returned by the fake ClientInstance's getLocalPlayer vtable
// slot (file-scope so the capture-less stub lambda can see it).
void* g_clientPlayer = nullptr;

// Link stubs for the config backend the module queries for its folder.
} // namespace

namespace bedrocktools::config {
ConfigManager::~ConfigManager() = default;
std::string ConfigManager::getConfigPath() const { return g_testConfigPath; }
} // namespace bedrocktools::config

namespace bedrocktools::events {
// Link stub: onInit() subscribes through the event bus, which the host test
// never drives. The EventBus class itself is fully inline.
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::memory {
// Link stub: the module resolves the render-hook signatures in onInit().
// The host never has libminecraftpe.so, so every signature resolves to 0 and
// the overlay renderer stays inert — exactly like the Wings patch test.
std::uintptr_t resolve(SignatureId) { return 0; }
bool resolveAll(std::string_view) { return false; }
} // namespace bedrocktools::memory

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

    writeImage(skinBase + off::SerializedSkinImpl::mSkinImage, 3, 64, 64,
               skinBlob, originalDeleter, skinPixels.size());
    writeImage(skinBase + off::SerializedSkinImpl::mCapeImage, 3, 64, 32,
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
    check(readU32(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mImageFormat) == 3,
          "mCapeImage format is RGBA8Unorm (3)");
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
    check(capeId[0] == static_cast<std::uint8_t>(18u << 1) &&
              std::memcmp(capeId + 1, "bedrocktoolsplus-1", 18) == 0,
          "mCapeId (336) holds the short-string id \"bedrocktoolsplus-1\"");
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
    // Skin change in-place: the game can rebuild the contents of the same
    // SerializedSkinImpl while keeping its address. It owns/frees the old
    // injected blob during that rebuild. The next tick must not double-free
    // that stale pointer, and disabling afterwards must restore the new skin's
    // cape rather than the pre-change backup.
    // ------------------------------------------------------------------
    std::printf("skin change in-place: fresh backup, no stale free\n");
    mod.enabled = true;
    mod.onLocalPlayerTick(player.data());
    void* inPlaceOldInjected = readPtr(skinBase + off::SerializedSkinImpl::mCapeImage,
                                       off::Image::mBytesOffset);
    auto* inPlaceEngineDeleter = reinterpret_cast<void (*)(unsigned char*)>(
        readPtr(skinBase + off::SerializedSkinImpl::mCapeImage,
                off::Image::mBlobDeleterOffset));
    check(inPlaceOldInjected != nullptr && inPlaceOldInjected != capeBlob,
          "patch is in place before in-place skin change");

    std::vector<std::uint8_t> changedCapePixels(64 * 32 * 4, 0xEF);
    void* const changedCapeBlob = changedCapePixels.data();
    inPlaceEngineDeleter(static_cast<unsigned char*>(inPlaceOldInjected));
    writeImage(skinBase + off::SerializedSkinImpl::mCapeImage, 3, 64, 32,
               changedCapeBlob, originalDeleter, changedCapePixels.size());
    std::memset(skinBase + off::SerializedSkinImpl::mCapeId, 0, 24);

    mod.onLocalPlayerTick(player.data());
    void* inPlaceNewInjected = readPtr(skinBase + off::SerializedSkinImpl::mCapeImage,
                                       off::Image::mBytesOffset);
    check(inPlaceNewInjected != nullptr && inPlaceNewInjected != changedCapeBlob,
          "custom cape reapplied after in-place skin rebuild");
    check(sameBytes(static_cast<const std::uint8_t*>(inPlaceNewInjected), expectedCape.data(),
                    expectedCape.size()),
          "reapplied in-place pixels match the resampled cape");

    mod.enabled = false;
    mod.onLocalPlayerTick(player.data());
    check(readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset) ==
              changedCapeBlob,
          "disable restores the changed skin's own cape blob");
    check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeId,
                    skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24),
          "disable clears the synthetic cape id after in-place rebuild");
    std::memcpy(skin.data(), skinSnapshot.data(), skin.size());

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
    // ImageFormat must stay inside the engine enum
    // {Unknown=0, R8Unorm=1, RGB8Unorm=2, RGBA8Unorm=3}. A cape image whose
    // format is out of that range is either memory that no longer belongs to
    // an mce::Image or a value the texture factory rejects (the format 4 bug:
    // 4 was a naive "4 channels" guess, but the enum tops out at 3, so the
    // engine never built a cape texture from it). Either way the patch must
    // refuse to touch it.
    // ------------------------------------------------------------------
    std::printf("out-of-range ImageFormat aborts the patch; a real RGBA8 (3) cape is accepted\n");
    std::memcpy(skin.data(), skinSnapshot.data(), skin.size());
    const std::uint32_t invalidFormat = 4;
    std::memcpy(skinBase + off::SerializedSkinImpl::mCapeImage + off::Image::mImageFormat,
                &invalidFormat, 4);
    {
        const std::vector<std::uint8_t> before = skin;
        mod.enabled = true;
        mod.onLocalPlayerTick(player.data());
        check(sameBytes(skinBase, before.data(), skin.size()),
              "format 4 (out of enum range) -> cape image is not touched");
        check(readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset) == capeBlob,
              "format 4 -> original blob left in place");
        mod.enabled = false;
        mod.onLocalPlayerTick(player.data());
    }

    // A player who already owns a vanilla cape: its image is a perfectly valid
    // RGBA8Unorm (3) texture, and the custom cape must replace it (the module
    // must not mistake a real owned cape for a corrupt layout).
    std::memcpy(skin.data(), skinSnapshot.data(), skin.size());
    {
        mod.enabled = true;
        mod.onLocalPlayerTick(player.data());
        void* ownerInjected = readPtr(skinBase + off::SerializedSkinImpl::mCapeImage,
                                      off::Image::mBytesOffset);
        check(readU32(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mImageFormat) == 3,
              "owned-cape player: patched image stays RGBA8Unorm (3)");
        check(ownerInjected != nullptr && ownerInjected != capeBlob,
              "owned-cape player: custom cape pixels replace the vanilla cape");
        check(sameBytes(static_cast<const std::uint8_t*>(ownerInjected), expectedCape.data(),
                        expectedCape.size()),
              "owned-cape player: injected pixels are the resampled cape");
        mod.enabled = false;
        mod.onLocalPlayerTick(player.data());
    }

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

    // ------------------------------------------------------------------
    // A player who owns no cape at all — the case the module exists for.
    // Their mCapeImage is a default-constructed mce::Image: every field is
    // zero (format Unknown, 0x0, depth 0, usage Unknown, no blob). The
    // engine builds the cape texture from exactly those fields, so the
    // patch has to fill in all of them; leaving depth/usage/format at 0
    // hands the texture factory an image whose getSizeInBytes() is
    // w*h*depth*4 == 0 and no cape is ever drawn (the reported symptom:
    // the selected PNG never shows up on the player).
    // ------------------------------------------------------------------
    std::printf("capeless player gets a render-valid cape image\n");
    {
        std::vector<std::uint8_t> bare(off::SerializedSkinImpl::mIsPersonaCapeOnClassicSkin + 32, 0);
        std::uint8_t* const bareBase = bare.data();

        // The player's own skin texture: 64x64 RGBA8, depth 1, SRGB usage —
        // an image the engine is already rendering successfully.
        std::vector<std::uint8_t> bareSkinPixels(64 * 64 * 4, 0x77);
        writeImage(bareBase + off::SerializedSkinImpl::mSkinImage, 3, 64, 64,
                   bareSkinPixels.data(), originalDeleter, bareSkinPixels.size());
        const std::uint32_t skinUsage = 1;
        std::memcpy(bareBase + off::SerializedSkinImpl::mSkinImage + off::Image::mUsage,
                    &skinUsage, 4);

        // mCapeImage is left all zero: that is the capeless player.
        const std::vector<std::uint8_t> bareSnapshot = bare;

        std::vector<std::uint8_t> barePlayer(off::Player::mSkin + sizeof(void*), 0);
        void* bareSkinSlot = bareBase;
        void* bareSkinSlotAddress = &bareSkinSlot;
        std::memcpy(barePlayer.data() + off::Player::mSkin, &bareSkinSlotAddress, sizeof(void*));
        void* bareLevel = (void*)0x3;
        std::memcpy(barePlayer.data() + off::Actor::mLevel, &bareLevel, sizeof(void*));

        mod.enabled = true;
        mod.onLocalPlayerTick(barePlayer.data());

        std::uint8_t* const bareCape = bareBase + off::SerializedSkinImpl::mCapeImage;
        const std::uint32_t format = readU32(bareCape, off::Image::mImageFormat);
        const std::uint32_t capeW = readU32(bareCape, off::SkinImage::mWidth);
        const std::uint32_t capeH = readU32(bareCape, off::SkinImage::mHeight);
        const std::uint32_t depth = readU32(bareCape, off::Image::mDepth);
        const std::uint32_t usage = readU32(bareCape, off::Image::mUsage);
        const std::size_t blobSize = readSize(bareCape, off::Image::mBlobSizeOffset);
        void* const bareInjected = readPtr(bareCape, off::Image::mBytesOffset);

        check(bareInjected != nullptr, "cape pixels injected into the empty cape image");
        check(capeW == 64 && capeH == 32, "cape dimensions are 64x32");
        check(depth == 1,
              "cape depth is 1 (depth 0 makes the engine reject the texture)");
        check(format == 3,
              "cape pixel format is RGBA8Unorm (3) — value 4 is out of the "
              "engine enum and the texture factory silently drops it");
        check(usage != 0, "cape image usage is no longer Unknown");
        check(blobSize == static_cast<std::size_t>(capeW) * capeH * depth * 4u,
              "blob size == width*height*depth*4 (mce::Image::getSizeInBytes)");
        check(bareInjected != nullptr &&
                  sameBytes(static_cast<const std::uint8_t*>(bareInjected), expectedCape.data(),
                            expectedCape.size()),
              "injected pixels are the resampled cape");
        check(sameBytes(bareBase + off::SerializedSkinImpl::mSkinImage,
                        bareSnapshot.data() + off::SerializedSkinImpl::mSkinImage,
                        off::Image::Size),
              "mSkinImage untouched for a capeless player too");

        mod.enabled = false;
        mod.onLocalPlayerTick(barePlayer.data());
        check(sameBytes(bareCape, bareSnapshot.data() + off::SerializedSkinImpl::mCapeImage,
                        off::Image::Size),
              "the all-zero cape image is restored byte-for-byte on disable");
    }


    // ------------------------------------------------------------------
    // End to end on the module's own sample cape: onInit() creates the
    // capes folder and writes "Sample Cape.png", the menu radio value
    // selects it, and a capeless player must end up wearing a fully
    // opaque, non-empty cape. This is the path reported as "the PNG does
    // not show on the player".
    // ------------------------------------------------------------------
    std::printf("generated Sample Cape.png reaches the player's skin\n");
    {
        const std::string sampleRoot = "/tmp/bt-customcapes-sample-test";
        std::filesystem::remove_all(sampleRoot);
        std::filesystem::create_directories(sampleRoot);
        g_testConfigPath = sampleRoot + "/config.json";

        CustomCapesModule sampleMod;
        sampleMod.onInit(); // creates <configDir>/capes and the sample PNG

        const std::vector<std::string> listed =
            customcapes::scanCapeFiles(sampleRoot + "/capes");
        check(listed.size() == 1 && listed[0] == "Sample Cape.png",
              "onInit() creates a selectable Sample Cape.png");

        // The value the launcher radio picker hands back.
        nlohmann::json sampleCfg;
        sampleCfg["m_cape"] = customcapes::makeRadioValue(1, listed);
        sampleMod.loadConfig(sampleCfg);

        std::vector<std::uint8_t> bare(off::SerializedSkinImpl::mIsPersonaCapeOnClassicSkin + 32, 0);
        std::uint8_t* const bareBase = bare.data();
        std::vector<std::uint8_t> bareSkinPixels(64 * 64 * 4, 0x77);
        writeImage(bareBase + off::SerializedSkinImpl::mSkinImage, 3, 64, 64,
                   bareSkinPixels.data(), originalDeleter, bareSkinPixels.size());

        std::vector<std::uint8_t> barePlayer(off::Player::mSkin + sizeof(void*), 0);
        void* bareSkinSlot = bareBase;
        void* bareSkinSlotAddress = &bareSkinSlot;
        std::memcpy(barePlayer.data() + off::Player::mSkin, &bareSkinSlotAddress, sizeof(void*));
        void* bareLevel = (void*)0x4;
        std::memcpy(barePlayer.data() + off::Actor::mLevel, &bareLevel, sizeof(void*));

        sampleMod.enabled = true;
        sampleMod.onLocalPlayerTick(barePlayer.data());

        std::uint8_t* const sampleCape = bareBase + off::SerializedSkinImpl::mCapeImage;
        auto* sampleInjected =
            static_cast<const std::uint8_t*>(readPtr(sampleCape, off::Image::mBytesOffset));
        check(sampleInjected != nullptr, "sample cape pixels are injected into the skin");
        check(readU32(sampleCape, off::Image::mImageFormat) == 3,
              "injected sample cape uses RGBA8Unorm (3), the engine's RGBA format");
        check(readU32(sampleCape, off::Image::mDepth) == 1 &&
                  readU32(sampleCape, off::SkinImage::mWidth) == 64 &&
                  readU32(sampleCape, off::SkinImage::mHeight) == 32,
              "injected sample cape describes a 64x32 depth-1 texture");

        if (sampleInjected != nullptr) {
            // The visible outer face is x=1..11, y=1..17 of the 64x32 canvas.
            std::size_t opaque = 0;
            std::size_t distinct = 0;
            std::uint64_t seen = 0;
            for (std::uint32_t y = 1; y <= 16; ++y) {
                for (std::uint32_t x = 1; x <= 10; ++x) {
                    const std::uint8_t* px = sampleInjected + (y * 64 + x) * 4;
                    if (px[3] != 0) ++opaque;
                    const std::uint32_t key = (px[0] / 16) | ((px[1] / 16) << 4) |
                                              ((px[2] / 16) << 8);
                    if (!((seen >> key) & 1u)) {
                        seen |= 1ull << key;
                        ++distinct;
                    }
                }
            }
            check(opaque == 160, "every pixel of the cape's outer face is opaque");
            check(distinct >= 4, "the outer face carries real artwork, not one flat color");
        }

        sampleMod.enabled = false;
        sampleMod.onLocalPlayerTick(barePlayer.data());
    }

    // ------------------------------------------------------------------
    // Early patch via ClientInstanceUpdateEvent (the KNOWN LIMITATION fix).
    // ClientInstance::update runs once per frame BEFORE the level render
    // pass; on the first frame after the local player is created this hook
    // patches the cape into SerializedSkinImpl before ActorRendererData is
    // built from the skin, so the engine creates a real cape mesh. The hook
    // is just a vtable dispatch to the local player followed by the same
    // guarded tick logic — this test drives the fake client through it.
    // ------------------------------------------------------------------
    std::printf("ClientInstanceUpdate early patch reaches the skin\n");
    {
        // Fake ClientInstance: object[0] holds the vtable pointer;
        // vtable[VTable::ClientInstanceGetLocalPlayer] returns the player.
        void* clientVtable[64] = {};
        clientVtable[off::VTable::ClientInstanceGetLocalPlayer] =
            reinterpret_cast<void*>(+[](void*) -> void* { return g_clientPlayer; });
        void* clientStorage[2] = {nullptr, nullptr};
        clientStorage[0] = clientVtable;

        // Capeless player, exactly like the standalone capeless test above.
        std::vector<std::uint8_t> earlySkin(off::SerializedSkinImpl::mIsPersonaCapeOnClassicSkin + 32, 0);
        std::uint8_t* const earlyBase = earlySkin.data();
        std::vector<std::uint8_t> earlySkinPixels(64 * 64 * 4, 0x66);
        writeImage(earlyBase + off::SerializedSkinImpl::mSkinImage, 3, 64, 64,
                   earlySkinPixels.data(), originalDeleter, earlySkinPixels.size());
        const std::uint32_t earlySkinUsage = 1;
        std::memcpy(earlyBase + off::SerializedSkinImpl::mSkinImage + off::Image::mUsage,
                    &earlySkinUsage, 4);
        // mCapeImage all zero = the capeless case.
        const std::vector<std::uint8_t> earlySnapshot = earlySkin;

        std::vector<std::uint8_t> earlyPlayer(off::Player::mSkin + sizeof(void*), 0);
        void* earlySkinSlot = earlyBase;
        void* earlySkinSlotAddress = &earlySkinSlot;
        std::memcpy(earlyPlayer.data() + off::Player::mSkin, &earlySkinSlotAddress, sizeof(void*));
        void* earlyLevel = (void*)0x5;
        std::memcpy(earlyPlayer.data() + off::Actor::mLevel, &earlyLevel, sizeof(void*));

        CustomCapesModule earlyMod;
        nlohmann::json earlyCfg;
        earlyCfg["m_cape"] = 1;
        earlyMod.loadConfig(earlyCfg);
        earlyMod.enabled = true;

        // No player yet (menus): the hook must be a safe no-op.
        g_clientPlayer = nullptr;
        earlyMod.onClientInstanceUpdate((void*)clientStorage);
        check(sameBytes(earlyBase, earlySnapshot.data(), earlySkin.size()),
              "client update with no local player -> no skin writes");

        // First frame with the player: patch lands before the render pass.
        g_clientPlayer = earlyPlayer.data();
        earlyMod.onClientInstanceUpdate((void*)clientStorage);

        std::uint8_t* const earlyCape = earlyBase + off::SerializedSkinImpl::mCapeImage;
        check(readU32(earlyCape, off::Image::mImageFormat) == 3,
              "client update patch writes RGBA8Unorm (3) into the empty cape image");
        check(readU32(earlyCape, off::SkinImage::mWidth) == 64 &&
                  readU32(earlyCape, off::SkinImage::mHeight) == 32,
              "client update patch fills 64x32 dimensions");
        check(readU32(earlyCape, off::Image::mDepth) == 1,
              "client update patch sets depth 1 (renderable texture)");
        check(readPtr(earlyCape, off::Image::mBytesOffset) != nullptr,
              "client update patch injects cape pixels");
        check(sameBytes(earlyBase + off::SerializedSkinImpl::mSkinImage,
                        earlySnapshot.data() + off::SerializedSkinImpl::mSkinImage,
                        off::Image::Size),
              "client update patch never touches mSkinImage");

        // Second frame: idempotent, no churn.
        void* const earlyInjected = readPtr(earlyCape, off::Image::mBytesOffset);
        earlyMod.onClientInstanceUpdate((void*)clientStorage);
        check(readPtr(earlyCape, off::Image::mBytesOffset) == earlyInjected,
              "client update second frame keeps the same blob (no churn)");

        // A null client instance is a safe no-op too.
        earlyMod.onClientInstanceUpdate(nullptr);
        check(readPtr(earlyCape, off::Image::mBytesOffset) == earlyInjected,
              "null client instance -> patch stays intact");

        // Disabled modules are skipped by the per-frame client hook (it only
        // exists to land the patch before the mesh is built); the per-tick
        // hook still restores the vanilla cape on the next tick.
        earlyMod.enabled = false;
        earlyMod.onClientInstanceUpdate((void*)clientStorage);
        check(readPtr(earlyCape, off::Image::mBytesOffset) == earlyInjected,
              "disabled module is skipped by the client hook (no churn)");
        earlyMod.onLocalPlayerTick(earlyPlayer.data());
        check(sameBytes(earlyCape, earlySnapshot.data() + off::SerializedSkinImpl::mCapeImage,
                        off::Image::Size),
              "disabling restores the vanilla cape on the next tick");
    }

    // ------------------------------------------------------------------
    // Overlay pose publishing crash fix:
    //   * The pose (AABB + body yaw) is read ONLY while the overlay is
    //     actually drawing (enabled + a cape selected + Overlay/Both). A
    //     disabled module on world entry must never touch Actor::mStateVector
    //     (0x208) / +0x210 / 0x218.
    //   * When it is read, it uses the Wings/Hitbox/Breadcrumbs layout:
    //     the AABB-component pointer comes from actor+0x210 and the rotation
    //     from actor+0x218, not by dereferencing 0x208.
    // ------------------------------------------------------------------
    std::printf("overlay pose is gated and uses the Wings actor layout\n");
    {
        std::vector<std::uint8_t> overlaySkin(off::SerializedSkinImpl::mIsPersonaCapeOnClassicSkin + 32, 0);
        std::uint8_t* const overlayBase = overlaySkin.data();
        std::vector<std::uint8_t> overlaySkinPixels(64 * 64 * 4, 0x44);
        writeImage(overlayBase + off::SerializedSkinImpl::mSkinImage, 3, 64, 64,
                   overlaySkinPixels.data(), originalDeleter, overlaySkinPixels.size());
        const std::uint32_t overlaySkinUsage = 1;
        std::memcpy(overlayBase + off::SerializedSkinImpl::mSkinImage + off::Image::mUsage,
                    &overlaySkinUsage, 4);
        // mCapeImage all zero = the capeless case (like the early-patch test).

        // Fake LocalPlayer positioned past both the skin pointer (Player::mSkin)
        // and the actor layout offsets used by readActorPose.
        const std::size_t playerSize = 0x220 + 32;
        std::vector<std::uint8_t> overlayPlayer(playerSize, 0);
        void* overlaySkinSlot = overlayBase;
        void* overlaySkinSlotAddress = &overlaySkinSlot;
        std::memcpy(overlayPlayer.data() + off::Player::mSkin, &overlaySkinSlotAddress,
                    sizeof(void*));
        void* overlayLevel = (void*)0x7;
        std::memcpy(overlayPlayer.data() + off::Actor::mLevel, &overlayLevel, sizeof(void*));

        // 0x210 deliberately points at invalid low memory. With the module
        // disabled the tick must not even look there; the old code read the
        // pose unconditionally on every tick and crashed at world entry.
        void* const invalidAabbPtr = (void*)0x1;
        const std::uintptr_t builtInValue = 0x12345678u;  // distinct from invalid memory
        std::memcpy(overlayPlayer.data() + off::Actor::mStateVectorComponent,
                    &builtInValue, sizeof(std::uintptr_t));
        std::memcpy(overlayPlayer.data() + off::Actor::mStateVectorComponent +
                        off::BuiltInActorComponents::mAABBShapeComponent,
                    &invalidAabbPtr, sizeof(void*));

        CustomCapesModule overlayMod;
        overlayMod.enabled = false;
        overlayMod.onLocalPlayerTick(overlayPlayer.data());
        check(true, "disabled module on world entry does not dereference actor offsets");

        // Now give it a correct Wings-style layout and drive the real overlay
        // path: AABB at actor+0x210, rotation at actor+0x218.
        float aabb[6] = {1.0f, 2.0f, 3.0f, 2.0f, 4.0f, 4.0f};
        float rot[2] = {0.0f, 90.0f};  // {pitch, yaw}
        void* aabbPtr = aabb;
        void* rotPtr = rot;
        std::memcpy(overlayPlayer.data() + off::Actor::mStateVectorComponent +
                        off::BuiltInActorComponents::mAABBShapeComponent,
                    &aabbPtr, sizeof(void*));
        std::memcpy(overlayPlayer.data() + off::Actor::mActorRotationComponent,
                    &rotPtr, sizeof(void*));

        const std::string overlayRoot = "/tmp/bt-customcapes-overlay-pose-test";
        std::filesystem::remove_all(overlayRoot);
        std::filesystem::create_directories(overlayRoot + "/capes");
        g_testConfigPath = overlayRoot + "/config.json";
        stbi_write_png((overlayRoot + "/capes/Pose.png").c_str(), kSrcW, kSrcH, 4,
                       red.data(), kSrcW * 4);

        nlohmann::json overlayCfg;
        overlayCfg["m_cape"] = 1;
        overlayCfg["m_capeRenderMode"] = std::string("1,Engine mesh,Overlay,Both");
        overlayMod.loadConfig(overlayCfg);
        check(overlayMod.renderMode() == CustomCapesModule::RenderModeOverlay,
              "overlay-pose module selected Overlay mode");
        overlayMod.enabled = true;
        overlayMod.onLocalPlayerTick(overlayPlayer.data());

        // Disabled/restore path still works after the overlay tick.
        overlayMod.enabled = false;
        overlayMod.onLocalPlayerTick(overlayPlayer.data());
        check(sameBytes(overlayBase + off::SerializedSkinImpl::mCapeImage,
                        overlaySkin.data() + off::SerializedSkinImpl::mCapeImage,
                        off::Image::Size),
              "overlay mode leaves the cape image untouched after pose tick");
    }

    // ------------------------------------------------------------------
    // Render mode config: "0,Engine mesh,Overlay,Both" radio.
    //   * RenderMode Overlay (1) must leave the skin untouched while the
    //     self-rendered cape draws in the RenderLevel hook.
    //   * Switching back to Engine mesh (0) re-applies the skin patch.
    //   * saveConfig round-trips the radio value.
    // ------------------------------------------------------------------
    std::printf("render mode config (radio) drives the skin patch\n");
    {
        std::vector<std::uint8_t> modeSkin(off::SerializedSkinImpl::mIsPersonaCapeOnClassicSkin + 32, 0);
        std::uint8_t* const modeBase = modeSkin.data();
        std::vector<std::uint8_t> modeSkinPixels(64 * 64 * 4, 0x55);
        writeImage(modeBase + off::SerializedSkinImpl::mSkinImage, 3, 64, 64,
                   modeSkinPixels.data(), originalDeleter, modeSkinPixels.size());
        const std::uint32_t modeSkinUsage = 1;
        std::memcpy(modeBase + off::SerializedSkinImpl::mSkinImage + off::Image::mUsage,
                    &modeSkinUsage, 4);
        const std::vector<std::uint8_t> modeSnapshot = modeSkin;

        std::vector<std::uint8_t> modePlayer(off::Player::mSkin + sizeof(void*), 0);
        void* modeSkinSlot = modeBase;
        void* modeSkinSlotAddress = &modeSkinSlot;
        std::memcpy(modePlayer.data() + off::Player::mSkin, &modeSkinSlotAddress, sizeof(void*));
        void* modeLevel = (void*)0x6;
        std::memcpy(modePlayer.data() + off::Actor::mLevel, &modeLevel, sizeof(void*));

        CustomCapesModule modeMod;
        nlohmann::json modeCfg;
        modeCfg["m_cape"] = 1;
        modeCfg["m_capeRenderMode"] = std::string("1,Engine mesh,Overlay,Both");
        modeMod.loadConfig(modeCfg);
        check(modeMod.renderMode() == CustomCapesModule::RenderModeOverlay,
              "radio value 1 selects Overlay render mode");

        modeMod.enabled = true;
        modeMod.onLocalPlayerTick(modePlayer.data());
        check(sameBytes(modeBase + off::SerializedSkinImpl::mCapeImage,
                        modeSnapshot.data() + off::SerializedSkinImpl::mCapeImage,
                        off::Image::Size),
              "Overlay mode leaves mCapeImage untouched (skin stays vanilla)");
        check(sameBytes(modeBase + off::SerializedSkinImpl::mCapeId,
                        modeSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24),
              "Overlay mode leaves mCapeId untouched");

        // A plain integer value (older configs) is accepted and clamped.
        modeCfg["m_capeRenderMode"] = 7;
        modeMod.loadConfig(modeCfg);
        check(modeMod.renderMode() == CustomCapesModule::RenderModeEngine,
              "out-of-range integer render mode falls back to Engine mesh");

        // Engine mesh re-applies the skin patch.
        modeMod.onLocalPlayerTick(modePlayer.data());
        std::uint8_t* const modeCape = modeBase + off::SerializedSkinImpl::mCapeImage;
        check(readU32(modeCape, off::Image::mImageFormat) == 3,
              "Engine mode re-applies the cape patch after Overlay");
        check(readPtr(modeCape, off::Image::mBytesOffset) != nullptr,
              "Engine mode injects cape pixels again");

        // Radio round-trip via saveConfig.
        nlohmann::json saved;
        modeMod.saveConfig(saved);
        const std::string radio = saved["m_capeRenderMode"].get<std::string>();
        int parsedIndex = -1;
        std::string parsedName;
        customcapes::parseRadioValue(radio, parsedIndex, parsedName);
        check(parsedIndex == 0 && parsedName == "Engine mesh",
              "saveConfig writes \"0,Engine mesh,Overlay,Both\" (default mode)");

        modeMod.enabled = false;
        modeMod.onLocalPlayerTick(modePlayer.data());
    }

    std::printf("\n%s\n", g_failures == 0 ? "all custom capes patch tests passed"
                                          : "SOME PATCH TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
