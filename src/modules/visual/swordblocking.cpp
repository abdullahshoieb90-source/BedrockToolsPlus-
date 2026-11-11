#include "swordblocking.hpp"
#include "swordblocking_thirdperson.hpp"

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

using bedrocktools::sdk::Vec3;

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

// Bedrock's MaterialPtr is two pointers, and RenderMaterialGroup::getMaterial
// takes a HashedString by const reference. The layouts here mirror the ones
// blockoutline.cpp and wings.cpp use for the same two game types.
struct MaterialPtr {
    void* data[2]{nullptr, nullptr};
    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr& other) {
        data[0] = other.data[0];
        data[1] = other.data[1];
        return *this;
    }
    explicit operator bool() const { return data[0] != nullptr; }
};

class HashedString {
public:
    std::uint64_t hash = 0;
    std::string value;
    mutable const HashedString* lastMatch = nullptr;

    explicit HashedString(const char* text) : value(text ? text : "") {
        if (value.empty()) return;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        hash = kOffset;
        for (char ch : value) {
            hash = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        }
    }
};

struct RenderMaterialGroup {
    std::uintptr_t address = 0;
};

RenderMaterialGroup s_renderMaterialGroup;

std::uintptr_t resolveAdrp(std::uint32_t* instructions, std::size_t count, std::uint32_t targetRegister) {
    if (!instructions) return 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t instruction = instructions[i];
        if ((instruction & 0x1F) != targetRegister) continue;
        if ((instruction & 0x9F000000) == 0x90000000) {
            const std::uint64_t immediateBits =
                ((static_cast<std::uint64_t>((instruction >> 3) & 0x1FFFFC)) |
                 (static_cast<std::uint64_t>((instruction >> 29) & 3))) << 43;
            const auto immediate = static_cast<std::int64_t>(immediateBits) >> 31;
            const std::uintptr_t page =
                (reinterpret_cast<std::uintptr_t>(&instructions[i]) & ~0xFFFULL) + immediate;
            for (std::size_t j = i + 1; j < count; ++j) {
                const std::uint32_t add = instructions[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetRegister &&
                    (add & 0x1F) == targetRegister) {
                    std::uint32_t immediate12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) immediate12 <<= 12;
                    return page + immediate12;
                }
                if ((add & 0x1F) == targetRegister) break;
            }
        }
    }
    return 0;
}

MaterialPtr s_selectionMaterial;

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
    const sb::Mat4 local = sb::posedGuardMatrix(g_swordBlocking->poseParams(),
                                                g_swordBlocking->guardRollDegrees(), weight);
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

// ---------------------------------------------------------------------------
// Third-person guard
//
// The game draws the carried item as part of the player model, through the
// actor renderer; none of the ItemInHandRenderer hooks above run for it, so
// there is no item matrix to pose. The guard is drawn instead: the sword sprite
// on two flat quads at the player's right hand, in world space, depth tested,
// from the RenderLevel pass. That is a representation of the block (the vanilla
// held item keeps drawing underneath), not a second weapon.
// ---------------------------------------------------------------------------

using TessellatorBeginFn = void (*)(void*, void*, int, int, int);
using TessellatorColorFn = void (*)(void*, float, float, float, float);
using TessellatorVertexFn = void (*)(void*, float, float, float);
using RenderMeshImmediatelyFn = void (*)(void*, void*, void*, char*);
using RenderLevelFn = void (*)(void*, void*, void*);

TessellatorBeginFn s_tessBegin = nullptr;
TessellatorColorFn s_tessColor = nullptr;
TessellatorVertexFn s_tessVertex = nullptr;
RenderMeshImmediatelyFn s_renderMesh = nullptr;
RenderLevelFn s_renderLevelOriginal = nullptr;
bedrocktools::hooks::Handle s_renderLevelHook = nullptr;

// Wall clock of the last player tick, used to interpolate the guard between
// ticks so it does not step at 20 Hz while the camera runs at 60+.
std::atomic<int64_t> s_lastTickUs{0};
std::atomic<int64_t> s_tickIntervalUs{50000};

// Where the player was on the last two ticks, for the interpolation above.
std::mutex s_bodyMutex;
sb::Vec3 s_bodyNow{};
sb::Vec3 s_bodyPrevious{};
float s_bodyYaw = 0.0f;
float s_bodyPreviousYaw = 0.0f;
bool s_bodyValid = false;

// parseColor lives further down with the other colour helpers; the
// third-person renderer needs it before its definition.
std::uint32_t parseColor(const std::string& value, std::uint32_t fallback);

// Reads the position and body yaw of `player` and pushes them into the history.
void sampleBody(void* player) {
    if (!player) return;
    const auto address = reinterpret_cast<std::uintptr_t>(player);
    if (address < 0x1000) return;

    const std::uintptr_t stateVector = read<std::uintptr_t>(player, Actor::mStateVectorComponent);
    if (stateVector < 0x1000) return;
    const auto position = read<Vec3>(reinterpret_cast<void*>(stateVector),
                                     StateVectorComponent::mPosition);
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return;

    float yaw = 0.0f;
    const std::uintptr_t rotation = read<std::uintptr_t>(player, Actor::mActorRotationComponent);
    if (rotation >= 0x1000) {
        const auto bodyRotation = read<bedrocktools::sdk::Vec2>(reinterpret_cast<void*>(rotation), 0);
        if (std::isfinite(bodyRotation.y)) yaw = bodyRotation.y;
    }

    const auto now = std::chrono::steady_clock::now();
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              now.time_since_epoch()).count();

    std::lock_guard lock(s_bodyMutex);
    const int64_t previousTickUs = s_lastTickUs.load(std::memory_order_relaxed);
    if (previousTickUs > 0) {
        const int64_t interval = nowUs - previousTickUs;
        // Only trust an interval that looks like a tick; a pause or a dimension
        // change would otherwise stretch the interpolation window.
        if (interval > 5000 && interval < 200000) {
            s_tickIntervalUs.store(interval, std::memory_order_relaxed);
        }
    }
    s_lastTickUs.store(nowUs, std::memory_order_relaxed);

    s_bodyPrevious = s_bodyValid ? s_bodyNow : position;
    s_bodyPreviousYaw = s_bodyValid ? s_bodyYaw : yaw;
    s_bodyNow = position;
    s_bodyYaw = yaw;
    s_bodyValid = true;
}

// The body as the renderer should draw it right now: tick position plus the
// fraction of a tick that has elapsed since, clamped so a hitch holds still.
bool currentBody(sb::BodyFrame& frame) {
    int64_t lastTickUs = 0;
    float yawNow = 0.0f;
    sb::Vec3 now{};
    sb::Vec3 previous{};
    float yawPrevious = 0.0f;
    bool valid = false;
    {
        std::lock_guard lock(s_bodyMutex);
        lastTickUs = s_lastTickUs.load(std::memory_order_relaxed);
        yawNow = s_bodyYaw;
        now = s_bodyNow;
        previous = s_bodyPrevious;
        yawPrevious = s_bodyPreviousYaw;
        valid = s_bodyValid;
    }
    if (!valid || lastTickUs <= 0) return false;

    const auto clock = std::chrono::steady_clock::now();
    const int64_t clockUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                clock.time_since_epoch()).count();
    const int64_t elapsedUs = std::clamp<int64_t>(clockUs - lastTickUs, 0, 200000);
    const float interval = static_cast<float>(std::max<int64_t>(s_tickIntervalUs.load(std::memory_order_relaxed), 1000));
    const float alpha = std::clamp(static_cast<float>(elapsedUs) / interval, 0.0f, 1.0f);

    frame.origin = Vec3{previous.x + (now.x - previous.x) * alpha,
                        previous.y + (now.y - previous.y) * alpha,
                        previous.z + (now.z - previous.z) * alpha};
    // Yaw wraps, so interpolate across the short way round.
    float delta = yawNow - yawPrevious;
    while (delta > 180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    frame.yawDegrees = yawPrevious + delta * alpha;
    return std::isfinite(frame.origin.x) && std::isfinite(frame.origin.y) &&
           std::isfinite(frame.origin.z) && std::isfinite(frame.yawDegrees);
}

// The material group is resolved here rather than in onInit: it has to decode
// the ADRP/ADD pair at the signature, which means reading the game's code, and
// the first third-person draw is the first moment the answer is actually
// needed. Everything else about the guard works without it.
void ensureMaterial() {
    if (!s_renderMaterialGroup.address) {
        const auto materialGroup = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
        // A resolved address has to look like code before its instructions are
        // decoded; anything smaller is a stub or a test double.
        if (materialGroup >= 0x10000) {
            const std::uintptr_t groupAddress = resolveAdrp(
                reinterpret_cast<std::uint32_t*>(materialGroup), 2, 0);
            if (groupAddress) {
                s_renderMaterialGroup.address = groupAddress + MaterialGroup::mRenderMaterialGroupOffset;
            }
        }
    }
    if (s_selectionMaterial || !s_renderMaterialGroup.address) return;
    auto** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup.address);
    if (!vtable || !vtable[VTable::RenderMaterialGroup_getMaterial]) return;
    HashedString name("selection_box");
    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    MaterialPtr material = reinterpret_cast<GetMaterialFn>(
        vtable[VTable::RenderMaterialGroup_getMaterial])(reinterpret_cast<void*>(s_renderMaterialGroup.address), &name);
    s_selectionMaterial = material;
}

void renderThirdPersonGuard(void* levelRenderer, void* screenContext) {
    auto* module = g_swordBlocking;
    if (!module || !module->enabled || !module->m_thirdPersonPose) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;

    const float weight = module->poseWeight();
    if (weight <= sb::activeThreshold) return;
    if (!module->m_thirdPerson.load(std::memory_order_relaxed)) return;

    if (!levelRenderer || reinterpret_cast<std::uintptr_t>(levelRenderer) < 0x1000 ||
        !screenContext || reinterpret_cast<std::uintptr_t>(screenContext) < 0x1000) {
        return;
    }

    const std::uintptr_t tessellatorAddress = read<std::uintptr_t>(screenContext, ScreenContext::mTessellator);
    if (tessellatorAddress < 0x1000) return;
    void* tessellator = reinterpret_cast<void*>(tessellatorAddress);

    const auto rendererAddress = reinterpret_cast<std::uintptr_t>(levelRenderer);
    const std::uintptr_t playerRenderer = read<std::uintptr_t>(levelRenderer, LevelRenderer::mLevelRendererPlayer);
    if (playerRenderer < 0x1000) return;
    const auto camera = read<Vec3>(reinterpret_cast<void*>(playerRenderer), LevelRendererPlayer::mCamPos);

    ensureMaterial();
    if (!s_selectionMaterial) return;

    sb::BodyFrame body{};
    if (!currentBody(body)) return;

    sb::HandOffset offset;
    offset.right = module->m_thirdPersonRight;
    offset.up = module->m_thirdPersonUp;
    offset.forward = module->m_thirdPersonForward;

    sb::BladeOrientation orientation;
    orientation.angleDegrees = module->m_thirdPersonAngle;
    orientation.leanDegrees = module->m_thirdPersonLean;

    sb::BladeGeometry geometry;
    geometry.length = module->m_thirdPersonLength;
    geometry.width = module->m_thirdPersonWidth;
    sb::BladeStyle style;

    const auto quads = sb::guardQuads(body, offset, orientation, geometry, style, weight);
    if (quads.empty()) return;

    const std::uint32_t rgb = parseColor(module->m_indicatorColor, 0xFF7FE3FFu) & 0x00FFFFFFu;

    s_tessBegin(tessellator, nullptr, 1, static_cast<int>(sb::quadVertexCount(quads)), 0);
    // The item sprite is a flat plate read against the light; a pale tint keeps
    // it readable against grass and stone without looking like a light source.
    s_tessColor(tessellator,
                static_cast<float>((rgb >> 16) & 0xFF) / 255.0f * 0.55f + 0.35f,
                static_cast<float>((rgb >> 8) & 0xFF) / 255.0f * 0.55f + 0.35f,
                static_cast<float>(rgb & 0xFF) / 255.0f * 0.55f + 0.35f,
                std::clamp(0.55f + weight * 0.35f, 0.0f, 1.0f));

    for (const auto& quad : quads) {
        for (const auto& point : quad.points) {
            s_tessVertex(tessellator, point.x - camera.x, point.y - camera.y, point.z - camera.z);
        }
    }

    char pad[0x58]{};
    s_renderMesh(screenContext, tessellator, static_cast<void*>(&s_selectionMaterial), pad);
}

void renderLevelDetour(void* levelRenderer, void* screenContext, void* renderParams) {
    if (s_renderLevelOriginal) s_renderLevelOriginal(levelRenderer, screenContext, renderParams);
    renderThirdPersonGuard(levelRenderer, screenContext);
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
    bedrocktools::hooks::remove(s_renderLevelHook);
    s_renderLevelHook = nullptr;
    s_renderLevelOriginal = nullptr;
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

    // Third-person guard: the world-space pass. RenderLevel is only hooked when
    // the option is on, so the common (first-person only) case adds no work to
    // the render thread.
    if (m_thirdPersonPose) {
        hookSignature(bedrocktools::memory::SignatureId::RenderLevel,
                      reinterpret_cast<void*>(&renderLevelDetour),
                      reinterpret_cast<void**>(&s_renderLevelOriginal),
                      s_renderLevelHook);
    }

    if (!s_tessBegin) {
        const auto begin = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
        if (begin) s_tessBegin = reinterpret_cast<TessellatorBeginFn>(begin);
    }
    if (!s_tessColor) {
        const auto color = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
        if (color) s_tessColor = reinterpret_cast<TessellatorColorFn>(color);
    }
    if (!s_tessVertex) {
        const auto vertex = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
        if (vertex) s_tessVertex = reinterpret_cast<TessellatorVertexFn>(vertex);
    }
    if (!s_renderMesh) {
        auto mesh = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
        if (!mesh) mesh = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (mesh) s_renderMesh = reinterpret_cast<RenderMeshImmediatelyFn>(mesh);
    }

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

void SwordBlockingModule::applyPosePreset(swordblocking::PosePreset preset) {
    if (preset == swordblocking::PosePreset::Custom ||
        preset == swordblocking::PosePreset::Count) {
        return;
    }
    const swordblocking::PoseParams pose = swordblocking::presetPose(preset);
    m_tiltDegrees = pose.tiltDegrees;
    m_pitchDegrees = pose.pitchDegrees;
    m_spinDegrees = pose.spinDegrees;
    m_lift = pose.lift;
    m_across = pose.across;
    m_forward = pose.forward;
    m_poseScale = pose.scale;
}

bool SwordBlockingModule::poseMatchesPreset() const {
    if (m_posePreset < 0 || m_posePreset >= static_cast<int>(swordblocking::PosePreset::Count)) return false;
    const auto preset = static_cast<swordblocking::PosePreset>(m_posePreset);
    if (preset == swordblocking::PosePreset::Custom) return true;
    const swordblocking::PoseParams pose = swordblocking::presetPose(preset);
    constexpr float tolerance = 1e-3f;
    return std::fabs(m_tiltDegrees - pose.tiltDegrees) < tolerance &&
           std::fabs(m_pitchDegrees - pose.pitchDegrees) < tolerance &&
           std::fabs(m_spinDegrees - pose.spinDegrees) < tolerance &&
           std::fabs(m_lift - pose.lift) < tolerance &&
           std::fabs(m_across - pose.across) < tolerance &&
           std::fabs(m_forward - pose.forward) < tolerance &&
           std::fabs(m_poseScale - pose.scale) < tolerance;
}

void SwordBlockingModule::applyGuardDirection(swordblocking::GuardDirection direction) {
    if (direction == swordblocking::GuardDirection::Custom ||
        direction == swordblocking::GuardDirection::Count) {
        return;
    }
    const auto pose = swordblocking::directionPose(direction);
    m_tiltDegrees = pose.tiltDegrees;
    m_lift = pose.lift;
}

swordblocking::GuardDirection SwordBlockingModule::currentGuardDirection() const {
    return swordblocking::directionFromPose(m_tiltDegrees, m_lift);
}

bool SwordBlockingModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    // The preset radio is the one setting the generic config path cannot apply,
    // because picking a preset has to write the eight pose values. Handling it
    // here (instead of in loadConfig) also means a slider drag is afterwards
    // able to move the radio to Custom, which is what the menu then shows.
    if (key == "m_guardDirection") {
        // A direction only owns Tilt and Lift, so choosing one leaves the three
        // rotations alone: that is what makes "direction + rotation" compose.
        m_guardDirection = swordblocking::guardDirectionIndex(std::string(value));
        const auto direction = static_cast<swordblocking::GuardDirection>(m_guardDirection);
        if (direction != swordblocking::GuardDirection::Custom) {
            applyGuardDirection(direction);
            // The pose is no longer the preset's, but the preset radio is only
            // rewritten on save; the sliders are what matter here.
            if (!poseMatchesPreset()) {
                m_posePreset = static_cast<int>(swordblocking::PosePreset::Custom);
            }
        }
        return true;
    }

    if (key != "m_posePreset") return false;

    m_posePreset = swordblocking::posePresetIndex(std::string(value));
    const auto preset = static_cast<swordblocking::PosePreset>(m_posePreset);
    if (preset != swordblocking::PosePreset::Custom) {
        applyPosePreset(preset);
        // A preset carries its own tilt, so the direction radio has to match.
        m_guardDirection = static_cast<int>(currentGuardDirection());
    }
    return true;
}

swordblocking::PoseParams SwordBlockingModule::poseParams() const {
    swordblocking::PoseParams pose;
    pose.tiltDegrees = m_tiltDegrees;
    pose.pitchDegrees = m_pitchDegrees;
    pose.spinDegrees = m_spinDegrees;
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
    // The third-person guard is placed from the body transform, so it is
    // sampled every tick even while the guard is down: interpolation from a
    // cold history would make it snap into place.
    if (m_thirdPersonPose) sampleBody(player);

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

    if (j.contains("m_posePreset") && j["m_posePreset"].is_string()) {
        m_posePreset = swordblocking::posePresetIndex(j["m_posePreset"].get<std::string>());
    }
    if (j.contains("m_guardDirection") && j["m_guardDirection"].is_string()) {
        m_guardDirection = swordblocking::guardDirectionIndex(j["m_guardDirection"].get<std::string>());
    }

    readFloat("m_fadeSpeed", m_fadeSpeed, swordblocking::minFadeSpeed, swordblocking::maxFadeSpeed);
    readFloat("m_tiltDegrees", m_tiltDegrees, -120.0f, 120.0f);
    readFloat("m_pitchDegrees", m_pitchDegrees, -120.0f, 120.0f);
    readFloat("m_spinDegrees", m_spinDegrees, -180.0f, 180.0f);
    readFloat("m_lift", m_lift, -16.0f, 16.0f);
    readFloat("m_across", m_across, -16.0f, 16.0f);
    readFloat("m_forward", m_forward, -16.0f, 16.0f);
    readFloat("m_poseScale", m_poseScale, 0.25f, 4.0f);
    readFloat("m_guardRotationDegrees", m_guardRotationDegrees, -180.0f, 180.0f);

    // Third-person blade.
    readFloat("m_thirdPersonRight", m_thirdPersonRight, -16.0f, 16.0f);
    readFloat("m_thirdPersonUp", m_thirdPersonUp, -16.0f, 32.0f);
    readFloat("m_thirdPersonForward", m_thirdPersonForward, -16.0f, 16.0f);
    readFloat("m_thirdPersonAngle", m_thirdPersonAngle, -180.0f, 180.0f);
    readFloat("m_thirdPersonLean", m_thirdPersonLean, -90.0f, 90.0f);
    readFloat("m_thirdPersonLength", m_thirdPersonLength, 0.2f, 2.0f);
    readFloat("m_thirdPersonWidth", m_thirdPersonWidth, 0.02f, 0.6f);
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
    if (j.contains("m_thirdPersonPose")) {
        try { m_thirdPersonPose = j["m_thirdPersonPose"].get<bool>(); } catch (...) {}
    }

    m_holdStyle = std::clamp(m_holdStyle, 0, static_cast<int>(swordblocking::HoldMode::Count) - 1);
    m_fadeSpeed = swordblocking::clampFadeSpeed(m_fadeSpeed);

    // A preset owns the pose while it is selected: adopting it on load means the
    // stored preset is what the player actually sees, even if the pose values in
    // the file were written by an older build (or edited by hand). Custom keeps
    // the values exactly as they were read.
    const auto preset = static_cast<swordblocking::PosePreset>(
        std::clamp(m_posePreset, 0, static_cast<int>(swordblocking::PosePreset::Count) - 1));
    m_posePreset = static_cast<int>(preset);
    if (preset != swordblocking::PosePreset::Custom) {
        applyPosePreset(preset);
    } else if (!j.contains("m_spinDegrees")) {
        // A "Custom" pose saved before the spin slider existed only carries the
        // old tilt/pitch pair, and the old defaults sat further right. Rather
        // than leave such a config looking wrong forever, fall back to the
        // current defaults; a config that has the new keys is left untouched.
        m_tiltDegrees = m_defaultTiltDegrees;
        m_pitchDegrees = m_defaultPitchDegrees;
        m_spinDegrees = m_defaultSpinDegrees;
        m_lift = m_defaultLift;
        m_across = m_defaultAcross;
        m_forward = m_defaultForward;
        m_poseScale = m_defaultPoseScale;
        m_posePreset = static_cast<int>(swordblocking::PosePreset::Classic);
    }

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

    // Keep the radio truthful: once a pose slider no longer holds the preset's
    // values, the selection becomes Custom so the next load leaves the tuned
    // pose alone instead of snapping it back to the preset.
    if (!poseMatchesPreset()) m_posePreset = static_cast<int>(swordblocking::PosePreset::Custom);
    // Keep the radios truthful the same way the preset radio is kept: a slider
    // that moved the guard off a named direction switches the radio to Custom.
    if (currentGuardDirection() == swordblocking::GuardDirection::Custom) {
        m_guardDirection = static_cast<int>(swordblocking::GuardDirection::Custom);
    } else {
        m_guardDirection = static_cast<int>(currentGuardDirection());
    }
    j["m_posePreset"] = swordblocking::posePresetRadioValue(m_posePreset);
    j["m_guardDirection"] = swordblocking::guardDirectionRadioValue(m_guardDirection);
    j["m_tiltDegrees"] = std::clamp(m_tiltDegrees, -120.0f, 120.0f);
    j["m_pitchDegrees"] = std::clamp(m_pitchDegrees, -120.0f, 120.0f);
    j["m_spinDegrees"] = std::clamp(m_spinDegrees, -180.0f, 180.0f);
    j["m_lift"] = std::clamp(m_lift, -16.0f, 16.0f);
    j["m_across"] = std::clamp(m_across, -16.0f, 16.0f);
    j["m_forward"] = std::clamp(m_forward, -16.0f, 16.0f);
    j["m_poseScale"] = std::clamp(m_poseScale, 0.25f, 4.0f);
    j["m_guardRotationDegrees"] = std::clamp(m_guardRotationDegrees, -180.0f, 180.0f);

    j["m_thirdPersonRight"] = std::clamp(m_thirdPersonRight, -16.0f, 16.0f);
    j["m_thirdPersonUp"] = std::clamp(m_thirdPersonUp, -16.0f, 32.0f);
    j["m_thirdPersonForward"] = std::clamp(m_thirdPersonForward, -16.0f, 16.0f);
    j["m_thirdPersonAngle"] = std::clamp(m_thirdPersonAngle, -180.0f, 180.0f);
    j["m_thirdPersonLean"] = std::clamp(m_thirdPersonLean, -90.0f, 90.0f);
    j["m_thirdPersonLength"] = std::clamp(m_thirdPersonLength, 0.2f, 2.0f);
    j["m_thirdPersonWidth"] = std::clamp(m_thirdPersonWidth, 0.02f, 0.6f);

    j["m_indicator"] = m_indicator;
    j["m_indicatorColor"] = colorString(parseColor(m_indicatorColor, 0xFF7FE3FFu));
    j["m_indicatorOpacity"] = std::clamp(m_indicatorOpacity, 0.0f, 1.0f);
    j["m_indicatorScale"] = std::clamp(m_indicatorScale, 0.25f, 4.0f);
    j["m_showThirdPerson"] = m_showThirdPerson;
    j["m_thirdPersonPose"] = m_thirdPersonPose;
}
