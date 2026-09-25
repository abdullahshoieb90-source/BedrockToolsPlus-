#include "lightoverlay.hpp"
#include "lightoverlay_glyphs.hpp"
#include "render_overlay.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace {

using bedrocktools::sdk::BlockPos;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;

namespace offsets = bedrocktools::sdk::offsets;

using BlockSourceGetBlockFn = void* (*)(void* region, const BlockPos& position);
using BlockSourceGetBrightnessFn = float (*)(void* region, const BlockPos& position);
using BlockSourceIsSolidBlockingBlockFn = bool (*)(void* region, const BlockPos& position);

// Well above the build limit, so this always resolves to the air block.
constexpr BlockPos kAirProbe{0, 32767, 0};

// The three BlockSource queries the overlay needs. Every one of them is
// optional: an unresolved signature degrades the overlay instead of
// dereferencing a null pointer.
struct BlockQueries {
    BlockSourceGetBlockFn getBlock = nullptr;
    BlockSourceGetBrightnessFn getBrightness = nullptr;
    BlockSourceIsSolidBlockingBlockFn isSolidBlockingBlock = nullptr;

    bool ready() const { return getBrightness && isSolidBlockingBlock; }

    bool isSolid(void* region, const BlockPos& position) const {
        return isSolidBlockingBlock(region, position);
    }

    // Resolved per region so a dimension or world change cannot leave a stale
    // block pointer behind.
    const void* airBlock(void* region) const {
        return getBlock ? getBlock(region, kAirProbe) : nullptr;
    }

    // Whether the block itself is worth labelling.
    bool isOpaque(void* region, const BlockPos& position, bool onlySolidBlocks,
                  const void* air) const {
        if (onlySolidBlocks) return isSolid(region, position);
        if (!getBlock) return isSolid(region, position);
        return getBlock(region, position) != air;
    }
};

BlockQueries g_queries;
renderoverlay::Overlay g_overlay;
LightOverlayModule* g_module = nullptr;

void* g_player = nullptr;
Vec3 g_playerPosition{0.0f, 0.0f, 0.0f};

void (*g_renderLevelOriginal)(void*, void*, void*) = nullptr;

void onLocalPlayerTick(void* player) {
    if (!g_module || !g_module->enabled || !renderoverlay::looksValid(player)) return;
    const auto component = *reinterpret_cast<const std::uintptr_t*>(
        static_cast<const char*>(player) + offsets::Actor::mStateVectorComponent);
    if (!renderoverlay::looksValid(reinterpret_cast<const void*>(component))) return;

    g_playerPosition = *reinterpret_cast<const Vec3*>(
        reinterpret_cast<const char*>(component) + offsets::StateVectorComponent::mPosition);
    // Published last so the render thread never sees a pointer whose cached
    // position belongs to the previous tick.
    g_player = player;
}

void* blockSourceOf(const void* player) {
    const auto dimension = *reinterpret_cast<const std::uintptr_t*>(
        static_cast<const char*>(player) + offsets::Actor::mDimension);
    if (!renderoverlay::looksValid(reinterpret_cast<const void*>(dimension))) return nullptr;
    const auto blockSource = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<const char*>(dimension) + offsets::Dimension::mBlockSource);
    return renderoverlay::looksValid(reinterpret_cast<const void*>(blockSource))
               ? reinterpret_cast<void*>(blockSource)
               : nullptr;
}

// Maps a point of the glyph cell onto the block face it is drawn on.
Vec3 project(const Vec3& centre, const Vec3& right, const Vec3& up, const Vec2& point) {
    const float x = point.x * lightoverlay::kLabelScale;
    const float y = point.y * lightoverlay::kLabelScale;
    return {centre.x + right.x * x + up.x * y, centre.y + right.y * x + up.y * y,
            centre.z + right.z * x + up.z * y};
}

void drawBlockLabels(const renderoverlay::Frame& frame, void* region, const BlockPos& block) {
    for (std::size_t index = 0; index < lightoverlay::kFaces.size(); ++index) {
        const auto& face = lightoverlay::kFaces[index];
        if (!lightoverlay::faceVisible(index, g_module->onlyTopFace)) continue;

        // The light that matters is the one in the air next to the face: that
        // is where a mob would spawn.
        const BlockPos neighbour{block.x + face.neighbour.x, block.y + face.neighbour.y,
                                 block.z + face.neighbour.z};
        if (g_queries.isSolid(region, neighbour)) continue;

        const int level =
            lightoverlay::lightLevelFromBrightness(g_queries.getBrightness(region, neighbour));
        g_overlay.color(frame, lightoverlay::isDangerous(level, g_module->dangerThreshold)
                                   ? g_module->dangerColor
                                   : g_module->safeColor);

        const Vec3 centre = lightoverlay::labelPosition(block, face.label);
        for (const auto& stroke : lightoverlay::numberStrokes(level)) {
            g_overlay.line(frame, project(centre, face.right, face.up, stroke.from),
                           project(centre, face.right, face.up, stroke.to));
        }
    }
}

void renderLightOverlay(void* levelRenderer, void* screenContext) {
    if (!g_module || !g_module->enabled) return;
    if (!g_queries.ready() || !renderoverlay::looksValid(g_player)) return;

    void* region = blockSourceOf(g_player);
    if (!region) return;

    auto frame = g_overlay.beginFrame(levelRenderer, screenContext);
    if (!frame) return;

    const int radiusX = lightoverlay::clampRadius(g_module->radiusHorizontal);
    const int radiusY = lightoverlay::clampRadius(g_module->radiusVertical);
    const BlockPos origin{static_cast<int>(std::floor(g_playerPosition.x)),
                          static_cast<int>(std::floor(g_playerPosition.y)),
                          static_cast<int>(std::floor(g_playerPosition.z))};
    const void* air = g_queries.airBlock(region);

    // The vertex count depends on how many faces turn out to be exposed, which
    // is only known while walking the volume; the Tessellator grows the batch
    // when it is started with 0.
    g_overlay.begin(frame, 0);
    for (int dx = -radiusX; dx <= radiusX; ++dx) {
        for (int dy = -radiusY; dy <= radiusY; ++dy) {
            for (int dz = -radiusX; dz <= radiusX; ++dz) {
                const BlockPos block{origin.x + dx, origin.y + dy, origin.z + dz};
                if (!g_queries.isOpaque(region, block, g_module->onlySolidBlocks, air)) continue;
                drawBlockLabels(frame, region, block);
            }
        }
    }
    g_overlay.flush(frame);

    g_overlay.endFrame(frame);
}

// The world is rendered first and the labels are submitted afterwards, like
// every other world overlay here; drawing them before renderLevel would let the
// terrain and sky paint over them.
void renderLevelHook(void* levelRenderer, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(levelRenderer, screenContext, a3);
    renderLightOverlay(levelRenderer, screenContext);
}

int readBoundedInt(const nlohmann::json& json, const char* key, int fallback, int minimum,
                   int maximum) {
    if (!json.contains(key) || !json[key].is_number_integer()) return fallback;
    return std::clamp(json[key].get<int>(), minimum, maximum);
}

std::uint32_t readColor(const nlohmann::json& json, const char* key, std::uint32_t fallback) {
    if (!json.contains(key) || !json[key].is_string()) return fallback;
    return renderoverlay::parseColor(json[key].get<std::string>(), fallback);
}

}  // namespace

LightOverlayModule::LightOverlayModule()
    : Module("Light Overlay",
             "Writes the light level on the faces of nearby blocks and colours the ones dark enough for hostile mobs to spawn.") {
    showInMenu = true;
    hideInHudEditor = true;  // world overlay, not HUD
    g_module = this;
}

LightOverlayModule::~LightOverlayModule() {
    if (g_module == this) g_module = nullptr;
}

void LightOverlayModule::onInit() {
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (address) m_patchTarget = reinterpret_cast<void*>(address);

    g_overlay.resolve();

    using bedrocktools::memory::resolve;
    using bedrocktools::memory::SignatureId;
    if (const auto getBlock = resolve(SignatureId::BlockSourceGetBlock)) {
        g_queries.getBlock = reinterpret_cast<BlockSourceGetBlockFn>(getBlock);
    }
    if (const auto getBrightness = resolve(SignatureId::BlockSourceGetBrightness)) {
        g_queries.getBrightness = reinterpret_cast<BlockSourceGetBrightnessFn>(getBrightness);
    }
    if (const auto isSolid = resolve(SignatureId::BlockSourceIsSolidBlockingBlock)) {
        g_queries.isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(isSolid);
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { onLocalPlayerTick(event.player); });
}

void LightOverlayModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    m_patched = bedrocktools::hooks::install(
                    m_patchTarget, reinterpret_cast<void*>(&renderLevelHook),
                    reinterpret_cast<void**>(&g_renderLevelOriginal)) != nullptr;
}

void LightOverlayModule::onEnable() {
    applyPatch();
}

void LightOverlayModule::onDisable() {
    // The cached player belongs to the world the module was enabled in; the
    // render hook must not dereference it after a world change.
    g_player = nullptr;
}

void LightOverlayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    radiusHorizontal = readBoundedInt(j, "radiusHorizontal", radiusHorizontal,
                                      lightoverlay::kMinRadius, lightoverlay::kMaxRadius);
    radiusVertical = readBoundedInt(j, "radiusVertical", radiusVertical, lightoverlay::kMinRadius,
                                    lightoverlay::kMaxRadius);
    dangerThreshold = readBoundedInt(j, "dangerThreshold", dangerThreshold, 0,
                                     lightoverlay::kMaxLightLevel);

    if (j.contains("onlyTopFace") && j["onlyTopFace"].is_boolean()) {
        onlyTopFace = j["onlyTopFace"].get<bool>();
    }
    if (j.contains("onlySolidBlocks") && j["onlySolidBlocks"].is_boolean()) {
        onlySolidBlocks = j["onlySolidBlocks"].get<bool>();
    }

    safeColor = readColor(j, "safeColor", safeColor);
    dangerColor = readColor(j, "dangerColor", dangerColor);
}

void LightOverlayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["radiusHorizontal"] = radiusHorizontal;
    j["radiusVertical"] = radiusVertical;
    j["onlyTopFace"] = onlyTopFace;
    j["onlySolidBlocks"] = onlySolidBlocks;
    j["dangerThreshold"] = dangerThreshold;
    j["safeColor"] = renderoverlay::colorString(safeColor);
    j["dangerColor"] = renderoverlay::colorString(dangerColor);
}
