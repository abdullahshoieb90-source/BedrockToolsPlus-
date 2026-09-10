// Host-side fake of the Preloader <pl/Mod.hpp> header for the Esp render test.
//
// Only the lifecycle context type is needed: core/Runtime.hpp (which the module
// pulls in through core/PixelFont.hpp, because the nametag font is registered
// from the package's own resource directory) names `pl::mod::ModContext` in its
// method signatures and nowhere else. The test defines the two Runtime members
// the module actually calls, exactly like tests/effectdisplay_test.cpp does.
#pragma once

namespace pl::mod {

struct ModContext {};

} // namespace pl::mod
