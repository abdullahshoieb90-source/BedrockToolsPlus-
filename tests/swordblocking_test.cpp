// Host tests for the Sword Blocking module.
//
// The module is compiled for real (swordblocking.cpp, including every hook
// detour) against in-memory stand-ins for the game, so this exercises the code
// that runs on the phone, not a copy of it:
//
//   * the name classifier only accepts "<material>_sword"
//   * the hold-mode radio is append-only and round-trips through the config
//   * the weight eases in and out, clamps, and survives paused frames
//   * the pose matrix is the identity at rest, respects the blend weight and
//     rejects non-finite input
//   * the render detours pose the *main-hand first-person* item only, write the
//     pose through the live matrix stack, and restore the matrix afterwards
//   * the carried item is sampled per tick (sword held yes/no)
//   * the indicator appears under the crosshair only while raised, honours the
//     third-person toggle, and is cleared when the module is disabled
//   * settings round-trip through saveConfig/loadConfig
//
// Build and run standalone:
//
//     g++ -std=c++20 -I include -I src -I tests/swordblocking_fakepl -I tests/fakejson
//         tests/swordblocking_test.cpp -o /tmp/swordblocking_test
//     /tmp/swordblocking_test
//
// tests/swordblocking_fakepl is a recording version of the preloader headers
// (it captures the submitted draw commands and the button definition); the
// plain tests/fakepl stubs are no-ops and cannot be used here.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId id) {
    // Non-null for every signature the module installs, so its detours are
    // created (the host hook stub hands back a null trampoline, which every
    // detour already has to tolerate).
    return static_cast<std::uintptr_t>(id) + 0x1000;
}
} // namespace bedrocktools::memory

// ---------------------------------------------------------------------------
// Module implementation
// ---------------------------------------------------------------------------

#include "modules/visual/swordblocking.cpp"

// The file-local detours are what the game would call; the tests call them
// directly instead of going through a trampoline.
namespace {

using namespace bedrocktools::sdk;
namespace off = bedrocktools::sdk::offsets;

int g_failures = 0;

void check(bool condition, const char* message) {
    std::printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++g_failures;
}

bool near(float a, float b, float epsilon = 0.0005f) {
    return std::fabs(a - b) <= epsilon;
}

template <class T>
void put(void* object, std::size_t offset, const T& value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(T));
}

// One FakeItem: its first word is the vtable pointer (exactly like a real
// polymorphic Item), and slot 5 of that vtable is Item::getDescriptionId, the
// only entry this module calls.
std::string g_identifier;
const std::string& fakeDescriptionId(void*) { return g_identifier; }

std::array<void*, 8> g_itemVtable{
    nullptr, nullptr, nullptr, nullptr, nullptr,
    reinterpret_cast<void*>(&fakeDescriptionId), nullptr, nullptr};

struct FakeItem {
    void** vtable = g_itemVtable.data();
};
FakeItem g_item;

// A 4x4 matrix stack laid out the way the game lays it out: a block table, a
// live start/size pair and the top element.
struct FakeMatrixStack {
    alignas(16) std::array<float, 16 * 16> block{};
    std::array<void*, 2> blocks{};
    std::array<std::byte, 0x80> stack{};
    std::array<std::byte, 0x40> wrapper{};
    std::array<std::byte, 0x40> renderContext{};

    FakeMatrixStack() {
        blocks[0] = block.data();
        put(stack.data(), off::MatrixStack::mBlocks, blocks.data());
        put(stack.data(), off::MatrixStack::mStart, std::size_t{0});
        put(stack.data(), off::MatrixStack::mSize, std::size_t{1});
        put(wrapper.data(), off::MatrixStackWrapper::mMatrixStack, stack.data());
        put(renderContext.data(), off::RenderContext::mMatrixStackWrapper, wrapper.data());
    }

    float* top() { return block.data(); }
    void* context() { return renderContext.data(); }
};

// Sets the item pointer the carried stack exposes.
struct FakeStack {
    std::array<std::byte, 0x40> storage{};
    std::array<std::byte, 0x10> counter{};

    FakeStack() {
        put(storage.data(), off::ShulkerPreview::ItemStackBaseItem, counter.data());
        put(counter.data(), off::ShulkerPreview::SharedCounterPointer,
            static_cast<void*>(&g_item));
    }
    void* stack() { return storage.data(); }
};

// Player vtable with slot 78 = getCarriedItem.
void* g_carriedStack = nullptr;
void* fakeGetCarriedItem(void*) { return g_carriedStack; }
std::array<void*, 96> g_playerVtable{};

struct FakePlayer {
    std::array<std::byte, 0x20> storage{};
    void* player() { return storage.data(); }
};

// Stand-in for the real renderObject: called by the module's detour exactly
// where the game would draw the item, so the matrix can be sampled at that
// moment (that is the whole point of the pose).
bool g_drawCalled = false;
bool g_mainHandDraw = false;
bool g_captureOffHand = false;
std::array<float, 16> g_matrixDuringDraw{};
void captureRenderObject(void*, void* renderContext, void*, void*, int) {
    float* matrix = currentMatrix(renderContext);
    if (!matrix) return;
    g_drawCalled = true;
    // The first-person pass draws the main hand and then the offhand; the
    // caller says which of the two it wants to sample.
    if (tl_offHandScope != g_captureOffHand) return;
    for (std::size_t i = 0; i < 16; ++i) g_matrixDuringDraw[i] = matrix[i];
    g_mainHandDraw = true;
}

// The real call chain nests like this:
//
//   renderFirstPerson
//     renderHand(main)
//       RenderItem(main)  ->  renderObject   <- matrix sampled here
//
// so the stand-ins for the *outer* functions call the module's detours from
// inside their own bodies, exactly like the game calls the inner functions.
// Calling the detours one after another instead would leave the thread-local
// scopes flat and test a call shape that never happens.
void outerFirstPerson(void* self, void* renderContext, void* previousProjection, int itemFlags) {
    // main hand, then offhand - both inside the first-person scope.
    renderHandDetour(self, renderContext, nullptr, 1, 0.5f);
    renderHandDetour(self, renderContext, nullptr, 0, 0.5f);
    (void)previousProjection;
    (void)itemFlags;
}

void outerRenderHandMain(void* self, void* renderContext, void* player, int renderingMainHand, float frameAlpha) {
    renderItemDetour(self, renderContext, player, nullptr, 0, 0, 0, renderingMainHand);
    (void)frameAlpha;
}

void outerRenderItem(void* self, void* renderContext, void* entity, void* item,
                     int posAndRotSet, int itemFlags, int useMatrixAsIs, int renderingMainHand) {
    renderObjectDetour(self, renderContext, nullptr, nullptr, 0);
    (void)entity;
    (void)item;
    (void)posAndRotSet;
    (void)itemFlags;
    (void)useMatrixAsIs;
    (void)renderingMainHand;
}

// Runs the module for `frames` frames of ~10 ms, which is what advances the
// weight clock (it is driven by steady_clock, like the real overlay).
void runFrames(SwordBlockingModule& module, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        module.onFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

int main() {
    using namespace swordblocking;

    std::printf("sword blocking pure logic\n");
    {
        check(isSwordName("diamond_sword"), "a vanilla sword id is accepted");
        check(isSwordName("netherite_sword"), "every material suffix works");
        check(!isSwordName("diamond_pickaxe"), "a pickaxe is not a sword");
        check(!isSwordName("sword"), "the bare word is not an item id");
        check(!isSwordName("diamond_swordfish"), "only the _sword suffix counts");
        check(!isSwordName(""), "an empty id is rejected");
        check(!isSwordName("diamond sword"), "an id with a space is rejected");

        check(holdModeIndex("2") == 2, "the radio index is read back");
        check(holdModeIndex("Screen Button") == 2, "the radio label is accepted too");
        check(holdModeIndex("bogus") == 0, "an unknown radio value falls back to Auto");
        check(holdModeRadioValue(3) == "3,Auto,Right Click,Screen Button,Hold Key,Always",
              "the radio value keeps the documented index,label layout");

        check(holdActive(HoldMode::Auto, false, false, false) == false, "Auto needs one input");
        check(holdActive(HoldMode::Auto, true, false, false), "Auto accepts the mouse");
        check(holdActive(HoldMode::Auto, false, true, false), "Auto accepts the screen button");
        check(holdActive(HoldMode::Auto, false, false, true), "Auto accepts the key");
        check(!holdActive(HoldMode::RightClick, false, true, true), "Right Click ignores other inputs");
        check(holdActive(HoldMode::HoldKey, false, false, true), "Hold Key uses only the key");
        check(holdActive(HoldMode::Always, false, false, false), "Always blocks with no input");
    }

    std::printf("weight animation\n");
    {
        check(near(advanceWeight(0.0f, true, 1.0f, 0.01f), 0.05f), "raising moves at the fade rate");
        check(near(advanceWeight(0.0f, true, 1.0f, 10.0f), 1.0f), "a long frame cannot overshoot");
        check(near(advanceWeight(1.0f, false, 1.0f, 10.0f), 0.0f), "falling cannot undershoot");
        check(near(advanceWeight(0.5f, true, 1.0f, 0.0f), 0.5f), "a zero delta keeps the weight");
        check(near(advanceWeight(0.5f, true, 1.0f, -1.0f), 0.5f), "a negative delta keeps the weight");
        check(near(advanceWeight(0.5f, true, 0.2f, 5.0f), 0.5f + 0.2f * fadeRateScale * maxFrameDelta),
              "a paused frame is capped by maxFrameDelta");
        check(near(advanceWeight(0.0f, true, 0.05f, 0.05f), 0.05f * fadeRateScale * 0.05f),
              "the slowest fade still moves");
        check(near(advanceWeight(std::nanf(""), true, 0.45f, 0.016f), 0.45f * fadeRateScale * 0.016f),
              "a NaN weight restarts from zero");
        check(near(clampFadeSpeed(std::nanf("")), defaultFadeSpeed), "a NaN speed falls back");

        float weight = 0.0f;
        for (int frame = 0; frame < 200; ++frame) weight = advanceWeight(weight, true, 0.45f, 1.0f / 60.0f);
        check(near(weight, 1.0f), "holding for long enough fully raises the sword");
    }

    std::printf("pose matrix\n");
    {
        PoseParams pose;
        const Mat4 rest = poseMatrix(pose, 0.0f);
        Mat4 identity;
        bool same = true;
        for (std::size_t i = 0; i < 16; ++i) same = same && near(rest.m[i], identity.m[i]);
        check(same, "a zero weight is the identity (nothing is touched when idle)");

        const Mat4 half = poseMatrix(pose, 0.5f);
        const Mat4 full = poseMatrix(pose, 1.0f);
        const auto halfTip = transformPoint(half, 0.0f, 10.0f, 0.0f);
        const auto fullTip = transformPoint(full, 0.0f, 10.0f, 0.0f);
        const auto halfOrigin = transformPoint(half, 0.0f, 0.0f, 0.0f);
        check(near(halfOrigin[0], pose.across * 0.5f) && near(halfOrigin[1], pose.lift * 0.5f),
              "the grip moves half as far at half weight");
        // The blade must travel further than the grip: that is the tilt.
        const float halfSwing = std::fabs(halfTip[0] - halfOrigin[0]);
        const float fullSwing = std::fabs(fullTip[0] - halfOrigin[0]);
        check(fullSwing > halfSwing && halfSwing > 0.0f, "the blade tilts across the face");

        PoseParams wild;
        wild.tiltDegrees = 10000.0f;
        wild.lift = -10000.0f;
        wild.across = std::nanf("");
        wild.forward = 10000.0f;
        wild.scale = 0.0f;
        const PoseParams clamped = clampedPose(wild);
        check(clamped.tiltDegrees <= 120.0f && clamped.lift >= -16.0f &&
              std::isfinite(clamped.across) && clamped.forward <= 16.0f && clamped.scale >= 0.25f,
              "out-of-range settings are clamped");
        const Mat4 sanitized = poseMatrix(wild, 1.0f);
        check(isFinite(sanitized), "a wild pose still produces a finite matrix");

        float target[16];
        Mat4 base;
        for (std::size_t i = 0; i < 16; ++i) target[i] = base.m[i];
        check(composeInto(target, half), "the pose composes onto a live matrix");
        check(near(target[12], pose.across * 0.5f), "the composed matrix carries the translation");
        float broken[16] = {};
        broken[0] = std::nanf("");
        check(!composeInto(broken, half), "a non-finite live matrix is left alone");
        check(!composeInto(nullptr, half), "a null matrix is refused");
    }

    std::printf("guard presets\n");
    {
        const auto classic = presetPose(PosePreset::Classic);
        const auto high = presetPose(PosePreset::High);
        const auto wide = presetPose(PosePreset::Wide);
        const auto subtle = presetPose(PosePreset::Subtle);
        check(high.lift > classic.lift && high.tiltDegrees > classic.tiltDegrees,
              "the High preset raises the guard above Classic");
        check(wide.spinDegrees > classic.spinDegrees && subtle.tiltDegrees < classic.tiltDegrees,
              "Wide spins the blade more, Subtle swings it less");

        check(presetPose(PosePreset::Custom).tiltDegrees == PoseParams{}.tiltDegrees,
              "Custom leaves the defaults untouched");

        check(posePresetIndex("1") == 1, "the preset radio index is read back");
        check(posePresetIndex("Wide") == 2, "the preset label is accepted too");
        check(posePresetIndex("nonsense") == 0, "an unknown preset falls back to Classic");
        check(posePresetRadioValue(2) == "2,Classic,High,Wide,Subtle,Custom",
              "the preset radio keeps the index,label layout");

        // The three rotations must be independent. A blade points along +Y, so
        // a roll about X tips it toward the camera, while a yaw about Y leaves
        // the blade where it is and turns its flat instead. The translations
        // are zeroed here so the check reads only the rotation.
        PoseParams rolled;
        rolled.tiltDegrees = 0.0f;
        rolled.pitchDegrees = 60.0f;
        rolled.spinDegrees = 0.0f;
        rolled.lift = rolled.across = rolled.forward = 0.0f;
        rolled.scale = 1.0f;
        const auto rollTip = transformPoint(poseMatrix(rolled, 1.0f), 0.0f, 10.0f, 0.0f);
        check(near(rollTip[0], 0.0f) && rollTip[1] < 9.0f && rollTip[2] > 4.0f,
              "a roll tips the blade toward the camera");

        PoseParams yawed;
        yawed.tiltDegrees = 0.0f;
        yawed.pitchDegrees = 0.0f;
        yawed.spinDegrees = 45.0f;
        yawed.lift = yawed.across = yawed.forward = 0.0f;
        yawed.scale = 1.0f;
        const Mat4 yawOnly = poseMatrix(yawed, 1.0f);
        const auto yawTip = transformPoint(yawOnly, 0.0f, 10.0f, 0.0f);
        const auto yawEdge = transformPoint(yawOnly, 2.0f, 0.0f, 0.0f);
        check(near(yawTip[0], 0.0f) && near(yawTip[1], 10.0f),
              "a yaw leaves the blade itself in place");
        check(std::fabs(yawEdge[2]) > 1.0f && std::fabs(yawEdge[0]) < 2.0f,
              "but turns the flat of the blade toward the camera");

        // The default guard has to move the blade noticeably: the reference
        // clip swings it roughly -9 -> -36 degrees across the screen.
        const auto tipAtRest = transformPoint(poseMatrix(PoseParams{}, 0.0f), 0.0f, 10.0f, 0.0f);
        const auto tipGuarding = transformPoint(poseMatrix(PoseParams{}, 1.0f), 0.0f, 10.0f, 0.0f);
        const float swing = std::fabs(tipGuarding[0] - tipAtRest[0]);
        check(swing > 3.0f, "the default preset swings the blade visibly across the face");
        check(tipGuarding[1] > tipAtRest[1], "and lifts it toward the eye line");

        PoseParams wildRotations;
        wildRotations.spinDegrees = std::nanf("");
        wildRotations.pitchDegrees = 1e9f;
        wildRotations.tiltDegrees = -1e9f;
        const PoseParams cleaned = clampedPose(wildRotations);
        check(std::isfinite(cleaned.spinDegrees) && std::fabs(cleaned.pitchDegrees) <= 120.0f &&
              std::fabs(cleaned.tiltDegrees) <= 120.0f,
              "the new rotation sliders are clamped too");
        check(isFinite(poseMatrix(wildRotations, 1.0f)),
              "a wild rotation still produces a finite matrix");
    }

    std::printf("indicator geometry\n");
    {
        IndicatorStyle style;
        check(indicatorLines(-20000.0f, -20000.0f, 0.0f, style).empty(),
              "nothing is drawn while the sword is down");
        const auto lines = indicatorLines(-20000.0f, -20000.0f, 1.0f, style);
        check(lines.size() == 6, "the shield outline is six strokes");
        bool below = true;
        for (const auto& line : lines) below = below && line.y1 > -20000.0f;
        check(below, "the indicator sits under the crosshair, never over it");
        bool alpha = true;
        for (const auto& line : lines) alpha = alpha && ((line.color >> 24) & 0xFFu) > 0u;
        check(alpha, "every stroke is visible at full weight");

        const auto faded = indicatorLines(-20000.0f, -20000.0f, 0.25f, style);
        check(!faded.empty() && ((faded[0].color >> 24) & 0xFFu) < ((lines[0].color >> 24) & 0xFFu),
              "the indicator fades with the pose weight");

        IndicatorStyle bigger;
        bigger.scale = 2.0f;
        const auto scaled = indicatorLines(-20000.0f, -20000.0f, 1.0f, bigger);
        check(scaled.size() == lines.size() && scaled[0].x1 < lines[0].x1,
              "a larger scale widens the shield");

        IndicatorStyle hidden;
        hidden.opacity = 0.0f;
        check(indicatorLines(-20000.0f, -20000.0f, 1.0f, hidden).empty(),
              "zero opacity draws nothing at all");
    }

    std::printf("module + render detours\n");
    {
        FakeStack stack;
        FakePlayer player;
        FakeMatrixStack matrices;
        g_carriedStack = stack.stack();
        g_playerVtable[off::VTable::PlayerGetCarriedItem] =
            reinterpret_cast<void*>(&fakeGetCarriedItem);
        put(player.storage.data(), 0, g_playerVtable.data());

        g_identifier = "iron_sword";

        SwordBlockingModule module;
        module.onInit();
        module.setMasterEnabled(true);
        check(g_swordBlocking == &module, "the module registers itself for the detours");

        // Tick sampling: a sword is carried, but nothing is held yet.
        module.onPlayerTick(player.player());
        check(module.guardActive() == false, "no input means no guard even with a sword");

        module.loadConfig(nlohmann::json{});
        check(module.m_screenButton, "the overlay button is on by default");

        // Right click press (Android button 2) is the Auto hold source.
        check(module.onMouseEvent(module.m_mouseButton, true) == false,
              "the use control is observed, not consumed");
        check(module.guardActive(), "holding the use control raises the guard");

        // Let the weight reach full (0.45 fade speed raises it in ~0.45 s).
        runFrames(module, 70);
        check(near(module.poseWeight(), 1.0f), "the guard fully raises while held");
        check(!pl::modmenu::g_lastCommands.empty(), "the indicator is submitted while raised");

        // ---- the real render call chain, with in-memory game objects -------
        Mat4 expectedBase;
        for (std::size_t i = 0; i < 16; ++i) matrices.top()[i] = expectedBase.m[i];

        const RenderObjectFn previousObject = s_renderObjectOriginal;
        const RenderItemFn previousItem = s_renderItemOriginal;
        const RenderHandFn previousHand = s_renderHandOriginal;

        // Main hand: firstPerson -> renderHand(main) -> RenderItem(main) ->
        // renderObject, sampling the matrix where the game would draw.
        s_renderObjectOriginal = captureRenderObject;
        s_renderItemOriginal = outerRenderItem;
        s_renderHandOriginal = outerRenderHandMain;

        const RenderFirstPersonFn savedFirstPerson = s_renderFirstPersonOriginal;
        s_renderFirstPersonOriginal = outerFirstPerson;

        g_drawCalled = false;
        g_mainHandDraw = false;
        g_matrixDuringDraw = {};
        for (std::size_t i = 0; i < 16; ++i) matrices.top()[i] = expectedBase.m[i];
        renderFirstPersonDetour(nullptr, matrices.context(), player.player(), 0);
        check(g_drawCalled && g_mainHandDraw, "the item is still drawn through the original path");
        check(!near(g_matrixDuringDraw[12], expectedBase.m[12]) ||
              !near(g_matrixDuringDraw[13], expectedBase.m[13]),
              "the guard pose moves the item matrix while it is drawn");
        check(near(matrices.top()[12], expectedBase.m[12]) &&
              near(matrices.top()[13], expectedBase.m[13]),
              "the matrix is restored after the item is drawn");
        check(near(matrices.top()[0], expectedBase.m[0]) && near(matrices.top()[5], expectedBase.m[5]),
              "the restore covers the whole matrix, not only the translation");

        // Offhand: the same chain with the main-hand flag clear is never posed.
        g_captureOffHand = true;
        g_drawCalled = false;
        g_mainHandDraw = false;
        g_matrixDuringDraw = {};
        for (std::size_t i = 0; i < 16; ++i) matrices.top()[i] = expectedBase.m[i];
        renderFirstPersonDetour(nullptr, matrices.context(), player.player(), 0);
        g_captureOffHand = false;
        check(g_drawCalled, "the offhand item is still drawn");
        check(near(g_matrixDuringDraw[12], expectedBase.m[12]) &&
              near(g_matrixDuringDraw[13], expectedBase.m[13]),
              "the offhand item is never posed");

        // Third person: the item goes through the actor renderer, so there is
        // no first-person scope and no pose.
        s_renderHandOriginal = previousHand;
        s_renderItemOriginal = outerRenderItem;
        g_drawCalled = false;
        g_matrixDuringDraw = {};
        for (std::size_t i = 0; i < 16; ++i) matrices.top()[i] = expectedBase.m[i];
        renderHandDetour(nullptr, matrices.context(), player.player(), 1, 0.5f);
        renderObjectDetour(nullptr, matrices.context(), nullptr, nullptr, 0);
        check(near(matrices.top()[12], expectedBase.m[12]),
              "third-person items are left to the actor renderer");

        s_renderFirstPersonOriginal = savedFirstPerson;
        s_renderObjectOriginal = previousObject;
        s_renderItemOriginal = previousItem;
        s_renderHandOriginal = previousHand;

        // Releasing the control brings the sword back down.
        module.onMouseEvent(module.m_mouseButton, false);
        check(!module.guardActive(), "releasing the control lowers the guard");
        runFrames(module, 70);
        check(near(module.poseWeight(), 0.0f), "the guard returns fully");
        check(pl::modmenu::g_lastCommands.empty(), "the indicator is cleared when lowered");

        // A non-sword in hand never guards.
        g_identifier = "iron_pickaxe";
        module.onPlayerTick(player.player());
        module.onMouseEvent(module.m_mouseButton, true);
        check(!module.guardActive(), "a pickaxe does not guard");

        // Third-person indicator toggle.
        g_identifier = "diamond_sword";
        module.onPlayerTick(player.player());
        runFrames(module, 70);
        check(!pl::modmenu::g_lastCommands.empty(), "the indicator is back with a sword");

        module.m_thirdPerson.store(true, std::memory_order_relaxed);
        module.onFrame();
        check(pl::modmenu::g_lastCommands.empty(), "the indicator hides in third person by default");
        module.m_showThirdPerson = true;
        module.onFrame();
        check(!pl::modmenu::g_lastCommands.empty(), "the third-person option shows it again");
        module.m_thirdPerson.store(false, std::memory_order_relaxed);

        // The launcher hold button drives the touch path.
        check(!pl::modmenu::g_buttons.empty(), "the hold button is registered");
        if (!pl::modmenu::g_buttons.empty()) {
            const auto& button = pl::modmenu::g_buttons.front();
            check(button.behavior == pl::modmenu::ButtonBehavior::Hold,
                  "the button uses hold semantics");
            module.onMouseEvent(module.m_mouseButton, false);
            check(!module.guardActive(), "released with the button still up");
            button.onEvent(button.buttonId, pl::modmenu::ButtonEvent::Down, 0.0f);
            check(module.guardActive(), "the on-screen button raises the guard");
            button.onEvent(button.buttonId, pl::modmenu::ButtonEvent::Up, 0.0f);
            check(!module.guardActive(), "releasing the on-screen button lowers it again");
        }

        // A launcher that refuses the button drops only the touch path: no
        // phantom hold from a stale Down event, and the mouse source still works.
        module.m_screenButton = true;
        pl::modmenu::g_rejectButtons = true;
        module.setMasterEnabled(false);
        module.setMasterEnabled(true);
        module.onPlayerTick(player.player());  // enabling clears the cached sword
        check(!module.overlayButtonRegistered(), "a refused button reports not registered");
        check(!module.m_buttonHeld.load(), "a refused button cannot leave the guard held");
        check(pl::modmenu::g_buttons.empty(), "a refused button is absent from the overlay");
        module.onMouseEvent(module.m_mouseButton, true);
        check(module.guardActive(), "the mouse source still guards without the button");
        module.onMouseEvent(module.m_mouseButton, false);
        check(!module.guardActive(), "and releases normally");
        pl::modmenu::g_rejectButtons = false;
        module.setMasterEnabled(false);
        module.setMasterEnabled(true);
        module.onPlayerTick(player.player());
        check(module.overlayButtonRegistered(), "the button registers once the launcher accepts it");

        // The hold key is the third source.
        module.m_holdStyle = static_cast<int>(HoldMode::HoldKey);
        module.m_holdKeybind = 71;
        module.onKeyEvent(71, true);
        check(module.guardActive(), "the configured hold key raises the guard");
        module.onKeyEvent(71, false);
        check(!module.guardActive(), "releasing the hold key lowers it");
        module.m_holdStyle = static_cast<int>(HoldMode::Auto);

        // Disabling clears everything at once.
        module.setMasterEnabled(false);
        check(near(module.poseWeight(), 0.0f), "disabling drops the weight immediately");
        check(pl::modmenu::g_lastCommands.empty(), "disabling removes the indicator");
        check(pl::modmenu::g_buttons.empty(), "disabling removes the hold button");

        // Re-enabling starts from rest even if the input is still held.
        module.setMasterEnabled(true);
        module.onFrame();
        check(near(module.poseWeight(), 0.0f),
              "re-enabling does not pop the sword up from a stale clock");
    }

    std::printf("config round-trip\n");
    {
        SwordBlockingModule module;
        module.onInit();
        module.m_holdStyle = static_cast<int>(HoldMode::ScreenButton);
        module.m_mouseButton = 4;
        module.m_holdKeybind = 71;
        module.m_screenButton = false;
        module.m_fadeSpeed = 0.9f;
        module.m_tiltDegrees = 40.0f;
        module.m_lift = 2.5f;
        module.m_across = -3.0f;
        module.m_forward = 2.0f;
        module.m_poseScale = 1.4f;
        module.m_indicator = false;
        module.m_indicatorColor = "#112233";
        module.m_indicatorOpacity = 0.4f;
        module.m_indicatorScale = 2.2f;
        module.m_showThirdPerson = true;

        nlohmann::json saved;
        module.saveConfig(saved);
        check(saved["m_holdStyle"].get<std::string>() ==
                  holdModeRadioValue(static_cast<int>(HoldMode::ScreenButton)),
              "the hold style is saved in radio format");
        check(saved["m_holdKeybind"].get<int>() == 71, "the hold keybind is saved");
        check(saved["m_screenButton"].get<bool>() == false, "the overlay button toggle is saved");

        SwordBlockingModule loaded;
        loaded.onInit();
        loaded.loadConfig(saved);
        check(loaded.m_holdStyle == static_cast<int>(HoldMode::ScreenButton), "hold style reloads");
        check(loaded.m_mouseButton == 4, "mouse button reloads");
        check(loaded.m_holdKeybind == 71, "hold keybind reloads");
        check(loaded.m_screenButton == false, "overlay button reloads");
        check(near(loaded.m_fadeSpeed, 0.9f), "fade speed reloads");
        check(near(loaded.m_tiltDegrees, 40.0f) && near(loaded.m_across, -3.0f),
              "the pose reloads");
        check(loaded.m_indicator == false, "the indicator toggle reloads");
        check(loaded.m_indicatorColor == "#112233", "the indicator color reloads");
        check(near(loaded.m_indicatorScale, 2.2f), "the indicator scale reloads");
        check(loaded.m_showThirdPerson, "the third-person option reloads");
        check(loaded.m_posePreset == static_cast<int>(PosePreset::Custom),
              "a hand-tuned pose saves as Custom, so it is not snapped back on load");
        check(near(loaded.m_pitchDegrees, module.m_pitchDegrees) &&
              near(loaded.m_spinDegrees, module.m_spinDegrees),
              "the new rotation sliders reload");

        // Picking a preset from the menu writes the pose; the radio is only
        // claimed for its own key so the generic path keeps handling the rest.
        SwordBlockingModule presets;
        presets.onInit();
        check(!presets.onMenuConfigChanged("m_tiltDegrees", "12"), "other keys are not intercepted");
        check(presets.onMenuConfigChanged("m_posePreset", "2"), "the preset radio is intercepted");
        const auto wide = presetPose(PosePreset::Wide);
        check(presets.m_posePreset == static_cast<int>(PosePreset::Wide) &&
              near(presets.m_spinDegrees, wide.spinDegrees) &&
              near(presets.m_poseScale, wide.scale),
              "choosing a preset applies it");
        check(presets.poseMatchesPreset(), "the applied pose matches its preset");
        presets.m_lift += 1.0f;
        check(!presets.poseMatchesPreset(), "editing a slider breaks the match");

        nlohmann::json presetSaved;
        presets.saveConfig(presetSaved);
        check(presetSaved["m_posePreset"].get<std::string>().rfind("4,", 0) == 0,
              "moving a slider after choosing a preset switches the radio to Custom");

        // A preset in the file wins over stale pose values, which is what an
        // upgrade from a build with different numbers looks like.
        nlohmann::json presetFile;
        presetFile["m_posePreset"] = std::string("1");
        presetFile["m_tiltDegrees"] = 5.0f;
        SwordBlockingModule upgraded;
        upgraded.onInit();
        upgraded.loadConfig(presetFile);
        check(upgraded.m_posePreset == static_cast<int>(PosePreset::High) &&
              near(upgraded.m_tiltDegrees, presetPose(PosePreset::High).tiltDegrees),
              "a stored preset overrides stale pose values on load");

        // Custom in the file keeps whatever the sliders said, as long as the
        // config was written by a build that has the spin slider.
        nlohmann::json customFile;
        customFile["m_posePreset"] = std::string("4");
        customFile["m_tiltDegrees"] = 5.0f;
        customFile["m_pitchDegrees"] = 3.0f;
        customFile["m_spinDegrees"] = 7.0f;
        SwordBlockingModule custom;
        custom.onInit();
        custom.loadConfig(customFile);
        check(near(custom.m_tiltDegrees, 5.0f) && near(custom.m_spinDegrees, 7.0f),
              "a stored Custom keeps the tuned pose");

        // A "Custom" pose saved before the spin slider existed carries only the
        // old tilt/pitch pair and the old right-shifted grip. It falls back to
        // the current Classic defaults instead of keeping a pose that predates
        // the guard rework.
        nlohmann::json legacyFile;
        legacyFile["m_posePreset"] = std::string("4");
        legacyFile["m_tiltDegrees"] = 65.0f;
        legacyFile["m_across"] = 4.0f;
        SwordBlockingModule legacy;
        legacy.onInit();
        legacy.loadConfig(legacyFile);
        check(near(legacy.m_tiltDegrees, SwordBlockingModule::m_defaultTiltDegrees) &&
              near(legacy.m_across, SwordBlockingModule::m_defaultAcross) &&
              legacy.m_posePreset == static_cast<int>(PosePreset::Classic),
              "a pre-spin Custom pose migrates to the current defaults");

        // Clamping on the way in.
        nlohmann::json hostile;
        hostile["m_fadeSpeed"] = 99.0f;
        hostile["m_tiltDegrees"] = -500.0f;
        hostile["m_indicatorOpacity"] = 5.0f;
        hostile["m_holdStyle"] = std::string("9");
        SwordBlockingModule clamped;
        clamped.onInit();
        clamped.loadConfig(hostile);
        check(clamped.m_fadeSpeed <= maxFadeSpeed, "an oversized fade speed is clamped");
        check(clamped.m_tiltDegrees >= 0.0f, "a negative tilt is clamped");
        check(clamped.m_indicatorOpacity <= 1.0f, "an oversized opacity is clamped");
        check(clamped.m_holdStyle >= 0 && clamped.m_holdStyle < static_cast<int>(HoldMode::Count),
              "an out-of-range hold style is clamped");

        // Key events feed the hold source.
        SwordBlockingModule keyModule;
        keyModule.onInit();
        keyModule.setMasterEnabled(true);
        keyModule.m_holdKeybind = 71;
        check(keyModule.onKeyEvent(71, true), "the hold key is consumed while it is down");
        g_carriedStack = nullptr;
        check(!keyModule.onKeyEvent(72, true), "other keys pass through");
    }

    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all sword blocking checks passed\n");
    return 0;
}
