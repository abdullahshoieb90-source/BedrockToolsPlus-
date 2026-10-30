// Exercise the real wings overlay alongside the shared shader-color guard.
// The game is represented by a CPU color holder and a cached GPU tint.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "fake_shader_color.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>

#include "modules/visual/wings.cpp"

// Asset writing is not exercised here, but the module's vtable references it.
extern "C" int stbi_write_png(const char*, int, int, int, const void*, int) { return 0; }

namespace bedrocktools::events {
EventBus& bus() { static EventBus instance; return instance; }
}
namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
}
namespace bedrocktools::config {
ConfigManager::~ConfigManager() = default;
std::string ConfigManager::getConfigPath() const { return "/tmp/bedrock-wings-test/config.json"; }
}

namespace {
using fake_render::Color;
int failures = 0;
int begins = 0, vertices = 0, flushes = 0;
Color vertexColor{};
Color firstColor{};
Color outputColor{};
fake_render::GpuColor gpu;
fake_render::ShaderColor* legacyHolder = nullptr;
bool legacyUntouched = true;
void* drawnMaterial = nullptr;

void check(bool condition, const char* message) {
    std::printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}
void fakeBegin(void*, void*, int, int, int) { ++begins; }
void fakeColor(void*, float r, float g, float b, float a) { vertexColor = {r, g, b, a}; }
void fakeVertex(void*, float, float, float) {
    if (vertices++ == 0) firstColor = vertexColor;
}
void fakeFlush(void* screen, void*, void* material, char*) {
    ++flushes;
    drawnMaterial = material;
    auto* holder = fake_render::colorHolder(screen);
    if (holder) outputColor = gpu.draw(*holder, firstColor);
    legacyUntouched = legacyUntouched && legacyHolder &&
                      legacyHolder->rgba == Color{0.8f, 0.7f, 0.6f, 0.5f};
}
template <typename T, std::size_t N>
void put(std::array<std::byte, N>& bytes, std::size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void resetCapture() {
    begins = vertices = flushes = 0;
    firstColor = outputColor = {};
    drawnMaterial = nullptr;
    legacyUntouched = true;
}
} // namespace

int main() {
    using bedrocktools::sdk::Vec3;
    namespace off = bedrocktools::sdk::offsets;
    std::printf("wings shared shader-color state\n");

    alignas(16) std::array<std::byte, 0xC0> screen{};
    alignas(16) std::array<std::byte, 0x500> renderer{};
    alignas(16) std::array<std::byte, 0x1100> camera{};
    const Color prior{0.1f, 0.2f, 0.3f, 0.5f};
    const Color black{0, 0, 0, 1};
    fake_render::ShaderColor holder{prior, 0};
    fake_render::ShaderColor oldSlot{{0.8f, 0.7f, 0.6f, 0.5f}, 0};
    legacyHolder = &oldSlot;
    std::uint64_t tessellator = 0;
    void* tessPtr = &tessellator;
    void* holderPtr = &holder;
    void* oldSlotPtr = &oldSlot;
    void* cameraPtr = camera.data();
    put(screen, off::ScreenContext::mTessellator, tessPtr);
    put(screen, fake_render::kColorHolderOffset, holderPtr);
    put(screen, fake_render::kOldColorHolderOffset, oldSlotPtr);
    put(renderer, off::LevelRenderer::mLevelRendererPlayer, cameraPtr);
    // A populated game-owned selection material at the 1.26.60 offset.
    void* fakeMaterial = &tessellator;
    put(camera, 0x10A0, fakeMaterial);

    WingsModule module;
    module.enabled = true;
    s_tessBegin = fakeBegin;
    s_tessColor = fakeColor;
    s_tessVertex = fakeVertex;
    s_renderMesh = fakeFlush;
    s_hasPlayer = true;
    s_curAABB = {{-0.3f, 64.0f, -0.3f}, {0.3f, 65.8f, 0.3f}};
    s_curRot = {0, 0};
    s_localPlayerPtr = nullptr; // use the captured tick sample
    s_hasPrevSample = false;
    s_lastTickTimeValid = false;

    // No first-person overlay should change the surrounding render state.
    put(camera, off::LevelRendererPlayer::mCamPos, Vec3{0.0f, 65.5f, 0.0f});
    gpu.rgba = black;
    renderWingsOverlay(renderer.data(), screen.data());
    check(begins == 0 && holder.rgba == prior && holder.dirty == 0,
          "first person leaves the game's shader color untouched");

    for (float cameraZ : {-4.0f, 4.0f}) {
        resetCapture();
        holder = {prior, 0};
        gpu.rgba = black;
        put(camera, off::LevelRendererPlayer::mCamPos, Vec3{0.0f, 65.5f, cameraZ});
        renderWingsOverlay(renderer.data(), screen.data());
        check(begins == 1 && vertices > 0 && flushes == 1,
              "third-person wings submit their mesh");
        check(firstColor[0] + firstColor[1] + firstColor[2] > 0.0f && outputColor == firstColor,
              "wing vertex colors are not multiplied by a stale black tint");
        check(holder.rgba == prior && holder.dirty == 1 && legacyUntouched,
              "wings restore the prior tint and leave the old offset untouched");
        check(drawnMaterial == camera.data() + 0x10A0,
              "wings use the corrected embedded-material fallback");
        gpu.upload(holder);
        check(gpu.rgba == prior, "the next game draw receives its original tint");
    }

    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all wings shader-color checks passed\n");
    return 0;
}
