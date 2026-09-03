#include "blockoutline.hpp"
#include "blockoutline_color.hpp"
#include "blockoutline_geometry.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using TessellatorBegin = void (*)(void*, void*, int, int, int);
using TessellatorColor = void (*)(void*, float, float, float, float);
using TessellatorVertex = void (*)(void*, float, float, float);
using RenderMeshImmediately = void (*)(void*, void*, void*, char*);
using RenderLevel = void (*)(void*, void*, void*);
using LevelGetHitResult = void* (*)(void*);

// Tessellator primitive modes used by Bedrock: 1 = quad list (4 vertices per
// quad), 4 = line list (2 vertices per line).
constexpr int kQuadPrimitive = 1;
constexpr int kLinePrimitive = 4;

// How long one full rainbow cycle takes in the "Rgb" mode.
constexpr double kRgbCycleSeconds = 3.0;

struct HashedString {
    std::uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}

    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }

private:
    static std::uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        std::uint64_t hash = kOffset;
        for (char ch : str) {
            hash = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^
                   (kPrime * hash);
        }
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    ~MaterialPtr() {}

    explicit operator bool() const { return sharedPtrData[0] != nullptr; }
};

static std::uintptr_t resolveADRP(std::uint32_t* insns, size_t count, std::uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        std::uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            std::uintptr_t page =
                ((std::uintptr_t)&insns[i] & ~0xFFFULL) +
                ((std::int64_t)((std::uint64_t)((insn >> 3) & 0x1FFFFC |
                                               (insn >> 29) & 3)
                                << 43) >>
                 31);

            for (size_t j = i + 1; j < count; j++) {
                std::uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg && (add & 0x1F) == targetReg) {
                    std::uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            std::int64_t imm =
                (std::int64_t)((std::uint64_t)((insn >> 3) & 0x1FFFFC |
                                              (insn >> 29))
                               << 43) >>
                43;
            return (std::uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

BlockOutlineModule* g_module = nullptr;
TessellatorBegin g_tessellatorBegin = nullptr;
TessellatorColor g_tessellatorColor = nullptr;
TessellatorVertex g_tessellatorVertex = nullptr;
RenderMeshImmediately g_renderMesh = nullptr;
RenderLevel g_renderLevelOriginal = nullptr;
LevelGetHitResult g_getHitResult = nullptr;

std::uintptr_t g_renderMaterialGroup = 0;
MaterialPtr g_matSelection;
MaterialPtr g_matFill;

std::mutex g_targetMutex;
bedrocktools::sdk::BlockPos g_target{};
bool g_hasTarget = false;

void clearTarget() {
    std::lock_guard lock(g_targetMutex);
    g_hasTarget = false;
}

void updateTarget(bedrocktools::sdk::Player* player) {
    if (!g_module || !g_module->enabled || !player || !g_getHitResult) {
        clearTarget();
        return;
    }

    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    void* level = *reinterpret_cast<void**>(
        playerAddress + bedrocktools::sdk::offsets::Actor::mLevel);
    void* hitResult = level ? g_getHitResult(level) : nullptr;
    if (!hitResult) {
        clearTarget();
        return;
    }

    const auto hitAddress = reinterpret_cast<std::uintptr_t>(hitResult);
    const int type = *reinterpret_cast<const int*>(
        hitAddress + bedrocktools::sdk::offsets::HitResult::mType);
    if (type != bedrocktools::sdk::offsets::HitResult::TypeBlock) {
        clearTarget();
        return;
    }

    const auto position = *reinterpret_cast<const bedrocktools::sdk::BlockPos*>(
        hitAddress + bedrocktools::sdk::offsets::HitResult::mBlockPos);
    std::lock_guard lock(g_targetMutex);
    g_target = position;
    g_hasTarget = true;
}

MaterialPtr getMaterial(const char* name) {
    if (!g_renderMaterialGroup) return {};

    HashedString hs(name);

    void** vtable = *reinterpret_cast<void***>(g_renderMaterialGroup);
    if (!vtable ||
        !vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]) {
        return {};
    }

    using GetMaterial = MaterialPtr (*)(void*, const HashedString*);
    return reinterpret_cast<GetMaterial>(
        vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial])(
        reinterpret_cast<void*>(g_renderMaterialGroup), &hs);
}

void ensureMaterials() {
    if (!g_renderMaterialGroup) return;

    if (!g_matSelection) g_matSelection = getMaterial("selection_box");

    // The hidden back edge pass in "Show 3D" mode needs a vertex-color
    // material whose depth state lets the far edges show through the block
    // itself. The selection overlay material is depth-tested (which keeps the
    // visible frame from X-raying through walls), so it would hide every back
    // edge. Prefer a vertex-color fill for the 3D edge pass and keep
    // selection_box for the opaque visible frame.
    if (!g_matFill) {
        static const char* kFillNames[] = {
            "ui_fill_color",
            "ui_textured_and_glcolor",
            "debug_filled_box",
        };
        for (const char* name : kFillNames) {
            g_matFill = getMaterial(name);
            if (g_matFill) break;
        }
    }
}

void renderLevelHook(void* levelRenderer, void* screenContext, void* renderParams) {
    if (g_renderLevelOriginal) {
        g_renderLevelOriginal(levelRenderer, screenContext, renderParams);
    }

    if (!g_module || !g_module->enabled || !screenContext || !levelRenderer ||
        !g_tessellatorBegin || !g_tessellatorColor || !g_tessellatorVertex ||
        !g_renderMesh) {
        return;
    }

    bedrocktools::sdk::BlockPos target{};
    {
        std::lock_guard lock(g_targetMutex);
        if (!g_hasTarget) return;
        target = g_target;
    }

    const auto contextAddress = reinterpret_cast<std::uintptr_t>(screenContext);
    void* tessellator = *reinterpret_cast<void**>(
        contextAddress + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    float* colorHolder = *reinterpret_cast<float**>(
        contextAddress + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (reinterpret_cast<std::uintptr_t>(tessellator) < 0x1000 ||
        reinterpret_cast<std::uintptr_t>(colorHolder) < 0x1000) {
        return;
    }

    const auto rendererAddress = reinterpret_cast<std::uintptr_t>(levelRenderer);
    void* playerRenderer = *reinterpret_cast<void**>(
        rendererAddress + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (reinterpret_cast<std::uintptr_t>(playerRenderer) < 0x1000) return;

    const auto playerRendererAddress = reinterpret_cast<std::uintptr_t>(playerRenderer);
    const auto camera = *reinterpret_cast<const bedrocktools::sdk::Vec3*>(
        playerRendererAddress + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    // The game's own selection material is always initialized with the level
    // renderer and has the depth state expected for a block outline. It keeps
    // the visible frame from X-raying through walls. The "Show 3D" back edge
    // pass uses a separate vertex-color fill material instead because that
    // material lets the far edges show through the block (see
    // ensureMaterials).
    void* overlayMaterial = reinterpret_cast<void*>(
        playerRendererAddress +
        bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    ensureMaterials();
    void* matInner = g_matSelection ? static_cast<void*>(&g_matSelection) : overlayMaterial;
    void* matFill = g_matFill ? static_cast<void*>(&g_matFill) : matInner;

    const float savedColor[4] = {
        colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]
    };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    float red, green, blue;
    if (g_module->rgb) {
        // Rainbow mode: the hue cycles continuously through the spectrum.
        // Phase is computed in double so precision survives long uptimes.
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const float phase =
            static_cast<float>(std::fmod(seconds / kRgbCycleSeconds, 1.0));
        const auto c = bedrocktools::modules::blockoutline::rainbowRgb(phase);
        red = c.r;
        green = c.g;
        blue = c.b;
    } else {
        const std::uint32_t color = g_module->outlineColor;
        red = static_cast<float>((color >> 16) & 0xFFu) / 255.0f;
        green = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
        blue = static_cast<float>(color & 0xFFu) / 255.0f;
    }

    const float camX = camera.x;
    const float camY = camera.y;
    const float camZ = camera.z;

    // Line size slider. 1.0 (or lower) keeps the classic hairline box; above
    // that the frame is widened with real geometry, because GL line width is
    // ignored by nearly every mobile GLES driver.
    float lineSize = g_module->lineThickness;
    if (lineSize < 1.0f) lineSize = 1.0f;
    if (lineSize > 10.0f) lineSize = 10.0f;
    const float frameWidth =
        bedrocktools::modules::blockoutline::frameWidthForLineSize(lineSize);
    const bool thickLines = frameWidth > 0.0f;

    char meshParams[0x58];
    const auto lines = bedrocktools::modules::blockoutline::makeBox(
        static_cast<float>(target.x),
        static_cast<float>(target.y),
        static_cast<float>(target.z));

    // The normal visible passes only emit geometry facing the camera. The
    // fill material used by the "Show 3D" back edge pass is not guaranteed to
    // depth-test custom world geometry, so that pass is strictly limited to
    // the faces that do not face the eye; without this culling the far side
    // would bleed through the block as a translucent sheet instead of staying
    // a crisp set of hidden edges.
    const bedrocktools::modules::blockoutline::Point eye{camX, camY, camZ};
    const auto edgeVisible = bedrocktools::modules::blockoutline::makeEdgeVisibility(lines, eye);
    int visibleEdgeCount = 0;
    for (const bool visible : edgeVisible) {
        if (visible) ++visibleEdgeCount;
    }

    // Faces of the block itself (no expansion) and which of them face the
    // eye. Shared by the thick frame and the "Show 3D" back edge pass so both
    // agree on what is "in front".
    const auto blockFaces = bedrocktools::modules::blockoutline::makeFaces(
        static_cast<float>(target.x),
        static_cast<float>(target.y),
        static_cast<float>(target.z),
        0.0f);
    const auto faceVisible =
        bedrocktools::modules::blockoutline::makeFaceVisibility(blockFaces, eye);

    // True when the camera sits inside the targeted block. Every edge then
    // counts as facing the eye, and thick geometry built around the camera
    // would fill the screen, so both the strip frame and the 3D bars step
    // aside there and leave the hairline outline on its own.
    const bool eyeInsideBlock =
        camX > static_cast<float>(target.x) && camX < static_cast<float>(target.x) + 1.0f &&
        camY > static_cast<float>(target.y) && camY < static_cast<float>(target.y) + 1.0f &&
        camZ > static_cast<float>(target.z) && camZ < static_cast<float>(target.z) + 1.0f;

    

    // Emits the selected edges as bars - a cross of two perpendicular quads
    // centred on each edge - and renders them with `material`. Both windings
    // are emitted per quad so back-face culling never eats one.
    auto drawEdgeBars = [&](const std::array<bool, 12>& mask, float width,
                            float lift, void* material) {
        const auto bars = bedrocktools::modules::blockoutline::makeEdgeBars(
            lines, mask, width, lift);
        if (bars.count == 0) return;

        g_tessellatorBegin(tessellator, nullptr, kQuadPrimitive,
                           static_cast<int>(bars.count) * 8, 0);
        g_tessellatorColor(tessellator, red, green, blue, 1.0f);
        for (std::size_t i = 0; i < bars.count; ++i) {
            const auto& q = bars.quads[i];
            const bedrocktools::sdk::Vec3 verts[4] = {
                {q.a.x - camX, q.a.y - camY, q.a.z - camZ},
                {q.b.x - camX, q.b.y - camY, q.b.z - camZ},
                {q.c.x - camX, q.c.y - camY, q.c.z - camZ},
                {q.d.x - camX, q.d.y - camY, q.d.z - camZ},
            };
            for (int j = 0; j < 4; ++j) {
                g_tessellatorVertex(tessellator, verts[j].x, verts[j].y, verts[j].z);
            }
            for (int j = 3; j >= 0; --j) {
                g_tessellatorVertex(tessellator, verts[j].x, verts[j].y, verts[j].z);
            }
        }
        std::memset(meshParams, 0, sizeof(meshParams));
        g_renderMesh(screenContext, tessellator, material, meshParams);
    };

    // Hairline pass for the selected edges. The bars shrink below a pixel at
    // distance (and are extremely thin at the default Line Size), so this
    // keeps those edges crisp at any range.
    auto drawEdgeHairlines = [&](const std::array<bool, 12>& mask, int edgeCount,
                                 void* material) {
        if (edgeCount <= 0) return;

        g_tessellatorBegin(tessellator, nullptr, kLinePrimitive, edgeCount * 2, 0);
        g_tessellatorColor(tessellator, red, green, blue, 1.0f);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!mask[i]) continue;
            const auto& line = lines[i];
            g_tessellatorVertex(tessellator,
                line.from.x - camX, line.from.y - camY, line.from.z - camZ);
            g_tessellatorVertex(tessellator,
                line.to.x - camX, line.to.y - camY, line.to.z - camZ);
        }
        std::memset(meshParams, 0, sizeof(meshParams));
        g_renderMesh(screenContext, tessellator, material, meshParams);
    };

    : keeps the edge crisp and visible even when the
    // strips shrink below a pixel at long range. Same visibility set as the
    // thick pass so both stay consistent.
    if (visibleEdgeCount > 0) {
        g_tessellatorBegin(tessellator, nullptr, kLinePrimitive,
                           visibleEdgeCount * 2, 0);
        g_tessellatorColor(tessellator, red, green, blue, 1.0f);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!edgeVisible[i]) continue;
            const auto& line = lines[i];
            g_tessellatorVertex(tessellator,
                line.from.x - camX, line.from.y - camY, line.from.z - camZ);
            g_tessellatorVertex(tessellator,
                line.to.x - camX, line.to.y - camY, line.to.z - camZ);
        }

        std::memset(meshParams, 0, sizeof(meshParams));
        g_renderMesh(screenContext, tessellator, matInner, meshParams);
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

} // namespace

BlockOutlineModule::BlockOutlineModule()
    : Module("Block Outline", "Draws a configurable outline around the block you are looking at. Optional full 3D edges and rainbow RGB colors.") {
    g_module = this;
}

BlockOutlineModule::~BlockOutlineModule() {
    if (g_module == this) g_module = nullptr;
}

void BlockOutlineModule::onInit() {
    using bedrocktools::memory::SignatureId;

    m_renderLevel = reinterpret_cast<void*>(
        bedrocktools::memory::resolve(SignatureId::RenderLevel));
    g_tessellatorBegin = reinterpret_cast<TessellatorBegin>(
        bedrocktools::memory::resolve(SignatureId::TessellatorBegin));
    g_tessellatorColor = reinterpret_cast<TessellatorColor>(
        bedrocktools::memory::resolve(SignatureId::TessellatorColor));
    g_tessellatorVertex = reinterpret_cast<TessellatorVertex>(
        bedrocktools::memory::resolve(SignatureId::TessellatorVertex));

    auto renderMeshAddress = bedrocktools::memory::resolve(
        SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!renderMeshAddress) {
        renderMeshAddress = bedrocktools::memory::resolve(
            SignatureId::MeshHelpersRenderMeshImmediately);
    }
    g_renderMesh = reinterpret_cast<RenderMeshImmediately>(renderMeshAddress);
    g_getHitResult = reinterpret_cast<LevelGetHitResult>(
        bedrocktools::memory::resolve(SignatureId::LevelGetHitResult));

    const auto renderMaterialGroup = bedrocktools::memory::resolve(
        SignatureId::RenderMaterialGroupCommon);
    if (renderMaterialGroup) {
        const auto groupAddress = resolveADRP(
            reinterpret_cast<std::uint32_t*>(renderMaterialGroup), 2, 0);
        if (groupAddress) {
            g_renderMaterialGroup =
                groupAddress +
                bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { updateTarget(event.player); });
}

void BlockOutlineModule::installRenderHook() {
    if (m_hookInstalled || !m_renderLevel) return;
    const auto handle = bedrocktools::hooks::install(
        m_renderLevel,
        reinterpret_cast<void*>(renderLevelHook),
        reinterpret_cast<void**>(&g_renderLevelOriginal));
    m_hookInstalled = handle != nullptr;
}

void BlockOutlineModule::onEnable() {
    installRenderHook();
}

void BlockOutlineModule::onDisable() {
    clearTarget();
}

void BlockOutlineModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);

    // The 3D edge view is an explicit opt-in toggle ("Show 3D" in the menu).
    // Current configs use the "show3d" key; the older "block3d" and
    // "outline3d" keys are still accepted so upgrading players keep their
    // setting.
    auto readBool = [&](const char* key, bool& out) {
        if (json.contains(key) && json[key].is_boolean()) out = json[key].get<bool>();
    };
    readBool("rgb", rgb);

    if (json.contains("lineThickness")) {
        try {
            lineThickness = json["lineThickness"].get<float>();
        } catch (...) {
            // Preserve the last valid thickness.
        }
    }
    if (lineThickness < 1.0f) lineThickness = 1.0f;
    if (lineThickness > 10.0f) lineThickness = 10.0f;

    if (json.contains("outlineColor") && json["outlineColor"].is_string()) {
        // Current configs store the single RGB picker value ("#RRGGBB").
        std::string value = json["outlineColor"].get<std::string>();
        if (!value.empty() && value.front() == '#') value.erase(value.begin());
        try {
            const auto parsed = std::stoul(value, nullptr, 16);
            // Six-digit colors are RGB; eight-digit colors are AARRGGBB. Alpha
            // is always forced opaque so the outline never washes out.
            outlineColor = value.size() <= 6
                ? (0xFF000000u | static_cast<std::uint32_t>(parsed))
                : (0xFF000000u | (static_cast<std::uint32_t>(parsed) & 0x00FFFFFFu));
        } catch (...) {
            // Preserve the last valid color.
        }
    } else {
        // Legacy configs stored three 0-255 channel sliders instead of the
        // RGB picker.
        auto readChannel = [&](const char* key, int fallback) -> int {
            if (!json.contains(key) || !json[key].is_number_integer()) return fallback;
            int value = json[key].get<int>();
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            return value;
        };

        const bool hasRgb = json.contains("outlineRed") ||
                            json.contains("outlineGreen") ||
                            json.contains("outlineBlue");
        if (hasRgb) {
            const int r = readChannel("outlineRed", 255);
            const int g = readChannel("outlineGreen", 255);
            const int b = readChannel("outlineBlue", 255);
            outlineColor = 0xFF000000u |
                           (static_cast<std::uint32_t>(r) << 16) |
                           (static_cast<std::uint32_t>(g) << 8) |
                           static_cast<std::uint32_t>(b);
        }
    }
}

void BlockOutlineModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);

    // Keep alpha opaque so the line color never washes out.
    outlineColor |= 0xFF000000u;

    // A single RGB picker value ("#RRGGBB") instead of the old separate
    // Red/Green/Blue sliders.
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%06X", outlineColor & 0x00FFFFFFu);
    json["outlineColor"] = std::string("#") + hex;

    json["rgb"] = rgb;
    json["lineThickness"] = lineThickness;
}
