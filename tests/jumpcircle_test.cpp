// Integration test for the real tick sampler and renderer, using
// fake game memory. No Minecraft or Android runtime is required.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>

namespace bedrocktools::events { EventBus& bus() { static EventBus b; return b; } }
namespace bedrocktools::memory { std::uintptr_t resolve(SignatureId) { return 0; } }
#include "modules/visual/jumpcircle.cpp"

namespace {
int vertices = 0, begins = 0, flushes = 0;
std::vector<float> alphas;
void fakeBegin(void*, void*, int mode, int count, int) { assert(mode == 1); assert(count == jumpcircle::segments * jumpcircle::bands * 4); ++begins; }
void fakeColor(void*, float, float, float, float a) { alphas.push_back(a); }
void fakeVertex(void*, float, float, float) { ++vertices; }
void fakeFlush(void*, void*, void*, char*) { ++flushes; }
template <typename T, std::size_t N>
void put(std::array<std::byte, N>& data, std::size_t offset, T value) { std::memcpy(data.data() + offset, &value, sizeof(value)); }
}

int main() {
    using namespace bedrocktools::sdk;
    namespace off = bedrocktools::sdk::offsets;
    alignas(std::max_align_t) std::array<std::byte, 0x400> player{};
    alignas(std::max_align_t) std::array<std::byte, 0x200> screen{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> renderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> camera{};
    std::array<float, 4> color{.2f,.3f,.4f,.5f};
    AABB box{{-.3f, 64, -.3f},{.3f,65.8f,.3f}};
    std::uintptr_t boxPtr = reinterpret_cast<std::uintptr_t>(&box);
    put(player, off::Actor::mStateVectorComponent + off::BuiltInActorComponents::mAABBShapeComponent, boxPtr);
    JumpCircleModule mod;
    mod.enabled = true;
    tick(player.data());
    assert(rings.empty());
    box.min.y += .42f; box.max.y += .42f;
    tick(player.data());
    assert(rings.size() == 1 && rings[0].center.y > 64 && rings[0].center.y < 64.1f);
    box.min.y += .34f; box.max.y += .34f;
    tick(player.data());
    assert(rings.size() == 1);
    box.min.y -= .08f; box.max.y -= .08f;
    tick(player.data());
    assert(rings.size() == 1); // still falling
    tick(player.data());
    assert(rings.size() == 2); // landed without any button input
    assert(rings[1].center.y > box.min.y && rings[1].center.y < box.min.y + .1f);
    tick(player.data());
    assert(rings.size() == 2); // no repeated landing while stationary
    nlohmann::json settings;
    settings["effectStyle"] = "2";
    mod.loadConfig(settings);
    assert(mod.style == 2);
    nlohmann::json saved;
    mod.saveConfig(saved);
    assert(saved["effectStyle"].get<std::string>() == "2,Circle,Square,Star");
    std::uintptr_t tess = reinterpret_cast<std::uintptr_t>(&vertices);
    put(screen, off::ScreenContext::mTessellator, tess);
    put(screen, off::ScreenContext::mColorHolder, reinterpret_cast<std::uintptr_t>(color.data()));
    put(renderer, off::LevelRenderer::mLevelRendererPlayer, reinterpret_cast<std::uintptr_t>(camera.data()));
    Vec3 cameraPos{0,65,0}; put(camera, off::LevelRendererPlayer::mCamPos, cameraPos);
    tessBegin = fakeBegin; tessColor = fakeColor; tessVertex = fakeVertex; renderMesh = fakeFlush;
    materialGroup = 1; translucent.data[0] = &vertices;
    render(renderer.data(), screen.data());
    assert(begins == 2 && flushes == 2 && vertices == 2 * jumpcircle::segments * jumpcircle::bands * 4);
    assert(alphas.size() == static_cast<std::size_t>(vertices));
    assert((color == std::array<float, 4>{.2f,.3f,.4f,.5f}));
    // A large teleport clears effects and cannot be mistaken for landing.
    box.min.x += 10; box.max.x += 10;
    tick(player.data());
    assert(rings.empty() && !motion.airborne);
    // Switching players, disabling and re-enabling must not inherit motion.
    mod.onDisable();
    assert(rings.empty() && !hasPrevious);
    mod.enabled = true;
    tick(player.data());
    assert(rings.empty());
    return 0;
}
