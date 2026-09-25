#include "breadcrumbs.hpp"
#include "render_overlay.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace {

using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Vec3;

namespace offsets = bedrocktools::sdk::offsets;

// Bounds the menu sliders; the module clamps to the same ranges.
constexpr int kMinTickInterval = 1;
constexpr int kMaxTickInterval = 40;
constexpr int kMinPoints = 1;
constexpr int kMaxPoints = 2000;

renderoverlay::Overlay g_overlay;
BreadcrumbsModule* g_module = nullptr;

void (*g_renderLevelOriginal)(void*, void*, void*) = nullptr;

// Samples are taken at the player's feet rather than at the eye position, so
// the trail follows the ground. The feet come from the AABB shape component;
// the state vector position is the fallback while it is not available yet.
Vec3 samplePosition(const void* player) {
    const auto* const actor = static_cast<const char*>(player);
    const auto component = *reinterpret_cast<const std::uintptr_t*>(actor + offsets::Actor::mStateVectorComponent);
    if (!renderoverlay::looksValid(reinterpret_cast<const void*>(component))) return {0.0f, 0.0f, 0.0f};

    Vec3 position = *reinterpret_cast<const Vec3*>(
        reinterpret_cast<const char*>(component) + offsets::StateVectorComponent::mPosition);

    const auto shape = *reinterpret_cast<const std::uintptr_t*>(
        actor + offsets::Actor::mStateVectorComponent + offsets::BuiltInActorComponents::mAABBShapeComponent);
    if (renderoverlay::looksValid(reinterpret_cast<const void*>(shape))) {
        const auto& aabb = *reinterpret_cast<const AABB*>(
            reinterpret_cast<const char*>(shape) + offsets::AABBShapeComponent::mAABB);
        position.y = aabb.min.y;
    }
    return position;
}

void onLocalPlayerTick(void* player) {
    if (!g_module || !g_module->enabled || !renderoverlay::looksValid(player)) return;
    const Vec3 position = samplePosition(player);
    std::lock_guard lock(g_module->trailMutex());
    g_module->trail().onTick(position);
}

// Batch 1: the block outline of every sample plus the line joining it to the
// previous one. Batch 2: a white arrowhead per step, on top of the trail colour
// so the direction stays readable whatever colour was picked.
void renderBreadcrumbs(void* levelRenderer, void* screenContext) {
    if (!g_module || !g_module->enabled) return;

    std::vector<Vec3> points;
    {
        std::lock_guard lock(g_module->trailMutex());
        points = g_module->trail().points();
    }
    if (points.empty()) return;

    auto frame = g_overlay.beginFrame(levelRenderer, screenContext);
    if (!frame) return;

    const std::uint32_t trail = g_module->trailColor;
    const float baseAlpha = renderoverlay::toRgba(trail).a;

    int vertexCount = static_cast<int>(points.size() * 4 * 2);
    if (points.size() > 1) vertexCount += static_cast<int>((points.size() - 1) * 2);
    g_overlay.begin(frame, vertexCount);
    for (std::size_t i = 0; i < points.size(); ++i) {
        g_overlay.color(frame, renderoverlay::withAlpha(
                                   trail, baseAlpha * breadcrumbs::Trail::fade(i, points.size())));

        const auto box = breadcrumbs::footprint(points[i]);
        for (std::size_t corner = 0; corner < box.size(); ++corner) {
            g_overlay.line(frame, box[corner], box[(corner + 1) % box.size()]);
        }
        if (i > 0) {
            g_overlay.line(frame, breadcrumbs::centre(points[i - 1]), breadcrumbs::centre(points[i]));
        }
    }
    g_overlay.flush(frame);

    if (points.size() > 1) {
        g_overlay.begin(frame, static_cast<int>((points.size() - 1) * 2 * 2));
        for (std::size_t i = 1; i < points.size(); ++i) {
            const auto head = breadcrumbs::arrowhead(breadcrumbs::centre(points[i - 1]),
                                                     breadcrumbs::centre(points[i]));
            if (!head.valid) continue;
            g_overlay.color(frame, renderoverlay::withAlpha(
                                       0xFFFFFFFFu,
                                       baseAlpha * breadcrumbs::Trail::fade(i, points.size())));
            g_overlay.line(frame, head.tip, head.left);
            g_overlay.line(frame, head.tip, head.right);
        }
        g_overlay.flush(frame);
    }

    g_overlay.endFrame(frame);
}

void renderLevelHook(void* levelRenderer, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(levelRenderer, screenContext, a3);
    renderBreadcrumbs(levelRenderer, screenContext);
}

int readBoundedInt(const nlohmann::json& json, const char* key, int fallback, int minimum,
                   int maximum) {
    if (!json.contains(key) || !json[key].is_number_integer()) return fallback;
    return std::clamp(json[key].get<int>(), minimum, maximum);
}

}  // namespace

BreadcrumbsModule::BreadcrumbsModule()
    : Module("Breadcrumbs",
             "Draws a fading trail of block outlines behind you as you walk, with an arrow on every step.") {
    showInMenu = true;
    hideInHudEditor = true;  // world overlay, not HUD
    g_module = this;
}

BreadcrumbsModule::~BreadcrumbsModule() {
    if (g_module == this) g_module = nullptr;
}

void BreadcrumbsModule::onInit() {
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (address) m_patchTarget = reinterpret_cast<void*>(address);

    g_overlay.resolve();

    m_trail.setInterval(tickInterval);
    m_trail.setMaxPoints(maxPoints);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { onLocalPlayerTick(event.player); });
}

void BreadcrumbsModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    m_patched = bedrocktools::hooks::install(
                    m_patchTarget, reinterpret_cast<void*>(&renderLevelHook),
                    reinterpret_cast<void**>(&g_renderLevelOriginal)) != nullptr;
}

void BreadcrumbsModule::onEnable() {
    applyPatch();
}

void BreadcrumbsModule::onDisable() {
    // The samples belong to the world the module was enabled in; dropping them
    // keeps a re-enable from drawing a trail through a different dimension.
    clearTrail();
}

void BreadcrumbsModule::clearTrail() {
    std::lock_guard lock(m_trailMutex);
    m_trail.clear();
}

void BreadcrumbsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    tickInterval = readBoundedInt(j, "tickInterval", tickInterval, kMinTickInterval, kMaxTickInterval);
    maxPoints = readBoundedInt(j, "maxPoints", maxPoints, kMinPoints, kMaxPoints);

    m_trail.setInterval(tickInterval);
    m_trail.setMaxPoints(maxPoints);

    if (j.contains("trailColor") && j["trailColor"].is_string()) {
        trailColor = renderoverlay::parseColor(j["trailColor"].get<std::string>(), trailColor);
    }

    // The launcher reports a button press as a config change. The stored value
    // is always false so the next launch does not clear the trail again.
    if (j.contains("clearTrailButton") && j["clearTrailButton"].is_boolean() &&
        j["clearTrailButton"].get<bool>()) {
        clearTrail();
    }
}

void BreadcrumbsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["tickInterval"] = tickInterval;
    j["maxPoints"] = maxPoints;
    j["trailColor"] = renderoverlay::colorString(trailColor);
    j["clearTrailButton"] = false;
}
