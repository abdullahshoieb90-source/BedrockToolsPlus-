#include "damagecircle.hpp"
#include "damagecircle_geometry.hpp"
#include "jumpcircle_geometry.hpp"
#include "core/memory/Hooks.hpp"
#include "core/RenderState.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace {
using namespace bedrocktools::sdk;
using namespace bedrocktools::sdk::offsets;
using Clock = std::chrono::steady_clock;
using Begin = void (*)(void*, void*, int, int, int);
using TessColor = void (*)(void*, float, float, float, float);
using Vertex = void (*)(void*, float, float, float);
using RenderMesh = void (*)(void*, void*, void*, char*);
using RenderLevel = void (*)(void*, void*, void*);

struct HashedString {
    std::uint64_t hash = 0;
    std::string value;
    mutable const HashedString* lastMatch = nullptr;
    explicit HashedString(const char* name) : value(name) {
        hash = 0xCBF29CE484222325ULL;
        for (char c : value) hash = static_cast<unsigned char>(c) ^ (0x100000001B3ULL * hash);
    }
};
// Non-owning mirror of the game-owned MaterialPtr; its non-trivial destructor
// is required for the game's return ABI.
struct MaterialPtr {
    void* data[2]{nullptr, nullptr};
    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;
    MaterialPtr(MaterialPtr&& o) noexcept : data{o.data[0], o.data[1]} { o.data[0] = o.data[1] = nullptr; }
    MaterialPtr& operator=(MaterialPtr&& o) noexcept {
        data[0] = o.data[0]; data[1] = o.data[1]; o.data[0] = o.data[1] = nullptr; return *this;
    }
    ~MaterialPtr() {}
    explicit operator bool() const { return data[0] != nullptr; }
};

DamageCircleModule* module = nullptr;
Begin tessBegin = nullptr;
TessColor tessColor = nullptr;
Vertex tessVertex = nullptr;
RenderMesh renderMesh = nullptr;
RenderLevel originalRender = nullptr;
std::uintptr_t materialGroup = 0;
MaterialPtr translucent;
struct Ring { jumpcircle::Point center; Clock::time_point start; };
std::mutex stateMutex;
std::vector<Ring> rings;
void* trackedPlayer = nullptr;
damagecircle::HurtTimeTracker hurtTime;
Clock::time_point lastTick{};

std::uintptr_t resolveAdrp(std::uint32_t* p, std::size_t count, std::uint32_t reg) {
    for (std::size_t i = 0; i < count; ++i) {
        auto insn = p[i];
        if ((insn & 0x1f) != reg) continue;
        if ((insn & 0x9f000000) == 0x90000000) {
            auto bits = ((static_cast<std::uint64_t>((insn >> 3) & 0x1ffffc) |
                          static_cast<std::uint64_t>((insn >> 29) & 3)) << 43);
            auto page = (reinterpret_cast<std::uintptr_t>(&p[i]) & ~0xfffULL) +
                        (static_cast<std::int64_t>(bits) >> 31);
            for (std::size_t j = i + 1; j < count; ++j) {
                auto add = p[j];
                if ((add & 0xff000000) == 0x91000000 && ((add >> 5) & 31) == reg && (add & 31) == reg) {
                    auto imm = (add >> 10) & 0xfff;
                    if (add & 0x400000) imm <<= 12;
                    return page + imm;
                }
                if ((add & 31) == reg) break;
            }
        }
        if ((insn & 0x9f000000) == 0x10000000) {
            auto bits = (static_cast<std::uint64_t>(((insn >> 3) & 0x1ffffc) | (insn >> 29)) << 43);
            return reinterpret_cast<std::uintptr_t>(&p[i]) + (static_cast<std::int64_t>(bits) >> 43);
        }
    }
    return 0;
}

void clearState() {
    std::lock_guard lock(stateMutex);
    rings.clear(); trackedPlayer = nullptr; hurtTime.reset(); lastTick = {};
}

void tick(void* rawPlayer) {
    if (!module || !module->enabled || reinterpret_cast<std::uintptr_t>(rawPlayer) < 0x1000) {
        clearState(); return;
    }
    auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(rawPlayer);
    const int currentHurtTime = player->hurtTime();
    const AABB box = player->bounds();
    const Vec3 feet{(box.min.x + box.max.x) * 0.5f, box.min.y, (box.min.z + box.max.z) * 0.5f};
    if (currentHurtTime < 0 || currentHurtTime > 100 ||
        !std::isfinite(feet.x) || !std::isfinite(feet.y) || !std::isfinite(feet.z)) {
        clearState(); return;
    }

    const auto now = Clock::now();
    std::lock_guard lock(stateMutex);
    if (trackedPlayer != rawPlayer) {
        rings.clear(); hurtTime.reset(); trackedPlayer = rawPlayer;
    }
    // Do not interpret a delayed tick after a pause as a new damage event.
    const bool resumedAfterPause = lastTick != Clock::time_point{} && now - lastTick > std::chrono::milliseconds(300);
    if (resumedAfterPause) hurtTime.reset();
    if (hurtTime.update(currentHurtTime) && !resumedAfterPause) {
        if (rings.size() >= 12) rings.erase(rings.begin());
        rings.push_back({{feet.x, feet.y + 0.025f, feet.z}, now});
    }
    lastTick = now;
    const float duration = std::isfinite(module->effectDuration)
        ? std::clamp(module->effectDuration, 0.1f, 10.0f) : 0.65f;
    std::erase_if(rings, [now, duration](const Ring& ring) {
        return std::chrono::duration<float>(now - ring.start).count() >= duration;
    });
}

void render(void* renderer, void* screen) {
    if (!module || !module->enabled || !tessBegin || !tessColor || !tessVertex || !renderMesh ||
        reinterpret_cast<std::uintptr_t>(renderer) < 0x1000 || reinterpret_cast<std::uintptr_t>(screen) < 0x1000) return;
    const auto now = Clock::now();
    std::vector<Ring> snapshot;
    {
        std::lock_guard lock(stateMutex);
        if (lastTick == Clock::time_point{} || now - lastTick > std::chrono::milliseconds(300)) return;
        snapshot = rings;
    }
    if (snapshot.empty() || !materialGroup) return;
    if (!translucent) {
        auto** vtable = *reinterpret_cast<void***>(materialGroup);
        if (!vtable || !vtable[VTable::RenderMaterialGroup_getMaterial]) return;
        HashedString name("name_tag_depth_tested");
        using GetMaterial = MaterialPtr (*)(void*, const HashedString*);
        translucent = reinterpret_cast<GetMaterial>(vtable[VTable::RenderMaterialGroup_getMaterial])(reinterpret_cast<void*>(materialGroup), &name);
    }
    if (!translucent) return;
    auto scr = reinterpret_cast<std::uintptr_t>(screen);
    auto tess = *reinterpret_cast<std::uintptr_t*>(scr + ScreenContext::mTessellator);
    auto lrp = *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(renderer) + LevelRenderer::mLevelRendererPlayer);
    if (tess < 0x1000 || lrp < 0x1000) return;
    const auto cam = *reinterpret_cast<const Vec3*>(lrp + LevelRendererPlayer::mCamPos);
    bedrocktools::core::ScopedShaderColor shaderColor(screen);
    if (!shaderColor) return;
    const float duration = std::isfinite(module->effectDuration)
        ? std::clamp(module->effectDuration, 0.1f, 10.0f) : 0.65f;
    for (const auto& ring : snapshot) {
        const float age = std::chrono::duration<float>(now - ring.start).count();
        if (age < 0 || age >= duration) continue;
        tessBegin(reinterpret_cast<void*>(tess), nullptr, 1, jumpcircle::segments * jumpcircle::bands * 4, 0);
        jumpcircle::emitRing(ring.center, age, duration, false, jumpcircle::Style::Circle, [&](jumpcircle::Point point, float alpha) {
            tessColor(reinterpret_cast<void*>(tess), 1.0f, 1.0f, 1.0f, alpha);
            tessVertex(reinterpret_cast<void*>(tess), point.x - cam.x, point.y - cam.y, point.z - cam.z);
        });
        char pad[0x100]{};
        renderMesh(screen, reinterpret_cast<void*>(tess), &translucent, pad);
    }
}

void renderHook(void* renderer, void* screen, void* params) {
    if (originalRender) originalRender(renderer, screen, params);
    render(renderer, screen);
}
} // namespace

DamageCircleModule::DamageCircleModule()
    : Module("Damage Circle", "Shows a brief white expanding ring when you take damage.") {
    module = this;
    hideInHudEditor = true;
}
DamageCircleModule::~DamageCircleModule() { if (module == this) module = nullptr; }
void DamageCircleModule::onInit() {
    using bedrocktools::memory::resolve;
    using bedrocktools::memory::SignatureId;
    m_patchTarget = reinterpret_cast<void*>(resolve(SignatureId::RenderLevel));
    tessBegin = reinterpret_cast<Begin>(resolve(SignatureId::TessellatorBegin));
    tessColor = reinterpret_cast<TessColor>(resolve(SignatureId::TessellatorColor));
    tessVertex = reinterpret_cast<Vertex>(resolve(SignatureId::TessellatorVertex));
    auto mesh = resolve(SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!mesh) mesh = resolve(SignatureId::MeshHelpersRenderMeshImmediately);
    renderMesh = reinterpret_cast<RenderMesh>(mesh);
    auto group = resolve(SignatureId::RenderMaterialGroupCommon);
    if (group) {
        auto address = resolveAdrp(reinterpret_cast<std::uint32_t*>(group), 2, 0);
        if (address) materialGroup = address + MaterialGroup::mRenderMaterialGroupOffset;
    }
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { tick(event.player); });
}
void DamageCircleModule::onEnable() {
    clearState();
    if (!m_patched && m_patchTarget)
        m_patched = bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(&renderHook), reinterpret_cast<void**>(&originalRender)) != nullptr;
}
void DamageCircleModule::onDisable() { clearState(); }
void DamageCircleModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("effectDuration")) {
        try {
            const float value = j["effectDuration"].get<float>();
            if (std::isfinite(value)) effectDuration = std::clamp(value, 0.1f, 10.0f);
        } catch (...) {}
    }
}
void DamageCircleModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["effectDuration"] = std::isfinite(effectDuration)
        ? std::clamp(effectDuration, 0.1f, 10.0f) : 0.65f;
}
