#include "blockoutline.hpp"
#include "blockoutline_geometry.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>

namespace {

using TessellatorBeginFn = void (*)(void*, void*, int, int, int);
using TessellatorColorFn = void (*)(void*, float, float, float, float);
using TessellatorVertexFn = void (*)(void*, float, float, float);
using RenderMeshImmediatelyFn = void (*)(void*, void*, void*, char*);
using LevelGetHitResultFn = void* (*)(void*);
using RenderLevelFn = void (*)(void*, void*, void*);

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
// RenderMaterialGroup, so this non-owning mirror intentionally does not run a
// game-side destructor when the mod unloads.
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

    // User-provided (rather than `= default`) to match the game's non-trivial
    // return ABI for MaterialPtr exactly.
    ~MaterialPtr() {}
    explicit operator bool() const { return data[0] != nullptr; }
};

struct TargetSnapshot {
    bool valid = false;
    bedrocktools::sdk::BlockPos position{};
    int facing = blockoutline::Up;
    std::chrono::steady_clock::time_point updated{};
};

BlockOutlineModule* g_blockOutline = nullptr;

TessellatorBeginFn s_tessBegin = nullptr;
TessellatorColorFn s_tessColor = nullptr;
TessellatorVertexFn s_tessVertex = nullptr;
RenderMeshImmediatelyFn s_renderMesh = nullptr;
LevelGetHitResultFn s_levelGetHitResult = nullptr;
RenderLevelFn s_renderLevelOriginal = nullptr;

std::uintptr_t s_renderMaterialGroup = 0;
MaterialPtr s_selectionMaterial;
MaterialPtr s_throughWallsMaterial;

std::mutex s_targetMutex;
TargetSnapshot s_target;

constexpr float kBoxExpansion = 0.002f;
constexpr auto kTargetMaxAge = std::chrono::milliseconds(500);

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
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]) return {};

    HashedString hashedName(name);
    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    auto getMaterialFn = reinterpret_cast<GetMaterialFn>(
        vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]);
    return getMaterialFn(reinterpret_cast<void*>(s_renderMaterialGroup), &hashedName);
}

void ensureMaterials() {
    if (!s_renderMaterialGroup) return;
    if (!s_selectionMaterial) s_selectionMaterial = getMaterial("selection_box");

    // ui_fill_color has no terrain-depth test in current Bedrock builds and is
    // therefore suitable for the explicit Through Walls option. If a future
    // build does not expose it, rendering safely falls back to selection_box.
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

void clearTarget() {
    std::lock_guard<std::mutex> lock(s_targetMutex);
    s_target = {};
}

void updateTarget(void* player) {
    if (!g_blockOutline || !g_blockOutline->enabled || !player || !s_levelGetHitResult) {
        clearTarget();
        return;
    }

    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    const std::uintptr_t level = *reinterpret_cast<std::uintptr_t*>(
        playerAddress + bedrocktools::sdk::offsets::Actor::mLevel);
    if (level < 0x1000) {
        clearTarget();
        return;
    }

    void* rawHit = s_levelGetHitResult(reinterpret_cast<void*>(level));
    if (!rawHit || reinterpret_cast<std::uintptr_t>(rawHit) < 0x1000) {
        clearTarget();
        return;
    }

    const auto hitAddress = reinterpret_cast<std::uintptr_t>(rawHit);
    const int type = *reinterpret_cast<const int*>(
        hitAddress + bedrocktools::sdk::offsets::HitResult::mType);
    if (type != bedrocktools::sdk::offsets::HitResult::TypeBlock) {
        clearTarget();
        return;
    }

    TargetSnapshot next;
    next.valid = true;
    next.position = *reinterpret_cast<const bedrocktools::sdk::BlockPos*>(
        hitAddress + bedrocktools::sdk::offsets::HitResult::mBlockPos);
    next.facing = *reinterpret_cast<const int*>(
        hitAddress + bedrocktools::sdk::offsets::HitResult::mFacing);
    if (!blockoutline::validFacing(next.facing)) next.facing = blockoutline::Up;
    next.updated = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(s_targetMutex);
    s_target = next;
}

bool currentTarget(TargetSnapshot& out, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(s_targetMutex);
    if (!s_target.valid || s_target.updated.time_since_epoch().count() == 0 ||
        now - s_target.updated > kTargetMaxAge) {
        return false;
    }
    out = s_target;
    return true;
}

void emitVertex(void* tessellator,
                const bedrocktools::sdk::Vec3& point,
                const bedrocktools::sdk::Vec3& camera) {
    s_tessVertex(tessellator,
                 point.x - camera.x,
                 point.y - camera.y,
                 point.z - camera.z);
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

void drawFill(void* screenContext,
              void* tessellator,
              void* material,
              const blockoutline::Box& box,
              int facing,
              bool faceOnly,
              const bedrocktools::sdk::Vec3& camera,
              std::uint32_t rgb,
              float alpha) {
    if (alpha <= 0.001f) return;

    if (faceOnly) {
        const blockoutline::Face face = blockoutline::boxFace(box, facing);
        s_tessBegin(tessellator, nullptr, 1, 4, 0); // quad
        setTessellatorColor(tessellator, rgb, alpha);
        for (const auto& vertex : face) emitVertex(tessellator, vertex, camera);
    } else {
        const auto faces = blockoutline::boxFaces(box);
        s_tessBegin(tessellator, nullptr, 1, static_cast<int>(faces.size() * 4), 0);
        setTessellatorColor(tessellator, rgb, alpha);
        for (const auto& face : faces) {
            for (const auto& vertex : face) emitVertex(tessellator, vertex, camera);
        }
    }

    flushMesh(screenContext, tessellator, material);
}

void drawOutline(void* screenContext,
                 void* tessellator,
                 void* material,
                 const blockoutline::Box& box,
                 const bedrocktools::sdk::Vec3& camera,
                 std::uint32_t rgb,
                 float alpha,
                 float thickness) {
    if (alpha <= 0.001f) return;

    const auto edges = blockoutline::boxEdges(box);
    const float safeThickness = std::clamp(thickness, 1.0f, 10.0f);
    const bool thick = safeThickness > 1.05f;

    // GLES drivers generally ignore glLineWidth. Above the hairline setting,
    // render each edge as a camera-facing quad so the menu slider has a real,
    // consistent effect on Android.
    if (thick) {
        const float halfWidth = safeThickness * 0.005f;
        s_tessBegin(tessellator, nullptr, 1, static_cast<int>(edges.size() * 8), 0);
        setTessellatorColor(tessellator, rgb, alpha);

        for (const auto& edge : edges) {
            bedrocktools::sdk::Vec3 p1{
                edge.from.x - camera.x,
                edge.from.y - camera.y,
                edge.from.z - camera.z,
            };
            bedrocktools::sdk::Vec3 p2{
                edge.to.x - camera.x,
                edge.to.y - camera.y,
                edge.to.z - camera.z,
            };

            float dx = p2.x - p1.x;
            float dy = p2.y - p1.y;
            float dz = p2.z - p1.z;
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (length < 0.00001f) continue;
            dx /= length;
            dy /= length;
            dz /= length;

            // Camera is the origin in this coordinate space. dir x midpoint
            // gives a vector perpendicular to both the edge and view ray.
            const float mx = (p1.x + p2.x) * 0.5f;
            const float my = (p1.y + p2.y) * 0.5f;
            const float mz = (p1.z + p2.z) * 0.5f;
            float sx = dy * mz - dz * my;
            float sy = dz * mx - dx * mz;
            float sz = dx * my - dy * mx;
            float sideLength = std::sqrt(sx * sx + sy * sy + sz * sz);
            if (sideLength < 0.00001f) {
                // Looking directly along an edge: choose a stable arbitrary
                // perpendicular instead of dropping that edge for one frame.
                if (std::fabs(dy) < 0.9f) {
                    sx = -dz; sy = 0.0f; sz = dx;
                } else {
                    sx = 1.0f; sy = 0.0f; sz = 0.0f;
                }
                sideLength = std::sqrt(sx * sx + sy * sy + sz * sz);
                if (sideLength < 0.00001f) continue;
            }
            sx = sx / sideLength * halfWidth;
            sy = sy / sideLength * halfWidth;
            sz = sz / sideLength * halfWidth;

            // Slightly overlap neighboring edge ends so all eight corners stay
            // closed at high thickness values.
            const float ex = dx * halfWidth;
            const float ey = dy * halfWidth;
            const float ez = dz * halfWidth;
            const bedrocktools::sdk::Vec3 quad[4] = {
                {p1.x - ex - sx, p1.y - ey - sy, p1.z - ez - sz},
                {p2.x + ex - sx, p2.y + ey - sy, p2.z + ez - sz},
                {p2.x + ex + sx, p2.y + ey + sy, p2.z + ez + sz},
                {p1.x - ex + sx, p1.y - ey + sy, p1.z - ez + sz},
            };
            for (const auto& vertex : quad) {
                s_tessVertex(tessellator, vertex.x, vertex.y, vertex.z);
            }
            // Both windings keep the strip visible with materials that enable
            // back-face culling.
            for (int i = 3; i >= 0; --i) {
                s_tessVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
            }
        }
        flushMesh(screenContext, tessellator, material);
    }

    // A final native line pass keeps distant edges crisp and is the complete
    // renderer for Thickness = 1.
    s_tessBegin(tessellator, nullptr, 4, static_cast<int>(edges.size() * 2), 0); // lines
    setTessellatorColor(tessellator, rgb, alpha);
    for (const auto& edge : edges) {
        emitVertex(tessellator, edge.from, camera);
        emitVertex(tessellator, edge.to, camera);
    }
    flushMesh(screenContext, tessellator, material);
}

void renderBlockOutline(void* levelRenderer, void* screenContext) {
    if (!g_blockOutline || !g_blockOutline->enabled) return;
    if (!levelRenderer || reinterpret_cast<std::uintptr_t>(levelRenderer) < 0x1000 ||
        !screenContext || reinterpret_cast<std::uintptr_t>(screenContext) < 0x1000) {
        return;
    }
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!g_blockOutline->outline && !g_blockOutline->fill) return;

    const auto now = std::chrono::steady_clock::now();
    TargetSnapshot target;
    if (!currentTarget(target, now)) return;

    const auto screenAddress = reinterpret_cast<std::uintptr_t>(screenContext);
    const std::uintptr_t tessellatorAddress = *reinterpret_cast<std::uintptr_t*>(
        screenAddress + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (tessellatorAddress < 0x1000) return;
    void* tessellator = reinterpret_cast<void*>(tessellatorAddress);

    const auto rendererAddress = reinterpret_cast<std::uintptr_t>(levelRenderer);
    const std::uintptr_t playerRenderer = *reinterpret_cast<std::uintptr_t*>(
        rendererAddress + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (playerRenderer < 0x1000) return;

    const bedrocktools::sdk::Vec3 camera = *reinterpret_cast<const bedrocktools::sdk::Vec3*>(
        playerRenderer + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);

    ensureMaterials();
    void* embeddedSelectionOverlay = reinterpret_cast<void*>(
        playerRenderer + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);
    void* normalOutlineMaterial = s_selectionMaterial
        ? static_cast<void*>(&s_selectionMaterial)
        : embeddedSelectionOverlay;
    // The embedded selection-overlay material blends vertex alpha and is a
    // better fit for translucent faces than selection_box. Through Walls uses
    // the same no-depth material for both passes so their occlusion agrees.
    void* normalFillMaterial = embeddedSelectionOverlay;
    void* outlineMaterial = normalOutlineMaterial;
    void* fillMaterial = normalFillMaterial;
    if (g_blockOutline->throughWalls && s_throughWallsMaterial) {
        outlineMaterial = static_cast<void*>(&s_throughWallsMaterial);
        fillMaterial = static_cast<void*>(&s_throughWallsMaterial);
    }
    if (!outlineMaterial || !fillMaterial) return;

    const std::uintptr_t colorHolderAddress = *reinterpret_cast<std::uintptr_t*>(
        screenAddress + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (colorHolderAddress < 0x1000) return;
    auto* colorHolder = reinterpret_cast<float*>(colorHolderAddress);
    const float savedColor[4] = {
        colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3],
    };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    const float pulse = blockoutline::pulseMultiplier(
        g_blockOutline->pulse, seconds, g_blockOutline->pulseSpeed);
    const std::uint32_t outlineRgb = blockoutline::animatedRgb(
        g_blockOutline->outlineColor,
        g_blockOutline->rainbow,
        seconds,
        g_blockOutline->rainbowSpeed);
    const std::uint32_t fillRgb = blockoutline::animatedRgb(
        g_blockOutline->fillColor,
        g_blockOutline->rainbow,
        seconds,
        g_blockOutline->rainbowSpeed);

    const blockoutline::Box box = blockoutline::makeBlockBox(target.position, kBoxExpansion);
    if (g_blockOutline->fill) {
        drawFill(screenContext,
                 tessellator,
                 fillMaterial,
                 box,
                 target.facing,
                 g_blockOutline->fillFaceOnly,
                 camera,
                 fillRgb,
                 blockoutline::clampedOpacity(g_blockOutline->fillOpacity, pulse));
    }
    if (g_blockOutline->outline) {
        drawOutline(screenContext,
                    tessellator,
                    outlineMaterial,
                    box,
                    camera,
                    outlineRgb,
                    blockoutline::clampedOpacity(g_blockOutline->outlineOpacity, pulse),
                    g_blockOutline->lineThickness);
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

void renderLevelHook(void* levelRenderer, void* screenContext, void* renderParams) {
    if (s_renderLevelOriginal) s_renderLevelOriginal(levelRenderer, screenContext, renderParams);
    renderBlockOutline(levelRenderer, screenContext);
}

template <typename T>
bool readFirst(const nlohmann::json& json,
               std::initializer_list<const char*> keys,
               T& destination) {
    for (const char* key : keys) {
        if (!json.contains(key)) continue;
        try {
            destination = json[key].get<T>();
            return true;
        } catch (...) {
        }
    }
    return false;
}

bool readColor(const nlohmann::json& json,
               std::initializer_list<const char*> keys,
               std::uint32_t& destination) {
    for (const char* key : keys) {
        if (!json.contains(key) || !json[key].is_string()) continue;
        std::string text;
        try {
            text = json[key].get<std::string>();
        } catch (...) {
            continue;
        }
        if (text.empty()) continue;
        if (text[0] == '#') {
            text.erase(0, 1);
        } else if (text.size() > 1 && text[0] == '0' &&
                   (text[1] == 'x' || text[1] == 'X')) {
            text.erase(0, 2);
        }
        try {
            const auto parsed = static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
            destination = 0xFF000000u | (parsed & 0x00FFFFFFu);
            return true;
        } catch (...) {
        }
    }
    return false;
}

std::string colorString(std::uint32_t color) {
    char text[10]{};
    std::snprintf(text, sizeof(text), "#%06X", color & 0x00FFFFFFu);
    return text;
}

} // namespace

BlockOutlineModule::BlockOutlineModule()
    : Module("Block Outline",
             "Customizes the selected block with an adjustable outline and optional fill. Supports color, opacity, Android-safe thickness, RGB, pulse, face-only fill and through-walls rendering.") {
    showInMenu = true;
    hideInHudEditor = true;
    g_blockOutline = this;
}

BlockOutlineModule::~BlockOutlineModule() {
    if (g_blockOutline == this) g_blockOutline = nullptr;
}

void BlockOutlineModule::onInit() {
    const std::uintptr_t renderLevel =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (renderLevel) m_patchTarget = reinterpret_cast<void*>(renderLevel);

    const std::uintptr_t tessBegin =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    const std::uintptr_t tessColor =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    const std::uintptr_t tessVertex =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tessBegin) s_tessBegin = reinterpret_cast<TessellatorBeginFn>(tessBegin);
    if (tessColor) s_tessColor = reinterpret_cast<TessellatorColorFn>(tessColor);
    if (tessVertex) s_tessVertex = reinterpret_cast<TessellatorVertexFn>(tessVertex);

    std::uintptr_t renderMesh = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!renderMesh) {
        renderMesh = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
    }
    if (renderMesh) s_renderMesh = reinterpret_cast<RenderMeshImmediatelyFn>(renderMesh);

    const std::uintptr_t hitResult =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelGetHitResult);
    if (hitResult) s_levelGetHitResult = reinterpret_cast<LevelGetHitResultFn>(hitResult);

    const std::uintptr_t materialGroup = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (materialGroup) {
        const std::uintptr_t groupAddress = resolveAdrp(
            reinterpret_cast<std::uint32_t*>(materialGroup), 2, 0);
        if (groupAddress) {
            s_renderMaterialGroup = groupAddress +
                bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { updateTarget(event.player); });
}

void BlockOutlineModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    const auto handle = bedrocktools::hooks::install(
        m_patchTarget,
        reinterpret_cast<void*>(&renderLevelHook),
        reinterpret_cast<void**>(&s_renderLevelOriginal));
    m_patched = handle != nullptr;
}

void BlockOutlineModule::onEnable() {
    applyPatch();
}

void BlockOutlineModule::onDisable() {
    clearTarget();
}

void BlockOutlineModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);

    // Current keys first, followed by aliases used by common/older Block
    // Outline configs so importing a config does not silently lose its look.
    readFirst(json, {"outline", "showOutline", "showoutline"}, outline);
    readColor(json, {"outlineColor", "color", "Color"}, outlineColor);
    readFirst(json, {"outlineOpacity", "opacity", "Opacity"}, outlineOpacity);
    readFirst(json, {"lineThickness", "thickness", "Thickness", "outlineWidth"}, lineThickness);

    readFirst(json, {"fill", "overlay", "showOverlay", "showoverlay"}, fill);
    readColor(json, {"fillColor", "overlayColor"}, fillColor);
    readFirst(json, {"fillOpacity", "overlayOpacity"}, fillOpacity);
    if (!readFirst(json, {"fillFaceOnly", "faceOverlay"}, fillFaceOnly)) {
        bool fullOverlay = true;
        if (readFirst(json, {"fullOverlay", "fulloverlay"}, fullOverlay)) {
            fillFaceOnly = !fullOverlay;
        }
    }

    readFirst(json, {"rainbow", "Rainbow"}, rainbow);
    readFirst(json, {"rainbowSpeed"}, rainbowSpeed);
    readFirst(json, {"pulse"}, pulse);
    readFirst(json, {"pulseSpeed"}, pulseSpeed);
    readFirst(json, {"throughWalls", "renderThrough", "fullOutline", "fulloutline"}, throughWalls);

    outlineOpacity = std::clamp(outlineOpacity, 0.0f, 1.0f);
    fillOpacity = std::clamp(fillOpacity, 0.0f, 1.0f);
    lineThickness = std::clamp(lineThickness, 1.0f, 10.0f);
    rainbowSpeed = std::clamp(rainbowSpeed, 0.05f, 1.0f);
    pulseSpeed = std::clamp(pulseSpeed, 0.05f, 1.0f);
    outlineColor = 0xFF000000u | (outlineColor & 0x00FFFFFFu);
    fillColor = 0xFF000000u | (fillColor & 0x00FFFFFFu);
}

void BlockOutlineModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);

    json["outline"] = outline;
    json["outlineColor"] = colorString(outlineColor);
    json["outlineOpacity"] = std::clamp(outlineOpacity, 0.0f, 1.0f);
    json["lineThickness"] = std::clamp(lineThickness, 1.0f, 10.0f);

    json["fill"] = fill;
    json["fillColor"] = colorString(fillColor);
    json["fillOpacity"] = std::clamp(fillOpacity, 0.0f, 1.0f);
    json["fillFaceOnly"] = fillFaceOnly;

    json["rainbow"] = rainbow;
    json["rainbowSpeed"] = std::clamp(rainbowSpeed, 0.05f, 1.0f);
    json["pulse"] = pulse;
    json["pulseSpeed"] = std::clamp(pulseSpeed, 0.05f, 1.0f);
    json["throughWalls"] = throughWalls;
}
