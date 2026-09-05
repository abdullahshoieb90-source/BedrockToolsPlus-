#include "hotbarslots.hpp"

#include "huditems.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace {

namespace huditems = bedrocktools::huditems;
using bedrocktools::hotbar::SlotRect;
using bedrocktools::hotbar::StripLayout;

constexpr std::size_t SlotCount = HotbarSlotsModule::SlotCount;

// android.view.KeyEvent.KEYCODE_1. The launcher forwards the key code of an
// overlay button straight to the game, which maps 1-9 to the hotbar slots.
constexpr int AndroidKeyCode1 = 8;

constexpr const char* HudElementId = "bedrocktools.hotbarslots.strip";

namespace sdkoffsets = bedrocktools::sdk::offsets;

// ---- Auto Build plumbing ---------------------------------------------------
//
// Placing a block is GameMode::useItemOn on the block the player is looking
// at. The GameMode pointer is not reachable from a stable offset, so it is
// picked up from the GameModeActionEvent the core hooks already publish for
// every attack / use / build the player performs.

struct InteractionResultValue {
    std::uint8_t value;
};

using LevelGetHitResultFn = void* (*)(void*);
using UseItemOnFn = InteractionResultValue (*)(void*, void*, const void*, std::uint8_t,
                                               const void*, const void*, bool);

LevelGetHitResultFn g_levelGetHitResult = nullptr;
UseItemOnFn g_gameModeUseItemOn = nullptr;
std::atomic<void*> g_gameMode{nullptr};

double nowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Android key codes 1-9 map onto the hotbar slots, both for the launcher
// overlay buttons (they forward their key code) and for a real keyboard.
int slotForKeyCode(int keyCode) {
    const int index = keyCode - AndroidKeyCode1;
    if (index < 0 || index >= static_cast<int>(SlotCount)) return -1;
    return index;
}

// The first nine slots of the player inventory are the hotbar.
std::array<void*, SlotCount> getHotbarStacks(void* player) {
    std::array<void*, SlotCount> stacks{};
    const huditems::ContainerSlots inventory = huditems::playerInventory(player);
    for (std::size_t i = 0; i < SlotCount; ++i) stacks[i] = inventory.stack(i);
    return stacks;
}

// The selected slot index is derived by matching the carried item stack
// against the hotbar stacks. That avoids depending on an Inventory field
// offset that changes between Minecraft versions.
int selectedSlotFor(void* player, const std::array<void*, SlotCount>& stacks) {
    void* carried = huditems::getCarriedItem(player);
    if (!carried) return -1;
    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (stacks[i] && stacks[i] == carried) return static_cast<int>(i);
    }
    return -1;
}

void tickListener(void* player, void* user) {
    auto* module = static_cast<HotbarSlotsModule*>(user);
    if (module && module->enabled) module->onLocalPlayerTick(player);
}

void renderListener(void* context, void* client, void* user) {
    auto* module = static_cast<HotbarSlotsModule*>(user);
    if (module && module->enabled) module->renderNative(context, client);
}

// Minecraft-style slot frame, matching the look of the launcher's hotbar
// buttons (a light bevel around a darker face).
constexpr const char* slotButtonSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
</svg>)svg";

constexpr const char* slotButtonActiveSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <g transform="translate(32, 32) scale(0.85) translate(-32, -32)">
        <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
    </g>
</svg>)svg";

} // namespace

HotbarSlotsModule::HotbarSlotsModule()
    : Module("Hotbar Slots",
             "Adds separate 1-9 buttons for selecting hotbar slots, with optional item icons on the HUD.") {
    m_slotEnabled.fill(true);
    // Auto Build is opt-in and, by default, only armed for slot 1.
    m_autoBuildSlots.fill(false);
    m_autoBuildSlots[0] = true;
}

HotbarSlotsModule::~HotbarSlotsModule() {
    unregisterOverlayButtons();
    huditems::removeRenderListener(renderListener, this);
}

std::string HotbarSlotsModule::buttonId(std::size_t index) {
    return "bedrocktoolsplus.HotbarSlots.Button" + std::to_string(index + 1);
}

void HotbarSlotsModule::onInit() {
    huditems::initialize();
    huditems::addRenderListener(renderListener, this);
    resolveAutoBuildFunctions();

    // Auto Build needs the live GameMode pointer; the core hooks hand one out
    // with every game-mode action the player performs.
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>(
        [](auto& event) {
            if (event.gameMode) g_gameMode.store(event.gameMode, std::memory_order_release);
        });
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { tickListener(event.player, this); });

    syncOverlayButtons();
}

bool HotbarSlotsModule::resolveAutoBuildFunctions() {
    using bedrocktools::memory::SignatureId;
    if (!g_levelGetHitResult) {
        g_levelGetHitResult = reinterpret_cast<LevelGetHitResultFn>(
            bedrocktools::memory::resolve(SignatureId::LevelGetHitResult));
    }
    if (!g_gameModeUseItemOn) {
        g_gameModeUseItemOn = reinterpret_cast<UseItemOnFn>(
            bedrocktools::memory::resolve(SignatureId::GameModeUseItemOn));
    }
    return g_levelGetHitResult && g_gameModeUseItemOn;
}

void HotbarSlotsModule::onEnable() {
    syncOverlayButtons();
}

void HotbarSlotsModule::onDisable() {
    clearRuntime();
    unregisterOverlayButtons();
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>{});
}

void HotbarSlotsModule::unregisterOverlayButtons() {
    for (std::size_t i = 0; i < SlotCount; ++i) pl::modmenu::unregisterButton(buttonId(i));
}

void HotbarSlotsModule::syncOverlayButtons() {
    unregisterOverlayButtons();
    if (!m_buttons) return;

    const float scale = std::clamp(m_buttonScale, 0.5f, 2.0f);
    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (!m_slotEnabled[i]) continue;
        const std::string label = std::to_string(i + 1);
        pl::modmenu::ButtonBuilder builder(buttonId(i), "Hotbar Slot " + label);
        builder.moduleId(moduleId)
            .label(label)
            // The launcher delivers this key code to Minecraft, which selects
            // the matching hotbar slot exactly like a hardware keyboard would.
            .androidKeyCode(AndroidKeyCode1 + static_cast<int>(i))
            .behavior(pl::modmenu::ButtonBehavior::Hold)
            .defaultVisible(true)
            .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
            .styleColors(0x00000001, 0x00000001, 0x00000001)
            .svgIcon(slotButtonSvg, false)
            .activeSvgIcon(slotButtonActiveSvg)
            .textColor(0xFF373737u)
            .activeTextColor(0xFF1F1F1Fu)
            .sizeScale(scale, scale);
        (void)builder.registerButton();
    }
}

HotbarSlotsModule::ConfigSnapshot HotbarSlotsModule::snapshotConfig() const {
    std::lock_guard lock(m_configMutex);
    ConfigSnapshot config;
    config.slots = m_slotEnabled;
    config.buttons = m_buttons;
    config.itemIcons = m_itemIcons;
    config.slotNumbers = m_slotNumbers;
    config.highlightSelected = m_highlightSelected;
    config.layout.x = hudPosX;
    config.layout.y = hudPosY;
    config.layout.slotSize = m_slotSize;
    config.layout.gap = m_slotGap;
    config.layout.vertical = m_vertical;
    config.buttonScale = m_buttonScale;
    config.numberTextSize = m_numberTextSize;
    config.numberColor = huditems::parseColor(m_numberColor, 0xFFFFFFFFu);
    config.highlightColor = huditems::withOpacity(huditems::parseColor(m_highlightColor, 0xFFFFFFFFu), m_highlightOpacity);
    config.gridSize = m_gridSize;
    config.gridGap = m_gridGap;
    config.snapThreshold = m_snapThreshold;
    config.autoBuild.enabled = m_autoBuild;
    config.autoBuild.slots = m_autoBuildSlots;
    config.autoBuild.holdDelayMs = m_autoBuildHoldDelay;
    config.autoBuild.intervalMs = m_autoBuildInterval;
    config.snapFlags =
        (m_snapToGrid ? pl::modmenu::HudSnapGrid : pl::modmenu::HudSnapNone) |
        (m_snapToElements ? pl::modmenu::HudSnapElements : pl::modmenu::HudSnapNone) |
        (m_snapToScreenCenter ? pl::modmenu::HudSnapScreenCenter : pl::modmenu::HudSnapNone);
    return config;
}

void HotbarSlotsModule::clearRuntime() {
    for (auto& slot : m_hasItem) slot.store(false, std::memory_order_release);
    for (auto& slot : m_hasBlock) slot.store(false, std::memory_order_release);
    m_selectedSlot.store(-1, std::memory_order_release);
    std::lock_guard lock(m_autoBuildMutex);
    m_autoBuildState.reset();
}

// A slot button (or the matching keyboard key) went down / up. The event is
// never consumed: Minecraft must still perform the normal slot selection, the
// module only starts or stops its own hold timer on top of it.
bool HotbarSlotsModule::onKeyEvent(int key, bool isDown) {
    if (!enabled) return false;
    const int slot = slotForKeyCode(key);
    if (slot < 0) return false;

    const ConfigSnapshot config = snapshotConfig();
    if (!config.autoBuild.enabled) {
        std::lock_guard lock(m_autoBuildMutex);
        m_autoBuildState.reset();
        return false;
    }
    // Chat / container screens own the keyboard while they are open.
    if (ModuleRegistry::get().keybindBlocked()) {
        std::lock_guard lock(m_autoBuildMutex);
        m_autoBuildState.reset();
        return false;
    }

    const double now = nowMs();
    std::lock_guard lock(m_autoBuildMutex);
    if (isDown) m_autoBuildState.pressed(config.autoBuild, static_cast<std::size_t>(slot), now);
    else m_autoBuildState.released(static_cast<std::size_t>(slot), now);
    return false;
}

// Runs on the game thread once per local player tick.
void HotbarSlotsModule::onLocalPlayerTick(void* player) {
    if (!player) return;
    const ConfigSnapshot config = snapshotConfig();
    if (!config.autoBuild.enabled) return;

    std::size_t slot = 0;
    {
        std::lock_guard lock(m_autoBuildMutex);
        if (!m_autoBuildState.held()) return;
        slot = m_autoBuildState.slot();
        if (slot >= SlotCount) return;
        // Only a slot that is currently selected *and* holds a placeable block
        // may build; anything else silently keeps behaving like vanilla.
        const bool placeable =
            m_selectedSlot.load(std::memory_order_acquire) == static_cast<int>(slot) &&
            m_hasBlock[slot].load(std::memory_order_acquire);
        if (!m_autoBuildState.shouldPlace(config.autoBuild, nowMs(), placeable)) return;
    }

    placeHeldBlock(player, slot);
}

// GameMode::useItemOn against the block the player is looking at, i.e. exactly
// what tapping the build button would do.
bool HotbarSlotsModule::placeHeldBlock(void* player, std::size_t slot) {
    (void)slot;
    if (!resolveAutoBuildFunctions()) return false;

    void* gameMode = g_gameMode.load(std::memory_order_acquire);
    if (!gameMode) return false;

    void* level = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(player) + sdkoffsets::Actor::mLevel);
    if (reinterpret_cast<std::uintptr_t>(level) < 0x1000) return false;

    void* hit = g_levelGetHitResult(level);
    if (!hit) return false;

    const auto hitAddress = reinterpret_cast<std::uintptr_t>(hit);
    const int type = *reinterpret_cast<int*>(hitAddress + sdkoffsets::HitResult::mType);
    if (type != sdkoffsets::HitResult::TypeBlock) return false;

    const auto* blockPos = reinterpret_cast<const void*>(hitAddress + sdkoffsets::HitResult::mBlockPos);
    const auto face = *reinterpret_cast<std::uint8_t*>(hitAddress + sdkoffsets::HitResult::mFacing);
    const auto* clickPos = reinterpret_cast<const void*>(hitAddress + sdkoffsets::HitResult::mPos);

    void* stack = huditems::getCarriedItem(player);
    if (!huditems::stackPlacesBlock(stack)) return false;

    const InteractionResultValue result =
        g_gameModeUseItemOn(gameMode, stack, blockPos, face, clickPos, nullptr, true);
    return (result.value & 1u) != 0;
}

void HotbarSlotsModule::renderNative(void* context, void* client) {
    const ConfigSnapshot config = snapshotConfig();

    huditems::IconPainter painter(context, client, config.itemIcons);
    void* localPlayer = painter.player();
    if (!localPlayer) {
        clearRuntime();
        return;
    }

    const auto stacks = getHotbarStacks(localPlayer);
    m_selectedSlot.store(selectedSlotFor(localPlayer, stacks), std::memory_order_release);

    for (std::size_t i = 0; i < SlotCount; ++i) {
        void* stack = stacks[i];
        void* item = huditems::stackItem(stack);
        m_hasItem[i].store(item != nullptr, std::memory_order_release);
        m_hasBlock[i].store(item != nullptr && huditems::stackPlacesBlock(stack),
                            std::memory_order_release);

        if (!config.slots[i] || !item || !painter.ready()) continue;

        const SlotRect rect = bedrocktools::hotbar::slotRect(config.layout, i);
        if (rect.size <= 0.0f) continue;
        painter.draw(stack, item, rect.x, rect.y, rect.size);
    }
}

void HotbarSlotsModule::onFrame() {
    if (!enabled) return;

    const ConfigSnapshot config = snapshotConfig();

    std::size_t visible = 0;
    for (bool slot : config.slots) visible += slot ? 1 : 0;

    std::vector<pl::modmenu::HudEditorElement> elements;
    if (visible > 0) {
        pl::modmenu::HudEditorElement element;
        element.elementId = HudElementId;
        element.displayName = "Hotbar Slots";
        element.positionKeyX = "hudPosX";
        element.positionKeyY = "hudPosY";
        element.x = config.layout.x;
        element.y = config.layout.y;
        element.width = std::max(1.0f, bedrocktools::hotbar::stripWidth(config.layout, SlotCount));
        element.height = std::max(1.0f, bedrocktools::hotbar::stripHeight(config.layout, SlotCount));
        element.gridSize = config.gridSize;
        element.snapThreshold = config.snapThreshold;
        element.gridGap = config.gridGap;
        element.snapFlags = config.snapFlags;
        elements.push_back(std::move(element));
    }
    pl::modmenu::submitHudEditorElements(moduleId, elements);

    std::vector<pl::modmenu::DrawCommand> commands;
    const int selected = m_selectedSlot.load(std::memory_order_acquire);

    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (!config.slots[i]) continue;
        const SlotRect rect = bedrocktools::hotbar::slotRect(config.layout, i);

        if (config.highlightSelected && selected == static_cast<int>(i)) {
            pl::modmenu::DrawCommand highlight;
            highlight.type = pl::modmenu::DrawCommandType::RectFilled;
            highlight.x = rect.x;
            highlight.y = rect.y;
            highlight.w = rect.size;
            highlight.h = rect.size;
            highlight.color = config.highlightColor;
            commands.push_back(std::move(highlight));
        }

        if (config.slotNumbers) {
            pl::modmenu::DrawCommand number;
            number.type = pl::modmenu::DrawCommandType::Text;
            number.x = rect.x + 2.0f;
            number.y = rect.y + config.numberTextSize;
            number.color = config.numberColor;
            number.size = config.numberTextSize;
            number.text = std::to_string(i + 1);
            commands.push_back(std::move(number));
        }
    }

    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void HotbarSlotsModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("slots")
        .category("slots", "Slots", "Choose which hotbar slots are shown")
        .category("autobuild", "Auto Build", "Hold a slot button to keep placing its block")
        .category("appearance", "Appearance", "Size, spacing and colors of the slot strip")
        .category("editor", "HUD Editor", "Placement and snapping while editing the HUD");

    auto node = [](std::string key, std::string title, std::string category, ConfigControlTypeV2 type) {
        ConfigNodeV2 value;
        value.id = key;
        value.key = std::move(key);
        value.title = std::move(title);
        value.category = std::move(category);
        value.type = type;
        return value;
    };
    auto section = [&](const char* id, const char* title, const char* category) {
        auto value = node(id, title, category, ConfigControlTypeV2::Section);
        value.key.clear();
        schema.node(std::move(value));
    };
    auto slider = [&](const char* key, std::string title, const char* category,
                      const char* sectionId, const char* min, const char* max,
                      const char* unit = " px") {
        auto value = node(key, std::move(title), category, ConfigControlTypeV2::SliderFloat);
        value.section = sectionId;
        value.minValue = min;
        value.maxValue = max;
        value.step = "1";
        value.unit = unit;
        schema.node(std::move(value));
    };

    section("slot_toggles", "Visible Slots", "slots");
    auto slots = node("slot_visibility", "Slots", "slots", ConfigControlTypeV2::ToggleGroup);
    slots.key.clear();
    slots.section = "slot_toggles";
    slots.choiceStyle = ConfigChoiceStyleV2::Chips;
    for (std::size_t i = 0; i < SlotCount; ++i) {
        const std::string key = "m_slot" + std::to_string(i + 1);
        slots.options.push_back({key, std::to_string(i + 1), {}, key});
    }
    schema.node(std::move(slots));

    section("slot_features", "Features", "slots");
    auto features = node("slot_features_group", "Show", "slots", ConfigControlTypeV2::ToggleGroup);
    features.key.clear();
    features.section = "slot_features";
    features.description = "On-screen buttons select the slot; the HUD strip shows what each slot holds.";
    features.choiceStyle = ConfigChoiceStyleV2::Checklist;
    features.options = {
        {"buttons", "On-Screen Buttons", {}, "m_buttons"},
        {"icons", "Item Icons", {}, "m_itemIcons"},
        {"numbers", "Slot Numbers", {}, "m_slotNumbers"},
        {"highlight", "Highlight Selected Slot", {}, "m_highlightSelected"}
    };
    schema.node(std::move(features));

    section("autobuild_main", "Auto Build", "autobuild");
    auto autoBuild = node("m_autoBuild", "Enable Auto Build", "autobuild", ConfigControlTypeV2::Toggle);
    autoBuild.section = "autobuild_main";
    autoBuild.description =
        "Tap a slot button to select it as usual. Keep holding it and, if the slot holds a "
        "placeable block, blocks are placed automatically without pressing the build button.";
    schema.node(std::move(autoBuild));

    auto autoSlots = node("autobuild_slots", "Auto Build Slots", "autobuild", ConfigControlTypeV2::ToggleGroup);
    autoSlots.key.clear();
    autoSlots.section = "autobuild_main";
    autoSlots.choiceStyle = ConfigChoiceStyleV2::Chips;
    for (std::size_t i = 0; i < SlotCount; ++i) {
        const std::string key = "m_autoBuildSlot" + std::to_string(i + 1);
        autoSlots.options.push_back({key, std::to_string(i + 1), {}, key});
    }
    schema.node(std::move(autoSlots));
    slider("m_autoBuildHoldDelay", "Hold Before Building", "autobuild", "autobuild_main", "50", "1000", " ms");
    slider("m_autoBuildInterval", "Delay Between Blocks", "autobuild", "autobuild_main", "20", "1000", " ms");

    section("strip", "Slot Strip", "appearance");
    auto orientation = node("m_vertical", "Vertical Strip", "appearance", ConfigControlTypeV2::Toggle);
    orientation.section = "strip";
    schema.node(std::move(orientation));
    slider("m_slotSize", "Slot Size", "appearance", "strip", "8", "100");
    slider("m_slotGap", "Gap Between Slots", "appearance", "strip", "0", "50");
    slider("m_buttonScale", "Button Scale", "appearance", "strip", "0.5", "2", "x");

    section("strip_text", "Text & Highlight", "appearance");
    slider("m_numberTextSize", "Number Size", "appearance", "strip_text", "6", "40");
    auto numberColor = node("m_numberColor", "Number Color", "appearance", ConfigControlTypeV2::Color);
    numberColor.section = "strip_text";
    numberColor.defaultValue = "#FFFFFF";
    schema.node(std::move(numberColor));
    auto highlightColor = node("m_highlightColor", "Highlight Color", "appearance", ConfigControlTypeV2::Color);
    highlightColor.section = "strip_text";
    highlightColor.defaultValue = "#FFFFFF";
    schema.node(std::move(highlightColor));
    slider("m_highlightOpacity", "Highlight Opacity", "appearance", "strip_text", "0", "1", "");

    auto help = node("editor_help", "Move The Strip In The HUD Editor", "editor", ConfigControlTypeV2::Info);
    help.key.clear();
    help.description = "Open the HUD Editor to drag the slot strip. The on-screen buttons keep their own positions.";
    schema.node(std::move(help));
    section("snapping", "Snapping", "editor");
    auto snapping = node("snap_targets", "Snap To", "editor", ConfigControlTypeV2::ToggleGroup);
    snapping.key.clear();
    snapping.section = "snapping";
    snapping.choiceStyle = ConfigChoiceStyleV2::Chips;
    snapping.options = {
        {"grid", "Grid", {}, "m_snapToGrid"},
        {"items", "Other Elements", {}, "m_snapToElements"},
        {"center", "Screen Center", {}, "m_snapToScreenCenter"}
    };
    schema.node(std::move(snapping));
    slider("m_gridSize", "Grid Size", "editor", "snapping", "1", "100");
    slider("m_gridGap", "Gap Between Elements", "editor", "snapping", "0", "100");
    slider("m_snapThreshold", "Snap Distance", "editor", "snapping", "1", "100");

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

void HotbarSlotsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    const auto previousSlots = m_slotEnabled;
    const bool previousButtons = m_buttons;
    const float previousScale = m_buttonScale;

    {
        std::lock_guard lock(m_configMutex);
        for (std::size_t i = 0; i < SlotCount; ++i) {
            const std::string key = "m_slot" + std::to_string(i + 1);
            if (j.contains(key) && j[key].is_boolean()) m_slotEnabled[i] = j[key].get<bool>();
        }
        if (j.contains("m_buttons")) m_buttons = j["m_buttons"].get<bool>();
        if (j.contains("m_itemIcons")) m_itemIcons = j["m_itemIcons"].get<bool>();
        if (j.contains("m_slotNumbers")) m_slotNumbers = j["m_slotNumbers"].get<bool>();
        if (j.contains("m_highlightSelected")) m_highlightSelected = j["m_highlightSelected"].get<bool>();
        if (j.contains("m_vertical")) m_vertical = j["m_vertical"].get<bool>();
        if (j.contains("m_autoBuild")) m_autoBuild = j["m_autoBuild"].get<bool>();
        for (std::size_t i = 0; i < SlotCount; ++i) {
            const std::string key = "m_autoBuildSlot" + std::to_string(i + 1);
            if (j.contains(key) && j[key].is_boolean()) m_autoBuildSlots[i] = j[key].get<bool>();
        }
        if (j.contains("m_autoBuildHoldDelay"))
            m_autoBuildHoldDelay = std::clamp(j["m_autoBuildHoldDelay"].get<float>(), 50.0f, 1000.0f);
        if (j.contains("m_autoBuildInterval"))
            m_autoBuildInterval = std::clamp(j["m_autoBuildInterval"].get<float>(), 20.0f, 1000.0f);
        if (j.contains("hudPosX")) hudPosX = std::clamp(j["hudPosX"].get<float>(), 0.0f, 4000.0f);
        if (j.contains("hudPosY")) hudPosY = std::clamp(j["hudPosY"].get<float>(), 0.0f, 4000.0f);
        if (j.contains("m_slotSize")) m_slotSize = std::clamp(j["m_slotSize"].get<float>(), 8.0f, 100.0f);
        if (j.contains("m_slotGap")) m_slotGap = std::clamp(j["m_slotGap"].get<float>(), 0.0f, 50.0f);
        if (j.contains("m_buttonScale")) m_buttonScale = std::clamp(j["m_buttonScale"].get<float>(), 0.5f, 2.0f);
        if (j.contains("m_numberTextSize"))
            m_numberTextSize = std::clamp(j["m_numberTextSize"].get<float>(), 6.0f, 40.0f);
        if (j.contains("m_numberColor")) m_numberColor = j["m_numberColor"].get<std::string>();
        if (j.contains("m_highlightColor")) m_highlightColor = j["m_highlightColor"].get<std::string>();
        if (j.contains("m_highlightOpacity"))
            m_highlightOpacity = std::clamp(j["m_highlightOpacity"].get<float>(), 0.0f, 1.0f);
        if (j.contains("m_gridSize")) m_gridSize = std::clamp(j["m_gridSize"].get<float>(), 1.0f, 100.0f);
        if (j.contains("m_gridGap")) m_gridGap = std::clamp(j["m_gridGap"].get<float>(), 0.0f, 100.0f);
        if (j.contains("m_snapThreshold")) m_snapThreshold = std::clamp(j["m_snapThreshold"].get<float>(), 1.0f, 100.0f);
        if (j.contains("m_snapToGrid")) m_snapToGrid = j["m_snapToGrid"].get<bool>();
        if (j.contains("m_snapToElements")) m_snapToElements = j["m_snapToElements"].get<bool>();
        if (j.contains("m_snapToScreenCenter")) m_snapToScreenCenter = j["m_snapToScreenCenter"].get<bool>();
    }

    // Registering the launcher buttons again is only needed when something the
    // launcher itself renders changed; unrelated edits must not churn the
    // button registry (and reset the user's button placement).
    bool overlayChanged = previousButtons != m_buttons || previousScale != m_buttonScale;
    for (std::size_t i = 0; i < SlotCount && !overlayChanged; ++i)
        overlayChanged = previousSlots[i] != m_slotEnabled[i];
    if (overlayChanged) syncOverlayButtons();
}

void HotbarSlotsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);

    for (std::size_t i = 0; i < SlotCount; ++i) j["m_slot" + std::to_string(i + 1)] = m_slotEnabled[i];
    j["m_buttons"] = m_buttons;
    j["m_itemIcons"] = m_itemIcons;
    j["m_slotNumbers"] = m_slotNumbers;
    j["m_highlightSelected"] = m_highlightSelected;
    j["m_vertical"] = m_vertical;
    j["m_autoBuild"] = m_autoBuild;
    for (std::size_t i = 0; i < SlotCount; ++i)
        j["m_autoBuildSlot" + std::to_string(i + 1)] = m_autoBuildSlots[i];
    j["m_autoBuildHoldDelay"] = m_autoBuildHoldDelay;
    j["m_autoBuildInterval"] = m_autoBuildInterval;
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["m_slotSize"] = m_slotSize;
    j["m_slotGap"] = m_slotGap;
    j["m_buttonScale"] = m_buttonScale;
    j["m_numberTextSize"] = m_numberTextSize;
    j["m_numberColor"] = m_numberColor;
    j["m_highlightColor"] = m_highlightColor;
    j["m_highlightOpacity"] = m_highlightOpacity;
    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
}
