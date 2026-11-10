#include "swordblocking.hpp"

#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

SwordBlockingModule* g_swordBlocking = nullptr;

namespace {

namespace sb = swordblocking;
using namespace bedrocktools::sdk::offsets;

using GetPerspectiveFn = int (*)(void*);
using RenderFirstPersonFn = void (*)(void*, void*, void*, int);
using RenderHandFn = void (*)(void*, void*, void*, int, float);
using RenderItemFn = void (*)(void*, void*, void*, void*, int, int, int, int);
using RenderObjectFn = void (*)(void*, void*, void*, void*, int);

// Item::getDescriptionId is vtable slot 5 (the inventory accessor already
// relies on it), and it is what the game keys the sword item class on.
constexpr std::size_t ItemGetDescriptionIdVtableIndex = 5;

// The launcher interprets these as "relative to the screen centre"; -20000 is
// the exact centre on every resolution (the same convention the Crosshair and
// debug-menu overlays use).
constexpr float kHudCenter = -20000.0f;

// renderObject's itemFlags bit 0 marks the first-person pass (the same bit the
// upstream View Model module keys on).
constexpr int kFirstPersonPass = 1 << 0;

// Player::getCarriedItem is a virtual accessor, so no signature is needed.
constexpr std::size_t PlayerGetCarriedItemVtable = VTable::PlayerGetCarriedItem;

GetPerspectiveFn s_getPerspectiveOriginal = nullptr;
RenderFirstPersonFn s_renderFirstPersonOriginal = nullptr;
RenderHandFn s_renderHandOriginal = nullptr;
RenderItemFn s_renderItemOriginal = nullptr;
RenderObjectFn s_renderObjectOriginal = nullptr;

bedrocktools::hooks::Handle s_perspectiveHook = nullptr;
bedrocktools::hooks::Handle s_renderFirstPersonHook = nullptr;
bedrocktools::hooks::Handle s_renderHandHook = nullptr;
bedrocktools::hooks::Handle s_renderItemHook = nullptr;
bedrocktools::hooks::Handle s_renderObjectHook = nullptr;

// Scope flags for the first-person item draw, all render-thread local:
//
//   ItemInHandRenderer::renderFirstPerson
//     ItemInHandRenderer::renderHand(main)
//       RenderItem(..., renderingMainHand = 1)
//         ItemInHandRenderer::renderObject  <- the matrix to pose lives here
//     ItemInHandRenderer::renderHand(offhand)
//       RenderItem(..., renderingMainHand = 0)
//
// renderObject itself does not say which hand it is drawing, so the enclosing
// scopes do: only the main-hand item may be posed, and only while the
// first-person pass is being drawn (in third person the item goes through the
// actor renderer and must not be touched).
thread_local bool tl_firstPersonScope = false;
thread_local bool tl_mainHandScope = false;
thread_local bool tl_offHandScope = false;

void** getVtable(void* object) {
    return object ? *reinterpret_cast<void***>(object) : nullptr;
}

void* getCarriedItem(void* player) {
    void** vtable = getVtable(player);
    if (!vtable || !vtable[PlayerGetCarriedItemVtable]) return nullptr;
    using Fn = void* (*)(void*);
    return reinterpret_cast<Fn>(vtable[PlayerGetCarriedItemVtable])(player);
}

template <class T>
T read(const void* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object) + offset);
}

// The current top of the render context's matrix stack: the matrix the game is
// about to use for the item. Mirrors the traversal the upstream View Model
// module performs (RenderContext -> MatrixStackWrapper -> MatrixStack -> block
// list); every step is range-checked so a layout change can only make the pose
// silently stop, never write through a bad pointer.
float* currentMatrix(void* renderContext) {
    if (reinterpret_cast<std::uintptr_t>(renderContext) < 0x1000) return nullptr;

    const auto wrapper = read<std::uintptr_t>(renderContext, RenderContext::mMatrixStackWrapper);
    if (wrapper < 0x1000) return nullptr;

    void* stackObject = reinterpret_cast<void*>(wrapper);
    const auto stack = read<std::uintptr_t>(stackObject, MatrixStackWrapper::mMatrixStack);
    if (stack < 0x1000) return nullptr;

    void* stackPtr = reinterpret_cast<void*>(stack);
    void* const* blocks = read<void* const*>(stackPtr, MatrixStack::mBlocks);
    const std::size_t start = read<std::size_t>(stackPtr, MatrixStack::mStart);
    const std::size_t size = read<std::size_t>(stackPtr, MatrixStack::mSize);
    if (!blocks || size == 0 || size > 1024 || start > 1024) return nullptr;

    constexpr std::size_t kElementsPerBlock = 4096 / (sizeof(float) * 16);
    const std::size_t last = start + size - 1;
    const std::size_t blockIndex = last / kElementsPerBlock;
    const std::size_t elementIndex = last % kElementsPerBlock;
    if (blockIndex > 64) return nullptr;
    void* block = blocks[blockIndex];
    if (!block) return nullptr;
    return reinterpret_cast<float*>(block) + elementIndex * 16;
}

// True when the carried stack's item description id ends in "_sword".
bool swordFromStack(void* stack) {
    if (reinterpret_cast<std::uintptr_t>(stack) < 0x1000) return false;

    // ItemStackBase -> shared counter -> Item*, the layout the inventory HUD
    // already reads (an empty slot has no counter).
    const void* counter = read<const void*>(stack, ShulkerPreview::ItemStackBaseItem);
    if (!counter) return false;
    void* item = const_cast<void*>(read<const void*>(counter, ShulkerPreview::SharedCounterPointer));
    if (reinterpret_cast<std::uintptr_t>(item) < 0x1000) return false;

    void** vtable = getVtable(item);
    if (!vtable || !vtable[ItemGetDescriptionIdVtableIndex]) return false;
    using GetDescriptionIdFn = const std::string& (*)(void*);
    const std::string& identifier = reinterpret_cast<GetDescriptionIdFn>(
        vtable[ItemGetDescriptionIdVtableIndex])(item);

    // Never run the name matcher over a string that cannot be one: a stale
    // pointer would otherwise make us read far past its owner.
    if (identifier.size() > 128) return false;
    for (const char character : identifier) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte > 0x7E) return false;
    }
    return sb::isSwordName(identifier);
}

// Applies the guard pose to the matrix the item is about to be drawn with.
void applyGuardPose(void* renderContext) {
    if (!g_swordBlocking || !g_swordBlocking->enabled) return;
    const float weight = g_swordBlocking->poseWeight();
    if (weight <= sb::activeThreshold) return;

    float* matrix = currentMatrix(renderContext);
    if (!matrix) return;
    const sb::Mat4 local = sb::poseMatrix(g_swordBlocking->poseParams(), weight);
    sb::composeInto(matrix, local);
}

void perspectiveDetour(void* self) {
    const int perspective = s_getPerspectiveOriginal ? s_getPerspectiveOriginal(self) : 0;
    if (g_swordBlocking) {
        g_swordBlocking->m_thirdPerson.store(perspective != 0, std::memory_order_relaxed);
    }
}

void renderFirstPersonDetour(void* self, void* renderContext, void* previousProjection, int itemFlags) {
    const bool previous = tl_firstPersonScope;
    tl_firstPersonScope = true;
    if (s_renderFirstPersonOriginal) {
        s_renderFirstPersonOriginal(self, renderContext, previousProjection, itemFlags);
    }
    tl_firstPersonScope = previous;
}

void renderHandDetour(void* self, void* renderContext, void* player, int renderingMainHand, float frameAlpha) {
    const bool previousMain = tl_mainHandScope;
    const bool previousOff = tl_offHandScope;
    if (renderingMainHand != 0) tl_mainHandScope = true;
    else tl_offHandScope = true;

    if (s_renderHandOriginal) s_renderHandOriginal(self, renderContext, player, renderingMainHand, frameAlpha);

    tl_mainHandScope = previousMain;
    tl_offHandScope = previousOff;
}

void renderItemDetour(void* self, void* renderContext, void* entity, void* item,
                      int posAndRotSet, int itemFlags, int useMatrixAsIs, int renderingMainHand) {
    const bool previousMain = tl_mainHandScope;
    const bool previousOff = tl_offHandScope;
    if (renderingMainHand != 0) tl_mainHandScope = true;
    else tl_offHandScope = true;

    if (s_renderItemOriginal) {
        s_renderItemOriginal(self, renderContext, entity, item, posAndRotSet, itemFlags,
                             useMatrixAsIs, renderingMainHand);
    }

    tl_mainHandScope = previousMain;
    tl_offHandScope = previousOff;
}

void renderObjectDetour(void* self, void* renderContext, void* renderObject,
                        void* renderMetadata, int itemFlags) {
    const bool firstPerson = tl_firstPersonScope || (itemFlags & kFirstPersonPass) != 0;
    const bool pose = firstPerson && tl_mainHandScope && !tl_offHandScope;

    // Save the live matrix, pose it, draw, restore: the game shares the stack
    // across everything it renders in this pass, so leaving our transform
    // behind would drag the arm (and anything drawn after the item) with it.
    float saved[16]{};
    float* matrix = nullptr;
    if (pose) {
        matrix = currentMatrix(renderContext);
        if (matrix) {
            for (std::size_t i = 0; i < 16; ++i) saved[i] = matrix[i];
            applyGuardPose(renderContext);
        }
    }

    if (s_renderObjectOriginal) {
        s_renderObjectOriginal(self, renderContext, renderObject, renderMetadata, itemFlags);
    }

    if (matrix) {
        for (std::size_t i = 0; i < 16; ++i) matrix[i] = saved[i];
    }
}

// Launcher overlay button (the touch path). The id is stable so the launcher
// remembers the position the user dragged it to.
constexpr const char* kHoldButtonId = "bedrocktoolsplus.SwordBlocking.Hold";

const char* kHoldButtonSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
    <path fill="#D8F3FF" stroke="#3B5A66" stroke-width="2" d="M32,13 L49,20 L49,34 C49,43 41,49 32,52 C23,49 15,43 15,34 L15,20 Z"/>
</svg>)svg";

const char* kHoldButtonActiveSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#FFFFFF" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <path fill="#B9E9FF" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
    <path fill="#FFFFFF" stroke="#1F4C5C" stroke-width="2" d="M32,13 L49,20 L49,34 C49,43 41,49 32,52 C23,49 15,43 15,34 L15,20 Z"/>
</svg>)svg";

// "RRGGBB" / "AARRGGBB" / "0xAARRGGBB" -> ARGB, `fallback` when unparsable.
std::uint32_t parseColor(const std::string& value, std::uint32_t fallback) {
    if (value.empty()) return fallback;
    std::string hex = value;
    if (hex[0] == '#') hex.erase(0, 1);
    else if (hex.size() > 1 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex.erase(0, 2);
    if (hex.size() != 6 && hex.size() != 8) return fallback;
    for (const char character : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(character))) return fallback;
    }
    try {
        const auto parsed = static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        return hex.size() == 8 ? parsed : (0xFF000000u | parsed);
    } catch (...) {
        return fallback;
    }
}

std::string colorString(std::uint32_t color) {
    char text[10]{};
    std::snprintf(text, sizeof(text), "#%06X", color & 0x00FFFFFFu);
    return text;
}

} // namespace

SwordBlockingModule::SwordBlockingModule()
    : Module("Sword Blocking",
             "Raises your sword into a Java-style guard pose while you hold the use control, "
             "with an optional shield indicator under the crosshair. Visual only: no damage, "
             "reach or knockback is changed.") {
    showInMenu = true;
    // Center-locked overlay, like the crosshair: nothing to drag in the editor.
    hideInHudEditor = true;
    g_swordBlocking = this;
}

SwordBlockingModule::~SwordBlockingModule() {
    unregisterOverlayButton();

    bedrocktools::hooks::remove(s_renderObjectHook);
    bedrocktools::hooks::remove(s_renderItemHook);
    bedrocktools::hooks::remove(s_renderHandHook);
    bedrocktools::hooks::remove(s_renderFirstPersonHook);
    bedrocktools::hooks::remove(s_perspectiveHook);
    s_renderObjectHook = nullptr;
    s_renderItemHook = nullptr;
    s_renderHandHook = nullptr;
    s_renderFirstPersonHook = nullptr;
    s_perspectiveHook = nullptr;

    s_renderObjectOriginal = nullptr;
    s_renderItemOriginal = nullptr;
    s_renderHandOriginal = nullptr;
    s_renderFirstPersonOriginal = nullptr;
    s_getPerspectiveOriginal = nullptr;

    if (g_swordBlocking == this) g_swordBlocking = nullptr;
}

void SwordBlockingModule::onInit() {
    // The hooks stay installed for the whole session and pass straight through
    // while the module is off or the weight is zero, so enabling the module
    // never has to patch the item renderer mid-frame.
    const auto hookSignature = [](bedrocktools::memory::SignatureId id, void* detour,
                                  void** original, bedrocktools::hooks::Handle& handle) {
        if (handle) return;
        const auto address = bedrocktools::memory::resolve(id);
        if (!address) return;
        handle = bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour, original);
    };

    hookSignature(bedrocktools::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                  reinterpret_cast<void*>(&renderFirstPersonDetour),
                  reinterpret_cast<void**>(&s_renderFirstPersonOriginal),
                  s_renderFirstPersonHook);
    hookSignature(bedrocktools::memory::SignatureId::ItemInHandRendererRenderHand,
                  reinterpret_cast<void*>(&renderHandDetour),
                  reinterpret_cast<void**>(&s_renderHandOriginal),
                  s_renderHandHook);
    hookSignature(bedrocktools::memory::SignatureId::RenderItem,
                  reinterpret_cast<void*>(&renderItemDetour),
                  reinterpret_cast<void**>(&s_renderItemOriginal),
                  s_renderItemHook);
    hookSignature(bedrocktools::memory::SignatureId::ItemInHandRenderObject,
                  reinterpret_cast<void*>(&renderObjectDetour),
                  reinterpret_cast<void**>(&s_renderObjectOriginal),
                  s_renderObjectHook);
    hookSignature(bedrocktools::memory::SignatureId::GetPerspective,
                  reinterpret_cast<void*>(&perspectiveDetour),
                  reinterpret_cast<void**>(&s_getPerspectiveOriginal),
                  s_perspectiveHook);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_swordBlocking) g_swordBlocking->onPlayerTick(event.player);
        });

    syncOverlayButton();
}

void SwordBlockingModule::onEnable() {
    m_swordHeld.store(false, std::memory_order_release);
    {
        std::lock_guard lock(m_weightMutex);
        m_weightClockValid = false;
    }
    syncOverlayButton();
}

void SwordBlockingModule::onDisable() {
    m_mouseHeld.store(false, std::memory_order_release);
    m_buttonHeld.store(false, std::memory_order_release);
    m_keyHeld.store(false, std::memory_order_release);
    m_weight.store(0.0f, std::memory_order_release);
    {
        std::lock_guard lock(m_weightMutex);
        m_weightClockValid = false;
    }
    m_submitted.store(false, std::memory_order_release);
    // onFrame stops running for disabled modules, so this is the only chance
    // to take the indicator down.
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    unregisterOverlayButton();
}

swordblocking::PoseParams SwordBlockingModule::poseParams() const {
    swordblocking::PoseParams pose;
    pose.tiltDegrees = m_tiltDegrees;
    pose.lift = m_lift;
    pose.across = m_across;
    pose.forward = m_forward;
    pose.scale = m_poseScale;
    return swordblocking::clampedPose(pose);
}

swordblocking::IndicatorStyle SwordBlockingModule::indicatorStyle() const {
    swordblocking::IndicatorStyle style;
    style.scale = m_indicatorScale;
    style.opacity = m_indicatorOpacity;
    style.rgb = parseColor(m_indicatorColor, 0xFF7FE3FFu) & 0x00FFFFFFu;
    return style;
}

bool SwordBlockingModule::guardActive() const {
    if (!enabled || !m_swordHeld.load(std::memory_order_acquire)) return false;
    const auto mode = static_cast<swordblocking::HoldMode>(
        std::clamp(m_holdStyle, 0, static_cast<int>(swordblocking::HoldMode::Count) - 1));
    return swordblocking::holdActive(mode,
                                     m_mouseHeld.load(std::memory_order_acquire),
                                     m_buttonHeld.load(std::memory_order_acquire),
                                     m_keyHeld.load(std::memory_order_acquire));
}

void SwordBlockingModule::onPlayerTick(void* player) {
    if (!enabled) {
        m_swordHeld.store(false, std::memory_order_release);
        return;
    }
    m_swordHeld.store(swordFromStack(getCarriedItem(player)), std::memory_order_release);
}

void SwordBlockingModule::refreshHoldState(std::chrono::steady_clock::time_point now) {
    const bool rising = guardActive();

    std::lock_guard lock(m_weightMutex);
    float weight = m_weight.load(std::memory_order_relaxed);
    if (!enabled) {
        weight = 0.0f;
        m_weightClockValid = false;
    } else if (!m_weightClockValid) {
        // First frame after enabling: measure from here, so a re-enable never
        // brings the sword up as if it had been held the whole time.
        m_weightClockValid = true;
        m_lastWeightUpdate = now;
        if (!rising) weight = 0.0f;
    } else {
        const float delta = std::chrono::duration<float>(now - m_lastWeightUpdate).count();
        m_lastWeightUpdate = now;
        weight = swordblocking::advanceWeight(weight, rising, m_fadeSpeed, delta);
    }
    m_weight.store(weight, std::memory_order_release);
}

void SwordBlockingModule::onFrame() {
    if (!enabled) return;

    refreshHoldState(std::chrono::steady_clock::now());

    std::vector<pl::modmenu::DrawCommand> commands;
    const float weight = poseWeight();
    const bool thirdPerson = m_thirdPerson.load(std::memory_order_relaxed);
    if (m_indicator && (!thirdPerson || m_showThirdPerson)) {
        const auto lines = swordblocking::indicatorLines(
            kHudCenter, kHudCenter, weight, indicatorStyle());
        commands.reserve(lines.size());
        for (const auto& line : lines) {
            pl::modmenu::DrawCommand command{};
            command.type = pl::modmenu::DrawCommandType::Line;
            command.x = line.x1;
            command.y = line.y1;
            command.w = line.x2 - line.x1;
            command.h = line.y2 - line.y1;
            command.size = line.thickness;
            command.color = line.color;
            commands.push_back(std::move(command));
        }
    }

    // Skip the submission while the overlay is already empty; the launcher
    // keeps the last list, so there is nothing to clear.
    if (commands.empty() && !m_submitted.load(std::memory_order_relaxed)) return;
    pl::modmenu::submitDrawCommands(moduleId, commands);
    m_submitted.store(!commands.empty(), std::memory_order_relaxed);
}

bool SwordBlockingModule::onMouseEvent(int button, bool isDown) {
    if (!enabled || m_mouseButton < 0 || button != m_mouseButton) return false;
    m_mouseHeld.store(isDown, std::memory_order_release);
    // Never consume the event: the game keeps its own use control, so breaking
    // and placing blocks behaves exactly as before.
    return false;
}

bool SwordBlockingModule::onKeyEvent(int key, bool isDown) {
    if (!enabled || m_holdKeybind <= 0 || key != m_holdKeybind) return false;
    m_keyHeld.store(isDown, std::memory_order_release);
    return true;
}

void SwordBlockingModule::syncOverlayButton() {
    unregisterOverlayButton();
    if (!m_screenButton) return;

    pl::modmenu::ButtonBuilder builder(kHoldButtonId, "Sword Block");
    builder.moduleId(moduleId)
        .label("Block")
        .behavior(pl::modmenu::ButtonBehavior::Hold)
        .defaultVisible(true)
        .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
        .styleColors(0x00000001, 0x00000001, 0x00000001)
        .svgIcon(kHoldButtonSvg, true)
        .activeSvgIcon(kHoldButtonActiveSvg)
        .onEvent([this](std::string_view, pl::modmenu::ButtonEvent event, float) {
            switch (event) {
                case pl::modmenu::ButtonEvent::Down:
                    m_buttonHeld.store(true, std::memory_order_release);
                    break;
                case pl::modmenu::ButtonEvent::Up:
                    m_buttonHeld.store(false, std::memory_order_release);
                    break;
                default:
                    break;
            }
        });
    m_buttonRegistered = builder.registerButton();
    if (!m_buttonRegistered) {
        // A launcher that rejects the button just loses the touch path; the
        // mouse and keybind sources keep working.
        m_buttonHeld.store(false, std::memory_order_release);
    }
}

void SwordBlockingModule::unregisterOverlayButton() {
    pl::modmenu::unregisterButton(kHoldButtonId);
    m_buttonHeld.store(false, std::memory_order_release);
    m_buttonRegistered = false;
}

void SwordBlockingModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("m_holdStyle") && j["m_holdStyle"].is_string()) {
        m_holdStyle = swordblocking::holdModeIndex(j["m_holdStyle"].get<std::string>());
    }
    if (j.contains("m_mouseButton")) {
        try { m_mouseButton = std::clamp(j["m_mouseButton"].get<int>(), 0, 8); } catch (...) {}
    }
    if (j.contains("m_holdKeybind")) {
        try { m_holdKeybind = std::max(0, j["m_holdKeybind"].get<int>()); } catch (...) {}
    }
    if (j.contains("m_screenButton")) {
        try { m_screenButton = j["m_screenButton"].get<bool>(); } catch (...) {}
    }

    const auto readFloat = [&j](const char* key, float& destination, float minimum, float maximum) {
        if (!j.contains(key)) return;
        try {
            const float value = j[key].get<float>();
            if (std::isfinite(value)) destination = std::clamp(value, minimum, maximum);
        } catch (...) {
        }
    };

    readFloat("m_fadeSpeed", m_fadeSpeed, swordblocking::minFadeSpeed, swordblocking::maxFadeSpeed);
    readFloat("m_tiltDegrees", m_tiltDegrees, 0.0f, 120.0f);
    readFloat("m_lift", m_lift, -16.0f, 16.0f);
    readFloat("m_across", m_across, -16.0f, 16.0f);
    readFloat("m_forward", m_forward, -16.0f, 16.0f);
    readFloat("m_poseScale", m_poseScale, 0.25f, 4.0f);
    readFloat("m_indicatorOpacity", m_indicatorOpacity, 0.0f, 1.0f);
    readFloat("m_indicatorScale", m_indicatorScale, 0.25f, 4.0f);

    if (j.contains("m_indicator")) {
        try { m_indicator = j["m_indicator"].get<bool>(); } catch (...) {}
    }
    if (j.contains("m_indicatorColor") && j["m_indicatorColor"].is_string()) {
        m_indicatorColor = colorString(parseColor(j["m_indicatorColor"].get<std::string>(), 0xFF7FE3FFu));
    }
    if (j.contains("m_showThirdPerson")) {
        try { m_showThirdPerson = j["m_showThirdPerson"].get<bool>(); } catch (...) {}
    }

    m_holdStyle = std::clamp(m_holdStyle, 0, static_cast<int>(swordblocking::HoldMode::Count) - 1);
    m_fadeSpeed = swordblocking::clampFadeSpeed(m_fadeSpeed);

    // The overlay button is part of the config, so a settings change has to
    // re-register (or drop) it.
    syncOverlayButton();
}

void SwordBlockingModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["m_holdStyle"] = swordblocking::holdModeRadioValue(m_holdStyle);
    j["m_mouseButton"] = std::clamp(m_mouseButton, 0, 8);
    j["m_holdKeybind"] = std::max(0, m_holdKeybind);
    j["m_screenButton"] = m_screenButton;

    j["m_fadeSpeed"] = swordblocking::clampFadeSpeed(m_fadeSpeed);

    j["m_tiltDegrees"] = std::clamp(m_tiltDegrees, 0.0f, 120.0f);
    j["m_lift"] = std::clamp(m_lift, -16.0f, 16.0f);
    j["m_across"] = std::clamp(m_across, -16.0f, 16.0f);
    j["m_forward"] = std::clamp(m_forward, -16.0f, 16.0f);
    j["m_poseScale"] = std::clamp(m_poseScale, 0.25f, 4.0f);

    j["m_indicator"] = m_indicator;
    j["m_indicatorColor"] = colorString(parseColor(m_indicatorColor, 0xFF7FE3FFu));
    j["m_indicatorOpacity"] = std::clamp(m_indicatorOpacity, 0.0f, 1.0f);
    j["m_indicatorScale"] = std::clamp(m_indicatorScale, 0.25f, 4.0f);
    j["m_showThirdPerson"] = m_showThirdPerson;
}
