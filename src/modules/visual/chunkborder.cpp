#include "chunkborder.hpp"
#include "chunkborder_geometry.hpp"
#include "render_overlay.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using bedrocktools::sdk::Vec3;

namespace offsets = bedrocktools::sdk::offsets;

renderoverlay::Overlay g_overlay;
ChunkBorderModule* g_module = nullptr;
Vec3 g_playerPosition{0.0f, 0.0f, 0.0f};

void (*g_renderLevelOriginal)(void*, void*, void*) = nullptr;

// The border is anchored to the player's chunk, which is only known on the
// client tick; the render thread just reads the cached position.
void onLocalPlayerTick(void* player) {
    if (!g_module || !g_module->enabled || !renderoverlay::looksValid(player)) return;
    const auto component = *reinterpret_cast<const std::uintptr_t*>(
        static_cast<const char*>(player) + offsets::Actor::mStateVectorComponent);
    if (!renderoverlay::looksValid(reinterpret_cast<const void*>(component))) return;
    g_playerPosition = *reinterpret_cast<const Vec3*>(
        reinterpret_cast<const char*>(component) + offsets::StateVectorComponent::mPosition);
}

// One colour per batch: the Tessellator is cheapest when the colour is set once
// and every line of that colour is submitted together.
void drawBatch(const renderoverlay::Frame& frame, const std::vector<chunkborder::Line>& lines,
               std::uint32_t color) {
    if (lines.empty()) return;
    g_overlay.begin(frame, static_cast<int>(lines.size() * 2));
    g_overlay.color(frame, color);
    for (const auto& line : lines) g_overlay.line(frame, line.from, line.to);
    g_overlay.flush(frame);
}

void renderChunkBorder(void* levelRenderer, void* screenContext) {
    if (!g_module || !g_module->enabled) return;

    auto frame = g_overlay.beginFrame(levelRenderer, screenContext);
    if (!frame) return;

    const auto border = chunkborder::buildBorder(g_playerPosition, g_module->vertLineSpacing,
                                                 g_module->horizLineSpacing);
    drawBatch(frame, border.corners, g_module->cornerColor);
    drawBatch(frame, border.grid, g_module->midColor);
    drawBatch(frame, border.adjacent, g_module->adjColor);

    g_overlay.endFrame(frame);
}

void renderLevelHook(void* levelRenderer, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(levelRenderer, screenContext, a3);
    renderChunkBorder(levelRenderer, screenContext);
}

// Grid spacings are meaningless past the size of a chunk, so both are clamped
// to 0..16 no matter what the config or the menu slider hands over.
float readSpacing(const nlohmann::json& json, const char* key, float fallback) {
    const float limit = static_cast<float>(chunkborder::kChunkSize);
    if (!json.contains(key) || !json[key].is_number()) return fallback;
    const float value = json[key].get<float>();
    return std::clamp(value, 0.0f, limit);
}

}  // namespace

ChunkBorderModule::ChunkBorderModule()
    : Module("Chunk Border",
             "Draws the borders of the chunk you are standing in, with separate colours for the chunk corners, the inner grid and the surrounding chunks.") {
    showInMenu = true;
    hideInHudEditor = true;  // world overlay, not HUD
    g_module = this;
}

ChunkBorderModule::~ChunkBorderModule() {
    if (g_module == this) g_module = nullptr;
}

void ChunkBorderModule::onInit() {
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (address) m_patchTarget = reinterpret_cast<void*>(address);

    g_overlay.resolve();

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { onLocalPlayerTick(event.player); });
}

void ChunkBorderModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    m_patched = bedrocktools::hooks::install(
                    m_patchTarget, reinterpret_cast<void*>(&renderLevelHook),
                    reinterpret_cast<void**>(&g_renderLevelOriginal)) != nullptr;
}

void ChunkBorderModule::onEnable() {
    applyPatch();
}

void ChunkBorderModule::onDisable() {}

void ChunkBorderModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    vertLineSpacing = readSpacing(j, "vertLineSpacing", vertLineSpacing);
    horizLineSpacing = static_cast<int>(readSpacing(
        j, "horizLineSpacing", static_cast<float>(horizLineSpacing)));

    cornerColor = renderoverlay::parseColor(
        j.contains("cornerColor") && j["cornerColor"].is_string()
            ? j["cornerColor"].get<std::string>()
            : std::string(),
        cornerColor);
    midColor = renderoverlay::parseColor(
        j.contains("midColor") && j["midColor"].is_string() ? j["midColor"].get<std::string>()
                                                            : std::string(),
        midColor);
    adjColor = renderoverlay::parseColor(
        j.contains("adjColor") && j["adjColor"].is_string() ? j["adjColor"].get<std::string>()
                                                            : std::string(),
        adjColor);
}

void ChunkBorderModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["vertLineSpacing"] = vertLineSpacing;
    j["horizLineSpacing"] = horizLineSpacing;
    j["cornerColor"] = renderoverlay::colorString(cornerColor);
    j["midColor"] = renderoverlay::colorString(midColor);
    j["adjColor"] = renderoverlay::colorString(adjColor);
}
