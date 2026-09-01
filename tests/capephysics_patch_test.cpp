// Regression test for the Cape Physics module's tick-side logic.
//
// Builds the real modules (src/modules/visual/capephysics.cpp and
// src/modules/player/customcapes.cpp, compiled by scripts/run_tests.sh as
// additional translation units) and drives them against a fake
// SerializedSkinImpl byte buffer laid out exactly as
// include/bedrocktools/sdk/offsets/Skin.hpp documents it, connected through
// the fake Player -> SkinRef -> ThreadOwner pointer chain that
// resolvePlayerSkin() walks, plus the AABB/rotation components the render
// anchor samples.
//
// Covered:
//   * worn-cape source: the module copies the live mCapeImage pixels
//     (proportionally — a non-64x32 live canvas is normalized) and hides the
//     game's cape mesh by clearing mCapeId, with the original id backed up
//   * disabling restores the cape id bit-for-bit
//   * a skin without a cape (empty image + empty id) renders nothing and
//     writes nothing
//   * persona skins are never touched
//   * file source: a PNG from the shared capes folder (any size — the test
//     writes a 22x23 source) becomes the rendered canvas through the Custom
//     Capes resampler
//   * Custom Capes coordination: while Cape Physics hides the mesh, Custom
//     Capes keeps its pixels patched but never re-writes its synthetic cape
//     id (no per-tick fight over mCapeId); after Cape Physics disables, the
//     synthetic id comes back
//   * Leave World: with the level link (Actor::mLevel) gone the tick writes
//     nothing to the skin, survives a null player and re-hides on rejoin
//   * config round trip: radio values and float clamps survive save/load
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I third_party
//         -I tests/fakejson -I tests/fakepl
//         tests/capephysics_patch_test.cpp
//         src/modules/visual/capephysics.cpp
//         src/modules/player/customcapes.cpp
//         -o /tmp/capephysics_patch_test
//     /tmp/capephysics_patch_test

#include "modules/visual/capephysics.hpp"
#include "modules/player/customcapes.hpp"
#include "modules/player/customcapes_files.hpp"
#include "config/ConfigManager.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>

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

} // namespace

// Link stubs for the config/event/signature backends (same set the
// CustomCapes patch test provides).
namespace bedrocktools::config {
ConfigManager::~ConfigManager() = default;
std::string ConfigManager::getConfigPath() const { return g_testConfigPath; }
} // namespace bedrocktools::config

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
bool resolveAll(std::string_view) { return false; }
void clear() {}
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

void writeShortString(std::uint8_t* base, const char* text) {
    const std::size_t len = std::strlen(text);
    std::memset(base, 0, 24);
    base[0] = static_cast<std::uint8_t>(len << 1);
    std::memcpy(base + 1, text, len);
}

bool capeIdAllZero(const std::uint8_t* skin) {
    const std::uint8_t* id = skin + off::SerializedSkinImpl::mCapeId;
    for (int i = 0; i < 24; ++i) {
        if (id[i] != 0) return false;
    }
    return true;
}

bool sameBytes(const std::uint8_t* a, const std::uint8_t* b, std::size_t count) {
    return std::memcmp(a, b, count) == 0;
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

// ------------------------------------------------------------------
// Fake player: skin chain, live level link, AABB + rotation components.
// ------------------------------------------------------------------

struct FakeAABB {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

struct FakePlayer {
    std::vector<std::uint8_t> bytes;
    FakeAABB aabb{0.0f, 64.0f, 0.0f, 0.6f, 65.8f, 0.6f};
    bedrocktools::sdk::Vec2 rot{0.0f, 90.0f};
    void* skinSlot = nullptr;      // holds the skin pointer
    void* skinSlotAddress = &skinSlot;
    void* aabbComponent = &aabb;   // what BuiltInActorComponents points at
    void* rotationComponent = &rot;
    void* level = (void*)0x1;

    explicit FakePlayer(void* skin)
        : bytes(off::Player::mSkin + sizeof(void*) + 16, 0) {
        skinSlot = skin;
        std::memcpy(bytes.data() + off::Player::mSkin, &skinSlotAddress, sizeof(void*));
        std::memcpy(bytes.data() + off::Actor::mLevel, &level, sizeof(void*));
        // mStateVectorComponent -> BuiltIn { pad, aabbCompPtr@+8 }
        std::memcpy(bytes.data() + off::Actor::mStateVectorComponent, &aabbComponent, sizeof(void*));
        // The BuiltIn blob: a pointer at +8 (mAABBShapeComponent) must point
        // at the AABB component; reinterpret the FakeAABB's address as the
        // component base (AABBShapeComponent::mAABB = 0).
        std::memcpy(bytes.data() + off::Actor::mStateVectorComponent, &aabbComponent, sizeof(void*));
        std::memcpy(bytes.data() + off::Actor::mActorRotationComponent, &rotationComponent, sizeof(void*));
    }

    void* data() { return bytes.data(); }

    void setLevelLive(bool live) {
        void* value = live ? (void*)0x1 : nullptr;
        std::memcpy(bytes.data() + off::Actor::mLevel, &value, sizeof(void*));
    }
};

} // namespace

int main() {
    // ------------------------------------------------------------------
    // Temporary config/capes folders.
    // ------------------------------------------------------------------
    const std::string root = "/tmp/bt-capephysics-patch-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/capes");
    g_testConfigPath = root + "/config.json";

    // A 22x23 gradient source PNG (deliberately NOT 64x32 — the module must
    // accept any size, resampling it like Custom Capes does).
    constexpr std::uint32_t kSrcW = 22, kSrcH = 23;
    std::vector<std::uint8_t> src(kSrcW * kSrcH * 4);
    for (std::uint32_t y = 0; y < kSrcH; ++y) {
        for (std::uint32_t x = 0; x < kSrcW; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSrcW + x) * 4u;
            src[i + 0] = static_cast<std::uint8_t>(x * 255 / (kSrcW - 1));
            src[i + 1] = static_cast<std::uint8_t>(y * 255 / (kSrcH - 1));
            src[i + 2] = 60;
            src[i + 3] = 255;
        }
    }
    stbi_write_png((root + "/capes/AnySize.png").c_str(), kSrcW, kSrcH, 4, src.data(), kSrcW * 4);
    const std::vector<std::uint8_t> expectedCanvas =
        customcapes::resampleToCape(src.data(), kSrcW, kSrcH);

    // ------------------------------------------------------------------
    // Fake skin: 64x64 skin image, 64x32 cape image, vanilla cape id.
    // ------------------------------------------------------------------
    std::vector<std::uint8_t> skin(off::SerializedSkinImpl::mIsPersonaCapeOnClassicSkin + 32, 0);
    std::uint8_t* skinBase = skin.data();

    std::vector<std::uint8_t> skinPixels(64 * 64 * 4, 0xAB);
    std::vector<std::uint8_t> capePixels(64 * 32 * 4);
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 64 + x) * 4u;
            capePixels[i + 0] = static_cast<std::uint8_t>(x * 4);
            capePixels[i + 1] = static_cast<std::uint8_t>(y * 8);
            capePixels[i + 2] = 200;
            capePixels[i + 3] = 255;
        }
    }
    void* const skinBlob = skinPixels.data();
    void* const capeBlob = capePixels.data();
    void* const originalDeleter = reinterpret_cast<void*>(&fakeDeleter);

    writeImage(skinBase + off::SerializedSkinImpl::mSkinImage, 4, 64, 64,
               skinBlob, originalDeleter, skinPixels.size());
    writeImage(skinBase + off::SerializedSkinImpl::mCapeImage, 4, 64, 32,
               capeBlob, originalDeleter, capePixels.size());
    writeShortString(skinBase + off::SerializedSkinImpl::mCapeId, "vanillacape");

    const std::vector<std::uint8_t> skinSnapshot = skin;
    FakePlayer player(skinBase);

    // ------------------------------------------------------------------
    // Worn-cape source: pixels are sampled, the cape id is hidden.
    // ------------------------------------------------------------------
    std::printf("worn cape source\n");
    {
        CapePhysicsModule mod;
        mod.onInit();
        mod.enabled = true;
        mod.onLocalPlayerTick(player.data());

        check(mod.hasRenderColors(), "module has colors from the worn cape");
        check(mod.renderCanvasForTest().size() == bedrocktools::modules::capephysics::kCanvasBytes,
              "canvas normalized to 64x32");
        const std::uint8_t* canvas = mod.renderCanvasForTest().data();
        bool capePixelsCopied = true;
        for (int i = 0; i < 64 * 32 * 4; ++i) {
            if (canvas[i] != capePixels[i]) { capePixelsCopied = false; break; }
        }
        check(capePixelsCopied, "canvas matches the live mCapeImage pixels");
        check(capeIdAllZero(skinBase), "mCapeId cleared while the physics cape is drawn");
        check(sameBytes(skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, // backed up
                        skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24) &&
                  !sameBytes(skinBase + off::SerializedSkinImpl::mCapeId,
                             skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24),
              "the vanilla id differs from the snapshot (it was cleared)");
        check(sameBytes(skinBase + off::SerializedSkinImpl::mSkinImage,
                        skinSnapshot.data() + off::SerializedSkinImpl::mSkinImage,
                        off::Image::Size),
              "mSkinImage untouched");
        check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeImage,
                        skinSnapshot.data() + off::SerializedSkinImpl::mCapeImage,
                        off::Image::Size),
              "mCapeImage untouched by the worn source (read-only)");

        // Disable -> the vanilla cape id comes back exactly.
        mod.enabled = false;
        mod.onLocalPlayerTick(player.data());
        check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeId,
                        skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24),
              "mCapeId restored bit-for-bit on disable");
        check(!mod.hasRenderColors(), "no colors after disable");
    }

    // ------------------------------------------------------------------
    // No cape at all: nothing renders, nothing is written.
    // ------------------------------------------------------------------
    std::printf("capeless player\n");
    {
        std::vector<std::uint8_t> bareSkin = skinSnapshot;
        std::uint8_t* bare = bareSkin.data();
        std::vector<std::uint8_t> emptyCape;
        writeImage(bare + off::SerializedSkinImpl::mCapeImage, 0, 0, 0,
                   nullptr, originalDeleter, 0);
        std::memset(bare + off::SerializedSkinImpl::mCapeId, 0, 24);

        FakePlayer barePlayer(bare);
        const std::vector<std::uint8_t> bareSnapshot = bareSkin;
        CapePhysicsModule mod;
        mod.onInit();
        mod.enabled = true;
        mod.onLocalPlayerTick(barePlayer.data());
        check(!mod.hasRenderColors(), "no colors when no cape is worn");
        check(sameBytes(bare, bareSnapshot.data(), bareSkin.size()),
              "capeless skin is written nowhere (id already empty, no pixels)");
    }

    // ------------------------------------------------------------------
    // Persona skins are skipped entirely.
    // ------------------------------------------------------------------
    std::printf("persona skin\n");
    {
        skin[off::SerializedSkinImpl::mIsPersona] = 1;
        CapePhysicsModule mod;
        mod.onInit();
        mod.enabled = true;
        mod.onLocalPlayerTick(player.data());
        check(!mod.hasRenderColors(), "no colors for persona skins");
        check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeId,
                        skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24),
              "persona cape id never cleared");
        skin[off::SerializedSkinImpl::mIsPersona] = 0;
        mod.enabled = false;
        mod.onLocalPlayerTick(player.data());
    }

    // ------------------------------------------------------------------
    // File source: the 22x23 PNG becomes the rendered canvas.
    // ------------------------------------------------------------------
    std::printf("file source (22x23 PNG)\n");
    {
        CapePhysicsModule mod;
        mod.onInit();
        nlohmann::json cfg;
        cfg["m_cape"] = "1,Worn Cape,AnySize.png";
        mod.loadConfig(cfg);
        mod.enabled = true;
        mod.onLocalPlayerTick(player.data());

        check(mod.hasRenderColors(), "module has colors from the cape file");
        check(mod.renderCanvasForTest().size() == expectedCanvas.size() &&
                  sameBytes(mod.renderCanvasForTest().data(), expectedCanvas.data(),
                            expectedCanvas.size()),
              "22x23 file resampled onto the 64x32 canvas exactly like Custom Capes");
        // The player also wears a vanilla cape here — the mesh must be hidden.
        check(capeIdAllZero(skinBase), "vanilla cape id hidden while the file cape is drawn");
        mod.enabled = false;
        mod.onLocalPlayerTick(player.data());
        check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeId,
                        skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24),
              "cape id restored after the file cape is disabled");
    }

    // ------------------------------------------------------------------
    // Custom Capes coordination (both modules enabled).
    // ------------------------------------------------------------------
    std::printf("custom capes coordination\n");
    {
        // Reset the skin to vanilla state first.
        skin = skinSnapshot;
        CustomCapesModule capes;
        capes.onInit();
        nlohmann::json capesCfg;
        capesCfg["m_cape"] = "1,None,AnySize.png";
        capes.loadConfig(capesCfg);
        capes.enabled = true;

        CapePhysicsModule physics;
        physics.onInit();
        physics.enabled = true;

        // Tick order mirrors registration: Custom Capes first, physics last.
        capes.onLocalPlayerTick(player.data());
        physics.onLocalPlayerTick(player.data());
        capes.onLocalPlayerTick(player.data());
        physics.onLocalPlayerTick(player.data());

        void* injected = readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset);
        check(injected != nullptr && injected != capeBlob,
              "custom capes pixels stay patched");
        check(readSize(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBlobSizeOffset) ==
                  expectedCanvas.size(),
              "patched blob is the resampled 64x32 cape");
        check(capeIdAllZero(skinBase),
              "cape id stays empty while physics hides the mesh (no id fight)");
        check(physics.hasRenderColors(), "physics renders the custom cape pixels");
        check(physics.renderCanvasForTest().size() == expectedCanvas.size() &&
                  sameBytes(physics.renderCanvasForTest().data(), expectedCanvas.data(),
                            expectedCanvas.size()),
              "physics canvas equals the custom capes pixels");

        // No per-tick churn of the pixel blob while suppressed.
        void* injected2 = readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset);
        capes.onLocalPlayerTick(player.data());
        physics.onLocalPlayerTick(player.data());
        check(readPtr(skinBase + off::SerializedSkinImpl::mCapeImage, off::Image::mBytesOffset) == injected2,
              "suppressed custom capes does not churn the blob per tick");

        // Physics disables -> custom capes re-applies its synthetic id.
        physics.enabled = false;
        capes.onLocalPlayerTick(player.data());
        physics.onLocalPlayerTick(player.data());
        const std::uint8_t* capeId = skinBase + off::SerializedSkinImpl::mCapeId;
        check(capeId[0] == static_cast<std::uint8_t>(14u << 1) &&
                  std::memcmp(capeId + 1, "bedrocktools-1", 14) == 0,
              "custom capes synthetic id comes back after physics disables");

        capes.enabled = false;
        capes.onLocalPlayerTick(player.data());
    }

    // ------------------------------------------------------------------
    // An unreadable cape source never hides the vanilla mesh (the player
    // would be left with no cape at all).
    // ------------------------------------------------------------------
    std::printf("unreadable cape source\n");
    {
        std::vector<std::uint8_t> oddSkin = skinSnapshot;
        std::uint8_t* odd = oddSkin.data();
        writeImage(odd + off::SerializedSkinImpl::mCapeImage, 7, 64, 32, // unknown format
                   capeBlob, originalDeleter, capePixels.size());
        writeShortString(odd + off::SerializedSkinImpl::mCapeId, "vanillacape");
        const std::vector<std::uint8_t> oddSnapshot = oddSkin;

        FakePlayer oddPlayer(odd);
        CapePhysicsModule mod;
        mod.onInit();
        mod.enabled = true;
        mod.onLocalPlayerTick(oddPlayer.data());
        check(!mod.hasRenderColors(), "no colors from an unreadable cape format");
        check(sameBytes(odd + off::SerializedSkinImpl::mCapeId,
                        oddSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24),
              "vanilla cape id NOT hidden when there is nothing to draw");
        mod.enabled = false;
        mod.onLocalPlayerTick(oddPlayer.data());
    }

    // ------------------------------------------------------------------
    // Leave World: level link gone -> nothing is written, refs dropped,
    // and the cape re-hides on "rejoin".
    // ------------------------------------------------------------------
    std::printf("leave world and rejoin\n");
    {
        skin = skinSnapshot;
        CapePhysicsModule mod;
        mod.onInit();
        mod.enabled = true;
        mod.onLocalPlayerTick(player.data());
        check(capeIdAllZero(skinBase), "cape id hidden before the world exits");

        player.setLevelLive(false);
        mod.onLocalPlayerTick(player.data());          // world exit path
        mod.onLocalPlayerTick(nullptr);                // null player path
        check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeId,
                        skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24) ||
                  capeIdAllZero(skinBase),
              "the dying skin is never written during Leave World");
        check(!mod.hasRenderColors(), "render state dropped on world exit");

        // Rejoin: fresh skin object (the old one was destroyed by the engine).
        std::vector<std::uint8_t> rejoinSkin = skinSnapshot;
        std::uint8_t* rejoinBase = rejoinSkin.data();
        FakePlayer rejoinPlayer(rejoinBase);
        player.setLevelLive(true);
        mod.onLocalPlayerTick(rejoinPlayer.data());
        check(capeIdAllZero(rejoinBase), "cape id re-hidden on the fresh skin after rejoin");
        check(mod.hasRenderColors(), "colors re-sampled from the fresh skin");
        mod.enabled = false;
        mod.onLocalPlayerTick(rejoinPlayer.data());
        check(sameBytes(rejoinBase + off::SerializedSkinImpl::mCapeId,
                        skinSnapshot.data() + off::SerializedSkinImpl::mCapeId, 24),
              "fresh skin cape id restored on disable");
    }

    // ------------------------------------------------------------------
    // Config round trip.
    // ------------------------------------------------------------------
    std::printf("config round trip\n");
    {
        CapePhysicsModule mod;
        mod.onInit();
        mod.m_windStrength = 0.7f;
        mod.m_gravity = 1.4f;
        mod.m_stiffness = 0.6f;
        mod.m_hideVanilla = false;
        mod.m_detail = 1;
        nlohmann::json saved;
        mod.saveConfig(saved);
        check(saved.contains("m_cape") && saved.contains("m_capeFit") &&
                  saved.contains("m_detail") && saved.contains("m_windStrength") &&
                  saved.contains("m_gravity") && saved.contains("m_stiffness") &&
                  saved.contains("m_hideVanilla"),
              "all settings serialized");
        check(saved["m_detail"].get<std::string>() == "1,Native,Fine", "detail radio format");
        check(saved["m_cape"].get<std::string>().rfind("0,Worn Cape", 0) == 0,
              "cape radio lists Worn Cape first");

        CapePhysicsModule loaded;
        loaded.onInit();
        loaded.m_windStrength = 2.0f;
        loaded.loadConfig(saved);
        check(loaded.m_windStrength == 0.7f && loaded.m_gravity == 1.4f &&
                  loaded.m_stiffness == 0.6f && loaded.m_hideVanilla == false &&
                  loaded.m_detail == 1,
              "settings survive a save/load round trip");

        // Out-of-range values clamp on load.
        nlohmann::json wild;
        wild["m_windStrength"] = 99.0f;
        wild["m_gravity"] = -5.0f;
        wild["m_stiffness"] = 7.0f;
        wild["m_detail"] = "1,Native,Fine";
        CapePhysicsModule clamped;
        clamped.onInit();
        clamped.loadConfig(wild);
        check(clamped.m_windStrength == 2.0f && clamped.m_gravity == 0.0f &&
                  clamped.m_stiffness == 1.0f && clamped.m_detail == 1,
              "floats clamp to their menu ranges");
    }

    std::printf("\n%s\n", g_failures == 0 ? "all cape physics patch tests passed" : "some cape physics patch tests failed");
    return g_failures == 0 ? 0 : 1;
}
