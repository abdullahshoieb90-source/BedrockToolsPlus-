#include "core/Runtime.hpp"
#include <pl/Mod.hpp>

class BedrockToolsPlusMod {
public:
    static BedrockToolsPlusMod& instance() {
        static BedrockToolsPlusMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext& context) { return bedrocktools::core::Runtime::get().load(context); }
    bool enable(pl::mod::ModContext& context) { return bedrocktools::core::Runtime::get().enable(context); }
    bool disable(pl::mod::ModContext& context) { return bedrocktools::core::Runtime::get().disable(context); }
    bool unload(pl::mod::ModContext& context) { return bedrocktools::core::Runtime::get().unload(context); }
};

PL_REGISTER_MOD(BedrockToolsPlusMod, BedrockToolsPlusMod::instance())
