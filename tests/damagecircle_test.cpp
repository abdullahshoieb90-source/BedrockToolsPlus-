// Integration test for damage-event sampling and the real ring renderer using
// fake player/render memory. No Minecraft or Android runtime is required.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include "fake_shader_color.hpp"

namespace bedrocktools::events { EventBus& bus() { static EventBus b; return b; } }
namespace bedrocktools::memory { std::uintptr_t resolve(SignatureId) { return 0; } }
#include "modules/visual/damagecircle.cpp"

namespace {
int vertices = 0, begins = 0, flushes = 0;
std::vector<std::array<float, 4>> colors;
fake_render::GpuColor gpu;
void fakeBegin(void*, void*, int mode, int count, int) {
    assert(mode == 1);
    assert(count == jumpcircle::segments * jumpcircle::bands * 4);
    ++begins;
}
void fakeColor(void*, float r, float g, float b, float a) { colors.push_back({r, g, b, a}); }
void fakeVertex(void*, float, float, float) { ++vertices; }
void fakeFlush(void* screen, void*, void*, char*) {
    ++flushes;
    auto* holder = fake_render::colorHolder(screen);
    assert(holder);
    gpu.upload(*holder);
}
template <typename T, std::size_t N>
void put(std::array<std::byte, N>& data, std::size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}
}

int main() {
    using namespace bedrocktools::sdk;
    namespace off = bedrocktools::sdk::offsets;
    alignas(std::max_align_t) std::array<std::byte, 0x500> player{};
    alignas(std::max_align_t) std::array<std::byte, 0x100> component{};
    alignas(std::max_align_t) std::array<std::byte, 0x200> screen{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> renderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> camera{};
    AABB box{{-.3f, 64, -.3f}, {.3f, 65.8f, .3f}};
    put(player, off::Actor::mHurtTime, 0);
    put(player, off::Actor::mStateVectorComponent + off::BuiltInActorComponents::mAABBShapeComponent,
        reinterpret_cast<std::uintptr_t>(component.data()));
    put(component, off::AABBShapeComponent::mAABB, box);

    DamageCircleModule mod;
    mod.onInit();
    mod.setMasterEnabled(true);
    tick(player.data()); // establish a baseline, no ring
    assert(rings.empty());

    put(player, off::Actor::mHurtTime, 10);
    tick(player.data());
    assert(rings.size() == 1);
    assert(std::abs(rings[0].center.x) < .001f);
    assert(std::abs(rings[0].center.y - 64.025f) < .001f);
    assert(std::abs(rings[0].center.z) < .001f);

    put(player, off::Actor::mHurtTime, 9);
    tick(player.data());
    assert(rings.size() == 1); // hurt-time countdown is not another hit
    put(player, off::Actor::mHurtTime, 10);
    tick(player.data());
    assert(rings.size() == 2); // a new upward reset means another hit

    nlohmann::json settings;
    settings["effectDuration"] = 2.5f;
    mod.loadConfig(settings);
    assert(std::abs(mod.effectDuration - 2.5f) < .001f);
    nlohmann::json saved;
    mod.saveConfig(saved);
    assert(std::abs(saved["effectDuration"].get<float>() - 2.5f) < .001f);
    nlohmann::json invalid;
    invalid["effectDuration"] = 99.0f;
    mod.loadConfig(invalid);
    assert(std::abs(mod.effectDuration - 10.0f) < .001f);

    std::uintptr_t tess = reinterpret_cast<std::uintptr_t>(&vertices);
    put(screen, off::ScreenContext::mTessellator, tess);
    fake_render::ShaderColor color{{.2f, .3f, .4f, .5f}, 0};
    put(screen, fake_render::kColorHolderOffset, reinterpret_cast<std::uintptr_t>(&color));
    put(renderer, off::LevelRenderer::mLevelRendererPlayer, reinterpret_cast<std::uintptr_t>(camera.data()));
    Vec3 cameraPos{0, 65, 0};
    put(camera, off::LevelRendererPlayer::mCamPos, cameraPos);
    tessBegin = fakeBegin;
    tessColor = fakeColor;
    tessVertex = fakeVertex;
    renderMesh = fakeFlush;
    materialGroup = 1;
    translucent.data[0] = &vertices;
    render(renderer.data(), screen.data());
    assert(begins == 2 && flushes == 2);
    assert(vertices == 2 * jumpcircle::segments * jumpcircle::bands * 4);
    assert(!colors.empty());
    for (const auto& colorValue : colors) {
        assert(colorValue[0] == 1.0f && colorValue[1] == 1.0f && colorValue[2] == 1.0f);
    }
    assert((color.rgba == fake_render::Color{.2f, .3f, .4f, .5f}) && color.dirty == 1);

    mod.setMasterEnabled(false);
    assert(rings.empty());
}
