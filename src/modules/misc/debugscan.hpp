#pragma once

#include "../Module.hpp"

// Temporary reverse-engineering probe. Scans the loaded libminecraftpe.so in
// memory for the RenderController overlay-color plumbing and prints every hit
// to logcat (tag "BTScan"). Not shown in the mod menu. This module is meant
// to be removed once a Damage Color hook pattern is verified in-game.
class DebugScanModule : public Module {
public:
    DebugScanModule();
    void onInit() override;
};
