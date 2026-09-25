#pragma once

#include "../Module.hpp"
#include "breadcrumbs_trail.hpp"

#include <cstdint>
#include <mutex>

// Leaves a fading trail of block outlines behind the player, with an arrow on
// every step so the direction of travel is readable. Purely visual and
// client-side.
class BreadcrumbsModule : public Module {
public:
    BreadcrumbsModule();
    ~BreadcrumbsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Stored as 0xAARRGGBB; the alpha drives how strongly the newest sample
    // shows before the trail fades towards its head.
    std::uint32_t trailColor = 0xFF00FF00u;
    // Client ticks between two samples.
    int tickInterval = 5;
    // Oldest samples are dropped past this.
    int maxPoints = 1000;

    // The trail is written on the client tick and read on the render thread, so
    // both sides go through this lock.
    std::mutex& trailMutex() { return m_trailMutex; }
    breadcrumbs::Trail& trail() { return m_trail; }

    void clearTrail();

private:
    void applyPatch();

    breadcrumbs::Trail m_trail;
    std::mutex m_trailMutex;
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};
