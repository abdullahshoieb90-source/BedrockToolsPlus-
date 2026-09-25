#pragma once

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

// Shared plumbing for the world-space overlay modules: Chunk Border,
// Breadcrumbs and Light Overlay.
//
// All three draw from inside a LevelRenderer::renderLevel hook with the game's
// own Tessellator and its "selection_box" render material, so they resolve the
// same entry points, run the same RenderMaterialGroup lookup and emit
// camera-relative vertices. HashedString and MaterialPtr mirror game types byte
// for byte (MaterialPtr is returned by value, so its layout is part of the
// calling convention), which is exactly why they are defined here once instead
// of copied into every module.
namespace renderoverlay {

using bedrocktools::sdk::Vec3;

using TessellatorBeginFn = void (*)(void* tessellator, void* debugCallback, int primitiveMode,
                                    int vertexCount, int noIndices);
using TessellatorColorFn = void (*)(void* tessellator, float r, float g, float b, float a);
using TessellatorVertexFn = void (*)(void* tessellator, float x, float y, float z);
using RenderMeshFn = void (*)(void* screenContext, void* tessellator, void* material, char* pad);

// Tessellator primitive mode for a line list.
inline constexpr int kLineList = 4;
// Trailing argument of MeshHelpers::renderMeshImmediately. The game only reads
// through the pointer, so a zeroed block of the documented size is enough.
inline constexpr std::size_t kRenderMeshPadding = 0x58;

// Key of RenderMaterialGroup::getMaterial. Same layout as the game's
// HashedString: a cached FNV-1a hash, the string itself, then a one-entry
// match cache.
struct HashedString {
    std::uint64_t hash = 0;
    std::string value;
    mutable const HashedString* lastMatch = nullptr;

    explicit HashedString(const char* text) : value(text ? text : "") {
        if (value.empty()) return;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        hash = kOffset;
        for (const char ch : value) {
            hash = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        }
    }
};

// Bedrock's MaterialPtr is two pointers. The game owns the materials handed out
// by RenderMaterialGroup, so this non-owning mirror deliberately runs no
// game-side destructor when the mod unloads.
struct MaterialPtr {
    void* data[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept : data{other.data[0], other.data[1]} {
        other.data[0] = nullptr;
        other.data[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            data[0] = other.data[0];
            data[1] = other.data[1];
            other.data[0] = nullptr;
            other.data[1] = nullptr;
        }
        return *this;
    }

    // User-provided (rather than `= default`) to match the game's non-trivial
    // return ABI for MaterialPtr exactly.
    ~MaterialPtr() {}

    explicit operator bool() const { return data[0] != nullptr; }
};

// Walks the first `count` arm64 instructions of a function looking for the
// address materialised into `targetReg`, either by ADRP+ADD or by a plain ADR.
// RenderMaterialGroupCommon resolves to a stub that loads the material-group
// singleton that way, which is the only stable route to it.
inline std::uintptr_t resolveAdrp(std::uint32_t* instructions, std::size_t count,
                                  std::uint32_t targetReg) {
    if (!instructions) return 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t insn = instructions[i];
        if ((insn & 0x1Fu) != targetReg) continue;

        if ((insn & 0x9F000000u) == 0x90000000u) {  // ADRP
            // imm = SignExtend(immlo:immhi, 21) << 12. Shifting the 21-bit
            // field up to bit 63 and back down arithmetic-extends its sign.
            const std::uint64_t immediate =
                static_cast<std::uint64_t>(((insn >> 3) & 0x1FFFFCu) | ((insn >> 29) & 3u)) << 43;
            const auto pageOffset = static_cast<std::int64_t>(immediate) >> 31;
            const auto instruction = static_cast<std::int64_t>(
                reinterpret_cast<std::uintptr_t>(&instructions[i]) & ~0xFFFull);
            const std::uintptr_t page = static_cast<std::uintptr_t>(instruction + pageOffset);

            for (std::size_t j = i + 1; j < count; ++j) {
                const std::uint32_t add = instructions[j];
                if ((add & 0xFF000000u) == 0x91000000u &&  // ADD (immediate)
                    ((add >> 5) & 0x1Fu) == targetReg && (add & 0x1Fu) == targetReg) {
                    std::uint32_t imm12 = (add >> 10) & 0xFFFu;
                    if (add & 0x400000u) imm12 <<= 12;  // 4 KB shift
                    return page + imm12;
                }
                if ((add & 0x1Fu) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000u) == 0x10000000u) {  // ADR
            const std::uint64_t immediate =
                static_cast<std::uint64_t>(((insn >> 3) & 0x1FFFFCu) | ((insn >> 29) & 3u)) << 43;
            const auto offset = static_cast<std::int64_t>(immediate) >> 43;
            return static_cast<std::uintptr_t>(
                static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&instructions[i])) +
                offset);
        }
    }
    return 0;
}

// Null and the first page are never a live game object, which is how the
// renderer tells "not in a world yet" from a real pointer.
inline bool looksValid(const void* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer) >= 0x1000u;
}

// Splits a 0xAARRGGBB value into the floats the Tessellator expects.
inline bedrocktools::sdk::Color toRgba(std::uint32_t argb) {
    return {static_cast<float>((argb >> 16) & 0xFFu) / 255.0f,
            static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
            static_cast<float>(argb & 0xFFu) / 255.0f,
            static_cast<float>((argb >> 24) & 0xFFu) / 255.0f};
}

// Colours are stored as 0xAARRGGBB. The launcher's colour picker writes
// "#RRGGBB" (no alpha byte) while the configs these modules write keep the
// alpha, so both widths are accepted. A missing alpha byte becomes opaque:
// treating "#RRGGBB" as 0x00RRGGBB would make the overlay fully transparent
// the first time the user picks a colour.
inline std::uint32_t parseColor(std::string_view text, std::uint32_t fallback) {
    std::string hex(text);
    if (!hex.empty() && hex[0] == '#') {
        hex.erase(0, 1);
    } else if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex.erase(0, 2);
    }
    if (hex.empty() || hex.size() > 8) return fallback;
    try {
        const auto parsed = static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        return hex.size() <= 6 ? (0xFF000000u | parsed) : parsed;
    } catch (...) {
        return fallback;
    }
}

inline std::string colorString(std::uint32_t argb) {
    char text[12]{};
    std::snprintf(text, sizeof(text), "#%08X", argb);
    return text;
}

// Replaces the alpha byte of a 0xAARRGGBB colour. Used by the overlays that
// fade along a trail instead of drawing every vertex at the same opacity.
inline std::uint32_t withAlpha(std::uint32_t argb, float alpha) {
    const float clamped = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const auto byte = static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
    return (argb & 0x00FFFFFFu) | (byte << 24);
}

// One renderLevel invocation: the handles the overlay draws with plus the
// ScreenContext colour it has to hand back unchanged.
struct Frame {
    void* screenContext = nullptr;
    void* tessellator = nullptr;
    void* material = nullptr;
    Vec3 camera{0.0f, 0.0f, 0.0f};
    float* colorHolder = nullptr;
    float savedColor[4]{0.0f, 0.0f, 0.0f, 0.0f};

    explicit operator bool() const { return tessellator != nullptr; }
};

// Everything the render hooks resolved once at init: the Tessellator entry
// points, the material-group singleton and the cached "selection_box" material.
class Overlay {
public:
    // Resolves the entry points. Safe to call repeatedly; only the first call
    // does any work.
    void resolve() {
        using bedrocktools::memory::resolve;
        using bedrocktools::memory::SignatureId;

        if (!m_tessBegin) {
            if (const auto address = resolve(SignatureId::TessellatorBegin)) {
                m_tessBegin = reinterpret_cast<TessellatorBeginFn>(address);
            }
        }
        if (!m_tessColor) {
            if (const auto address = resolve(SignatureId::TessellatorColor)) {
                m_tessColor = reinterpret_cast<TessellatorColorFn>(address);
            }
        }
        if (!m_tessVertex) {
            if (const auto address = resolve(SignatureId::TessellatorVertex)) {
                m_tessVertex = reinterpret_cast<TessellatorVertexFn>(address);
            }
        }
        if (!m_renderMesh) {
            // The two-argument variant is newer; fall back to the older one.
            auto address = resolve(SignatureId::MeshHelpersRenderMeshImmediately2);
            if (!address) address = resolve(SignatureId::MeshHelpersRenderMeshImmediately);
            if (address) m_renderMesh = reinterpret_cast<RenderMeshFn>(address);
        }
        if (m_materialGroup == 0) {
            if (const auto stub = resolve(SignatureId::RenderMaterialGroupCommon)) {
                const auto group = resolveAdrp(reinterpret_cast<std::uint32_t*>(stub), 2, 0);
                if (group) {
                    m_materialGroup =
                        group + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
                }
            }
        }
    }

    bool ready() const { return m_tessBegin && m_tessColor && m_tessVertex && m_renderMesh; }

    // Collects the per-frame handles and parks the ScreenContext colour at
    // opaque white so the material's own tint is not applied on top of the
    // per-vertex colours. Returns an empty (falsy) frame when the world is not
    // ready to draw into.
    Frame beginFrame(void* levelRenderer, void* screenContext) {
        Frame frame;
        if (!ready() || !looksValid(levelRenderer) || !looksValid(screenContext)) return frame;

        const auto* const screen = static_cast<const char*>(screenContext);
        const auto tessellator = *reinterpret_cast<const std::uintptr_t*>(
            screen + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
        if (!looksValid(reinterpret_cast<const void*>(tessellator))) return frame;

        const auto playerRenderer = *reinterpret_cast<const std::uintptr_t*>(
            static_cast<const char*>(levelRenderer) +
            bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
        if (!looksValid(reinterpret_cast<const void*>(playerRenderer))) return frame;

        ensureMaterial();

        frame.screenContext = screenContext;
        frame.tessellator = reinterpret_cast<void*>(tessellator);
        frame.camera = *reinterpret_cast<const Vec3*>(
            reinterpret_cast<const char*>(playerRenderer) +
            bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
        frame.material = m_material
                             ? static_cast<void*>(&m_material)
                             : reinterpret_cast<void*>(
                                   playerRenderer +
                                   bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

        const auto colorHolder = *reinterpret_cast<const std::uintptr_t*>(
            screen + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
        if (looksValid(reinterpret_cast<const void*>(colorHolder))) {
            frame.colorHolder = reinterpret_cast<float*>(colorHolder);
            for (int i = 0; i < 4; ++i) {
                frame.savedColor[i] = frame.colorHolder[i];
                frame.colorHolder[i] = 1.0f;
            }
        }
        return frame;
    }

    void begin(const Frame& frame, int vertexCount) const {
        if (!frame || !m_tessBegin) return;
        m_tessBegin(frame.tessellator, nullptr, kLineList, vertexCount, 0);
    }

    void color(const Frame& frame, std::uint32_t argb) const {
        if (!frame || !m_tessColor) return;
        const auto rgba = toRgba(argb);
        m_tessColor(frame.tessellator, rgba.r, rgba.g, rgba.b, rgba.a);
    }

    // renderLevel draws from the camera, so every vertex is camera relative.
    void vertex(const Frame& frame, const Vec3& world) const {
        if (!frame || !m_tessVertex) return;
        m_tessVertex(frame.tessellator, world.x - frame.camera.x, world.y - frame.camera.y,
                     world.z - frame.camera.z);
    }

    void line(const Frame& frame, const Vec3& from, const Vec3& to) const {
        vertex(frame, from);
        vertex(frame, to);
    }

    // Submits the vertices collected since begin().
    void flush(const Frame& frame) const {
        if (!frame || !m_renderMesh) return;
        char pad[kRenderMeshPadding];
        std::memset(pad, 0, sizeof(pad));
        m_renderMesh(frame.screenContext, frame.tessellator, frame.material, pad);
    }

    // Gives the ScreenContext colour back to whatever the game had set.
    void endFrame(Frame& frame) const {
        if (!frame.colorHolder) return;
        for (int i = 0; i < 4; ++i) frame.colorHolder[i] = frame.savedColor[i];
        frame.colorHolder = nullptr;
    }

private:
    void ensureMaterial() {
        if (m_material || m_materialGroup == 0) return;
        const auto vtable = *reinterpret_cast<void* const* const*>(m_materialGroup);
        constexpr auto slot = bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial;
        if (!vtable || !vtable[slot]) return;
        using getMaterialFn = MaterialPtr (*)(void*, const HashedString*);
        const HashedString name("selection_box");
        m_material = reinterpret_cast<getMaterialFn>(vtable[slot])(
            reinterpret_cast<void*>(m_materialGroup), &name);
    }

    TessellatorBeginFn m_tessBegin = nullptr;
    TessellatorColorFn m_tessColor = nullptr;
    TessellatorVertexFn m_tessVertex = nullptr;
    RenderMeshFn m_renderMesh = nullptr;
    MaterialPtr m_material;
    std::uintptr_t m_materialGroup = 0;
};

}  // namespace renderoverlay
