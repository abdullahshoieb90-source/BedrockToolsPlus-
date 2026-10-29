#include "esp.hpp"
#include "blockoutline_geometry.hpp"
#include "hitbox_interp.hpp"
#include "thick_lines.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Vec3;
namespace offsets = bedrocktools::sdk::offsets;
using SignatureId = bedrocktools::memory::SignatureId;

using TessellatorBeginFn = void (*)(void*, void*, int, int, int);
using TessellatorColorFn = void (*)(void*, float, float, float, float);
using TessellatorVertexFn = void (*)(void*, float, float, float);
using RenderMeshImmediatelyFn = void (*)(void*, void*, void*, char*);
using RenderLevelFn = void (*)(void*, void*, void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorIsInvisibleFn = bool (*)(void*);

// Same fetch ABI the Hitbox overlay uses (Actor::fetchNearbyActorsSorted).
struct DistanceSortedActor {
    void* mActor;
    float mDistance;
    float _pad;
};
struct ActorVec {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* cap;
};
using ActorFetchNearbyFn = ActorVec (*)(void* actor, void* extent, int actorType);

struct HashedString {
    std::uint64_t hash = 0;
    std::string value;
    mutable const HashedString* lastMatch = nullptr;

    explicit HashedString(const char* text) : value(text ? text : "") {
        if (value.empty()) return;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        hash = kOffset;
        for (char ch : value) {
            hash = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        }
    }
};

// Bedrock's MaterialPtr is two pointers. The game owns materials returned by
// RenderMaterialGroup, so this non-owning mirror intentionally does not run
// a game-side destructor when the mod unloads (same as Block Outline).
struct MaterialPtr {
    void* data[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept : data{other.data[0], other.data[1]} {
        other.data[0] = nullptr;
        other.data[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            data[0] = other.data[0];
            data[1] = other.data[1];
            other.data[0] = nullptr;
            other.data[1] = nullptr;
        }
        return *this;
    }

    // User-provided (rather than `= default`) to match the game's
    // non-trivial return ABI for MaterialPtr exactly.
    ~MaterialPtr() {}
    explicit operator bool() const { return data[0] != nullptr; }
};

ESPModule* g_esp = nullptr;

TessellatorBeginFn s_tessBegin = nullptr;
TessellatorColorFn s_tessColor = nullptr;
TessellatorVertexFn s_tessVertex = nullptr;
RenderMeshImmediatelyFn s_renderMesh = nullptr;
RenderLevelFn s_renderLevelOriginal = nullptr;
ActorIsPlayerFn s_actorIsPlayer = nullptr;
ActorIsInvisibleFn s_actorIsInvisible = nullptr;
ActorFetchNearbyFn s_fetchNearby = nullptr;

std::uintptr_t s_renderMaterialGroup = 0;
MaterialPtr s_selectionMaterial;    // depth-tested, alpha ignored by the game
MaterialPtr s_translucentMaterial;  // depth-tested, alpha blending
MaterialPtr s_throughWallsMaterial; // no depth test against terrain

void* s_localPlayer = nullptr;

constexpr float kMinEspRange = 8.0f;
constexpr float kMaxEspRange = 128.0f;

float clampedEspRange(float range) {
    if (!std::isfinite(range)) return 64.0f;
    return std::clamp(range, kMinEspRange, kMaxEspRange);
}

std::uintptr_t resolveAdrp(std::uint32_t* instructions,
                           std::size_t count,
                           std::uint32_t targetRegister) {
    if (!instructions) return 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t instruction = instructions[i];
        if ((instruction & 0x1F) != targetRegister) continue;

        if ((instruction & 0x9F000000) == 0x90000000) {
            const std::uint64_t immediateBits =
                ((static_cast<std::uint64_t>((instruction >> 3) & 0x1FFFFC)) |
                 (static_cast<std::uint64_t>((instruction >> 29) & 3))) << 43;
            const auto immediate = static_cast<std::int64_t>(immediateBits) >> 31;
            const std::uintptr_t page =
                (reinterpret_cast<std::uintptr_t>(&instructions[i]) & ~0xFFFULL) + immediate;

            for (std::size_t j = i + 1; j < count; ++j) {
                const std::uint32_t add = instructions[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetRegister &&
                    (add & 0x1F) == targetRegister) {
                    std::uint32_t immediate12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) immediate12 <<= 12;
                    return page + immediate12;
                }
                if ((add & 0x1F) == targetRegister) break;
            }
        }

        if ((instruction & 0x9F000000) == 0x10000000) {
            const std::uint64_t immediateBits =
                ((static_cast<std::uint64_t>((instruction >> 3) & 0x1FFFFC)) |
                 static_cast<std::uint64_t>(instruction >> 29)) << 43;
            const auto immediate = static_cast<std::int64_t>(immediateBits) >> 43;
            return reinterpret_cast<std::uintptr_t>(&instructions[i]) + immediate;
        }
    }
    return 0;
}

MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};
    auto** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[offsets::VTable::RenderMaterialGroup_getMaterial]) return {};

    HashedString hashedName(name);
    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    auto getMaterialFn = reinterpret_cast<GetMaterialFn>(
        vtable[offsets::VTable::RenderMaterialGroup_getMaterial]);
    return getMaterialFn(reinterpret_cast<void*>(s_renderMaterialGroup), &hashedName);
}

void ensureMaterials() {
    if (!s_renderMaterialGroup) return;
    if (!s_selectionMaterial) s_selectionMaterial = getMaterial("selection_box");

    // selection_box discards the tessellator alpha (no Blending state), so
    // the translucent Fill pass uses the nametag material instead: blending
    // enabled, still depth-tested (see Block Outline).
    if (!s_translucentMaterial) s_translucentMaterial = getMaterial("name_tag_depth_tested");

    // ui_fill_color has no terrain-depth test in current Bedrock builds,
    // which is exactly what Through Walls needs. If a build does not expose
    // it, getMaterial returns empty and rendering falls back to the
    // depth-tested materials.
    if (!s_throughWallsMaterial) {
        static constexpr const char* kCandidates[] = {
            "ui_fill_color",
            "ui_textured_and_glcolor",
        };
        for (const char* candidate : kCandidates) {
            s_throughWallsMaterial = getMaterial(candidate);
            if (s_throughWallsMaterial) break;
        }
    }
}

AABB getActorAABB(void* actor) {
    AABB aabb{{0, 0, 0}, {0, 0, 0}};
    const auto actorAddr = reinterpret_cast<std::uintptr_t>(actor);
    const auto builtIn = *reinterpret_cast<const std::uintptr_t*>(
        actorAddr + offsets::Actor::mStateVectorComponent);
    if (!builtIn) return aabb;
    const auto shape = *reinterpret_cast<const std::uintptr_t*>(
        actorAddr + offsets::Actor::mStateVectorComponent +
        offsets::BuiltInActorComponents::mAABBShapeComponent);
    if (shape) {
        aabb = *reinterpret_cast<const AABB*>(shape + offsets::AABBShapeComponent::mAABB);
    }
    return aabb;
}

bool hasCategory(void* actor, std::uint32_t categoryBit) {
    const auto categories = *reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<std::uintptr_t>(actor) + offsets::Actor::mCategories);
    return (categories & categoryBit) != 0;
}

bool isDrawableBox(const AABB& box) {
    if (box.min.x == 0.0f && box.min.y == 0.0f && box.min.z == 0.0f &&
        box.max.x == 0.0f && box.max.y == 0.0f && box.max.z == 0.0f) {
        return false;
    }
    return hitbox::isFinite(box.min) && hitbox::isFinite(box.max) &&
           box.max.x >= box.min.x && box.max.y >= box.min.y && box.max.z >= box.min.z;
}

// Per-actor render-position interpolation, exactly like the Hitbox overlay:
// the collision box is a tick sample, the mesh is drawn interpolated, so the
// ESP box must be moved onto the interpolated position or it lags a tick
// behind every moving entity. The sample history is this module's own (the
// two overlays may be enabled independently).
struct EspInterp {
    hitbox::Track track;
    std::chrono::steady_clock::time_point sampleTime{};
    bool hasSampleTime = false;
    std::uint64_t lastSeenFrame = 0;
};

std::unordered_map<void*, EspInterp> s_interpHistory;
std::uint64_t s_frameCounter = 0;

void trimInterpHistory() {
    if (s_interpHistory.size() <= 256) return;
    for (auto it = s_interpHistory.begin(); it != s_interpHistory.end();) {
        if (s_frameCounter - it->second.lastSeenFrame > 120) it = s_interpHistory.erase(it);
        else ++it;
    }
}

AABB getSmoothedAABB(void* actor) {
    AABB aabb = getActorAABB(actor);
    if (!isDrawableBox(aabb)) return aabb;

    const auto now = std::chrono::steady_clock::now();
    EspInterp& entry = s_interpHistory[actor];
    entry.lastSeenFrame = s_frameCounter;

    float elapsed = 0.0f;
    if (entry.hasSampleTime) {
        elapsed = std::chrono::duration<float>(now - entry.sampleTime).count();
        if (!(elapsed > 0.0f)) elapsed = 0.0f;
        if (elapsed > 1.0f) elapsed = 1.0f;
    }

    const Vec3 center{(aabb.min.x + aabb.max.x) * 0.5f,
                      (aabb.min.y + aabb.max.y) * 0.5f,
                      (aabb.min.z + aabb.max.z) * 0.5f};
    if (hitbox::pushSample(entry.track, center, elapsed)) {
        entry.sampleTime = now;
        entry.hasSampleTime = true;
        elapsed = 0.0f;
    }

    if (!g_esp || !g_esp->smoothBoxes) return aabb;
    if (!entry.track.hasPrev) return aabb;

    const float alpha = hitbox::sampleFraction(elapsed, entry.track.intervalSeconds);
    const Vec3 offset = hitbox::sampleOffset(entry.track.prevCenter, entry.track.curCenter, alpha);
    if (!hitbox::isFinite(offset)) return aabb;

    aabb.min.x += offset.x;
    aabb.min.y += offset.y;
    aabb.min.z += offset.z;
    aabb.max.x += offset.x;
    aabb.max.y += offset.y;
    aabb.max.z += offset.z;
    return aabb;
}

void emitVertex(void* tessellator, const Vec3& point, const Vec3& camera) {
    s_tessVertex(tessellator, point.x - camera.x, point.y - camera.y, point.z - camera.z);
}

void setTessellatorColor(void* tessellator, std::uint32_t rgb, float alpha) {
    s_tessColor(tessellator,
                static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                static_cast<float>(rgb & 0xFF) / 255.0f,
                std::clamp(alpha, 0.0f, 1.0f));
}

void flushMesh(void* screenContext, void* tessellator, void* material) {
    char pad[0x58]{};
    s_renderMesh(screenContext, tessellator, material, pad);
}

// Geometry collected for one actor group (players/mobs/items) so each color
// costs at most one begin/render per pass instead of one per actor.
struct GroupGeometry {
    std::uint32_t rgb = 0xFFFFFFu;
    std::vector<std::pair<Vec3, Vec3>> lines;
    std::vector<blockoutline::Face> faces;
};

void drawGroup(void* screenContext, void* tessellator,
               const GroupGeometry& group, const Vec3& camera,
               void* lineMaterial, void* fillMaterial,
               float fillAlpha, float thickness) {
    // Fill first so the box edges stay readable on top of the shell.
    if (!group.faces.empty() && fillAlpha > 0.001f) {
        s_tessBegin(tessellator, nullptr, 1, static_cast<int>(group.faces.size() * 4), 0);
        setTessellatorColor(tessellator, group.rgb, fillAlpha);
        for (const auto& face : group.faces) {
            for (const auto& vertex : face) emitVertex(tessellator, vertex, camera);
        }
        flushMesh(screenContext, tessellator, fillMaterial);
    }

    if (group.lines.empty()) return;

    const float safeThickness = std::clamp(thickness, 1.0f, 10.0f);
    if (safeThickness > 1.05f) {
        // GLES ignores glLineWidth: above the hairline every segment becomes
        // a camera-facing quad and shared corners get joint dots (see
        // thick_lines.hpp, shared with Hitbox/Block Outline).
        const float halfWidth = safeThickness * 0.005f;
        const auto joints = thicklines::collectJoints(group.lines);
        s_tessBegin(tessellator, nullptr, 1,
                    thicklines::thickVertexCount(group.lines.size(), joints.size()), 0);
        setTessellatorColor(tessellator, group.rgb, 1.0f);
        auto emit = [&tessellator](const Vec3& v) { s_tessVertex(tessellator, v.x, v.y, v.z); };
        for (const auto& line : group.lines) {
            thicklines::emitSegment(line.first, line.second, camera, halfWidth, emit);
        }
        for (const auto& joint : joints) {
            thicklines::emitJointDot(joint, camera, halfWidth, emit);
        }
        flushMesh(screenContext, tessellator, lineMaterial);
    }

    // Hairline pass: the whole renderer at thickness 1, and a crisp core
    // inside the thick quads above it.
    s_tessBegin(tessellator, nullptr, 4, static_cast<int>(group.lines.size() * 2), 0);
    setTessellatorColor(tessellator, group.rgb, 1.0f);
    for (const auto& line : group.lines) {
        emitVertex(tessellator, line.first, camera);
        emitVertex(tessellator, line.second, camera);
    }
    flushMesh(screenContext, tessellator, lineMaterial);
}

void renderEsp(void* levelRenderer, void* screenContext) {
    if (!g_esp || !g_esp->enabled) return;
    if (!s_localPlayer || reinterpret_cast<std::uintptr_t>(s_localPlayer) < 0x1000) return;
    if (!levelRenderer || reinterpret_cast<std::uintptr_t>(levelRenderer) < 0x1000 ||
        !screenContext || reinterpret_cast<std::uintptr_t>(screenContext) < 0x1000) {
        return;
    }
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh || !s_fetchNearby) return;
    if (!g_esp->box && !g_esp->fill && !g_esp->tracers) return;
    if (!g_esp->showPlayers && !g_esp->showMobs && !g_esp->showItems) return;

    const auto screenAddress = reinterpret_cast<std::uintptr_t>(screenContext);
    const auto tessellatorAddress = *reinterpret_cast<const std::uintptr_t*>(
        screenAddress + offsets::ScreenContext::mTessellator);
    if (tessellatorAddress < 0x1000) return;
    void* tessellator = reinterpret_cast<void*>(tessellatorAddress);

    const auto playerRenderer = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(levelRenderer) +
        offsets::LevelRenderer::mLevelRendererPlayer);
    if (playerRenderer < 0x1000) return;

    const Vec3 camera = *reinterpret_cast<const Vec3*>(
        playerRenderer + offsets::LevelRendererPlayer::mCamPos);

    ensureMaterials();

    void* lineMaterial = s_selectionMaterial ? static_cast<void*>(&s_selectionMaterial) : nullptr;
    void* fillMaterial = s_translucentMaterial ? static_cast<void*>(&s_translucentMaterial)
                                                : lineMaterial;
    // Last-resort line material: the renderer's embedded selection overlay,
    // only when it looks like a populated MaterialPtr (see Block Outline).
    if (!lineMaterial) {
        const std::uintptr_t embeddedAddress =
            playerRenderer + offsets::LevelRendererPlayer::mSelectionOverlayMaterial;
        const auto embeddedData = *reinterpret_cast<const std::uintptr_t*>(embeddedAddress);
        if (embeddedData >= 0x1000) lineMaterial = reinterpret_cast<void*>(embeddedAddress);
        if (!fillMaterial) fillMaterial = lineMaterial;
    }
    if (g_esp->throughWalls && s_throughWallsMaterial) {
        lineMaterial = static_cast<void*>(&s_throughWallsMaterial);
        fillMaterial = static_cast<void*>(&s_throughWallsMaterial);
    }
    if (!lineMaterial || !fillMaterial) return;

    ++s_frameCounter;
    trimInterpHistory();

    const float range = clampedEspRange(g_esp->espRange);
    Vec3 extent{range, range, range};
    const ActorVec actors = s_fetchNearby(s_localPlayer, &extent, 1);
    if (!actors.begin || !actors.end) return;

    // Group order: players, mobs, items.
    std::array<GroupGeometry, 3> groups;
    groups[0].rgb = g_esp->showPlayersColor & 0x00FFFFFFu;
    groups[1].rgb = g_esp->showMobsColor & 0x00FFFFFFu;
    groups[2].rgb = g_esp->showItemsColor & 0x00FFFFFFu;

    for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
        void* actor = it->mActor;
        if (!actor || actor == s_localPlayer) continue;

        // Players carry the mob category bit too, so test them first (same
        // classification order as Hitbox/Kill Aura).
        std::size_t groupIndex;
        const bool isPlayer = s_actorIsPlayer ? s_actorIsPlayer(actor)
                                              : hasCategory(actor, offsets::ActorCategories::IsPlayer);
        if (isPlayer) {
            if (!g_esp->showPlayers) continue;
            groupIndex = 0;
        } else if (hasCategory(actor, offsets::ActorCategories::IsMob)) {
            if (!g_esp->showMobs) continue;
            groupIndex = 1;
        } else {
            if (!g_esp->showItems) continue;
            groupIndex = 2;
        }

        if (!g_esp->showInvisible && s_actorIsInvisible && s_actorIsInvisible(actor)) continue;

        const AABB box = getSmoothedAABB(actor);
        if (!isDrawableBox(box)) continue;

        GroupGeometry& group = groups[groupIndex];
        const blockoutline::Box outlineBox{box.min, box.max};

        if (g_esp->box) {
            for (const auto& edge : blockoutline::boxEdges(outlineBox)) {
                group.lines.emplace_back(edge.from, edge.to);
            }
        }
        if (g_esp->tracers) {
            // From just in front of the camera (a vertex exactly on the
            // camera can be clipped into a screen-filling streak) to the
            // center of the box.
            const Vec3 center{(box.min.x + box.max.x) * 0.5f,
                              (box.min.y + box.max.y) * 0.5f,
                              (box.min.z + box.max.z) * 0.5f};
            Vec3 dir{center.x - camera.x, center.y - camera.y, center.z - camera.z};
            const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            Vec3 start = camera;
            if (length > 1e-4f) {
                const float step = 0.1f / length;
                start = Vec3{camera.x + dir.x * step, camera.y + dir.y * step, camera.z + dir.z * step};
            }
            group.lines.emplace_back(start, center);
        }
        if (g_esp->fill) {
            for (const auto& face : blockoutline::boxFaces(outlineBox)) {
                group.faces.push_back(face);
            }
        }
    }

    bool anyGeometry = false;
    for (const auto& group : groups) {
        if (!group.lines.empty() || !group.faces.empty()) anyGeometry = true;
    }
    if (!anyGeometry) return;

    const auto colorHolderAddress = *reinterpret_cast<const std::uintptr_t*>(
        screenAddress + offsets::ScreenContext::mColorHolder);
    if (colorHolderAddress < 0x1000) return;
    auto* colorHolder = reinterpret_cast<float*>(colorHolderAddress);
    const float savedColor[4] = {colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]};
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    const float fillAlpha = g_esp->fill ? std::clamp(g_esp->fillOpacity, 0.0f, 1.0f) : 0.0f;
    for (const auto& group : groups) {
        drawGroup(screenContext, tessellator, group, camera,
                  lineMaterial, fillMaterial, fillAlpha, g_esp->lineThickness);
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

void renderLevelHook(void* levelRenderer, void* screenContext, void* renderParams) {
    if (s_renderLevelOriginal) s_renderLevelOriginal(levelRenderer, screenContext, renderParams);
    renderEsp(levelRenderer, screenContext);
}

// --- config helpers (same "#RRGGBB" storage as Hitbox/Block Outline) -------

bool readBool(const nlohmann::json& json, const char* key, bool& out) {
    if (!json.contains(key)) return false;
    try {
        out = json[key].get<bool>();
        return true;
    } catch (...) {
        return false;
    }
}

bool readFloat(const nlohmann::json& json, const char* key, float& out) {
    if (!json.contains(key)) return false;
    try {
        out = json[key].get<float>();
        return true;
    } catch (...) {
        return false;
    }
}

void readColor(const nlohmann::json& json, const char* key, std::uint32_t& out) {
    if (!json.contains(key) || !json[key].is_string()) return;
    std::string text;
    try {
        text = json[key].get<std::string>();
    } catch (...) {
        return;
    }
    if (text.empty()) return;
    if (text[0] == '#') {
        text.erase(0, 1);
    } else if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.erase(0, 2);
    }
    try {
        const auto parsed = static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
        out = 0xFF000000u | (parsed & 0x00FFFFFFu);
    } catch (...) {
    }
}

std::string colorString(std::uint32_t color) {
    char text[10]{};
    std::snprintf(text, sizeof(text), "#%06X", color & 0x00FFFFFFu);
    return text;
}

} // namespace

ESPModule::ESPModule()
    : Module("ESP",
             "Draws players, mobs and items through walls with boxes, fill and tracers.") {
    showInMenu = true;
    hideInHudEditor = true; // world overlay, not a HUD element
    g_esp = this;
}

ESPModule::~ESPModule() {
    if (g_esp == this) g_esp = nullptr;
}

void ESPModule::onInit() {
    const auto renderLevel = bedrocktools::memory::resolve(SignatureId::RenderLevel);
    if (renderLevel) m_patchTarget = reinterpret_cast<void*>(renderLevel);

    const auto tessBegin = bedrocktools::memory::resolve(SignatureId::TessellatorBegin);
    const auto tessColor = bedrocktools::memory::resolve(SignatureId::TessellatorColor);
    const auto tessVertex = bedrocktools::memory::resolve(SignatureId::TessellatorVertex);
    if (tessBegin) s_tessBegin = reinterpret_cast<TessellatorBeginFn>(tessBegin);
    if (tessColor) s_tessColor = reinterpret_cast<TessellatorColorFn>(tessColor);
    if (tessVertex) s_tessVertex = reinterpret_cast<TessellatorVertexFn>(tessVertex);

    auto renderMesh = bedrocktools::memory::resolve(SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!renderMesh) {
        renderMesh = bedrocktools::memory::resolve(SignatureId::MeshHelpersRenderMeshImmediately);
    }
    if (renderMesh) s_renderMesh = reinterpret_cast<RenderMeshImmediatelyFn>(renderMesh);

    const auto isPlayer = bedrocktools::memory::resolve(SignatureId::ActorIsPlayer);
    if (isPlayer) s_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(isPlayer);

    const auto isInvisible = bedrocktools::memory::resolve(SignatureId::ActorIsInvisible);
    if (isInvisible) s_actorIsInvisible = reinterpret_cast<ActorIsInvisibleFn>(isInvisible);

    const auto fetch = bedrocktools::memory::resolve(SignatureId::ActorFetchNearbyActorsSorted);
    if (fetch) s_fetchNearby = reinterpret_cast<ActorFetchNearbyFn>(fetch);

    const auto materialGroup = bedrocktools::memory::resolve(SignatureId::RenderMaterialGroupCommon);
    if (materialGroup) {
        const auto groupAddress = resolveAdrp(
            reinterpret_cast<std::uint32_t*>(materialGroup), 2, 0);
        if (groupAddress) {
            s_renderMaterialGroup = groupAddress + offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { s_localPlayer = event.player; });
}

void ESPModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    const auto handle = bedrocktools::hooks::install(
        m_patchTarget,
        reinterpret_cast<void*>(&renderLevelHook),
        reinterpret_cast<void**>(&s_renderLevelOriginal));
    m_patched = handle != nullptr;
}

void ESPModule::onEnable() {
    applyPatch();
}

void ESPModule::onDisable() {
    // Deliberately keeps the interpolation history: clearing it here would
    // race a render thread that is already past the enabled check
    // mid-frame, and stale entries are harmless anyway - pushSample treats
    // an implausible jump (teleport / recycled pointer) as a fresh track,
    // and trimInterpHistory drops actors unseen for 120 frames.
}

void ESPModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    readFloat(j, "espRange", espRange);
    readBool(j, "showPlayers", showPlayers);
    readColor(j, "showPlayersColor", showPlayersColor);
    readBool(j, "showMobs", showMobs);
    readColor(j, "showMobsColor", showMobsColor);
    readBool(j, "showItems", showItems);
    readColor(j, "showItemsColor", showItemsColor);
    readBool(j, "showInvisible", showInvisible);
    readBool(j, "box", box);
    readFloat(j, "lineThickness", lineThickness);
    readBool(j, "fill", fill);
    readFloat(j, "fillOpacity", fillOpacity);
    readBool(j, "tracers", tracers);
    readBool(j, "throughWalls", throughWalls);
    readBool(j, "smoothBoxes", smoothBoxes);

    espRange = clampedEspRange(espRange);
    lineThickness = std::clamp(lineThickness, 1.0f, 10.0f);
    fillOpacity = std::clamp(fillOpacity, 0.0f, 1.0f);
    showPlayersColor = 0xFF000000u | (showPlayersColor & 0x00FFFFFFu);
    showMobsColor = 0xFF000000u | (showMobsColor & 0x00FFFFFFu);
    showItemsColor = 0xFF000000u | (showItemsColor & 0x00FFFFFFu);
}

void ESPModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["espRange"] = clampedEspRange(espRange);
    j["showPlayers"] = showPlayers;
    j["showPlayersColor"] = colorString(showPlayersColor);
    j["showMobs"] = showMobs;
    j["showMobsColor"] = colorString(showMobsColor);
    j["showItems"] = showItems;
    j["showItemsColor"] = colorString(showItemsColor);
    j["showInvisible"] = showInvisible;
    j["box"] = box;
    j["lineThickness"] = std::clamp(lineThickness, 1.0f, 10.0f);
    j["fill"] = fill;
    j["fillOpacity"] = std::clamp(fillOpacity, 0.0f, 1.0f);
    j["tracers"] = tracers;
    j["throughWalls"] = throughWalls;
    j["smoothBoxes"] = smoothBoxes;
}
