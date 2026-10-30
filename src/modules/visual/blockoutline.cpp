#include "blockoutline.hpp"
#include "blockoutline_geometry.hpp"
#include "thick_lines.hpp"

#include "core/memory/Hooks.hpp"
#include "core/RenderState.hpp"
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
// Blending, still depth-tested (see ensureMaterials).
MaterialPtr s_translucentMaterial;

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

    // selection_box has no Blending render state in the vanilla material
    // definitions, so the game discards the tessellator's alpha channel and
    // the Opacity/Pulse sliders have no visible effect on it. When a pass
    // needs real translucency it swaps to name_tag_depth_tested instead:
    // same material group, alpha blending enabled, and it still depth-tests
    // against terrain (depthFunc LessEqual) with no depth writes.
    if (!s_translucentMaterial) s_translucentMaterial = getMaterial("name_tag_depth_tested");

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
    // consistent effect on Android. Segment quads are cut square at the box
    // corners and joint dots close the corners, so higher thickness values
    // keep clean right angles instead of growing spurs past the corners.
    if (thick) {
        const float halfWidth = safeThickness * 0.005f;
        const auto joints = thicklines::collectJoints(edges);
        s_tessBegin(tessellator, nullptr, 1,
                    thicklines::thickVertexCount(edges.size(), joints.size()), 0);
        setTessellatorColor(tessellator, rgb, alpha);

        auto emit = [&tessellator](const bedrocktools::sdk::Vec3& v) {
            s_tessVertex(tessellator, v.x, v.y, v.z);
        };
        for (const auto& edge : edges) {
            thicklines::emitSegment(edge.from, edge.to, camera, halfWidth, emit);
        }
        for (const auto& joint : joints) {
            thicklines::emitJointDot(joint, camera, halfWidth, emit);
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
    const float outlineAlpha = blockoutline::clampedOpacity(g_blockOutline->outlineOpacity, pulse);
    const float fillAlpha = blockoutline::clampedOpacity(g_blockOutline->fillOpacity, pulse);

    void* normalOutlineMaterial = s_selectionMaterial
        ? static_cast<void*>(&s_selectionMaterial)
        : nullptr;

    // The embedded LevelRendererPlayer selection-overlay material lives at a
    // build-specific offset. Using it as the primary Fill material crashed the
    // renderer when the offset/layout did not match, so it is only a last
    // resort and only when it looks like a populated MaterialPtr.
    void* embeddedSelectionOverlay = nullptr;
    {
        const std::uintptr_t embeddedAddress = playerRenderer +
            bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial;
        const std::uintptr_t embeddedData = *reinterpret_cast<const std::uintptr_t*>(embeddedAddress);
        if (embeddedData >= 0x1000) embeddedSelectionOverlay = reinterpret_cast<void*>(embeddedAddress);
    }
    if (!normalOutlineMaterial) normalOutlineMaterial = embeddedSelectionOverlay;

    // Fill uses the same game-owned material as the outline (known-good here)
    // so both passes share depth/blend behavior. Through Walls swaps both to
    // the no-depth material so their occlusion agrees.
    void* normalFillMaterial = normalOutlineMaterial ? normalOutlineMaterial : embeddedSelectionOverlay;
    void* outlineMaterial = normalOutlineMaterial;
    void* fillMaterial = normalFillMaterial;

    // Passes that can render below full alpha need the blending material; the
    // vanilla selection_box material ignores the alpha channel entirely.
    if (s_translucentMaterial) {
        void* translucent = static_cast<void*>(&s_translucentMaterial);
        if (blockoutline::needsBlending(g_blockOutline->outlineOpacity, g_blockOutline->pulse)) {
            outlineMaterial = translucent;
        }
        if (blockoutline::needsBlending(g_blockOutline->fillOpacity, g_blockOutline->pulse)) {
            fillMaterial = translucent;
        }
    }
    if (g_blockOutline->throughWalls && s_throughWallsMaterial) {
        outlineMaterial = static_cast<void*>(&s_throughWallsMaterial);
        fillMaterial = static_cast<void*>(&s_throughWallsMaterial);
    }
    if (!outlineMaterial || !fillMaterial) return;

    bedrocktools::core::ScopedShaderColor shaderColor(screenContext);
    if (!shaderColor) return;

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
                 fillAlpha);
    }
    if (g_blockOutline->outline) {
        drawOutline(screenContext,
                    tessellator,
                    outlineMaterial,
                    box,
                    camera,
                    outlineRgb,
                    outlineAlpha,
                    g_blockOutline->lineThickness);
    }
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
