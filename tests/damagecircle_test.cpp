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
std::vector<std::array<float, 3>> vertexPositions;
fake_render::GpuColor gpu;
void fakeBegin(void*, void*, int mode, int count, int) {
    assert(mode == 1);
    assert(count == damagecircle::segments * damagecircle::bands * 4);
    ++begins;
}
void fakeColor(void*, float r, float g, float b, float a) { colors.push_back({r, g, b, a}); }
void fakeVertex(void*, float x, float y, float z) {
    ++vertices;
    vertexPositions.push_back({x, y, z});
}
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
    assert(std::abs(playerFeet.x) < .001f);
    assert(std::abs(playerFeet.y - 64.025f) < .001f);
    assert(std::abs(playerFeet.z) < .001f);
    assert(std::abs(playerHalfWidth - 0.3f) < .001f);

    const auto startState = damagecircle::animate(0.0f, 0.65f, 0.3f, 1.8f);
    const auto middleState = damagecircle::animate(0.325f, 0.65f, 0.3f, 1.8f);
    const auto endState = damagecircle::animate(0.65f, 0.65f, 0.3f, 1.8f);
    assert(std::abs(startState.lift) < .001f && std::abs(startState.radius - 0.265f) < .001f);
    assert(std::abs(middleState.lift - 0.9f) < .001f && std::abs(middleState.radius - startState.radius) < .001f);
    assert(std::abs(endState.lift - 1.8f) < .001f && std::abs(endState.radius - middleState.radius) < .001f);
    float minY = 100.0f, maxY = -100.0f, minRadius = 100.0f, maxRadius = 0.0f;
    damagecircle::emitRisingRing({0.0f, 64.025f, 0.0f}, 0.3f, 1.8f, 0.325f, 0.65f,
        [&](damagecircle::Point point, float) {
            minY = std::min(minY, point.y);
            maxY = std::max(maxY, point.y);
            const float radius = std::sqrt(point.x * point.x + point.z * point.z);
            minRadius = std::min(minRadius, radius);
            maxRadius = std::max(maxRadius, radius);
        });
    assert(std::abs(minY - 64.925f) < .002f && std::abs(maxY - minY) < .002f);
    assert(minRadius > 0.23f && std::abs(maxRadius - 0.3f) < .002f); // fixed to the player's width

    put(player, off::Actor::mHurtTime, 9);
    tick(player.data());
    assert(rings.size() == 1); // hurt-time countdown is not another hit
    put(player, off::Actor::mHurtTime, 10);
    tick(player.data());
    assert(rings.size() == 2); // a new upward reset means another hit

    // Every active ring follows the player instead of staying at the damage location.
    box.min.x += 3.0f;
    box.max.x += 3.0f;
    put(component, off::AABBShapeComponent::mAABB, box);
    tick(player.data());
    assert(rings.size() == 2 && std::abs(playerFeet.x - 3.0f) < .001f);

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
    Vec3 cameraPos{3, 65, 0};
    put(camera, off::LevelRendererPlayer::mCamPos, cameraPos);
    tessBegin = fakeBegin;
    tessColor = fakeColor;
    tessVertex = fakeVertex;
    renderMesh = fakeFlush;
    materialGroup = 1;
    translucent.data[0] = &vertices;
    render(renderer.data(), screen.data());
    assert(begins == 2 && flushes == 2);
    assert(vertices == 2 * damagecircle::segments * damagecircle::bands * 4);
    assert(!colors.empty() && !vertexPositions.empty());
    float maxAbsX = 0.0f;
    for (const auto& position : vertexPositions) maxAbsX = std::max(maxAbsX, std::abs(position[0]));
    assert(maxAbsX > 0.29f && maxAbsX <= 0.301f); // ring stayed centered on the moved player, at exact width
    for (const auto& colorValue : colors) {
        assert(colorValue[0] == 1.0f && colorValue[1] == 1.0f && colorValue[2] == 1.0f);
    }
    assert((color.rgba == fake_render::Color{.2f, .3f, .4f, .5f}) && color.dirty == 1);

    mod.setMasterEnabled(false);
    assert(rings.empty());
}
