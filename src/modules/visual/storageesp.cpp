#include "storageesp.hpp"
#include "blockoutline_geometry.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using BlockSourceGetBlockFn = const void* (*)(void* region,
                                              const bedrocktools::sdk::BlockPos*,
                                              int);
using RenderLevelFn = void (*)(void*, void*, void*);
using TessellatorBeginFn = void (*)(void*, void*, int, int, int);
using TessellatorColorFn = void (*)(void*, float, float, float, float);
using TessellatorVertexFn = void (*)(void*, float, float, float);
using RenderMeshImmediatelyFn = void (*)(void*, void*, void*, char*);

// A found container, cheap enough to copy to the render thread every time the
// world scan changes something.
using FoundBlocks = std::vector<storageesp::FoundBlock>;

struct HashedString {
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

// Bedrock's MaterialPtr is two pointers. The game owns materials returned by
// RenderMaterialGroup, so this non-owning mirror intentionally does not run a
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

StorageEspModule* g_storageEsp = nullptr;

BlockSourceGetBlockFn s_getBlock = nullptr;
RenderLevelFn s_renderLevelOriginal = nullptr;
TessellatorBeginFn s_tessBegin = nullptr;
TessellatorColorFn s_tessColor = nullptr;
TessellatorVertexFn s_tessVertex = nullptr;
RenderMeshImmediatelyFn s_renderMesh = nullptr;

std::uintptr_t s_renderMaterialGroup = 0;
MaterialPtr s_selectionMaterial;
MaterialPtr s_throughWallsMaterial;

// Game-thread only: the world scan and everything it memoizes. The menu thread
// never touches these directly; it only bumps the two atomics below, and the
// next tick applies the request. That keeps a sweep from reading a cursor (or a
// chunk list) the menu freed underneath it.
storageesp::TypeKindCache s_typeCache;
storageesp::StorageCache s_cache;
bool s_dirty = false;

std::atomic<std::uint64_t> s_scanRevision{0};
std::atomic<bool> s_resetRequested{false};

// Published to the render thread as an immutable snapshot, so a frame never
// reads the cache while the sweep is mutating it.
std::mutex s_publishMutex;
std::shared_ptr<const FoundBlocks> s_published = std::make_shared<const FoundBlocks>();

// What the tracer pass needs about the local player, sampled on the tick that
// runs the sweep: the render thread only ever reads a camera *position* out of
// the level renderer, and a tracer anchored to the player's feet (or clipped
// against the eye plane) needs a point and a view direction that follow the
// player between block updates.
struct PublishedView {
    bedrocktools::sdk::Vec3 feet{};
    bedrocktools::sdk::Vec2 rotation{};
    bool rotationValid = false;
};

PublishedView s_publishedView{};

// Where the current sweep stands. Restarting it is cheap: the cache survives,
// only the cursor and the chunk list are rebuilt.
struct ScanState {
    storageesp::ScanRegion region{};
    std::vector<storageesp::ScanCell> cells;
    std::size_t cellIndex = 0;
    std::size_t blockIndex = 0;
    std::uint64_t revision = 0;
    void* regionPointer = nullptr;
    bool active = false;
};

ScanState s_scan;

constexpr float kBoxExpansion = 0.002f;      // same as Block Outline, avoids z-fighting
// Camera-facing line widths, in world units per thickness step. Box edges are
// about a block long and are drawn at this literal width; a tracer's width is
// this value at one block of range and grows with distance from there, so it
// keeps a steady width on screen however far the container is.
constexpr float kBoxHalfWidthPerThickness = 0.005f;
constexpr float kTracerHalfWidthPerThickness = 0.002f;
constexpr int kReanchorDistance = 12;        // blocks the player may move before restarting
constexpr int kKeepXMargin = 16;             // cached blocks stay this far past the scan area
constexpr int kKeepYMargin = 8;
constexpr int kMinScanRadius = 8;
constexpr int kMaxScanRadius = 64;
constexpr int kMinScanHeight = 2;
constexpr int kMaxScanHeight = 64;

std::uintptr_t resolveAdrp(std::uint32_t* instructions,
                           std::size_t count,
                           std::uint32_t targetRegister) {
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

        if ((instruction & 0x9F000000) == 0x10000000) {
            const std::uint64_t immediateBits =
                ((static_cast<std::uint64_t>((instruction >> 3) & 0x1FFFFC)) |
                 static_cast<std::uint64_t>(instruction >> 29)) << 43;
            const auto immediate = static_cast<std::int64_t>(immediateBits) >> 43;
            return reinterpret_cast<std::uintptr_t>(&instructions[i]) + immediate;
        }
    }
    return 0;
}

MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};
    auto** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]) return {};

    HashedString hashedName(name);
    using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);
    auto getMaterialFn = reinterpret_cast<GetMaterialFn>(
        vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]);
    return getMaterialFn(reinterpret_cast<void*>(s_renderMaterialGroup), &hashedName);
}

void ensureMaterials() {
    if (!s_renderMaterialGroup) return;
    if (!s_selectionMaterial) s_selectionMaterial = getMaterial("selection_box");

    // ui_fill_color has no terrain-depth test in current Bedrock builds, which
    // is exactly what an ESP highlight through walls needs. If a future build
    // stops exposing it, rendering falls back to the depth-tested material.
    if (!s_throughWallsMaterial) {
        static constexpr const char* kCandidates[] = {
            "ui_fill_color",
            "ui_textured_and_glcolor",
        };
        for (const char* candidate : kCandidates) {
            s_throughWallsMaterial = getMaterial(candidate);
            if (s_throughWallsMaterial) break;
        }
    }
}

void publishSnapshot() {
    if (!s_dirty) return;
    s_dirty = false;
    auto next = std::make_shared<const FoundBlocks>(s_cache.snapshot());
    std::lock_guard<std::mutex> lock(s_publishMutex);
    s_published = std::move(next);
}

// Replaces what the overlay draws with nothing, right now. The frame in flight
// keeps its own shared pointer, so this is safe from any thread.
void publishCleared() {
    std::lock_guard<std::mutex> lock(s_publishMutex);
    s_published = std::make_shared<const FoundBlocks>();
}

// Publishes the tracer anchors every tick, on their own: a tracer that only
// refreshed together with the block snapshot would keep pointing at where the
// player stood the last time a container was found or lost.
void publishView(const bedrocktools::sdk::Player* player, const bedrocktools::sdk::Vec3& feet) {
    PublishedView next;
    next.feet = feet;
    if (player->rotationComponent()) {
        next.rotation = player->rotation();
        next.rotationValid = true;
    }
    std::lock_guard<std::mutex> lock(s_publishMutex);
    s_publishedView = next;
}

// Puts the sweep cursor back at the player, and `clearFound` additionally
// forgets what was remembered: only the world changing calls for that, since a
// position that stays in the area is re-confirmed (or dropped) by every sweep.
void restartSweep(bool clearFound) {
    s_scan.cells.clear();
    s_scan.cellIndex = 0;
    s_scan.blockIndex = 0;
    s_scan.active = false;
    s_scan.revision = s_scanRevision.load();
    if (clearFound) {
        s_cache.clear();
        s_typeCache.clear();
        s_dirty = true;
    }
    publishSnapshot();
}

bool regionChanged(void* region) {
    if (s_scan.regionPointer == region) return false;
    s_scan.regionPointer = region;
    // Block pointers and BlockLegacy identities belong to the previous
    // dimension/world, so nothing that was memoized may be reused.
    restartSweep(true);
    return true;
}

void startSweep(const storageesp::ScanRegion& region) {
    s_scan.region = region;
    std::vector<storageesp::ScanCell> cells(storageesp::maxCellCount(region));
    const std::size_t count =
        storageesp::collectScanCells(region, cells.data(), cells.size());
    cells.resize(count);
    s_scan.cells = std::move(cells);
    s_scan.cellIndex = 0;
    s_scan.blockIndex = 0;
    s_scan.active = !s_scan.cells.empty();
    s_scan.revision = s_scanRevision.load();
}

bool needsNewSweep(const storageesp::ScanRegion& wanted) {
    if (!s_scan.active) return true;
    const auto& current = s_scan.region;
    if (current.blockRadius != wanted.blockRadius || current.verticalRadius != wanted.verticalRadius) {
        return true;
    }
    const int dx = std::abs(current.anchor.x - wanted.anchor.x);
    const int dy = std::abs(current.anchor.y - wanted.anchor.y);
    const int dz = std::abs(current.anchor.z - wanted.anchor.z);
    return std::max({dx, dy, dz}) > kReanchorDistance;
}

// One block of the sweep: read its type, map it to a highlight group and
// remember (or forget) the position.
void visitBlock(void* region, const bedrocktools::sdk::BlockPos& position) {
    const void* block = s_getBlock(region, &position, 0);
    if (!block || reinterpret_cast<std::uintptr_t>(block) < 0x1000) return;

    const std::uintptr_t legacy = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(block) + bedrocktools::sdk::offsets::Block::mBlockType);
    if (legacy < 0x1000) return;

    const auto kind = s_typeCache.kindFor(reinterpret_cast<const void*>(legacy), [&] {
        const auto* named = static_cast<const bedrocktools::sdk::Block*>(block);
        const std::string* fullName = named->fullName();
        if (!fullName || fullName->empty() || fullName->size() > 256 || !fullName->data()) {
            return static_cast<int>(storageesp::StorageKind::None);
        }
        return static_cast<int>(storageesp::classify({fullName->data(), fullName->size()}));
    });

    if (kind == storageesp::StorageKind::None) {
        // Cheap while nothing was found yet; once there is something, a block
        // that stopped being storage (broken, replaced) is dropped right away.
        if (!s_cache.empty() && s_cache.erase(position)) s_dirty = true;
        return;
    }

    const bool alreadyKnown = s_cache.contains(position);
    s_cache.put(position, kind);
    if (!alreadyKnown) s_dirty = true;
}

void scanStep(bedrocktools::sdk::Player* player) {
    const bool requestedReset = s_resetRequested.exchange(false);
    if (!g_storageEsp || !g_storageEsp->enabled) {
        if (requestedReset && (s_scan.active || !s_cache.empty())) restartSweep(true);
        return;
    }
    if (!s_getBlock || !player || reinterpret_cast<std::uintptr_t>(player) < 0x1000) return;

    auto* dimension = player->dimension();
    if (!dimension || reinterpret_cast<std::uintptr_t>(dimension) < 0x1000) return;
    auto* region = dimension->blockSource();
    if (!region || reinterpret_cast<std::uintptr_t>(region) < 0x1000) return;

    const bool fresh = regionChanged(region);
    if (requestedReset) restartSweep(true);

    const bedrocktools::sdk::Vec3 position = player->position();
    publishView(player, position);

    storageesp::ScanRegion wanted;
    wanted.anchor = {static_cast<int>(std::floor(position.x)),
                     static_cast<int>(std::floor(position.y)),
                     static_cast<int>(std::floor(position.z))};
    wanted.blockRadius = std::clamp(g_storageEsp->scanRadius, kMinScanRadius, kMaxScanRadius);
    wanted.verticalRadius = std::clamp(g_storageEsp->scanHeight, kMinScanHeight, kMaxScanHeight);

    if (fresh || s_scan.revision != s_scanRevision.load() || needsNewSweep(wanted)) {
        startSweep(wanted);
    }
    if (!s_scan.active) return;

    const std::size_t budget = storageesp::scanSpeedBudget(g_storageEsp->scanSpeed);
    const std::size_t blocksPerCell = storageesp::blocksPerCell(s_scan.region);
    for (std::size_t step = 0; step < budget; ++step) {
        if (s_scan.blockIndex >= blocksPerCell) {
            s_scan.blockIndex = 0;
            s_scan.cellIndex = (s_scan.cellIndex + 1) % s_scan.cells.size();
        }
        visitBlock(region,
                   storageesp::blockInCell(s_scan.region,
                                           s_scan.cells[s_scan.cellIndex],
                                           s_scan.blockIndex));
        ++s_scan.blockIndex;
    }

    // Anything the area no longer covers can never be confirmed again, so it
    // goes here instead of lingering as a highlight nobody can validate.
    if (!s_cache.empty()) {
        const std::size_t before = s_cache.size();
        s_cache.dropUnreachable(s_scan.region.anchor, wanted.blockRadius + kKeepXMargin,
                                wanted.verticalRadius + kKeepYMargin);
        if (s_cache.size() != before) s_dirty = true;
    }

    publishSnapshot();
}

void setTessellatorColor(void* tessellator, std::uint32_t rgb, float alpha) {
    s_tessColor(tessellator,
                static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                static_cast<float>(rgb & 0xFF) / 255.0f,
                std::clamp(alpha, 0.0f, 1.0f));
}

void emitVertex(void* tessellator,
                const bedrocktools::sdk::Vec3& point,
                const bedrocktools::sdk::Vec3& camera) {
    s_tessVertex(tessellator,
                 point.x - camera.x,
                 point.y - camera.y,
                 point.z - camera.z);
}

void flushMesh(void* screenContext, void* tessellator, void* material) {
    char pad[0x58]{};
    s_renderMesh(screenContext, tessellator, material, pad);
}

void drawFill(void* screenContext,
              void* tessellator,
              void* material,
              const std::vector<blockoutline::Box>& boxes,
              const bedrocktools::sdk::Vec3& camera,
              std::uint32_t rgb,
              float alpha) {
    if (boxes.empty() || alpha <= 0.001f) return;

    s_tessBegin(tessellator, nullptr, 1, static_cast<int>(boxes.size()) * 6 * 4, 0);
    setTessellatorColor(tessellator, rgb, alpha);
    for (const auto& box : boxes) {
        const auto faces = blockoutline::boxFaces(box);
        for (const auto& face : faces) {
            for (const auto& vertex : face) emitVertex(tessellator, vertex, camera);
        }
    }
    flushMesh(screenContext, tessellator, material);
}

// Draws world-space line segments: box edges and tracer lines share this pass,
// because both need the same two-step trick on Android.
//
// `constantScreenWidth` picks how the camera-facing quads are widened. Box
// edges are about a block long, so one world-space width reads the same from
// any distance. A tracer spans tens of blocks and is seen end-on: with a fixed
// world width its far end is a fraction of a pixel, so its width instead grows
// with the distance of each end, which keeps one steady width on screen.
void drawSegments(void* screenContext,
                  void* tessellator,
                  void* material,
                  const std::vector<blockoutline::Edge>& segments,
                  const bedrocktools::sdk::Vec3& camera,
                  std::uint32_t rgb,
                  float alpha,
                  float thickness,
                  bool constantScreenWidth) {
    if (segments.empty() || alpha <= 0.001f) return;

    const float safeThickness = std::clamp(thickness, 1.0f, 10.0f);

    // Above the hairline setting every segment becomes a camera-facing quad, so
    // the slider has a real effect on GLES drivers that ignore line width.
    if (safeThickness > 1.05f) {
        const float halfWidth = safeThickness *
            (constantScreenWidth ? kTracerHalfWidthPerThickness : kBoxHalfWidthPerThickness);
        s_tessBegin(tessellator, nullptr, 1, static_cast<int>(segments.size()) * 8, 0);
        setTessellatorColor(tessellator, rgb, alpha);
        for (const auto& segment : segments) {
            std::array<bedrocktools::sdk::Vec3, 4> quad{};
            const bool built = constantScreenWidth
                ? blockoutline::makeTaperedBeam(
                      segment, camera,
                      blockoutline::screenConstantHalfWidth(halfWidth, segment.from, camera,
                                                            storageesp::kTracerWidthFloor),
                      blockoutline::screenConstantHalfWidth(halfWidth, segment.to, camera,
                                                            storageesp::kTracerWidthFloor),
                      quad)
                : blockoutline::makeEdgeBeam(segment, camera, halfWidth, quad);
            if (!built) continue;
            for (const auto& vertex : quad) {
                s_tessVertex(tessellator, vertex.x, vertex.y, vertex.z);
            }
            // Both windings keep the strip visible with materials that
            // enable back-face culling.
            for (int i = 3; i >= 0; --i) {
                s_tessVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
            }
        }
        flushMesh(screenContext, tessellator, material);
    }

    // A final native line pass keeps distant segments crisp and is the complete
    // renderer for Thickness = 1.
    s_tessBegin(tessellator, nullptr, 4, static_cast<int>(segments.size()) * 2, 0);
    setTessellatorColor(tessellator, rgb, alpha);
    for (const auto& segment : segments) {
        emitVertex(tessellator, segment.from, camera);
        emitVertex(tessellator, segment.to, camera);
    }
    flushMesh(screenContext, tessellator, material);
}

void drawOutline(void* screenContext,
                 void* tessellator,
                 void* material,
                 const std::vector<blockoutline::Box>& boxes,
                 const bedrocktools::sdk::Vec3& camera,
                 std::uint32_t rgb,
                 float alpha,
                 float thickness) {
    if (boxes.empty() || alpha <= 0.001f) return;

    static thread_local std::vector<blockoutline::Edge> segments;
    blockoutline::collectBoxEdges(boxes, segments);
    drawSegments(screenContext, tessellator, material, segments, camera, rgb, alpha, thickness,
                 /*constantScreenWidth=*/false);
}

// One line per container of a highlight group, from the configured tracer
// origin to the middle of the box that container is drawn with. Segments that
// cannot be seen (container behind the eye plane) are dropped before anything
// is submitted, and a start sitting on the camera is pulled down the line, so
// no degenerate vertex ever reaches the driver.
void drawTracers(void* screenContext,
                 void* tessellator,
                 void* material,
                 const std::vector<storageesp::OverlayTarget>& targets,
                 storageesp::StorageKind kind,
                 const bedrocktools::sdk::Vec3& origin,
                 const storageesp::TracerView& view,
                 bool modelSized,
                 std::uint32_t rgb,
                 float alpha,
                 float thickness) {
    if (targets.empty() || alpha <= 0.001f) return;

    static thread_local std::vector<blockoutline::Edge> segments;
    storageesp::collectTracers(targets, kind, origin, modelSized, view, segments);
    drawSegments(screenContext, tessellator, material, segments, view.camera, rgb, alpha, thickness,
                 /*constantScreenWidth=*/true);
}

void renderStorageEsp(void* levelRenderer, void* screenContext) {
    if (!g_storageEsp || !g_storageEsp->enabled) return;
    if (!levelRenderer || reinterpret_cast<std::uintptr_t>(levelRenderer) < 0x1000 ||
        !screenContext || reinterpret_cast<std::uintptr_t>(screenContext) < 0x1000) {
        return;
    }
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!g_storageEsp->outline && !g_storageEsp->fill && !g_storageEsp->tracer) return;

    const auto screenAddress = reinterpret_cast<std::uintptr_t>(screenContext);
    const std::uintptr_t tessellatorAddress = *reinterpret_cast<std::uintptr_t*>(
        screenAddress + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (tessellatorAddress < 0x1000) return;
    void* tessellator = reinterpret_cast<void*>(tessellatorAddress);

    const auto rendererAddress = reinterpret_cast<std::uintptr_t>(levelRenderer);
    const std::uintptr_t playerRenderer = *reinterpret_cast<std::uintptr_t*>(
        rendererAddress + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (playerRenderer < 0x1000) return;

    const bedrocktools::sdk::Vec3 camera = *reinterpret_cast<const bedrocktools::sdk::Vec3*>(
        playerRenderer + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);

    std::shared_ptr<const FoundBlocks> snapshot;
    PublishedView view;
    {
        std::lock_guard<std::mutex> lock(s_publishMutex);
        snapshot = s_published;
        view = s_publishedView;
    }
    if (!snapshot || snapshot->empty()) return;

    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    const float pulse = blockoutline::pulseMultiplier(g_storageEsp->pulse, seconds,
                                                      g_storageEsp->pulseSpeed);

    static thread_local std::vector<storageesp::OverlayTarget> targets;
    storageesp::collectVisible(*snapshot,
                               camera,
                               static_cast<float>(std::clamp(g_storageEsp->scanRadius, kMinScanRadius, kMaxScanRadius)),
                               static_cast<std::size_t>(std::max(g_storageEsp->maxBoxes, 1)),
                               g_storageEsp->filter(),
                               targets);
    if (targets.empty()) return;

    ensureMaterials();
    void* embeddedSelectionOverlay = reinterpret_cast<void*>(
        playerRenderer + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);
    void* outlineMaterial = s_selectionMaterial
        ? static_cast<void*>(&s_selectionMaterial)
        : embeddedSelectionOverlay;
    // The embedded selection-overlay material blends vertex alpha and is a
    // better fit for translucent faces than selection_box. Through Walls uses
    // the same no-depth material for both passes so their occlusion agrees.
    void* fillMaterial = embeddedSelectionOverlay;
    if (g_storageEsp->throughWalls && s_throughWallsMaterial) {
        outlineMaterial = static_cast<void*>(&s_throughWallsMaterial);
        fillMaterial = static_cast<void*>(&s_throughWallsMaterial);
    }
    if (!outlineMaterial || !fillMaterial) return;

    const std::uintptr_t colorHolderAddress = *reinterpret_cast<std::uintptr_t*>(
        screenAddress + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (colorHolderAddress < 0x1000) return;
    auto* colorHolder = reinterpret_cast<float*>(colorHolderAddress);
    const float savedColor[4] = {
        colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3],
    };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    // One batch per highlight group, so the colors stay independent while the
    // number of tessellator submissions stays small.
    storageesp::TracerView tracerView;
    tracerView.camera = camera;
    if (view.rotationValid) tracerView.forward = storageesp::viewForward(view.rotation);
    const auto tracerOrigin = storageesp::tracerOriginPoint(
        static_cast<storageesp::TracerOrigin>(g_storageEsp->tracerOrigin), camera, view.feet);
    for (std::size_t index = 1; index < storageesp::kindCount; ++index) {
        const auto kind = static_cast<storageesp::StorageKind>(index);
        if (!storageesp::enabled(g_storageEsp->filter(), kind)) continue;

        const auto boxes = storageesp::boxesForKind(targets, kind, kBoxExpansion,
                                                    g_storageEsp->modelSizedBoxes);
        if (boxes.empty()) continue;

        const std::uint32_t rgb = blockoutline::animatedRgb(
            g_storageEsp->colorFor(kind), g_storageEsp->rainbow, seconds,
            g_storageEsp->rainbowSpeed);
        if (g_storageEsp->fill) {
            drawFill(screenContext, tessellator, fillMaterial, boxes, camera, rgb,
                     blockoutline::clampedOpacity(g_storageEsp->fillOpacity, pulse));
        }
        if (g_storageEsp->outline) {
            drawOutline(screenContext, tessellator, outlineMaterial, boxes, camera, rgb,
                        blockoutline::clampedOpacity(1.0f, pulse), g_storageEsp->lineThickness);
        }
        if (g_storageEsp->tracer) {
            drawTracers(screenContext, tessellator, outlineMaterial, targets, kind,
                        tracerOrigin, tracerView, g_storageEsp->modelSizedBoxes, rgb,
                        blockoutline::clampedOpacity(g_storageEsp->tracerOpacity, pulse),
                        g_storageEsp->tracerThickness);
        }
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

void renderLevelHook(void* levelRenderer, void* screenContext, void* renderParams) {
    if (s_renderLevelOriginal) s_renderLevelOriginal(levelRenderer, screenContext, renderParams);
    renderStorageEsp(levelRenderer, screenContext);
}

template <typename T>
bool readFirst(const nlohmann::json& json,
               std::initializer_list<const char*> keys,
               T& destination) {
    for (const char* key : keys) {
        if (!json.contains(key)) continue;
        try {
            destination = json[key].get<T>();
            return true;
        } catch (...) {
        }
    }
    return false;
}

bool readColor(const nlohmann::json& json,
               std::initializer_list<const char*> keys,
               std::uint32_t& destination) {
    for (const char* key : keys) {
        if (!json.contains(key) || !json[key].is_string()) continue;
        std::string text;
        try {
            text = json[key].get<std::string>();
        } catch (...) {
            continue;
        }
        if (text.empty()) continue;
        if (text[0] == '#') {
            text.erase(0, 1);
        } else if (text.size() > 1 && text[0] == '0' &&
                   (text[1] == 'x' || text[1] == 'X')) {
            text.erase(0, 2);
        }
        try {
            const auto parsed = static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
            destination = 0xFF000000u | (parsed & 0x00FFFFFFu);
            return true;
        } catch (...) {
        }
    }
    return false;
}

std::string colorString(std::uint32_t color) {
    char text[10]{};
    std::snprintf(text, sizeof(text), "#%06X", color & 0x00FFFFFFu);
    return text;
}

} // namespace

StorageEspModule::StorageEspModule()
    : Module("Storage ESP",
             "Highlights chests, copper chests, trapped chests, ender chests, shulker boxes, barrels, hoppers, furnaces and dispensers around you with per-category ESP boxes, optional tracer lines, optionally through walls.") {
    showInMenu = true;
    hideInHudEditor = true;
    g_storageEsp = this;
}

StorageEspModule::~StorageEspModule() {
    if (g_storageEsp == this) g_storageEsp = nullptr;
}

void StorageEspModule::clampSettings() {
    scanRadius = std::clamp(scanRadius, kMinScanRadius, kMaxScanRadius);
    scanHeight = std::clamp(scanHeight, kMinScanHeight, kMaxScanHeight);
    maxBoxes = std::clamp(maxBoxes, 1, 200);
    lineThickness = std::clamp(lineThickness, 1.0f, 10.0f);
    fillOpacity = std::clamp(fillOpacity, 0.0f, 1.0f);
    tracerThickness = std::clamp(tracerThickness, 1.0f, 10.0f);
    tracerOpacity = std::clamp(tracerOpacity, 0.0f, 1.0f);
    rainbowSpeed = std::clamp(rainbowSpeed, 0.05f, 1.0f);
    pulseSpeed = std::clamp(pulseSpeed, 0.05f, 1.0f);
    if (scanSpeed < 0 || static_cast<std::size_t>(scanSpeed) >= storageesp::kScanSpeedCount) {
        scanSpeed = storageesp::kDefaultScanSpeed;
    }
    if (tracerOrigin < 0 ||
        static_cast<std::size_t>(tracerOrigin) >= storageesp::kTracerOriginCount) {
        tracerOrigin = storageesp::kDefaultTracerOrigin;
    }

    // The scan is what these settings describe, so ask for a restart whenever a
    // value changed while a sweep was running. The tick applies it.
    if (s_scan.region.blockRadius != scanRadius || s_scan.region.verticalRadius != scanHeight) {
        s_scanRevision.fetch_add(1);
    }
}

void StorageEspModule::onInit() {
    const std::uintptr_t renderLevel =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (renderLevel) m_patchTarget = reinterpret_cast<void*>(renderLevel);

    const std::uintptr_t getBlock =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock);
    if (getBlock) s_getBlock = reinterpret_cast<BlockSourceGetBlockFn>(getBlock);

    const std::uintptr_t tessBegin =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    const std::uintptr_t tessColor =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    const std::uintptr_t tessVertex =
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tessBegin) s_tessBegin = reinterpret_cast<TessellatorBeginFn>(tessBegin);
    if (tessColor) s_tessColor = reinterpret_cast<TessellatorColorFn>(tessColor);
    if (tessVertex) s_tessVertex = reinterpret_cast<TessellatorVertexFn>(tessVertex);

    std::uintptr_t renderMesh = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!renderMesh) {
        renderMesh = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
    }
    if (renderMesh) s_renderMesh = reinterpret_cast<RenderMeshImmediatelyFn>(renderMesh);

    const std::uintptr_t materialGroup = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (materialGroup) {
        const std::uintptr_t groupAddress = resolveAdrp(
            reinterpret_cast<std::uint32_t*>(materialGroup), 2, 0);
        if (groupAddress) {
            s_renderMaterialGroup = groupAddress +
                bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    clampSettings();

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { scanStep(event.player); });
}

void StorageEspModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    const auto handle = bedrocktools::hooks::install(
        m_patchTarget,
        reinterpret_cast<void*>(&renderLevelHook),
        reinterpret_cast<void**>(&s_renderLevelOriginal));
    m_patched = handle != nullptr;
}

void StorageEspModule::onEnable() {
    // Positions found in a previous world (or with different settings) must not
    // be trusted; the first sweep after enabling fills the cache again.
    publishCleared();
    s_scanRevision.fetch_add(1);
    s_resetRequested.store(true);
    applyPatch();
}

void StorageEspModule::onDisable() {
    publishCleared();
    s_resetRequested.store(true);
}

void StorageEspModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);

    readFirst(json, {"showChests"}, showChests);
    readColor(json, {"showChestsColor"}, showChestsColor);
    readFirst(json, {"showCopperChests"}, showCopperChests);
    readColor(json, {"showCopperChestsColor"}, showCopperChestsColor);
    readFirst(json, {"showTrappedChests"}, showTrappedChests);
    readColor(json, {"showTrappedChestsColor"}, showTrappedChestsColor);
    readFirst(json, {"showEnderChests"}, showEnderChests);
    readColor(json, {"showEnderChestsColor"}, showEnderChestsColor);
    readFirst(json, {"showShulkerBoxes"}, showShulkerBoxes);
    readColor(json, {"showShulkerBoxesColor"}, showShulkerBoxesColor);
    readFirst(json, {"showBarrels"}, showBarrels);
    readColor(json, {"showBarrelsColor"}, showBarrelsColor);
    readFirst(json, {"showHoppers"}, showHoppers);
    readColor(json, {"showHoppersColor"}, showHoppersColor);
    readFirst(json, {"showFurnaces"}, showFurnaces);
    readColor(json, {"showFurnacesColor"}, showFurnacesColor);
    readFirst(json, {"showDispensers"}, showDispensers);
    readColor(json, {"showDispensersColor"}, showDispensersColor);

    readFirst(json, {"outline", "showOutline"}, outline);
    readFirst(json, {"lineThickness", "thickness"}, lineThickness);
    readFirst(json, {"fill", "showFill"}, fill);
    readFirst(json, {"fillOpacity", "opacity"}, fillOpacity);
    readFirst(json, {"modelSizedBoxes", "tightBoxes"}, modelSizedBoxes);
    readFirst(json, {"throughWalls", "xray"}, throughWalls);
    readFirst(json, {"tracer", "showTracers"}, tracer);
    readFirst(json, {"tracerThickness", "tracerWidth"}, tracerThickness);
    readFirst(json, {"tracerOpacity"}, tracerOpacity);
    readFirst(json, {"rainbow"}, rainbow);
    readFirst(json, {"rainbowSpeed"}, rainbowSpeed);
    readFirst(json, {"pulse"}, pulse);
    readFirst(json, {"pulseSpeed"}, pulseSpeed);

    readFirst(json, {"scanRadius", "radius"}, scanRadius);
    readFirst(json, {"scanHeight", "verticalRadius"}, scanHeight);
    readFirst(json, {"maxBoxes", "maxHighlights"}, maxBoxes);

    if (json.contains("scanSpeed")) {
        const nlohmann::json& speed = json["scanSpeed"];
        if (speed.is_string()) {
            scanSpeed = storageesp::resolveScanSpeed(speed.get<std::string>());
        } else {
            int index = storageesp::kDefaultScanSpeed;
            readFirst(json, {"scanSpeed"}, index);
            scanSpeed = index;
        }
    }

    if (json.contains("tracerOrigin")) {
        const nlohmann::json& origin = json["tracerOrigin"];
        if (origin.is_string()) {
            tracerOrigin = storageesp::resolveTracerOrigin(origin.get<std::string>());
        } else {
            int index = storageesp::kDefaultTracerOrigin;
            readFirst(json, {"tracerOrigin"}, index);
            tracerOrigin = index;
        }
    }

    for (std::uint32_t* color : {&showChestsColor, &showCopperChestsColor, &showTrappedChestsColor,
                                 &showEnderChestsColor, &showShulkerBoxesColor, &showBarrelsColor,
                                 &showHoppersColor, &showFurnacesColor, &showDispensersColor}) {
        *color = 0xFF000000u | (*color & 0x00FFFFFFu);
    }

    clampSettings();
}

void StorageEspModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);

    json["showChests"] = showChests;
    json["showChestsColor"] = colorString(showChestsColor);
    json["showCopperChests"] = showCopperChests;
    json["showCopperChestsColor"] = colorString(showCopperChestsColor);
    json["showTrappedChests"] = showTrappedChests;
    json["showTrappedChestsColor"] = colorString(showTrappedChestsColor);
    json["showEnderChests"] = showEnderChests;
    json["showEnderChestsColor"] = colorString(showEnderChestsColor);
    json["showShulkerBoxes"] = showShulkerBoxes;
    json["showShulkerBoxesColor"] = colorString(showShulkerBoxesColor);
    json["showBarrels"] = showBarrels;
    json["showBarrelsColor"] = colorString(showBarrelsColor);
    json["showHoppers"] = showHoppers;
    json["showHoppersColor"] = colorString(showHoppersColor);
    json["showFurnaces"] = showFurnaces;
    json["showFurnacesColor"] = colorString(showFurnacesColor);
    json["showDispensers"] = showDispensers;
    json["showDispensersColor"] = colorString(showDispensersColor);

    json["outline"] = outline;
    json["lineThickness"] = std::clamp(lineThickness, 1.0f, 10.0f);
    json["fill"] = fill;
    json["fillOpacity"] = std::clamp(fillOpacity, 0.0f, 1.0f);
    json["modelSizedBoxes"] = modelSizedBoxes;
    json["throughWalls"] = throughWalls;
    json["tracer"] = tracer;
    json["tracerThickness"] = std::clamp(tracerThickness, 1.0f, 10.0f);
    json["tracerOpacity"] = std::clamp(tracerOpacity, 0.0f, 1.0f);
    json["rainbow"] = rainbow;
    json["rainbowSpeed"] = std::clamp(rainbowSpeed, 0.05f, 1.0f);
    json["pulse"] = pulse;
    json["pulseSpeed"] = std::clamp(pulseSpeed, 0.05f, 1.0f);

    json["scanRadius"] = std::clamp(scanRadius, kMinScanRadius, kMaxScanRadius);
    json["scanHeight"] = std::clamp(scanHeight, kMinScanHeight, kMaxScanHeight);
    json["maxBoxes"] = std::clamp(maxBoxes, 1, 200);
    json["scanSpeed"] = storageesp::scanSpeedRadioValue(scanSpeed);
    json["tracerOrigin"] = storageesp::tracerOriginRadioValue(tracerOrigin);
}
