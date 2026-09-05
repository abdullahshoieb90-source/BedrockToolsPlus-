#include "nopotionbar.hpp"

namespace {

// The single module instance, mirroring g_effectDisplay in effectdisplay.cpp.
// The shared vanilla potion-bar detour reads it through
// NoPotionBarModule::suppressesVanillaBar().
NoPotionBarModule* g_noPotionBar = nullptr;

} // namespace

NoPotionBarModule::NoPotionBarModule()
    : Module("No Potion Bar",
             "Removes the default potion bar the game draws for active status effects. "
             "Use it alone for a clean HUD, or together with Effect Display to replace "
             "the bar with your own layout.") {
    g_noPotionBar = this;
}

NoPotionBarModule::~NoPotionBarModule() {
    if (g_noPotionBar == this) g_noPotionBar = nullptr;
}

bool NoPotionBarModule::suppressesVanillaBar() {
    return g_noPotionBar != nullptr && g_noPotionBar->enabled;
}
