#include "ModuleMenu.hpp"
#include "ExternalButtonRefresh.hpp"
#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// Child settings are normally attached to a toggle whose key is a prefix of
// theirs (e.g. "crosshairIndicator" owns "crosshairIndicatorColor"). A few
// settings are named differently but are still owned by a toggle, so they are
// mapped explicitly here. The parent must be a toggle that exists in the same
// module for the dependency to be applied.
struct ExplicitDependency {
    const char* child;
    const char* parent;
};

static const ExplicitDependency kExplicitDependencies[] = {
    // Hitbox module: the indicator colors only matter while the hitbox
    // indicator itself is enabled.
    {"indicatorDefaultColor", "hitboxIndicator"},
    {"indicatorActiveColor", "hitboxIndicator"},

    // Block Outline: these names do not begin with their owning toggle, and
    // toggle children need an explicit relationship.
    {"lineThickness", "outline"},
    {"fillFaceOnly", "fill"},

    // Effect Display: how the level is rendered only matters while the level
    // is shown at all.
    {"m_romanLevels", "m_showLevel"},
    {"m_hideLevelOne", "m_showLevel"},

    // Effect Display: the potion-icon size only matters while the icons are
    // shown at all.
    {"m_iconScale", "m_showIcons"},
};

// Some string settings hold a comma separated *selector* value even though the
// module also reads them as a plain number (or the value is a plain string).
// Listing them here keeps the launcher from rendering them as a free text
// field, which would let a user type a non-existent option.
struct KnownRadio {
    const char* key;
    const char* options; // "<option0>,<option1>,..."
};

static const KnownRadio kKnownRadioSettings[] = {
};

static const char* knownRadioOptionsFor(const std::string& key) {
    for (const auto& radio : kKnownRadioSettings) {
        if (key == radio.key) return radio.options;
    }
    return nullptr;
}

static const char* explicitParentFor(const std::string& key) {
    for (const auto& dep : kExplicitDependencies) {
        if (key == dep.child) return dep.parent;
    }
    return nullptr;
}

// The launcher rejects a *whole* module definition when a single config entry
// fails validation — an empty key, an over-long string, an unknown type. When
// that happens at registration time the module simply does not appear in the
// menu (and, because registration failure is silent, it looks like every
// module vanished). Entries are therefore checked here, before they are handed
// over, so a malformed entry is dropped instead of taking the module with it.
static bool isRegistrableConfigEntry(const std::string& key,
                                     const std::string& displayName,
                                     std::string* fields, std::size_t fieldCount) {
    constexpr std::size_t kMaxConfigStringLength = 2048; // mirrors the launcher
    if (key.empty() || key.size() > kMaxConfigStringLength) return false;
    if (displayName.empty() || displayName.size() > kMaxConfigStringLength) return false;
    for (std::size_t i = 0; i < fieldCount; ++i) {
        if (fields[i].size() > kMaxConfigStringLength) return false;
    }
    return true;
}

static void onModuleToggle(std::string_view module_id, bool enabled) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    mod->setMasterEnabled(enabled);
    bedrocktools::config::ConfigManager::get().save();
}

static void onModuleKeybind(std::string_view module_id, std::string_view key, bool isDown) {
    if (ModuleRegistry::get().keybindBlocked()) return;
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    mod->onKeybindEvent(std::string(key), isDown);
}

static void onModuleConfigChanged(std::string_view module_id, std::string_view key, std::string_view value) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    if (mod->onMenuConfigChanged(key, value)) {
        bedrocktools::config::ConfigManager::get().save();
        return;
    }

    nlohmann::json j;
    mod->saveConfig(j);

    std::string safeValue(value);
    std::string safeKey(key);
    try {
        if (j.contains(safeKey)) {
            if (j[safeKey].is_boolean()) {
                if (safeValue == "true") j[safeKey] = true;
                else if (safeValue == "false") j[safeKey] = false;
            } else if (j[safeKey].is_number_integer()) {
                char* end = nullptr;
                const long val = std::strtol(safeValue.c_str(), &end, 10);
                if (end != safeValue.c_str() && *end == '\0') j[safeKey] = val;
            } else if (j[safeKey].is_number_float()) {
                char* end = nullptr;
                const float val = std::strtof(safeValue.c_str(), &end);
                if (end != safeValue.c_str() && *end == '\0') j[safeKey] = val;
            } else {
                // Text fields need to be allowed to become empty as well;
                // otherwise deleting the last character leaves the previous
                // value in the module and in its overlay button.
                j[safeKey] = safeValue;
            }
        } else {
            j[safeKey] = safeValue;
        }
    } catch (...) {
        j[safeKey] = safeValue;
    }
    mod->loadConfig(j);

    if (module_id == "bedrocktoolsplus.CommentKey" ||
        module_id == "bedrocktoolsplus.Command Hotkey") {
        // Refresh the Java view in place. It replaces the button definition
        // and reapplies its label without hiding the overlay.
        bedrocktools::launcher::refreshExternalButtonsForModule(module_id);
    }

    bedrocktools::config::ConfigManager::get().save();
}

// Registers a single module with the launcher. Kept separate from the loop in
// registerModulesWithLauncher() so one module's failure can be caught without
// taking the rest of the menu down with it.
static void registerSingleModule(Module* mod) {
        pl::modmenu::ModuleBuilder builder(mod->moduleId, mod->name);
        builder.description(mod->description)
                .defaultEnabled(mod->masterEnabled)
                .hideInHudEditor(mod->hideInHudEditor)
                .onToggle(onModuleToggle)
                .onConfigChanged(onModuleConfigChanged)
                .onKeybind(onModuleKeybind);

        nlohmann::json j;
        mod->saveConfig(j);

        static const char* baseKeys[] = {
                "enabled", "masterEnabled", "keybindActive", "shortcutEnabled", "shortcutSize", "shortcutOpacity",
                "lockPosition", "shortcutPosX", "shortcutPosY",
                "isHudModule", "hudPosX", "hudPosY"
        };

        struct TmpConfigEntry {
            std::string key;
            std::string displayName;
            pl::modmenu::ConfigType type;
            std::string default_value;
            std::string min_value;
            std::string max_value;
            std::string depends_on;
        };
        std::vector<TmpConfigEntry> configs;

        for (auto& [k, v] : j.items()) {
            bool isBase = false;
            for (const char* bk : baseKeys) {
                if (k == bk) { isBase = true; break; }
            }

            std::string keyLower = k;
            std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);
            const bool isHudPosition = keyLower.rfind("hud", 0) == 0 &&
                (keyLower.ends_with("posx") || keyLower.ends_with("posy"));
            if (isBase || isHudPosition || !mod->showInLegacyMenu(k)) continue;

            std::string displayName;
            std::string sourceKey = k;
            if (sourceKey.size() > 2 && sourceKey[0] == 'm' && sourceKey[1] == '_') {
                sourceKey = sourceKey.substr(2);
            }
            for (size_t i = 0; i < sourceKey.size(); ++i) {
                if (i == 0) {
                    displayName += toupper(sourceKey[i]);
                } else if (isupper(sourceKey[i])) {
                    displayName += ' ';
                    displayName += sourceKey[i];
                } else if (isdigit(sourceKey[i]) && i > 0 && !isdigit(sourceKey[i - 1])) {
                    // Split before a digit so keys like "show3d" render as
                    // "Show 3d" instead of "Show3d".
                    displayName += ' ';
                    displayName += sourceKey[i];
                } else if (isalpha(sourceKey[i]) && isdigit(sourceKey[i - 1])) {
                    // A letter glued to a digit is a unit/dimension suffix, so
                    // it reads better upper-cased: "show3d" -> "Show 3D".
                    displayName += toupper(sourceKey[i]);
                } else {
                    displayName += sourceKey[i];
                }
            }

            TmpConfigEntry entry;
            entry.key = k;
            entry.displayName = displayName;

            if (v.is_boolean()) {
                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);
                if (kLower.find("button") != std::string::npos) {
                    // An action button reports a press as the string "true"
                    // through onConfigChanged; its default must be "false" so
                    // the button never starts in a pressed state.
                    entry.type = pl::modmenu::ConfigType::Button;
                    entry.default_value = "false";
                } else {
                    entry.type = pl::modmenu::ConfigType::Toggle;
                    entry.default_value = v.get<bool>() ? "true" : "false";
                }
            } else if (v.is_number_integer()) {
                entry.type = pl::modmenu::ConfigType::SliderInt;
                entry.default_value = std::to_string(v.get<int>());

                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

                if (kLower.find("keybind") != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Keybind;
                } else {
                    int minVal = 0;
                    int maxVal = 200;
                    if (kLower.find("cps") != std::string::npos) {
                        minVal = 1;
                        maxVal = 30;
                    } else if (kLower.find("time") != std::string::npos) {
                        maxVal = 24000;
                    } else if (kLower.find("red") != std::string::npos ||
                               kLower.find("green") != std::string::npos ||
                               kLower.find("blue") != std::string::npos) {
                        // Legacy RGB channel sliders (0-255); modules now use
                        // a single "#RRGGBB" color picker instead.
                        maxVal = 255;
                    }

                    entry.min_value = std::to_string(minVal);
                    entry.max_value = std::to_string(maxVal);
                }
            } else if (v.is_number_float()) {
                entry.type = pl::modmenu::ConfigType::SliderFloat;
                entry.default_value = std::to_string(v.get<float>());

                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

                float minVal = 0.0f;
                float maxVal = 100.0f;

                if (kLower == "effectduration") {
                    minVal = 0.1f;
                    maxVal = 10.0f;
                } else if (kLower == "effectspeed") {
                    minVal = 0.25f;
                    maxVal = 4.0f;
                } else if (kLower == "effectsize") {
                    minVal = 0.5f;
                    maxVal = 2.5f;
                } else if (kLower.find("opacity") != std::string::npos ||
                    kLower.find("color") != std::string::npos ||
                    kLower.find("alpha") != std::string::npos) {
                    maxVal = 1.0f;
                } else if (kLower.find("volume") != std::string::npos) {
                    // Hit Sound playback volume: 0.0 (mute) .. 1.0 (full).
                    maxVal = 1.0f;
                } else if (kLower.find("iconscale") != std::string::npos) {
                    // Potion-icon size multiplier (Effect Display); the module
                    // clamps to the same range at render time.
                    minVal = 0.25f;
                    maxVal = 4.0f;
                } else if (kLower.find("buttonscale") != std::string::npos) {
                    // Uniform size multiplier for the Zoom-style external
                    // command buttons.
                    minVal = 0.5f;
                    maxVal = 2.0f;
                } else if (kLower.find("scale") != std::string::npos) {
                    minVal = 0.1f;
                    maxVal = 5.0f;
                } else if (kLower == "borderwidth") {
                    maxVal = 4.0f;
                } else if (kLower.find("borderwidth") != std::string::npos) {
                    // Outline thickness in pixels (e.g. the launcher-style
                    // border around the Command Hotkey buttons).
                    maxVal = 8.0f;
                } else if (kLower.find("width") != std::string::npos) {
                    maxVal = 1000.0f;
                } else if (kLower.find("position") != std::string::npos ||
                           kLower.find("posx") != std::string::npos ||
                           kLower.find("posy") != std::string::npos) {
                    maxVal = 2000.0f;
                } else if (kLower.find("fov") != std::string::npos) {
                    minVal = 1.0f;
                    maxVal = 179.0f;
                } else if (kLower.find("intensity") != std::string::npos) {
                    maxVal = 10.0f;
                } else if (kLower.find("flapspeed") != std::string::npos) {
                    // Wings flap speed multiplier (Wings module).
                    minVal = 0.1f;
                    maxVal = 10.0f;
                } else if (kLower.find("speed") != std::string::npos || kLower.find("strength") != std::string::npos) {
                    minVal = 0.05f;
                    maxVal = 1.0f;
                } else if (kLower == "linethickness") {
                    // 1.0 == classic hairline box, higher values widen the
                    // hitbox lines (Hitbox module).
                    minVal = 1.0f;
                    maxVal = 10.0f;
                } else if (kLower.find("thick") != std::string::npos) {
                    maxVal = 20.0f;
                }

                entry.min_value = std::to_string(minVal);
                entry.max_value = std::to_string(maxVal);
            } else if (v.is_string()) {
                std::string str = v.get<std::string>();
                if (const char* knownOptions = knownRadioOptionsFor(k)) {
                    entry.type = pl::modmenu::ConfigType::Radio;
                    entry.default_value = str.substr(0, str.find(','));
                    entry.min_value = knownOptions;
                } else if (str.find(',') != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Radio;
                    size_t firstComma = str.find(',');
                    entry.default_value = str.substr(0, firstComma);
                    entry.min_value = str.substr(firstComma + 1);
                } else if (!str.empty() && str[0] == '#') {
                    entry.type = pl::modmenu::ConfigType::Color;
                    entry.default_value = str;
                } else {
                    // Plain text field. Modules that only want a read-only
                    // summary emit it as a text key and ignore any change
                    // reported back for it. This API has no dedicated info
                    // entry.
                    entry.type = pl::modmenu::ConfigType::Text;
                    entry.default_value = str;
                }
            } else {
                continue;
            }
            configs.push_back(entry);
        }

                for (auto& entry : configs) {
            std::string k = entry.key;
            // A toggle can still be owned by another toggle, but only through
            // an explicit mapping: the name-prefix heuristic below would
            // otherwise nest unrelated toggles under each other.
            const char* explicitParent = explicitParentFor(k);
            if (entry.type != pl::modmenu::ConfigType::Toggle || explicitParent != nullptr) {
                std::string bestParent = "";
                if (explicitParent) {
                    for (const auto& parentCandidate : configs) {
                        if (parentCandidate.type == pl::modmenu::ConfigType::Toggle &&
                            parentCandidate.key == explicitParent) {
                            bestParent = parentCandidate.key;
                            break;
                        }
                    }
                }
                if (bestParent.empty() && entry.type != pl::modmenu::ConfigType::Toggle) {
                    for (const auto& parentCandidate : configs) {
                        if (parentCandidate.type == pl::modmenu::ConfigType::Toggle) {
                            std::string pKey = parentCandidate.key;
                            if (k.length() > pKey.length() && k.compare(0, pKey.length(), pKey) == 0) {
                                if (pKey.length() > bestParent.length()) {
                                    bestParent = pKey;
                                }
                            }
                        }
                    }
                }
                if (!bestParent.empty()) entry.depends_on = bestParent;
            }

            // A radio needs an option list; an empty one makes the picker
            // useless, so fall back to a plain text field.
            if (entry.type == pl::modmenu::ConfigType::Radio && entry.min_value.empty()) {
                entry.type = pl::modmenu::ConfigType::Text;
            }

            std::string fields[] = {entry.default_value, entry.min_value,
                                    entry.max_value, entry.depends_on};
            if (!isRegistrableConfigEntry(entry.key, entry.displayName, fields, 4)) {
                // Dropping one broken setting keeps the module (and every other
                // module after it) visible in the menu.
                continue;
            }

            builder.config(entry.key, entry.displayName, entry.type, entry.default_value, entry.min_value, entry.max_value, entry.depends_on);
        }

        if (builder.registerModule()) {
            mod->onMenuRegistered();
        } else {
            // The launcher refused the definition (duplicate id, invalid entry
            // left after filtering, ...). Keep going so the remaining modules
            // are still registered; a single bad module must not empty the menu.
            bedrocktools::launcher::reportModuleRegistrationFailure(mod->moduleId);
        }
}

void registerModulesWithLauncher() {
    auto& modules = ModuleRegistry::get().modules();

    for (auto* mod : modules) {
        if (!mod->showInMenu) continue;

        // A module whose saveConfig() throws (bad JSON type, allocation
        // failure, a filesystem scan inside saveConfig) must not abort the
        // whole registration pass: the launcher would then be left without a
        // single module and the menu would look empty.
        try {
            registerSingleModule(mod);
        } catch (...) {
            bedrocktools::launcher::reportModuleRegistrationFailure(mod->moduleId);
        }
    }
}
