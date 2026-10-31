#pragma once
#include "modules/Module.hpp"
#include <bedrocktools/events/EventBus.hpp>

// Opt-in read-only diagnostics, NOT the Spatial GUI feature itself.
class SpatialGuiDiagnostics final : public Module {
public:
    SpatialGuiDiagnostics();
    ~SpatialGuiDiagnostics() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
private:
    bedrocktools::events::Subscription m_screenSubscription = 0;
    bedrocktools::events::Subscription m_frameSubscription = 0;
    void* m_target = nullptr;
    bool m_installed = false;
};
