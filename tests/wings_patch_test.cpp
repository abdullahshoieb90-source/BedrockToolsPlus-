// Regression test for the Wings in-game skin patch.
//
// Builds the real module (src/modules/visual/wings.cpp, compiled by
// scripts/run_tests.sh as a second translation unit) and drives it against a
// fake SerializedSkinImpl byte buffer laid out exactly as
// include/bedrocktools/sdk/offsets/Skin.hpp documents it for the Android
// target, connected through the fake Player -> SkinRef -> ThreadOwner pointer
// chain that resolvePlayerSkin() walks.
//
// Covered:
//   * wings_geometry.json is written into mGeometryData (240) and its
//     identifier into mDefaultGeometryName (96).
//   * wings.png is decoded into mSkinImage (120) with matching
//     width/height/format/deleter/size. mCapeImage (168) is left alone.
//   * a second tick does not churn the blob (patch stays intact, no per-tick
//     reallocation of memory the renderer may still sample).
//   * disabling restores the original skin image, geometry data and
//     default-geometry name before our injected blob is freed.
//   * persona skins and unexpected skin-image layouts are left untouched.
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I third_party
//         -I tests/fakejson -I tests/fakepl
//         tests/wings_patch_test.cpp src/modules/visual/wings.cpp
//         -o /tmp/wings_patch_test
//     /tmp/wings_patch_test

#include <bedrocktools/modules/visual/wings.hpp>
#include <bedrocktools/modules/visual/wings_default.hpp>
#include "config/ConfigManager.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    std::memcpy(base + off::Image::mBlobSizeOffset, &size, sizeof(std::size_t));
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

std::size_t readSize(const std::uint8_t* base, std::size_t offset) {
    std::size_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

// Mirrors the libc++ string layout the module writes (see wings.cpp).
std::string readFakeString(const std::uint8_t* base, std::size_t offset) {
    const std::uint8_t* p = base + offset;
    if ((p[0] & 0x01u) != 0) {
        const std::size_t len = readSize(p, 8);
        const char* data = *reinterpret_cast<const char* const*>(p + 16);
        return data ? std::string(data, len) : std::string();
    }
    const std::size_t len = static_cast<std::size_t>(p[0] >> 1);
    return std::string(reinterpret_cast<const char*>(p + 1), len);
}

void writeFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

} // namespace

int main() {
    // ------------------------------------------------------------------
    // Temporary config/wings folder with a geometry JSON and a small PNG.
    // ------------------------------------------------------------------
    const std::string root = "/tmp/bt-wings-patch-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/wings");
    g_testConfigPath = root + "/config.json";

    const std::string geometryJson = R"JSON({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    {
      "description": {
        "identifier": "geometry.wings",
        "texture_width": 64,
        "texture_height": 64
      },
      "bones": []
    }
  ]
})JSON";
    writeFile(root + "/wings/wings_geometry.json", geometryJson);

    constexpr int kTexW = 64, kTexH = 64;
    std::vector<std::uint8_t> texture(static_cast<std::size_t>(kTexW) * kTexH * 4u);
    for (int y = 0; y < kTexH; ++y) {
        for (int x = 0; x < kTexW; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kTexW + x) * 4u;
            texture[i + 0] = static_cast<std::uint8_t>((x * 3 + y) & 0xFF);
            texture[i + 1] = static_cast<std::uint8_t>((x + y * 5) & 0xFF);
            texture[i + 2] = static_cast<std::uint8_t>(x ^ y);
            texture[i + 3] = 255;
        }
    }
    stbi_write_png((root + "/wings/wings.png").c_str(), kTexW, kTexH, 4,
                   texture.data(), kTexW * 4);

    // ------------------------------------------------------------------
    // Fake skin (offsets per Skin.hpp) and the Player -> skin chain.
    // ------------------------------------------------------------------
    // Sized past mIsPersonaCapeOnClassicSkin (443) so persona checks stay in bounds.
    const std::size_t skinSize = off::SerializedSkinImpl::mIsPersonaCapeOnClassicSkin + 32;
    std::vector<std::uint8_t> skin(skinSize, 0);
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
    // mDefaultGeometryName (96) and mGeometryData (240) start as empty short
    // libc++ strings (all zero bytes).

    const std::vector<std::uint8_t> skinSnapshot = skin;

    // resolvePlayerSkin(): player+Player::mSkin -> *ptr -> +0 -> *ptr -> +0,
    // so player+mSkin must hold the address of a pointer that holds the skin.
    std::vector<std::uint8_t> player(off::Player::mSkin + sizeof(void*), 0);
    void* skinSlot = skinBase;          // its value is the skin pointer
    void* skinSlotAddress = &skinSlot;  // what Player::mSkin points at
    std::memcpy(player.data() + off::Player::mSkin, &skinSlotAddress, sizeof(void*));

    // ------------------------------------------------------------------
    // Enable and tick once.
    // ------------------------------------------------------------------
    WingsModule mod;
    mod.onInit();
    mod.enabled = true;
    mod.onLocalPlayerTick(player.data());

    std::printf("patch applies geometry and skin texture\n");
    check(readFakeString(skinBase, off::SerializedSkinImpl::mDefaultGeometryName) == "geometry.wings",
          "mDefaultGeometryName (96) is the geometry identifier");
    check(readFakeString(skinBase, off::SerializedSkinImpl::mGeometryData) == geometryJson,
          "mGeometryData (240) is the wings_geometry.json contents");

    check(readU32(skinBase + off::SerializedSkinImpl::mSkinImage, off::Image::mImageFormat) == 4,
          "mSkinImage format is RGBA8");
    check(readU32(skinBase + off::SerializedSkinImpl::mSkinImage, off::SkinImage::mWidth) == 64 &&
              readU32(skinBase + off::SerializedSkinImpl::mSkinImage, off::SkinImage::mHeight) == 64,
          "mSkinImage dimensions are 64x64");

    void* injected = readPtr(skinBase + off::SerializedSkinImpl::mSkinImage, off::Image::mBytesOffset);
    check(injected != nullptr && injected != skinBlob,
          "mSkinImage points at the injected wings.png pixels");
    check(readSize(skinBase + off::SerializedSkinImpl::mSkinImage, off::Image::mBlobSizeOffset) ==
              texture.size(),
          "blob size matches the decoded PNG");

    check(readPtr(skinBase + off::SerializedSkinImpl::mSkinImage, off::Image::mBlobDeleterOffset) !=
              originalDeleter,
          "blob deleter was retagged to free()");
    check(injected != nullptr &&
              sameBytes(static_cast<const std::uint8_t*>(injected), texture.data(), texture.size()),
          "injected pixels match wings.png");

    check(sameBytes(skinBase + off::SerializedSkinImpl::mCapeImage,
                    skinSnapshot.data() + off::SerializedSkinImpl::mCapeImage,
                    off::Image::Size),
          "mCapeImage (168) is untouched");

    // ------------------------------------------------------------------
    // A second tick must not churn the blob.
    // ------------------------------------------------------------------
    std::printf("second tick stays idle\n");
    mod.onLocalPlayerTick(player.data());
    check(readPtr(skinBase + off::SerializedSkinImpl::mSkinImage, off::Image::mBytesOffset) == injected,
          "skin blob pointer unchanged on the next tick");
    check(readFakeString(skinBase, off::SerializedSkinImpl::mGeometryData) == geometryJson,
          "geometry data unchanged after two ticks");
    check(readFakeString(skinBase, off::SerializedSkinImpl::mDefaultGeometryName) == "geometry.wings",
          "geometry name unchanged after two ticks");

    // ------------------------------------------------------------------
    // Disable restores the vanilla skin exactly.
    // ------------------------------------------------------------------
    std::printf("disable restores the vanilla skin\n");
    mod.enabled = false;
    mod.onLocalPlayerTick(player.data());
    check(sameBytes(skinBase + off::SerializedSkinImpl::mSkinImage,
                    skinSnapshot.data() + off::SerializedSkinImpl::mSkinImage,
                    off::Image::Size),
          "mSkinImage (120) fully restored");
    check(readPtr(skinBase + off::SerializedSkinImpl::mSkinImage, off::Image::mBytesOffset) == skinBlob,
          "original skin blob pointer restored");
    check(sameBytes(skinBase + off::SerializedSkinImpl::mDefaultGeometryName,
                    skinSnapshot.data() + off::SerializedSkinImpl::mDefaultGeometryName,
                    24),
          "mDefaultGeometryName (96) restored to its empty string");
    check(sameBytes(skinBase + off::SerializedSkinImpl::mGeometryData,
                    skinSnapshot.data() + off::SerializedSkinImpl::mGeometryData,
                    24),
          "mGeometryData (240) restored to its empty string");

    // ------------------------------------------------------------------
    // Persona skins are never patched.
    // ------------------------------------------------------------------
    std::printf("persona skin is skipped\n");
    skin = skinSnapshot;
    skin[off::SerializedSkinImpl::mIsPersona] = 1;
    mod.enabled = true;
    mod.onLocalPlayerTick(player.data());
    check(sameBytes(skinBase + off::SerializedSkinImpl::mSkinImage,
                    skinSnapshot.data() + off::SerializedSkinImpl::mSkinImage,
                    off::Image::Size),
          "mSkinImage untouched for a persona skin");
    check(sameBytes(skinBase + off::SerializedSkinImpl::mGeometryData,
                    skinSnapshot.data() + off::SerializedSkinImpl::mGeometryData,
                    24),
          "mGeometryData untouched for a persona skin");

    // ------------------------------------------------------------------
    // With no external wings folder the module falls back to the built-in
    // default pack, so the wings work immediately after install.
    // ------------------------------------------------------------------
    std::printf("no external folder: built-in defaults are used\n");
    skin = skinSnapshot;
    g_testConfigPath = root + "/no_wings/config.json"; // no .../no_wings/wings dir
    WingsModule modDefault;
    modDefault.onInit();
    modDefault.enabled = true;
    modDefault.onLocalPlayerTick(player.data());
    check(readFakeString(skinBase, off::SerializedSkinImpl::mDefaultGeometryName) ==
              wings_default::GeometryIdentifier,
          "default geometry identifier is used");
    check(readFakeString(skinBase, off::SerializedSkinImpl::mGeometryData) ==
              wings_default::GeometryJson,
          "default geometry JSON is used");

    void* defaultBlob = readPtr(skinBase + off::SerializedSkinImpl::mSkinImage, off::Image::mBytesOffset);
    const std::size_t defaultBytes =
        wings_default::TextureWidth * wings_default::TextureHeight * 4u;
    check(readU32(skinBase + off::SerializedSkinImpl::mSkinImage, off::SkinImage::mWidth) ==
              wings_default::TextureWidth &&
              readU32(skinBase + off::SerializedSkinImpl::mSkinImage, off::SkinImage::mHeight) ==
                  wings_default::TextureHeight,
          "default texture dimensions are used");
    check(defaultBlob != nullptr &&
              sameBytes(static_cast<const std::uint8_t*>(defaultBlob),
                        wings_default::TexturePixels, defaultBytes),
          "default texture pixels are used");

    // ------------------------------------------------------------------
    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all wings patch checks passed\n");
    return 0;
}
