#pragma once

#include "../Module.hpp"

// No Potion Bar — removes the game's default status-effect (potion) bar.
//
// Bedrock draws that bar from HudScreen::_renderStatusEffects. The draw call
// is skipped by a single shared hook that the Effect Display module installs
// once per session (see effectdisplay.cpp); this module is pure state. While
// it is enabled, that shared detour returns early, so the vanilla bar
// disappears even when Effect Display itself is off and nothing replaces it.
//
// The module therefore needs no onInit/onEnable/onDisable of its own: the
// detour reads its enabled flag live, every frame, through
// suppressesVanillaBar().
class NoPotionBarModule final : public Module {
public:
    NoPotionBarModule();
    ~NoPotionBarModule() override;

    // True while this module is enabled and the shared vanilla potion-bar
    // detour must skip the game's own draw call. Only reads the module's
    // enabled flag, so it is safe to call from the render thread.
    static bool suppressesVanillaBar();
};
