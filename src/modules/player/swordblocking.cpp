#include "swordblocking.hpp"

#include "core/RenderState.hpp"
#include "core/memory/Hooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

using namespace bedrocktools::sdk;
using namespace bedrocktools::sdk::offsets;

// ---------------------------------------------------------------------------
// Tessellator / material plumbing (same shape as the Wings module: the game
// hands us a vertex buffer, we feed colours and positions in camera space,
// then MeshHelpers flushes it with a material.)
// ---------------------------------------------------------------------------

using Tessellator_begin_t = void (*)(void*, void*, int, int, int);
using Tessellator_color_t = void (*)(void*, float, float, float, float);
using Tessellator_vertex_t = void (*)(void*, float, float, float);
using MeshHelpers_renderMeshImmediately_t = void (*)(void*, void*, void*, char*);
using RenderLevel_t = void (*)(void*, void*, void*);

// FNV-1a, matching the game's HashedString constructor.
struct HashedString {
    std::uint64_t hash = 0;
    std::string value;
    mutable const HashedString* lastMatch = nullptr;

    explicit HashedString(const char* name) : value(name ? name : "") {
        hash = 0xCBF29CE484222325ULL;
        for (char c : value) {
            hash = static_cast<std::uint64_t>(static_cast<unsigned char>(c)) ^ (0x100000001B3ULL * hash);
        }
    }
};

// Non-owning mirror of the game-owned MaterialPtr. The non-trivial destructor
// is required because the game returns it in registers under the ARM64 ABI.
struct MaterialPtr {
    void* data[2]{nullptr, nullptr};
    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;
    MaterialPtr(MaterialPtr&& other) noexcept : data{other.data[0], other.data[1]} {
        other.data[0] = other.data[1] = nullptr;
    }
    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        data[0] = other.data[0];
        data[1] = other.data[1];
        other.data[0] = other.data[1] = nullptr;
        return *this;
    }
    ~MaterialPtr() {}
    explicit operator bool() const { return data[0] != nullptr; }
};

// Resolves an ADRP/ADD pair so the static material group pointer can be
// recovered from RenderMaterialGroup::common without another signature
// (same trick the Wings and Hitbox modules use).
std::uintptr_t resolveADRP(std::uint32_t* insns, std::size_t count, std::uint32_t targetReg) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            const std::uintptr_t page = (reinterpret_cast<std::uintptr_t>(&insns[i]) & ~0xFFFULL) +
                ((static_cast<std::int64_t>(
                    static_cast<std::uint64_t>((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31));
            for (std::size_t j = i + 1; j < count; ++j) {
                const std::uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 && ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    std::uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            const std::int64_t imm =
                static_cast<std::int64_t>(static_cast<std::uint64_t>((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return reinterpret_cast<std::uintptr_t>(&insns[i]) + imm;
        }
    }
    return 0;
}

SwordBlockingModule* g_module = nullptr;

Tessellator_begin_t s_tessBegin = nullptr;
Tessellator_color_t s_tessColor = nullptr;
Tessellator_vertex_t s_tessVertex = nullptr;
MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;
RenderLevel_t s_renderLevelOrig = nullptr;
std::uintptr_t s_renderMaterialGroup = 0;
MaterialPtr s_material;

// The local player pointer, written on the tick thread and read on the render
// thread (the Wings module's s_localPlayerPtr pattern). The render hook must
// not resolve the local player itself: ClientInstance::localPlayer() pays a
// dlopen on every call.
void* s_localPlayer = nullptr;

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Rgb {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

// A rigid transform in world space; the pose boxes are expressed in it.
struct Transform {
    Vec3 origin;
    Vec3 right;   // local +X
    Vec3 up;      // local +Y (the blade direction)
    Vec3 forward; // local +Z
};

Vec3 applyTransform(const Transform& t, float x, float y, float z) {
    return Vec3{
        t.origin.x + t.right.x * x + t.up.x * y + t.forward.x * z,
        t.origin.y + t.right.y * x + t.up.y * y + t.forward.y * z,
        t.origin.z + t.right.z * x + t.up.z * y + t.forward.z * z};
}

struct Box {
    Vec3 min;
    Vec3 max;
    Rgb color;
};

Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 normalize(const Vec3& v) {
    const float length = std::sqrt(dot(v, v));
    if (length < 1e-6f) return Vec3{0.0f, 1.0f, 0.0f};
    return Vec3{v.x / length, v.y / length, v.z / length};
}

// Appends one box as tessellator quads. Both windings are emitted so no
// culling state can hide the blade. Returns the number of vertices written
// (6 faces * 4 corners * 2 windings).
int emitBox(void* tess, const Transform& t, const Box& box, const Vec3& camera, Rgb shade) {
    const float x0 = box.min.x, x1 = box.max.x;
    const float y0 = box.min.y, y1 = box.max.y;
    const float z0 = box.min.z, z1 = box.max.z;

    // 8 corners in transform space.
    const Vec3 corners[8] = {
        applyTransform(t, x0, y0, z0), applyTransform(t, x1, y0, z0),
        applyTransform(t, x1, y0, z1), applyTransform(t, x0, y0, z1),
        applyTransform(t, x0, y1, z0), applyTransform(t, x1, y1, z0),
        applyTransform(t, x1, y1, z1), applyTransform(t, x0, y1, z1),
    };

    // Face corner indices, ordered counter-clockwise.
    const int faces[6][4] = {
        {0, 3, 2, 1}, // bottom (-Y)
        {4, 5, 6, 7}, // top (+Y)
        {0, 1, 5, 4}, // -Z
        {3, 7, 6, 2}, // +Z
        {0, 4, 7, 3}, // -X
        {1, 2, 6, 5}, // +X
    };

    const float r = box.color.r * shade.r;
    const float g = box.color.g * shade.g;
    const float b = box.color.b * shade.b;

    for (const auto& face : faces) {
        for (int pass = 0; pass < 2; ++pass) {
            for (int k = 0; k < 4; ++k) {
                const int idx = pass == 0 ? face[k] : face[3 - k];
                const Vec3& v = corners[idx];
                s_tessColor(tess, r, g, b, 1.0f);
                // Camera-space position, the convention the game's
                // Tessellator expects for these overlay passes.
                s_tessVertex(tess, v.x - camera.x, v.y - camera.y, v.z - camera.z);
            }
        }
    }
    return 6 * 4 * 2;
}

// ---------------------------------------------------------------------------
// Colour / config helpers
// ---------------------------------------------------------------------------

Rgb unpackRgb(std::uint32_t value) {
    return Rgb{
        static_cast<float>((value >> 16) & 0xFF) / 255.0f,
        static_cast<float>((value >> 8) & 0xFF) / 255.0f,
        static_cast<float>(value & 0xFF) / 255.0f};
}

// The launcher stores a radio as "current,option0,option1,...". Only the part
// before the first comma is the live value, so it is split off before
// matching; tolerating the bare name keeps hand-edited configs working.
int parseTrigger(const std::string& raw) {
    std::string value = raw.substr(0, raw.find(','));
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();

    if (value == "Always" || value == "1") return 1;
    if (value == "Toggle" || value == "2") return 2;
    return 0;
}

// ---------------------------------------------------------------------------
// Actor reads (same layout the Wings/Hitbox modules use)
// ---------------------------------------------------------------------------

struct ActorBox {
    Vec3 min;
    Vec3 max;
};

ActorBox getActorBox(void* actor) {
    ActorBox box{};
    const auto address = reinterpret_cast<std::uintptr_t>(actor);
    if (address < 0x1000) return box;
    const auto aabbComponent =
        *reinterpret_cast<std::uintptr_t*>(address + Actor::mStateVectorComponent +
                                          BuiltInActorComponents::mAABBShapeComponent);
    if (aabbComponent < 0x1000) return box;
    box = *reinterpret_cast<ActorBox*>(aabbComponent + AABBShapeComponent::mAABB);
    return box;
}

Vec2 getBodyRotation(void* actor) {
    const auto address = reinterpret_cast<std::uintptr_t>(actor);
    if (address < 0x1000) return Vec2{0.0f, 0.0f};
    const auto rotationComponent = *reinterpret_cast<std::uintptr_t*>(address + Actor::mActorRotationComponent);
    if (rotationComponent < 0x1000) return Vec2{0.0f, 0.0f};
    return *reinterpret_cast<Vec2*>(rotationComponent);
}

std::uint8_t readU8(const std::byte* object, std::size_t offset) {
    return *reinterpret_cast<const std::uint8_t*>(object + offset);
}

const void* readPtr(const std::byte* object, std::size_t offset) {
    return *reinterpret_cast<const void* const*>(object + offset);
}

// Player::getCarriedItem via the vtable slot the HUD modules already rely on
// (same call shape as modules/hud/huditems.cpp).
void* getCarriedItem(void* player) {
    if (reinterpret_cast<std::uintptr_t>(player) < 0x1000) return nullptr;
    void** vtable = *reinterpret_cast<void***>(player);
    if (!vtable || !vtable[VTable::PlayerGetCarriedItem]) return nullptr;
    using GetCarriedItemFn = void* (*)(void*);
    return reinterpret_cast<GetCarriedItemFn>(vtable[VTable::PlayerGetCarriedItem])(player);
}

// ItemStackBase -> ItemCounter -> Item, then Item::getDescriptionId() through
// its vtable (the same double indirection core/InventoryAccess.cpp uses).
// Returns an empty view when the stack or item is missing.
std::string_view itemIdentifier(const void* stack) {
    if (!stack) return {};
    const auto stackBytes = static_cast<const std::byte*>(stack);
    if (readU8(stackBytes, Inventory::ItemStackValid) == 0) return {};
    if (readU8(stackBytes, Inventory::ItemStackCount) == 0) return {};
    const void* counter = readPtr(stackBytes, ShulkerPreview::ItemStackBaseItem);
    if (!counter) return {};
    const void* item = readPtr(static_cast<const std::byte*>(counter), ShulkerPreview::SharedCounterPointer);
    if (!item) return {};
    const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(item);
    if (!vtable || !vtable[Inventory::ItemGetDescriptionIdVtableIndex]) return {};
    using GetDescriptionIdFn = const std::string& (*)(const void*);
    return reinterpret_cast<GetDescriptionIdFn>(vtable[Inventory::ItemGetDescriptionIdVtableIndex])(item);
}

// True for every vanilla sword, plus modded items whose identifier ends in
// "_sword" (accepted only with Any Sword on).
bool isSwordItem(const void* stack, bool anySword) {
    const std::string_view id = itemIdentifier(stack);
    if (id.empty()) return false;
    if (!anySword) {
        return id == "minecraft:iron_sword" || id == "minecraft:diamond_sword" ||
               id == "minecraft:netherite_sword";
    }
    if (id.size() >= 6 && id.compare(id.size() - 6, 6, "_sword") == 0) return true;
    return id.find(":sword") != std::string_view::npos;
}

MaterialPtr requestMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};
    HashedString hashed(name);
    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[VTable::RenderMaterialGroup_getMaterial]) return {};
    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    return reinterpret_cast<GetMaterialFn>(vtable[VTable::RenderMaterialGroup_getMaterial])(
        reinterpret_cast<void*>(s_renderMaterialGroup), &hashed);
}

void ensureMaterial() {
    if (s_material || !s_renderMaterialGroup) return;
    // All three names are already used by the other overlay modules:
    // selection_box is the vanilla selection material (depth-tested,
    // vertex-coloured, no blending) - the closest match for a solid pose.
    static const char* kCandidates[] = {"selection_box", "debug_filled_box", "ui_fill_color"};
    for (const char* name : kCandidates) {
        s_material = requestMaterial(name);
        if (s_material) break;
    }
}

// ---------------------------------------------------------------------------
// The pose itself
// ---------------------------------------------------------------------------
//
// Player model space is 16 units per block with the origin at the feet. The
// blade is held in front of the chest angled up and across - the silhouette
// of the 1.7 block pose. Everything is scaled by m_scale so the whole
// assembly can be shrunk or enlarged from the menu.

int buildPose(void* tess, const Vec3& camera, const Transform& body, Rgb blade, Rgb guard, bool withArm) {
    int vertices = 0;

    // Blade: 2 units wide, 1 deep, 15 long, rising from the grip.
    Box bladeBox;
    bladeBox.min = Vec3{-1.0f, 0.0f, -0.5f};
    bladeBox.max = Vec3{1.0f, 15.0f, 0.5f};
    bladeBox.color = blade;
    vertices += emitBox(tess, body, bladeBox, camera, Rgb{0.86f, 0.90f, 1.0f});

    // Crossguard: 7 wide, 1.5 tall, 1.2 deep at the base of the blade.
    Box guardBox;
    guardBox.min = Vec3{-3.5f, -1.5f, -0.6f};
    guardBox.max = Vec3{3.5f, 0.0f, 0.6f};
    guardBox.color = guard;
    vertices += emitBox(tess, body, guardBox, camera, Rgb{1.0f, 1.0f, 1.0f});

    // Grip + pommel below the crossguard.
    Box gripBox;
    gripBox.min = Vec3{-0.8f, -5.5f, -0.5f};
    gripBox.max = Vec3{0.8f, -1.5f, 0.5f};
    gripBox.color = Rgb{0.42f, 0.27f, 0.14f};
    vertices += emitBox(tess, body, gripBox, camera, Rgb{1.0f, 1.0f, 1.0f});

    if (withArm) {
        // Brace arm: the forearm that holds the sword across the chest, kept
        // as thin slabs so it reads as an arm without pretending to be the
        // real mesh.
        vertices += emitBox(tess, body, Box{Vec3{-2.6f, -5.1f, 0.2f}, Vec3{2.6f, 0.1f, 3.0f},
                                           Rgb{0.78f, 0.60f, 0.47f}},
                            camera, Rgb{1.0f, 1.0f, 1.0f});
        vertices += emitBox(tess, body, Box{Vec3{-2.2f, -8.2f, 2.2f}, Vec3{2.2f, -3.8f, 4.6f},
                                           Rgb{0.72f, 0.55f, 0.43f}},
                            camera, Rgb{1.0f, 1.0f, 1.0f});
    }

    return vertices;
}

// ---------------------------------------------------------------------------
// Render hook
// ---------------------------------------------------------------------------

void renderPose(void* levelRenderer, void* screenContext) {
    if (!g_module || !g_module->enabled || !g_module->poseActive()) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;

    const auto player = s_localPlayer;
    if (reinterpret_cast<std::uintptr_t>(player) < 0x1000) return;

    const ActorBox box = getActorBox(player);
    const Vec2 rotation = getBodyRotation(player);
    if (!std::isfinite(box.min.x) || !std::isfinite(box.min.y) || !std::isfinite(box.min.z) ||
        !std::isfinite(box.max.x) || !std::isfinite(box.max.y) || !std::isfinite(box.max.z) ||
        box.max.y <= box.min.y) return;

    const auto tessPtr = *reinterpret_cast<std::uintptr_t*>(
        static_cast<std::byte*>(screenContext) + ScreenContext::mTessellator);
    if (tessPtr < 0x1000) return;
    void* tess = reinterpret_cast<void*>(tessPtr);

    // Camera position: LevelRendererPlayer::mCamPos, recovered from the
    // LevelRenderer the RenderLevel hook is called with (the Wings module's
    // route - no extra renderer argument has to be trusted).
    Vec3 camera{0.0f, 0.0f, 0.0f};
    const auto lrp = *reinterpret_cast<std::uintptr_t*>(
        reinterpret_cast<std::byte*>(levelRenderer) + LevelRenderer::mLevelRendererPlayer);
    if (lrp >= 0x1000) {
        camera = *reinterpret_cast<Vec3*>(lrp + LevelRendererPlayer::mCamPos);
    }

    const float feetX = (box.min.x + box.max.x) * 0.5f;
    const float feetY = box.min.y;
    const float feetZ = (box.min.z + box.max.z) * 0.5f;

    // First person: the camera sits inside the player's own box, so the pose
    // would smear across the screen. Skip unless the user asked for it.
    constexpr float kMargin = 0.05f;
    const bool cameraInside =
        camera.x >= box.min.x - kMargin && camera.x <= box.max.x + kMargin &&
        camera.y >= box.min.y - kMargin && camera.y <= box.max.y + kMargin &&
        camera.z >= box.min.z - kMargin && camera.z <= box.max.z + kMargin;
    if (cameraInside && !g_module->m_showInFirstPerson) return;

    ensureMaterial();
    if (!s_material) return;

    const bedrocktools::core::ScopedShaderColor shaderColor(screenContext);
    if (!shaderColor) return;

    constexpr float kPi = 3.14159265358979323846f;
    const float yawRad = rotation.y * kPi / 180.0f;
    const float cosYaw = std::cos(yawRad);
    const float sinYaw = std::sin(yawRad);

    // Body basis. right = (-cos, 0, -sin), forward = (-sin, 0, cos) - the
    // same derivation the Hitbox module documents for the player's facing.
    const Vec3 right{-cosYaw, 0.0f, -sinYaw};
    const Vec3 forward{-sinYaw, 0.0f, cosYaw};

    const float scale = g_module->m_scale <= 0.0f ? 1.0f : g_module->m_scale;
    const float unit = scale * 0.0625f; // 1 model pixel = 1/16 block

    // Grip position: chest height, slightly to the right, in front of the
    // body, plus the user's fine-nudge offsets.
    const Vec3 grip{
        feetX + forward.x * 0.30f + right.x * 0.16f + g_module->m_offsetX,
        feetY + 1.18f * scale + g_module->m_offsetY,
        feetZ + forward.z * 0.30f + right.z * 0.16f + g_module->m_offsetZ};

    // The blade tilts up and across the chest: lean it forward out of the
    // body plane and towards the player's left. Build a proper orthonormal
    // frame from the blade direction so the boxes stay rigid (a sheared
    // basis would stretch the blade).
    Vec3 up{
        forward.x * 0.40f - right.x * 0.30f,
        0.87f,
        forward.z * 0.40f - right.z * 0.30f};
    up = normalize(up);
    Vec3 rightAxis = normalize(cross(up, Vec3{forward.x, 0.0f, forward.z}));
    Vec3 forwardAxis = cross(rightAxis, up);

    Transform body;
    body.origin = grip;
    body.right = Vec3{rightAxis.x * unit, rightAxis.y * unit, rightAxis.z * unit};
    body.up = Vec3{up.x * unit, up.y * unit, up.z * unit};
    body.forward = Vec3{forwardAxis.x * unit, forwardAxis.y * unit, forwardAxis.z * unit};

    char pad[0x58]{};

    // Bedrock needs the exact vertex count up front; the pose is a fixed
    // number of boxes, so the count is a constant per configuration.
    const int verticesPerBox = 6 * 4 * 2; // both windings
    const int total = verticesPerBox * (g_module->m_showArm ? 5 : 3);

    s_tessBegin(tess, nullptr, 1, total, 0); // 1 = quad
    const int written = buildPose(tess, camera, body, unpackRgb(g_module->m_bladeColor),
                                  unpackRgb(g_module->m_guardColor), g_module->m_showArm);
    if (written != total) {
        // Should never happen; skip the flush rather than hand the GPU a
        // mismatched buffer.
        return;
    }
    s_renderMesh(screenContext, tess, reinterpret_cast<void*>(&s_material), pad);
}

void renderLevelDetour(void* self, void* screenContext, void* a3) {
    if (s_renderLevelOrig) s_renderLevelOrig(self, screenContext, a3);
    renderPose(self, screenContext);
}

// ---------------------------------------------------------------------------
// Sneak state
// ---------------------------------------------------------------------------
//
// Sneak comes from the same MoveInput component the upstream Keystrokes
// module reads: LocalPlayer -> IMoveInputComponent -> raw state. The input
// bits carry SneakDown (bit 0) for the physical button/key, and the
// component's latched flag values carry Sneaking (bit 0) for the held state;
// both are checked so a keyboard sneak and the latched mobile sneak button
// both count.

bool isSneaking(void* player) {
    const auto address = reinterpret_cast<std::uintptr_t>(player);
    if (address < 0x1000) return false;

    const auto component = *reinterpret_cast<std::uintptr_t*>(address + MoveInput::mComponent);
    if (component < 0x1000) return false;

    const auto rawState = component + MoveInput::mRawInputState;
    const auto inputBits = *reinterpret_cast<const std::uint32_t*>(rawState + MoveInputState::mInputBits);
    constexpr std::uint32_t kSneakDown = 1u << 0;
    if ((inputBits & kSneakDown) != 0) return true;

    // MoveInputComponent::mFlagValues (Sneaking = bit 0).
    constexpr std::size_t kFlagValuesOffset = 0x60;
    const auto flagBits = *reinterpret_cast<const std::uint16_t*>(component + kFlagValuesOffset);
    constexpr std::uint16_t kSneaking = 1u << 0;
    return (flagBits & kSneaking) != 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

SwordBlockingModule::SwordBlockingModule()
    : Module("Sword Blocking",
             "Shows the classic Java 1.7 sword-block pose while a sword is in "
             "hand: hold sneak (or use the Always/Toggle triggers). Client-side "
             "only - nothing is sent to the server.") {
    g_module = this;
    showInMenu = true;
    hideInHudEditor = true; // world overlay, not a HUD element
}

SwordBlockingModule::~SwordBlockingModule() {
    if (g_module == this) g_module = nullptr;
}

void SwordBlockingModule::onInit() {
    // Resolve everything the renderer needs once, at load.
    if (const auto begin = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin)) {
        s_tessBegin = reinterpret_cast<Tessellator_begin_t>(begin);
    }
    if (const auto color = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor)) {
        s_tessColor = reinterpret_cast<Tessellator_color_t>(color);
    }
    if (const auto vertex = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex)) {
        s_tessVertex = reinterpret_cast<Tessellator_vertex_t>(vertex);
    }
    if (const auto mesh = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2)) {
        s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(mesh);
    } else if (const auto mesh5 = bedrocktools::memory::resolve(
                   bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately)) {
        s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(mesh5);
    }
    if (const auto group = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderMaterialGroupCommon)) {
        const auto target = resolveADRP(reinterpret_cast<std::uint32_t*>(group), 2, 0);
        if (target) s_renderMaterialGroup = target + MaterialGroup::mRenderMaterialGroupOffset;
    }
    if (const auto renderLevel = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel)) {
        m_patchTarget = reinterpret_cast<void*>(renderLevel);
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_module) g_module->onLocalPlayerTick(event.player);
        });
}

void SwordBlockingModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    const auto handle = bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(renderLevelDetour),
                                                     reinterpret_cast<void**>(&s_renderLevelOrig));
    m_patched = handle != nullptr;
}

void SwordBlockingModule::onEnable() {
    applyPatch();
    resetPose();
}

void SwordBlockingModule::onFrame() {
    // Make sure the render hook is applied if the signature was not available
    // during onInit (the Hitbox module does the same retry while enabled).
    if (enabled && !m_patched) {
        if (!m_patchTarget) {
            if (const auto renderLevel =
                    bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel)) {
                m_patchTarget = reinterpret_cast<void*>(renderLevel);
            }
        }
        applyPatch();
    }
}

void SwordBlockingModule::onDisable() {
    resetPose();
}

void SwordBlockingModule::resetPose() {
    m_poseActive = false;
    m_latched = false;
}

void SwordBlockingModule::onKeybindEvent(const std::string& key, bool isDown) {
    if (key != "keybind" || !isDown || m_trigger != 2) {
        Module::onKeybindEvent(key, isDown);
        return;
    }
    // Toggle trigger: the keybind latches the pose instead of arming/disarming
    // the module. From a disabled state the first press arms the module and
    // turns the pose on; while armed, each press flips the latch.
    if (!enabled) {
        setKeybindActive(true); // run onEnable first, then arm the latch
        m_latched = true;
    } else {
        m_latched = !m_latched;
        if (!m_latched) m_poseActive = false;
    }
}

void SwordBlockingModule::onLocalPlayerTick(bedrocktools::sdk::Actor* actor) {
    if (!actor || !enabled) {
        s_localPlayer = actor ? reinterpret_cast<void*>(actor) : nullptr;
        m_poseActive = false;
        m_hasSword = false;
        return;
    }

    void* player = reinterpret_cast<void*>(actor);
    s_localPlayer = player;

    const void* carried = getCarriedItem(player);
    m_hasSword = isSwordItem(carried, m_anySword);

    bool active = false;
    if (m_hasSword) {
        switch (m_trigger) {
            case 1: // Always
                active = true;
                break;
            case 2: // Toggle
                active = m_latched;
                break;
            default: // Sneak
                active = isSneaking(player);
                break;
        }
    }
    m_poseActive = active;
}

void SwordBlockingModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("m_trigger")) {
        if (j["m_trigger"].is_string()) {
            m_trigger = parseTrigger(j["m_trigger"].get<std::string>());
        } else if (j["m_trigger"].is_number_integer()) {
            m_trigger = j["m_trigger"].get<int>();
        }
        if (m_trigger < 0 || m_trigger > 2) m_trigger = 0;
    }
    m_anySword = j.value("m_anySword", m_anySword);
    m_showArm = j.value("m_showArm", m_showArm);
    m_showInFirstPerson = j.value("m_showInFirstPerson", m_showInFirstPerson);

    auto readColor = [&j](const char* key, std::uint32_t& target) {
        if (!j.contains(key)) return;
        const auto& value = j[key];
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (text.size() == 7 && text[0] == '#') {
                try {
                    target = 0xFF000000u | static_cast<std::uint32_t>(std::stoul(text.substr(1), nullptr, 16));
                } catch (...) {
                }
            }
        } else if (value.is_number_integer()) {
            target = 0xFF000000u | (static_cast<std::uint32_t>(value.get<int>()) & 0xFFFFFFu);
        }
    };
    readColor("m_bladeColor", m_bladeColor);
    readColor("m_guardColor", m_guardColor);

    if (j.contains("m_scale")) {
        m_scale = j.value("m_scale", m_scale);
        if (m_scale < 0.5f) m_scale = 0.5f;
        if (m_scale > 2.0f) m_scale = 2.0f;
    }
    m_offsetX = j.value("m_offsetX", m_offsetX);
    m_offsetY = j.value("m_offsetY", m_offsetY);
    m_offsetZ = j.value("m_offsetZ", m_offsetZ);

    // Clamp the nudges so a bad config cannot fling the pose into the void.
    auto clampOffset = [](float value) {
        if (!std::isfinite(value)) return 0.0f;
        return std::max(-2.0f, std::min(2.0f, value));
    };
    m_offsetX = clampOffset(m_offsetX);
    m_offsetY = clampOffset(m_offsetY);
    m_offsetZ = clampOffset(m_offsetZ);
}

void SwordBlockingModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    static const char* kTriggers[] = {"Sneak", "Always", "Toggle"};
    const int index = (m_trigger >= 0 && m_trigger < 3) ? m_trigger : 0;
    // A radio value is stored as "current,option0,option1,..." so the launcher
    // renders a picker (see src/launcher/ModuleMenu.cpp).
    j["m_trigger"] = std::string(kTriggers[index]) + "," + kTriggers[0] + "," + kTriggers[1] + "," + kTriggers[2];
    j["m_anySword"] = m_anySword;
    j["m_showArm"] = m_showArm;
    j["m_showInFirstPerson"] = m_showInFirstPerson;

    auto formatColor = [](std::uint32_t value) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", (value >> 16) & 0xFF, (value >> 8) & 0xFF,
                      value & 0xFF);
        return std::string(buffer);
    };
    j["m_bladeColor"] = formatColor(m_bladeColor);
    j["m_guardColor"] = formatColor(m_guardColor);

    j["m_scale"] = m_scale;
    j["m_offsetX"] = m_offsetX;
    j["m_offsetY"] = m_offsetY;
    j["m_offsetZ"] = m_offsetZ;
}
