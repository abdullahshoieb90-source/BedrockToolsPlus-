#include "damagecircle.hpp"
#include "damagecircle_geometry.hpp"
#include "core/memory/Hooks.hpp"
#include "core/RenderState.hpp"
#include <bedrocktools/events/AttackEvent.hpp>
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
constexpr auto attackConfirmWindow = std::chrono::milliseconds(1000);
constexpr std::size_t maxPendingAttacks = 8;
constexpr std::size_t maxRings = 24;
struct Ring {
    bedrocktools::sdk::Actor* actor = nullptr;
    damagecircle::Point feet{};
    float height = 1.8f;
    Clock::time_point start{};
};
struct PendingAttack {
    bedrocktools::sdk::Actor* target = nullptr;
    int baselineHurtTime = 0;
    Clock::time_point at{};
};
std::mutex stateMutex;
std::vector<Ring> rings;
std::vector<PendingAttack> pendingAttacks;
void* trackedPlayer = nullptr;
damagecircle::Point playerFeet{};
float playerHeight = 1.8f;
bool hasPlayerPosition = false;
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

bool actorPose(bedrocktools::sdk::Actor* actor, damagecircle::Point& feet, float& height) {
    if (reinterpret_cast<std::uintptr_t>(actor) < 0x1000) return false;
    const AABB box = actor->bounds();
    feet = {(box.min.x + box.max.x) * 0.5f, box.min.y + 0.025f,
            (box.min.z + box.max.z) * 0.5f};
    height = box.max.y - box.min.y;
    return std::isfinite(feet.x) && std::isfinite(feet.y) && std::isfinite(feet.z) &&
           std::isfinite(height) && height > 0.0f;
}

bool targetTypeEnabled(bedrocktools::sdk::Actor* actor) {
    if (!actor || !module) return false;
    const auto categories = actor->categories();
    if ((categories & bedrocktools::sdk::offsets::ActorCategories::IsPlayer) != 0)
        return module->showPlayers;
    if ((categories & bedrocktools::sdk::offsets::ActorCategories::IsMob) != 0)
        return module->showMobs;
    return false;
}

void addRing(bedrocktools::sdk::Actor* actor, const damagecircle::Point& feet,
             float height, Clock::time_point now) {
    if (rings.size() >= maxRings) rings.erase(rings.begin());
    rings.push_back({actor, feet, height, now});
}

void clearState() {
    std::lock_guard lock(stateMutex);
    rings.clear(); pendingAttacks.clear(); trackedPlayer = nullptr;
    playerFeet = {}; hasPlayerPosition = false; hurtTime.reset(); lastTick = {};
}

void tick(void* rawPlayer) {
    if (!module || !module->enabled || reinterpret_cast<std::uintptr_t>(rawPlayer) < 0x1000) {
        clearState(); return;
    }
    auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(rawPlayer);
    const int currentHurtTime = player->hurtTime();
    const AABB box = player->bounds();
    const Vec3 feet{(box.min.x + box.max.x) * 0.5f, box.min.y, (box.min.z + box.max.z) * 0.5f};
    const float height = box.max.y - box.min.y;
    if (currentHurtTime < 0 || currentHurtTime > 100 ||
        !std::isfinite(feet.x) || !std::isfinite(feet.y) || !std::isfinite(feet.z) ||
        !std::isfinite(height) || height <= 0.0f) {
        clearState(); return;
    }

    const auto now = Clock::now();
    std::lock_guard lock(stateMutex);
    if (trackedPlayer != rawPlayer) {
        rings.clear(); pendingAttacks.clear(); hurtTime.reset(); trackedPlayer = rawPlayer;
    }
    playerFeet = {feet.x, feet.y + 0.025f, feet.z};
    playerHeight = height;
    hasPlayerPosition = true;

    // Self-damage is detected directly from the local actor's hurt timer.
    const bool resumedAfterPause = lastTick != Clock::time_point{} &&
        now - lastTick > std::chrono::milliseconds(300);
    if (resumedAfterPause) hurtTime.reset();
    if (hurtTime.update(currentHurtTime) && !resumedAfterPause)
        addRing(player, playerFeet, playerHeight, now);

    // Other entities only get an effect after an attack is confirmed by their
    // hurt-time increasing. Whiffs and blocked attacks never create a ring.
    for (auto it = pendingAttacks.begin(); it != pendingAttacks.end();) {
        if (now - it->at > attackConfirmWindow) {
            it = pendingAttacks.erase(it);
            continue;
        }
        const int targetHurtTime = it->target->hurtTime();
        if (targetHurtTime < 0 || targetHurtTime > 100) {
            it = pendingAttacks.erase(it);
            continue;
        }
        if (targetHurtTime > it->baselineHurtTime && targetTypeEnabled(it->target)) {
            damagecircle::Point targetFeet{};
            float targetHeight = 0.0f;
            if (actorPose(it->target, targetFeet, targetHeight))
                addRing(it->target, targetFeet, targetHeight, now);
        }
        it->baselineHurtTime = targetHurtTime;
        ++it;
    }

    // Keep effects attached to their actors while they move.
    const float duration = damagecircle::safeDuration(module->effectDuration);
    for (auto it = rings.begin(); it != rings.end();) {
        if (std::chrono::duration<float>(now - it->start).count() >= duration) {
            it = rings.erase(it);
            continue;
        }
        if (it->actor == player) {
            it->feet = playerFeet;
            it->height = playerHeight;
        } else if (!actorPose(it->actor, it->feet, it->height)) {
            it = rings.erase(it);
            continue;
        }
        ++it;
    }
    lastTick = now;
}
void render(void* renderer, void* screen) {
    if (!module || !module->enabled || !tessBegin || !tessColor || !tessVertex || !renderMesh ||
        reinterpret_cast<std::uintptr_t>(renderer) < 0x1000 || reinterpret_cast<std::uintptr_t>(screen) < 0x1000) return;
    const auto now = Clock::now();
    std::vector<Ring> snapshot;
    {
        std::lock_guard lock(stateMutex);
        if (!hasPlayerPosition || lastTick == Clock::time_point{} ||
            now - lastTick > std::chrono::milliseconds(300)) return;
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
    const float duration = damagecircle::safeDuration(module->effectDuration);
    for (const auto& ring : snapshot) {
        const float age = std::chrono::duration<float>(now - ring.start).count();
        if (age < 0 || age >= duration) continue;
        tessBegin(reinterpret_cast<void*>(tess), nullptr, 1, damagecircle::segments * damagecircle::bands * 4, 0);
        damagecircle::emitVerticalRing(ring.feet, ring.height, age, duration,
            damagecircle::safeSize(module->effectSize), damagecircle::safeSpeed(module->effectSpeed),
            [&](damagecircle::Point point, float alpha) {
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
    : Module("Damage Circle", "Shows a white ring on you when damaged, and optionally on players or mobs you hit; set its visibility time separately from its up/down speed.") {
    module = this;
    hideInHudEditor = true;
}
DamageCircleModule::~DamageCircleModule() { if (module == this) module = nullptr; }
void DamageCircleModule::onAttack(bedrocktools::sdk::Actor* target) {
    if (!enabled || !target || !targetTypeEnabled(target)) return;
    const int baseline = target->hurtTime();
    if (baseline < 0 || baseline > 100) return;

    const auto now = Clock::now();
    std::lock_guard lock(stateMutex);
    for (auto& pending : pendingAttacks) {
        if (pending.target == target) {
            pending.baselineHurtTime = baseline;
            pending.at = now;
            return;
        }
    }
    if (pendingAttacks.size() >= maxPendingAttacks) pendingAttacks.erase(pendingAttacks.begin());
    pendingAttacks.push_back({target, baseline, now});
}
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
    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>(
        [](auto& event) { if (module) module->onAttack(event.target); });
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
            if (std::isfinite(value)) effectDuration = damagecircle::safeDuration(value);
        } catch (...) {}
    }
    if (j.contains("effectSize")) {
        try {
            const float value = j["effectSize"].get<float>();
            if (std::isfinite(value)) effectSize = damagecircle::safeSize(value);
        } catch (...) {}
    }
    if (j.contains("effectSpeed")) {
        try {
            const float value = j["effectSpeed"].get<float>();
            if (std::isfinite(value)) effectSpeed = damagecircle::safeSpeed(value);
        } catch (...) {}
    }
    if (j.contains("showPlayers")) { try { showPlayers = j["showPlayers"].get<bool>(); } catch (...) {} }
    if (j.contains("showMobs")) { try { showMobs = j["showMobs"].get<bool>(); } catch (...) {} }
}
void DamageCircleModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["effectDuration"] = damagecircle::safeDuration(effectDuration);
    j["effectSize"] = damagecircle::safeSize(effectSize);
    j["effectSpeed"] = damagecircle::safeSpeed(effectSpeed);
    j["showPlayers"] = showPlayers;
    j["showMobs"] = showMobs;
}
