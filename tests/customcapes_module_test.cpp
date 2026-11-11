// Integration test for the real Custom Capes module using a fake player and
// SerializedSkin allocation. No Minecraft process is required.
#include "modules/visual/customcapes.hpp"
#include "config/ConfigManager.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/offsets/Skin.hpp>
#include <bedrocktools/sdk/offsets/World.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

namespace {
int failures = 0;
std::string configPath = "/tmp/bt-custom-capes-module/config.json";
void check(bool condition, const char* message) {
    std::printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}
}

namespace bedrocktools::config {
ConfigManager::~ConfigManager() = default;
std::string ConfigManager::getConfigPath() const { return configPath; }
}
namespace bedrocktools::events {
EventBus& bus() { static EventBus instance; return instance; }
}

int main() {
    using namespace bedrocktools::sdk::offsets;
    const std::filesystem::path root = "/tmp/bt-custom-capes-module";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "capes");
    std::vector<unsigned char> red(64 * 32 * 4);
    for (std::size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255; red[i + 1] = 10; red[i + 2] = 20; red[i + 3] = 255;
    }
    check(stbi_write_png((root / "capes/red.png").string().c_str(), 64, 32, 4,
                         red.data(), 64 * 4) != 0, "test PNG was written");

    CustomCapesModule module;
    module.onInit();
    check(module.capesDirectory() == (root / "capes").string(), "capes directory is next to config.json");
    check(std::filesystem::exists(root / "capes/README.txt"), "first launch writes usage instructions");

    nlohmann::json selection;
    selection["m_cape"] = "2,Default Cape,No Cape,red.png";
    module.loadConfig(selection);
    nlohmann::json saved;
    module.saveConfig(saved);
    check(saved["m_cape"].get<std::string>() == "2,Default Cape,No Cape,red.png",
          "discovered PNG appears in the saved module picker");

    alignas(16) std::array<unsigned char, 512> skin{};
    auto* originalCapeId = new (skin.data() + SerializedSkinImpl::mCapeId) std::string("vanilla-cape");
    unsigned char vanillaPixel[4] = {1, 2, 3, 255};
    auto* image = skin.data() + SerializedSkinImpl::mCapeImage;
    *reinterpret_cast<std::uint32_t*>(image + Image::mImageFormat) = 3;
    *reinterpret_cast<std::uint32_t*>(image + Image::mWidth) = 64;
    *reinterpret_cast<std::uint32_t*>(image + Image::mHeight) = 32;
    *reinterpret_cast<std::uint32_t*>(image + Image::mDepth) = 1;
    *reinterpret_cast<void**>(image + Image::mBytesOffset) = vanillaPixel;
    *reinterpret_cast<std::size_t*>(image + Image::mBlobSizeOffset) = 4;

    void* skinImpl = skin.data();
    std::vector<unsigned char> player(Player::mSkin + sizeof(void*) + 8, 0);
    *reinterpret_cast<void**>(player.data() + Player::mSkin) = &skinImpl;

    module.setMasterEnabled(true);
    module.onLocalPlayerTick(player.data());
    auto** installedPixels = reinterpret_cast<unsigned char**>(image + Image::mBytesOffset);
    check(*reinterpret_cast<std::uint32_t*>(image + Image::mWidth) == 64 &&
          *reinterpret_cast<std::uint32_t*>(image + Image::mHeight) == 32,
          "selected PNG dimensions are injected into mce::Image");
    check(*reinterpret_cast<std::uint32_t*>(image + Image::mDepth) == 1 &&
          *reinterpret_cast<std::uint32_t*>(image + Image::mImageFormat) == 3,
          "injected image has valid depth and RGBA8 format");
    check(installedPixels && *installedPixels && (*installedPixels)[0] == 255 &&
          (*installedPixels)[1] == 10 && (*installedPixels)[2] == 20,
          "decoded PNG RGBA pixels reach the game image");
    check(*originalCapeId == "bedrocktoolsplus.custom_cape.red.png", "a non-empty custom cape id enables rendering");

    nlohmann::json hide;
    hide["m_cape"] = "1";
    module.loadConfig(hide);
    module.onLocalPlayerTick(player.data());
    check(*reinterpret_cast<void**>(image + Image::mBytesOffset) == nullptr && originalCapeId->empty(),
          "No Cape clears both the image and cape id");

    module.setMasterEnabled(false);
    check(*reinterpret_cast<void**>(image + Image::mBytesOffset) == vanillaPixel &&
          *originalCapeId == "vanilla-cape", "disabling restores the exact original cape");

    originalCapeId->~basic_string();
    std::filesystem::remove_all(root);
    if (failures) {
        std::printf("%d custom capes module check(s) failed\n", failures);
        return 1;
    }
    std::printf("all custom capes module checks passed\n");
    return 0;
}
