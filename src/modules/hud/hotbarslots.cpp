#include "hotbarslots.hpp"

#include "core/GameHooks.hpp"
#include "launcher/ExternalButtonRefresh.hpp"

#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace {

HotbarSlotsModule* g_instance = nullptr;

// Same base the Command Hotkey module uses: LeviLauncher derives an
// independent persisted HUD position from each stable button ID.
constexpr float kLauncherButtonBaseSize = 52.0f;

constexpr std::uint32_t kDefaultTextColor = 0xFF373737u;
constexpr std::uint32_t kActiveTextColor = 0xFF1F1F1Fu;
// Occupied-slot highlight, mirroring the light pressed number LeviLaunchroid
// draws on its hotbar buttons.
constexpr std::uint32_t kOccupiedTextColor = 0xFFFFFFFFu;

// Minecraft-style square frame shared with the Zoom / Command Hotkey overlay
// buttons; the launcher draws the slot number on top as the button label.
const char* slotButtonSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
</svg>)svg";

const char* slotButtonActiveSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <g transform="translate(32, 32) scale(0.85) translate(-32, -32)">
        <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
    </g>
</svg>)svg";

std::string slotKey(int slot, const char* suffix) {
    return "m_slot" + std::to_string(slot) + suffix;
}

} // namespace

HotbarSlotsModule::HotbarSlotsModule()
    : Module("Hotbar Slots", "On-screen 1-9 buttons for selecting hotbar slots, with optional item awareness.") {
    g_instance = this;
    // On-screen slots are launcher overlay buttons. The parent module has no
    // custom draw surface of its own in the HUD editor.
    hideInHudEditor = true;
}

HotbarSlotsModule::~HotbarSlotsModule() {
    unregisterOverlayButtons();
    if (g_instance == this) g_instance = nullptr;
}

HotbarSlotsModule* HotbarSlotsModule::instance() {
    return g_instance;
}

std::string HotbarSlotsModule::slotButtonId(int slot) {
    // No spaces: the launcher persists per-button HUD positions keyed by ID.
    return "bedrocktoolsplus.HotbarSlots.Button" + std::to_string(slot);
}

void HotbarSlotsModule::onInit() {
    // The launcher owns each on-screen button and its independent HUD-editor
    // position; input hooks are installed once by Runtime.
    syncOverlayButtons();
}

void HotbarSlotsModule::onEnable() {
    syncOverlayButtons();
}

void HotbarSlotsModule::onDisable() {
    flushPendingKeyUp();
}

void HotbarSlotsModule::selectSlot(int slot) {
    if (!enabled || slot < 1 || slot > SlotCount) return;
    if (!m_slots[static_cast<std::size_t>(slot - 1)].enabled) return;
    // A tap only spans one launcher click event, so hold the key down and let
    // the next frame release it. That keeps the key visible to the game's
    // input poll for at least one tick (press-and-hold parity with
    // HotbarSlotOverlay, which sends down on press-start and up on press-end).
    flushPendingKeyUp();
    // Bedrock key codes '1'..'9', exactly what HotbarSlotOverlay.sendSlotKey
    // passes to MoreButtonsMod.sendKey.
    if (platformSendKey('0' + slot, true)) m_pendingKeyUpSlot = slot;
}

void HotbarSlotsModule::flushPendingKeyUp() {
    if (m_pendingKeyUpSlot <= 0) return;
    const int slot = m_pendingKeyUpSlot;
    m_pendingKeyUpSlot = 0;
    platformSendKey('0' + slot, false);
}

void* HotbarSlotsModule::localPlayer() {
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) return nullptr;
    void** vtable = *reinterpret_cast<void***>(client);
    if (!vtable) return nullptr;
    auto getPlayer = reinterpret_cast<void* (*)(void*)>(
        vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer]);
    if (!getPlayer) return nullptr;
    return getPlayer(client);
}

bool HotbarSlotsModule::hotbarHasItem(const void* player, int slot) {
    using namespace bedrocktools::sdk::offsets::Inventory;
    if (!player || slot < 0 || slot >= SlotCount) return false;
    const auto* bytes = static_cast<const std::byte*>(player);
    const void* proxy = *reinterpret_cast<const void* const*>(bytes + PlayerInventory);
    if (!proxy) return false;
    const void* inventory = *reinterpret_cast<const void* const*>(
        static_cast<const std::byte*>(proxy) + PlayerInventoryContainer);
    if (!inventory) return false;
    // Hotbar occupies the first SlotCount entries of the player inventory
    // container (slots 0..8), matching the `slot - 1` indexing HotbarSlotMod
    // uses on the Java side.
    const auto* items = static_cast<const std::byte*>(inventory) + FillingContainerItems;
    const std::uintptr_t begin = *reinterpret_cast<const std::uintptr_t*>(items);
    const std::uintptr_t end = *reinterpret_cast<const std::uintptr_t*>(items + sizeof(void*));
    if (!begin || end < begin) return false;
    const std::size_t count = (end - begin) / ItemStackSize;
    if (static_cast<std::size_t>(slot) >= count) return false;
    const std::byte* stack =
        reinterpret_cast<const std::byte*>(begin) + static_cast<std::size_t>(slot) * ItemStackSize;
    if (!*reinterpret_cast<const std::uint8_t*>(stack + ItemStackValid)) return false;
    return *reinterpret_cast<const std::uint8_t*>(stack + ItemStackCount) != 0;
}

void HotbarSlotsModule::onFrame() {
    flushPendingKeyUp();
    if (!m_itemIcons) return;
    void* player = localPlayer();
    bool changed = false;
    for (int i = 0; i < SlotCount; ++i) {
        const bool occupied = player && m_slots[static_cast<std::size_t>(i)].enabled
            ? hotbarHasItem(player, i)
            : false;
        if (occupied != m_occupied[static_cast<std::size_t>(i)]) {
            m_occupied[static_cast<std::size_t>(i)] = occupied;
            changed = true;
        }
    }
    // Same refresh contract as Command Hotkey: re-register the native button
    // definitions so the launcher snapshot picks up the new colors, then ask
    // the launcher to re-apply them in place (needs the ModuleMenu entry for
    // this module ID).
    if (changed) {
        syncOverlayButtons();
        bedrocktools::launcher::refreshExternalButtonsForModule(moduleId);
    }
}

std::vector<HotbarSlotsModule::SlotButtonDef> HotbarSlotsModule::buildButtonDefs() const {
    std::vector<SlotButtonDef> defs;
    for (int i = 0; i < SlotCount; ++i) {
        const auto& cfg = m_slots[static_cast<std::size_t>(i)];
        if (!cfg.enabled) continue;
        SlotButtonDef def;
        def.slot = i + 1;
        def.id = slotButtonId(def.slot);
        def.label = std::to_string(def.slot);
        const float sizeDp = std::clamp(cfg.sizeDp, MinSizeDp, MaxSizeDp);
        const float scale = (sizeDp / kLauncherButtonBaseSize) * m_buttonScale;
        def.scaleX = scale;
        def.scaleY = scale;
        const bool occupied = m_itemIcons && m_occupied[static_cast<std::size_t>(i)];
        def.occupied = occupied;
        def.textColor = occupied ? kOccupiedTextColor : kDefaultTextColor;
        def.activeTextColor = kActiveTextColor;
        defs.push_back(std::move(def));
    }
    return defs;
}

void HotbarSlotsModule::unregisterOverlayButtons() {
    for (int slot = 1; slot <= SlotCount; ++slot)
        pl::modmenu::unregisterButton(slotButtonId(slot));
}

void HotbarSlotsModule::syncOverlayButtons() {
    // Registering a duplicate button id is rejected, so unregister first
    // (same pattern as CommandHotkeyModule::syncOverlayButtons).
    unregisterOverlayButtons();
    for (const auto& def : buildButtonDefs()) {
        const int slot = def.slot;
        pl::modmenu::ButtonBuilder builder(def.id, "Hotbar " + std::to_string(slot));
        builder.moduleId(moduleId)
            .label(def.label)
            .behavior(pl::modmenu::ButtonBehavior::Click)
            .defaultVisible(true)
            .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
            .styleColors(0x00000001, 0x00000001, 0x00000001)
            .svgIcon(slotButtonSvg, false)
            .activeSvgIcon(slotButtonActiveSvg)
            .textColor(def.textColor)
            .activeTextColor(def.activeTextColor)
            .sizeScale(def.scaleX, def.scaleY)
            .onEvent([this, slot](std::string_view, pl::modmenu::ButtonEvent event, float) {
                if (event == pl::modmenu::ButtonEvent::Click) selectSlot(slot);
            });
        (void)builder.registerButton();
    }
}

bool HotbarSlotsModule::platformSendKey(int bedrockCode, bool down) {
#if defined(__ANDROID__)
    // Same call HotbarSlotOverlay.sendSlotKey makes: MoreButtonsMod.sendKey
    // validates 0 < keyCode < 256 and loads libinbuiltmods itself, so no
    // explicit initialize() is needed here.
    if (bedrockCode <= 0 || bedrockCode >= 256) return false;
    auto* vm = reinterpret_cast<JavaVM*>(bedrocktools::launcher::javaVm());
    if (!vm) return false;
    JNIEnv* env = nullptr;
    const bool detached = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
    if (detached && (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env)) return false;
    if (!env) {
        if (detached) vm->DetachCurrentThread();
        return false;
    }
    bool ok = false;
    jclass cls = env->FindClass("org/levimc/launcher/core/mods/inbuilt/nativemod/MoreButtonsMod");
    if (cls && !env->ExceptionCheck()) {
        jmethodID sendKey = env->GetStaticMethodID(cls, "sendKey", "(IZ)Z");
        if (sendKey && !env->ExceptionCheck()) {
            ok = env->CallStaticBooleanMethod(cls, sendKey, static_cast<jint>(bedrockCode),
                                              static_cast<jboolean>(down ? JNI_TRUE : JNI_FALSE)) == JNI_TRUE;
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        ok = false;
    }
    if (cls) env->DeleteLocalRef(cls);
    if (detached) vm->DetachCurrentThread();
    return ok;
#else
    (void)bedrockCode;
    (void)down;
    return false;
#endif
}

// Config readers that compile against both the real nlohmann_json headers and
// the host fake in tests/fakejson (which only provides contains / get<T> /
// is_boolean / is_string). Non-numeric values fall back to the default instead
// of throwing.
namespace {

bool jsonBool(const nlohmann::json& j, const std::string& key, bool fallback) {
    try {
        if (!j.contains(key) || !j[key].is_boolean()) return fallback;
        return j[key].get<bool>();
    } catch (...) {
        return fallback;
    }
}

float jsonFloat(const nlohmann::json& j, const std::string& key, float fallback, float lo, float hi) {
    try {
        if (!j.contains(key)) return fallback;
        const auto& v = j[key];
        if (v.is_boolean() || v.is_string()) return fallback;
        return std::clamp(v.get<float>(), lo, hi);
    } catch (...) {
        return fallback;
    }
}

} // namespace

void HotbarSlotsModule::loadConfig(const nlohmann::json& j) {
    const auto previousDefs = buildButtonDefs();
    const float previousScale = m_buttonScale;
    const bool previousIcons = m_itemIcons;
    Module::loadConfig(j);

    m_itemIcons = jsonBool(j, "m_itemIcons", m_itemIcons);
    m_buttonScale = jsonFloat(j, "m_buttonScale", m_buttonScale, 0.5f, 2.0f);
    m_buttonOpacity = jsonFloat(j, "m_buttonOpacity", m_buttonOpacity, 0.05f, 1.0f);

    for (int slot = 1; slot <= SlotCount; ++slot) {
        auto& cfg = m_slots[static_cast<std::size_t>(slot - 1)];
        cfg.enabled = jsonBool(j, slotKey(slot, ""), cfg.enabled);
        cfg.sizeDp = jsonFloat(j, slotKey(slot, "Size"), cfg.sizeDp, MinSizeDp, MaxSizeDp);
        cfg.opacity = jsonFloat(j, slotKey(slot, "Opacity"), cfg.opacity, 0.0f, 1.0f);
    }

    // NOTE: per-slot opacity is persisted for forward compatibility (same as
    // CommandHotkeyModule::m_buttonOpacity): the current ButtonBuilder API
    // exposes size but no opacity setter, so opacity stays launcher-managed
    // until the API grows one.

    if (!m_itemIcons) m_occupied.fill(false);

    // Re-register the native button definitions whenever anything shown on a
    // button changed (visibility, size, scale, icon highlight). The launcher
    // keeps its own snapshot of each ExternalButton, so the native definition
    // must be refreshed before refreshExternalButtonsForModule re-applies it
    // to the visible overlay.
    const bool overlayChanged =
        previousScale != m_buttonScale || previousIcons != m_itemIcons || previousDefs.size() != buildButtonDefs().size() ||
        !std::equal(previousDefs.begin(), previousDefs.end(), buildButtonDefs().begin(), buildButtonDefs().end(),
                    [](const SlotButtonDef& a, const SlotButtonDef& b) {
                        return a.id == b.id && a.label == b.label && a.scaleX == b.scaleX &&
                            a.scaleY == b.scaleY && a.textColor == b.textColor;
                    });
    if (overlayChanged) syncOverlayButtons();
}

void HotbarSlotsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_itemIcons"] = m_itemIcons;
    j["m_buttonScale"] = m_buttonScale;
    j["m_buttonOpacity"] = m_buttonOpacity;
    for (int slot = 1; slot <= SlotCount; ++slot) {
        const auto& cfg = m_slots[static_cast<std::size_t>(slot - 1)];
        j[slotKey(slot, "")] = cfg.enabled;
        j[slotKey(slot, "Size")] = cfg.sizeDp;
        j[slotKey(slot, "Opacity")] = cfg.opacity;
    }
}
