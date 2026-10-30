#include "jumpcircle.hpp"
#include "jumpcircle_geometry.hpp"
#include "blockoutline_geometry.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

JumpCircleModule* module = nullptr;
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
Vec3 previousFeet{};
jumpcircle::MotionTracker motion;
bool hasPrevious = false;
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
    rings.clear(); trackedPlayer = nullptr; hasPrevious = false; motion = {}; lastTick = {};
}

void tick(void* player) {
    if (!module || !module->enabled || reinterpret_cast<std::uintptr_t>(player) < 0x1000) {
        clearState(); return;
    }
    auto actor = reinterpret_cast<std::uintptr_t>(player);
    auto component = *reinterpret_cast<std::uintptr_t*>(actor + bedrocktools::sdk::offsets::Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent);
    if (component < 0x1000) { clearState(); return; }
    auto box = *reinterpret_cast<const AABB*>(component + AABBShapeComponent::mAABB);
    Vec3 feet{(box.min.x + box.max.x) * 0.5f, box.min.y, (box.min.z + box.max.z) * 0.5f};
    if (!std::isfinite(feet.x) || !std::isfinite(feet.y) || !std::isfinite(feet.z)) { clearState(); return; }
    auto now = Clock::now();
    std::lock_guard lock(stateMutex);
    if (trackedPlayer != player) {
        rings.clear(); hasPrevious = false; motion = {}; trackedPlayer = player;
    } else if (hasPrevious && now - lastTick > std::chrono::milliseconds(300)) {
        // Resuming after a pause must not invent an event from stale samples.
        // Keep persistent effects belonging to this player.
        hasPrevious = false; motion = {};
    }
    if (hasPrevious) {
        const float dy = feet.y - previousFeet.y;
        const float dx = feet.x - previousFeet.x, dz = feet.z - previousFeet.z;
        if (dx * dx + dz * dz >= 4.0f || std::abs(dy) > 4.0f) {
            // Do not leave stale effects or synthesize events on teleports.
            rings.clear(); motion = {};
        } else {
            const auto events = motion.update(dy);
            auto spawn = [&](const Vec3& position) {
                if (rings.size() >= jumpcircle::maxEffects) rings.erase(rings.begin());
                rings.push_back({{position.x, position.y + 0.025f, position.z}, now});
            };
            // On an immediate jump after landing, the previous feet position
            // is the best available ground sample for both effects.
            if (events.landing) spawn(events.takeoff ? previousFeet : feet);
            if (events.takeoff) spawn(previousFeet);
        }
    }
    previousFeet = feet; hasPrevious = true; lastTick = now;
    if (!module->persistent) {
        const float duration = jumpcircle::safeDuration(module->effectDuration);
        std::erase_if(rings, [now, duration](const Ring& ring) {
            return std::chrono::duration<float>(now - ring.start).count() >= duration;
        });
    }
}

void render(void* renderer, void* screen) {
    if (!module || !module->enabled || !tessBegin || !tessColor || !tessVertex || !renderMesh ||
        reinterpret_cast<std::uintptr_t>(renderer) < 0x1000 || reinterpret_cast<std::uintptr_t>(screen) < 0x1000) return;
    const auto now = Clock::now();
    std::vector<Ring> snapshot;
    {
        std::lock_guard lock(stateMutex);
        if (!hasPrevious || now - lastTick > std::chrono::milliseconds(300)) return;
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
    auto holder = *reinterpret_cast<std::uintptr_t*>(scr + ScreenContext::mColorHolder);
    auto lrp = *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(renderer) + LevelRenderer::mLevelRendererPlayer);
    if (tess < 0x1000 || holder < 0x1000 || lrp < 0x1000) return;
    auto cam = *reinterpret_cast<const Vec3*>(lrp + LevelRendererPlayer::mCamPos);
    auto* colors = reinterpret_cast<float*>(holder);
    const float saved[]{colors[0], colors[1], colors[2], colors[3]};
    colors[0] = colors[1] = colors[2] = colors[3] = 1.0f;
    const float duration = jumpcircle::safeDuration(module->effectDuration);
    const bool persistent = module->persistent;
    const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    const float speed = std::isfinite(module->rainbowSpeed) ? std::clamp(module->rainbowSpeed, 0.05f, 1.0f) : 0.20f;
    // Reduce the phase in double precision before conversion to float: the
    // steady clock may have accumulated millions of seconds on a phone.
    const auto rgb = module->rainbow
        ? blockoutline::hsvToRgb(static_cast<float>(std::fmod(seconds * speed, 1.0) * 360.0))
        : module->effectColor;
    const float red = ((rgb >> 16) & 255) / 255.0f;
    const float green = ((rgb >> 8) & 255) / 255.0f;
    const float blue = (rgb & 255) / 255.0f;
    const float colorAlpha = ((module->effectColor >> 24) & 255) / 255.0f;
    for (const auto& ring : snapshot) {
        float age = std::chrono::duration<float>(now - ring.start).count();
        if (age < 0 || (!persistent && age >= duration)) continue;
        tessBegin(reinterpret_cast<void*>(tess), nullptr, 1, jumpcircle::segments * jumpcircle::bands * 4, 0);
        jumpcircle::emitRing(ring.center, age, duration, persistent, static_cast<jumpcircle::Style>(std::clamp(module->style, 0, jumpcircle::styleCount - 1)), [&](jumpcircle::Point point, float alpha) {
            tessColor(reinterpret_cast<void*>(tess), red, green, blue, alpha * colorAlpha);
            tessVertex(reinterpret_cast<void*>(tess), point.x - cam.x, point.y - cam.y, point.z - cam.z);
        });
        char pad[0x100]{};
        renderMesh(screen, reinterpret_cast<void*>(tess), &translucent, pad);
    }
    for (int i = 0; i < 4; ++i) colors[i] = saved[i];
}
void renderHook(void* renderer, void* screen, void* params) {
    if (originalRender) originalRender(renderer, screen, params);
    render(renderer, screen);
}
}

JumpCircleModule::JumpCircleModule() : Module("Jump Circle", "Shows a configurable expanding glow when you jump, fall and land.") {
    module = this; hideInHudEditor = true;
}
JumpCircleModule::~JumpCircleModule() { if (module == this) module = nullptr; }
void JumpCircleModule::onInit() {
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
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { tick(event.player); });
}
void JumpCircleModule::onEnable() {
    clearState();
    if (!m_patched && m_patchTarget)
        m_patched = bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(&renderHook), reinterpret_cast<void**>(&originalRender)) != nullptr;
}
void JumpCircleModule::onDisable() { clearState(); }

void JumpCircleModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("effectStyle") && j["effectStyle"].is_string())
        style = jumpcircle::styleIndex(j["effectStyle"].get<std::string>());
    if (j.contains("effectDuration")) {
        try { effectDuration = jumpcircle::safeDuration(j["effectDuration"].get<float>()); } catch (...) {}
    }
    if (j.contains("effectColor") && j["effectColor"].is_string()) {
        const std::string text = j["effectColor"].get<std::string>();
        // The launcher's legacy color picker sends #AARRGGBB, even when the
        // initial value was #RRGGBB. Support both and preserve the alpha slider.
        // Malformed configs retain the previous color.
        if ((text.size() == 7 || text.size() == 9) && text[0] == '#' &&
            text.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos) {
            try {
                const auto value = static_cast<std::uint32_t>(std::stoul(text.substr(1), nullptr, 16));
                effectColor = text.size() == 7 ? 0xFF000000u | value : value;
            } catch (...) {}
        }
    }
    if (j.contains("rainbow")) { try { rainbow = j["rainbow"].get<bool>(); } catch (...) {} }
    if (j.contains("persistent")) { try { persistent = j["persistent"].get<bool>(); } catch (...) {} }
    if (j.contains("rainbowSpeed")) {
        try {
            const float speed = j["rainbowSpeed"].get<float>();
            if (std::isfinite(speed)) rainbowSpeed = std::clamp(speed, 0.05f, 1.0f);
        } catch (...) {}
    }
}
void JumpCircleModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["effectStyle"] = jumpcircle::styleRadioValue(style);
    j["effectDuration"] = jumpcircle::safeDuration(effectDuration);
    char color[10];
    std::snprintf(color, sizeof(color), "#%08X", effectColor);
    j["effectColor"] = color;
    j["rainbow"] = rainbow;
    j["rainbowSpeed"] = std::isfinite(rainbowSpeed) ? std::clamp(rainbowSpeed, 0.05f, 1.0f) : 0.20f;
    j["persistent"] = persistent;
}
