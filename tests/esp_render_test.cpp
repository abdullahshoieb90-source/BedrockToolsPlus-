// Integration-style host test for the Esp overlay render path.
//
// The overlay is world-space geometry handed to the game's renderer (the same
// pass the Hitbox module draws in): the boxes, the tracers and the distance
// readouts. This test drives the real render hook with small in-memory
// Minecraft stand-ins -- a local player, a mob, the camera the level is
// rendered with, a ScreenContext and its tessellator -- and inspects both
// halves of the overlay: the tessellator calls the geometry turns into, and
// the draw commands the HUD layer (nametag, health) submits.
//
// The cases mirror the bug reports the module has to keep getting right:
//
//   * the geometry sits on the entity, in world space, relative to the camera
//     the level was drawn with;
//   * turning the view cannot move it, because the module no longer projects
//     it (this is the regression the screen-space box, tracer and distance
//     readout used to have);
//   * the overlay keeps drawing through walls, and only hides behind terrain
//     when Through Walls is switched off.
//
// Build: g++ -std=c++20 -I include -I src -I tests/esp_fakepl -I tests/fakejson
//        tests/esp_render_test.cpp -o /tmp/esp_render_test
// Run:   /tmp/esp_render_test

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <pl/ModMenu.hpp>

#include "bedrocktools/events/EventBus.hpp"
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/sdk/Offsets.hpp"
#include "bedrocktools/sdk/Types.hpp"
#include "core/Runtime.hpp"

namespace {

constexpr float kSurfaceWidth = 1000.0f;
constexpr float kSurfaceHeight = 1000.0f;

// Per-actor vertex budgets the assertions below use. The wireframe is twelve
// edges * two vertices; the world-space distance readout for "10.0m" is
// fifteen merged glyph rectangles * four corners * two windings ('1' = 2,
// '0' = 4, '.' = 1, '0' = 4, 'm' = 4).
constexpr std::size_t kWireVertsPerActor = 24;
constexpr std::size_t kReadoutVertsForTenMeters = 15 * 8;

// Captured HUD-layer commands (nametags, distance, tracers, health bar).
std::vector<pl::modmenu::DrawCommand> g_commands;

// Captured world-space geometry: one entry per Tessellator flush, with the
// vertices (already camera-relative) and the primitive mode.
struct MeshBatch {
    int mode = -1;
    int reservedVertices = 0;
    std::vector<std::array<float, 3>> vertices;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 0.0f};
};
std::vector<MeshBatch> g_batches;
MeshBatch g_currentBatch;

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

// ---- Fake game functions ---------------------------------------------------
// The ESP module resolves these by signature; the pointers it calls go here.
// They live in a named namespace so the module's own anonymous-namespace
// declarations (which land in this one once esp.cpp is included below) cannot
// collide with them.

namespace fake {

bool g_actorIsPlayer = false;
bool actorIsPlayer(void*) { return g_actorIsPlayer; }
bool g_actorIsInvisible = false;
bool actorIsInvisible(void*) { return g_actorIsInvisible; }

// Layout-compatible stand-ins for the module's own DistanceSortedActor /
// ActorVec.
struct FetchedActor {
    void* mActor;
    float mDistance;
    float _pad;
};

struct FetchedActorVec {
    FetchedActor* begin;
    FetchedActor* end;
    FetchedActor* cap;
};

constexpr int kFetchedCapacity = 80;
FetchedActor g_fetched[kFetchedCapacity];
int g_fetchedCount = 0;

FetchedActorVec fetchNearby(void*, void*, int) {
    FetchedActorVec out{};
    out.begin = g_fetched;
    out.end = g_fetched + g_fetchedCount;
    out.cap = g_fetched + kFetchedCapacity;
    return out;
}

// Wall probe. `g_solidBlocks` lets a case decide whether *every* voxel counts
// as solid, which is all the occlusion cull needs to be exercised.
bool g_solidBlocks = false;
struct TestBlockPos {
    int x, y, z;
};
bool isSolidBlockingBlock(void*, const TestBlockPos&) { return g_solidBlocks; }

void renderLevel(void*, void*, void*) {}

void tessellatorBegin(void*, void*, int mode, int vertexCount, int) {
    g_currentBatch = MeshBatch{};
    g_currentBatch.mode = mode;
    g_currentBatch.reservedVertices = vertexCount;
}
void tessellatorColor(void*, float r, float g, float b, float a) {
    g_currentBatch.color = {r, g, b, a};
}
void tessellatorVertex(void*, float x, float y, float z) {
    g_currentBatch.vertices.push_back({x, y, z});
}
void renderMeshImmediately(void*, void*, void*, char*) {
    g_batches.push_back(g_currentBatch);
    g_currentBatch = MeshBatch{};
}

} // namespace fake

} // namespace

// The fake preloader declares these; the test defines them so it can control
// the surface size and capture what the module draws.
pl::modmenu::HudSurfaceSize pl::modmenu::getHudSurfaceSize() {
    return {kSurfaceWidth, kSurfaceHeight};
}

void pl::modmenu::submitDrawCommands(std::string_view, std::span<const pl::modmenu::DrawCommand> commands) {
    g_commands.assign(commands.begin(), commands.end());
}

// The module asks the runtime for the package's resource directory, because the
// nametag font has to be registered before a label may be measured in it (see
// core/PixelFont.hpp). There is no packaged font on the host, so the answer is
// the empty path and the module stays on the launcher's own face until a case
// below switches the module's cached answer by hand.
namespace bedrocktools::core {
Runtime& Runtime::get() {
    static Runtime instance;
    return instance;
}
const std::filesystem::path& Runtime::resourceDirectory() const noexcept {
    static const std::filesystem::path empty;
    return empty;
}
} // namespace bedrocktools::core

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId id) {
    switch (id) {
        case SignatureId::ActorIsPlayer:
            return reinterpret_cast<std::uintptr_t>(&fake::actorIsPlayer);
        case SignatureId::ActorIsInvisible:
            return reinterpret_cast<std::uintptr_t>(&fake::actorIsInvisible);
        case SignatureId::ActorFetchNearbyActorsSorted:
            return reinterpret_cast<std::uintptr_t>(&fake::fetchNearby);
        case SignatureId::BlockSourceIsSolidBlockingBlock:
            return reinterpret_cast<std::uintptr_t>(&fake::isSolidBlockingBlock);
        case SignatureId::RenderLevel:
            return reinterpret_cast<std::uintptr_t>(&fake::renderLevel);
        case SignatureId::TessellatorBegin:
            return reinterpret_cast<std::uintptr_t>(&fake::tessellatorBegin);
        case SignatureId::TessellatorColor:
            return reinterpret_cast<std::uintptr_t>(&fake::tessellatorColor);
        case SignatureId::TessellatorVertex:
            return reinterpret_cast<std::uintptr_t>(&fake::tessellatorVertex);
        case SignatureId::MeshHelpersRenderMeshImmediately2:
            return reinterpret_cast<std::uintptr_t>(&fake::renderMeshImmediately);
        default:
            // No material group (the overlay then uses the material the
            // renderer already owns) and no perspective hook (so the
            // local-player ESP stays hidden), which is what these tests want.
            return 0;
    }
}
} // namespace bedrocktools::memory

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

// Include the production module so this test can invoke its render hook and
// its HUD-label pass without adding a test-only API to the module.
#include "modules/visual/esp.cpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::printf("  ok   %s\n", message);
    } else {
        std::printf("  FAIL %s\n", message);
        ++g_failures;
    }
}

bool near(float a, float b, float epsilon = 0.01f) {
    return std::fabs(a - b) <= epsilon;
}

int lineBatchCount() {
    int count = 0;
    for (const auto& batch : g_batches) {
        if (batch.mode == 4) ++count;
    }
    return count;
}

// Vertices of the line batches (the wireframe, plus the world-space tracers
// once the tracer option is on).
std::vector<std::array<float, 3>> lineBatchVertices() {
    std::vector<std::array<float, 3>> out;
    for (const auto& batch : g_batches) {
        if (batch.mode == 4) {
            out.insert(out.end(), batch.vertices.begin(), batch.vertices.end());
        }
    }
    return out;
}

// Vertices of the quad batches (the box fill, plus the world-space distance
// readouts).
std::vector<std::array<float, 3>> quadBatchVertices() {
    std::vector<std::array<float, 3>> out;
    for (const auto& batch : g_batches) {
        if (batch.mode == 1) {
            out.insert(out.end(), batch.vertices.begin(), batch.vertices.end());
        }
    }
    return out;
}

int quadBatchCount() {
    int count = 0;
    for (const auto& batch : g_batches) {
        if (batch.mode == 1) ++count;
    }
    return count;
}

// The last line batch (GL_LINES), i.e. the tracer batch once tracers are on:
// the wireframe flushes first, the tracer right after it.
const MeshBatch* lastLineBatch() {
    const MeshBatch* out = nullptr;
    for (const auto& batch : g_batches) {
        if (batch.mode == 4) out = &batch;
    }
    return out;
}

// The last quad batch (GL_QUADS): the distance readout, which flushes after
// the box fill (see the flush order in drawWorldOverlay).
const MeshBatch* distanceBatch() {
    const MeshBatch* out = nullptr;
    for (const auto& batch : g_batches) {
        if (batch.mode == 1) out = &batch;
    }
    return out;
}

// Every vertex of every batch, flattened, for geometry-wide assertions.
std::vector<std::array<float, 3>> allVertices() {
    std::vector<std::array<float, 3>> out;
    for (const auto& batch : g_batches) {
        out.insert(out.end(), batch.vertices.begin(), batch.vertices.end());
    }
    return out;
}

bool anyVertexCloseTo(const std::vector<std::array<float, 3>>& vertices,
                      const bedrocktools::sdk::Vec3& world,
                      const bedrocktools::sdk::Vec3& camera) {
    const bedrocktools::sdk::Vec3 expected{world.x - camera.x, world.y - camera.y,
                                           world.z - camera.z};
    return std::any_of(vertices.begin(), vertices.end(), [&](const auto& vertex) {
        return near(vertex[0], expected.x) && near(vertex[1], expected.y) &&
               near(vertex[2], expected.z);
    });
}

// True when every vertex of the batch sits below a world-space height (the
// entity's feet), i.e. the readout hangs under the hitbox.
bool allVerticesBelow(const MeshBatch& batch, float worldY,
                      const bedrocktools::sdk::Vec3& camera) {
    return std::all_of(batch.vertices.begin(), batch.vertices.end(),
                       [&](const auto& vertex) {
                           return vertex[1] + camera.y < worldY;
                       });
}

// The HUD lines the module submits (the crosshair-origin tracer). The launcher
// takes the start point from x/y and the *delta* to the end point from w/h, so
// the end point of a captured line is (x + w, y + h).
std::vector<pl::modmenu::DrawCommand> lineCommands() {
    std::vector<pl::modmenu::DrawCommand> out;
    for (const auto& cmd : g_commands) {
        if (cmd.type == pl::modmenu::DrawCommandType::Line) out.push_back(cmd);
    }
    return out;
}

// The HUD text commands, in submission order.
std::vector<const pl::modmenu::DrawCommand*> textCommands() {
    std::vector<const pl::modmenu::DrawCommand*> out;
    for (const auto& cmd : g_commands) {
        if (cmd.type == pl::modmenu::DrawCommandType::Text) out.push_back(&cmd);
    }
    return out;
}

int textCommandCount() {
    int count = 0;
    for (const auto& cmd : g_commands) {
        if (cmd.type == pl::modmenu::DrawCommandType::Text) ++count;
    }
    return count;
}

// The HUD text command drawing `text` (the last one wins), or nullptr. Used by
// the cases that care about how a label was measured rather than that it exists.
const pl::modmenu::DrawCommand* findText(const std::string& text) {
    for (auto it = g_commands.rbegin(); it != g_commands.rend(); ++it) {
        if (it->type == pl::modmenu::DrawCommandType::Text && it->text == text) {
            return &*it;
        }
    }
    return nullptr;
}

// The screen-space box is gone; the only 2D rectangle the HUD layer still
// draws is the health bar, so a RectFilled command means "the bar is up".
int filledRectCount() {
    int count = 0;
    for (const auto& cmd : g_commands) {
        if (cmd.type == pl::modmenu::DrawCommandType::RectFilled) ++count;
    }
    return count;
}

// Places a player name (and the isPlayer flag) on the mob for the blocks that
// need a HUD label on screen. The distance readout is world geometry now, so
// a nametag is the representative HUD label these tests steer by.
struct PlayerNameGuard {
    std::string* field;

    explicit PlayerNameGuard(std::byte* actor, const char* name)
        : field(new (actor + bedrocktools::sdk::offsets::Player::mName)
                    std::string(name)) {
        fake::g_actorIsPlayer = true;
    }

    ~PlayerNameGuard() {
        fake::g_actorIsPlayer = false;
        std::destroy_at(field);
    }
};

} // namespace

int main() {
    using namespace bedrocktools::sdk;
    using namespace bedrocktools::sdk::offsets;

    std::printf("esp render integration\n");

    // --- Fake world --------------------------------------------------------
    alignas(std::max_align_t) std::array<std::byte, 0x800> player{};
    alignas(std::max_align_t) std::array<std::byte, 64> playerRotation{};
    alignas(std::max_align_t) std::array<std::byte, 64> playerState{};
    alignas(std::max_align_t) std::array<std::byte, 64> playerAabbComponent{};
    alignas(std::max_align_t) std::array<std::byte, 64> dimension{};
    alignas(std::max_align_t) std::array<std::byte, 64> blockSource{};

    // 0xC00 bytes: room for the Player name std::string at mName (2824).
    alignas(std::max_align_t) std::array<std::byte, 0xC00> mob{};
    alignas(std::max_align_t) std::array<std::byte, 64> mobAabbComponent{};
    alignas(std::max_align_t) std::array<std::byte, 64> mobHealthAttribute{};

    alignas(std::max_align_t) std::array<std::byte, 0x500> levelRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> playerRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x200> screenContext{};
    alignas(std::max_align_t) std::array<float, 4> colorHolder{0.2f, 0.3f, 0.4f, 0.5f};
    alignas(std::max_align_t) std::uint64_t tessellatorObject = 0;

    // Local player: rotation (the camera orientation) and a dimension for the
    // occlusion test.
    writeAt(player, Actor::mActorRotationComponent, static_cast<void*>(playerRotation.data()));
    writeAt(player, Actor::mStateVectorComponent, static_cast<void*>(playerState.data()));
    writeAt(player, Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent,
            static_cast<void*>(playerAabbComponent.data()));
    writeAt(player, Actor::mCategories, static_cast<std::uint32_t>(ActorCategories::IsPlayer));
    writeAt(player, Actor::mDimension, static_cast<void*>(dimension.data()));
    writeAt(dimension, Dimension::mBlockSource, static_cast<void*>(blockSource.data()));
    // The local player's own collision box: the distance readout measures
    // from its bottom-center (feet), so it has to be wired like the mob's.
    const AABB playerBounds{{-0.3f, 0.0f, -0.3f}, {0.3f, 1.8f, 0.3f}};
    writeAt(playerAabbComponent, AABBShapeComponent::mAABB, playerBounds);
    // Mob::mHealthAttribute stays null, so readHealth reports "no health".

    // Mob: an AABB shape component plus the mob category bit.
    writeAt(mob, Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent,
            static_cast<void*>(mobAabbComponent.data()));
    writeAt(mob, Actor::mCategories, static_cast<std::uint32_t>(ActorCategories::IsMob));

    // ClientInstance is unused by the render hook (it is handed the renderer),
    // so only the render-side chain has to be wired up.
    writeAt(levelRenderer, LevelRenderer::mLevelRendererPlayer,
            static_cast<void*>(playerRenderer.data()));
    const Vec3 cameraPosition{0.0f, 1.62f, 0.0f};
    writeAt(playerRenderer, LevelRendererPlayer::mCamPos, cameraPosition);
    writeAt(screenContext, ScreenContext::mTessellator, &tessellatorObject);
    writeAt(screenContext, ScreenContext::mColorHolder, colorHolder.data());

    auto setCameraRotation = [&](float pitch, float yaw) {
        const Vec2 rot{pitch, yaw};
        writeAt(playerRotation, 0, rot);
    };
    auto setMobBox = [&](const Vec3& min, const Vec3& max) {
        const AABB bounds{min, max};
        writeAt(mobAabbComponent, AABBShapeComponent::mAABB, bounds);
    };

    // --- Module ------------------------------------------------------------
    EspModule esp;
    esp.onInit();
    esp.setMasterEnabled(true);
    // 90 degrees vertical FOV on a square surface: tan(half fov) == 1 and the
    // aspect is 1, so 45 degrees off-axis lands exactly on the screen edge and
    // the expected label coordinates below stay readable.
    esp.fov = 90.0f;
    // Hairline wireframe: without it every edge would also emit a beam pass
    // and the vertex counts below would not describe the wire alone.
    esp.boxThickness = 1.0f;

    bedrocktools::events::LocalPlayerTickEvent tick{
        reinterpret_cast<bedrocktools::sdk::Player*>(player.data())};
    bedrocktools::events::bus().publish(tick);

    fake::g_fetched[0] = {mob.data(), 14.0f, 0.0f};
    fake::g_fetchedCount = 1;

    const float centerX = kSurfaceWidth * 0.5f;

    auto renderFrame = [&]() {
        g_batches.clear();
        g_currentBatch = MeshBatch{};
        g_commands.clear();
        renderOverlay(levelRenderer.data(), screenContext.data());
    };

    // --- The box is the entity's own AABB, in world space ------------------
    // Camera at the origin facing south (yaw 0); the mob sits east (+X) and
    // ahead. The overlay is emitted relative to the camera, so the mob's box
    // corners have to come out as (world - camera) with no projection in
    // between -- which is precisely what used to drift.
    {
        setCameraRotation(0.0f, 0.0f);
        const Vec3 mobMin{2.7f, 0.0f, 9.7f};
        const Vec3 mobMax{3.3f, 1.8f, 10.3f};
        setMobBox(mobMin, mobMax);
        renderFrame();

        const int lines = lineBatchCount();
        check(lines == 1, "the box style draws one wireframe batch");
        const auto wire = lineBatchVertices();
        check(wire.size() == kWireVertsPerActor,
              "twelve box edges emit twenty-four vertices");
        check(anyVertexCloseTo(wire, mobMin, cameraPosition) &&
                  anyVertexCloseTo(wire, mobMax, cameraPosition),
              "the box corners are the entity AABB corners, camera-relative");

        // Hairline by default: nothing but the line list for the box.
        check(g_batches[0].mode == 4,
              "thickness 1 keeps the box a crisp line pass");

        // The distance readout left the HUD layer: it is world geometry
        // under the entity's feet now, pinned by the game like the box. The
        // mob is sqrt(109) = 10.4 blocks away, and "10.4m" is fourteen glyph
        // rectangles ('4' merges into three).
        const MeshBatch* readout = distanceBatch();
        check(readout != nullptr && readout->vertices.size() == 14 * 8,
              "the distance readout is world geometry (10.4m = 14 glyph rects)");
        check(readout != nullptr &&
                  allVerticesBelow(*readout, 0.0f, cameraPosition),
              "and it hangs below the entity's feet, not next to the hitbox");
        check(textCommandCount() == 0,
              "no HUD text is left for a nameless, healthless mob");
    }

    // --- Turning the view cannot move the geometry --------------------------
    // The old screen-space box re-projected every frame from the module's own
    // camera model, so a mismatch there read as "the box moves when I move the
    // screen". World-space edges are the same numbers whatever the view does.
    // The tracer and the distance readout used to slide off the hitbox the
    // same way from the HUD layer; both are world geometry now, so this also
    // pins their anchors while the view turns.
    {
        PlayerNameGuard name(mob.data(), "Steve"); // a HUD label to steer by
        setMobBox({0.7f, 0.0f, 9.7f}, {1.3f, 1.8f, 10.3f});

        setCameraRotation(0.0f, 0.0f);
        renderFrame();
        const auto levelWire = lineBatchVertices();
        const auto levelReadout = quadBatchVertices();
        const auto levelLabels = g_commands;

        setCameraRotation(-20.0f, 15.0f); // look up and turn right, mob still on screen
        renderFrame();
        const auto turnedWire = lineBatchVertices();
        const auto turnedReadout = quadBatchVertices();

        check(levelWire.size() == turnedWire.size() && !levelWire.empty(),
              "the box is drawn at every view angle");
        check(levelWire == turnedWire,
              "the box geometry is identical while the view turns");
        check(turnedReadout.size() == levelReadout.size() && !levelReadout.empty(),
              "the distance readout is drawn at every view angle");

        // The readout stays under the hitbox: its world anchor is the
        // entity's feet (x=1, z=10, just under y=0), so turning the view may
        // only tilt and resize the billboard, never move it off the box.
        // (The HUD projection this replaces moved the readout by whole box
        // widths whenever its camera model disagreed with the game's.)
        auto centroid = [](const std::vector<std::array<float, 3>>& vertices) {
            std::array<float, 3> sum{0.0f, 0.0f, 0.0f};
            for (const auto& vertex : vertices) {
                sum[0] += vertex[0];
                sum[1] += vertex[1];
                sum[2] += vertex[2];
            }
            if (vertices.empty()) return sum;
            return std::array<float, 3>{sum[0] / vertices.size(),
                                        sum[1] / vertices.size(),
                                        sum[2] / vertices.size()};
        };
        const auto levelCenter = centroid(levelReadout);
        const auto turnedCenter = centroid(turnedReadout);
        const float drift = std::sqrt(
            (levelCenter[0] - turnedCenter[0]) * (levelCenter[0] - turnedCenter[0]) +
            (levelCenter[1] - turnedCenter[1]) * (levelCenter[1] - turnedCenter[1]) +
            (levelCenter[2] - turnedCenter[2]) * (levelCenter[2] - turnedCenter[2]));
        check(drift < 0.25f,
              "turning the view cannot slide the readout off the hitbox");
        check(levelCenter[0] > 0.5f && levelCenter[0] < 1.5f &&
                  levelCenter[2] > 9.5f && levelCenter[2] < 10.5f,
              "the readout stays centered under the entity's feet");

        check(g_commands.size() == levelLabels.size(),
              "the labels keep being drawn while the view turns");
        check(!levelLabels.empty() && !g_commands.empty() &&
                  std::fabs(g_commands.back().x - levelLabels.back().x) > 1.0f,
              "the labels do follow the view, which is what a projection is for");
    }

    // --- Through walls is the default --------------------------------------
    // A wall of solid blocks between the camera and the mob must not hide the
    // overlay; that is the whole point of the ESP layer being drawn with a
    // material that never consults the depth buffer.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({-0.3f, 0.0f, 9.7f}, {0.3f, 1.8f, 10.3f});
        check(esp.throughWalls, "Through Walls is on by default");

        fake::g_solidBlocks = true;
        renderFrame();
        check(allVertices().size() == kWireVertsPerActor + kReadoutVertsForTenMeters,
              "an actor behind a wall is still drawn, readout included");
        check(textCommandCount() == 0,
              "and nothing needs the HUD layer for it anymore");
    }

    // --- Through Walls off: the occlusion cull takes over ------------------
    {
        esp.throughWalls = false;
        renderFrame();
        check(g_batches.empty() && g_commands.empty(),
              "with Through Walls off, a walled-off actor is culled entirely");

        fake::g_solidBlocks = false;
        renderFrame();
        check(allVertices().size() == kWireVertsPerActor + kReadoutVertsForTenMeters,
              "and it comes back as soon as the raycast is clear");

        fake::g_solidBlocks = true;
        esp.throughWalls = true;
        renderFrame();
        check(allVertices().size() == kWireVertsPerActor + kReadoutVertsForTenMeters,
              "the cull never runs while Through Walls is on, even with a region");
    }

    // --- Distance is measured from the player, not the camera ---------------
    // The readout is the feet-to-feet distance between the two collision
    // boxes, so it cannot depend on the perspective: pulling the camera back
    // into third person must not change what the same entity reports. The
    // value itself is not readable from the geometry (it is quads now), but
    // its shape is: "10.0m" is exactly fifteen glyph rectangles.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({-0.3f, 0.0f, 9.7f}, {0.3f, 1.8f, 10.3f});
        esp.throughWalls = true;
        fake::g_solidBlocks = false;

        renderFrame();
        const MeshBatch* readout = distanceBatch();
        check(readout != nullptr &&
                  readout->vertices.size() == kReadoutVertsForTenMeters,
              "ten blocks away reads as the five-glyph 10.0m readout");

        // The readout hangs just under the hitbox: every vertex below the
        // entity's feet, spread around the feet column, not floating in a
        // screen-space stack above the box.
        check(readout != nullptr &&
                  allVerticesBelow(*readout, 0.0f, cameraPosition),
              "the readout sits just under the hitbox");

        // Third-person boom: the camera swings several blocks behind and
        // above the player while the player itself does not move.
        const Vec3 thirdPersonCam{0.0f, 3.0f, -4.0f};
        writeAt(playerRenderer, LevelRendererPlayer::mCamPos, thirdPersonCam);
        renderFrame();
        check(distanceBatch() != nullptr &&
                  distanceBatch()->vertices.size() == kReadoutVertsForTenMeters,
              "the same entity still reads 10.0m in third person");

        // Degraded local box: with no feet anchor there is no second
        // measurement to fall back to, so the readout is hidden instead of
        // showing a differently-measured number.
        const AABB zeroBox{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
        writeAt(playerAabbComponent, AABBShapeComponent::mAABB, zeroBox);
        renderFrame();
        check(quadBatchCount() == 0 && !lineBatchVertices().empty(),
              "without a local box no distance is shown, not a second one");
        writeAt(playerAabbComponent, AABBShapeComponent::mAABB, playerBounds);

        writeAt(playerRenderer, LevelRendererPlayer::mCamPos, cameraPosition);
        renderFrame();
        check(distanceBatch() != nullptr &&
                  distanceBatch()->vertices.size() == kReadoutVertsForTenMeters,
              "the feet anchor is used again once the local box is back");
    }

    // --- Thick lines become real geometry ---------------------------------
    {
        esp.boxThickness = 6.0f;
        renderFrame();
        check(g_batches.size() == 3,
              "a thicker box adds a beam pass, the readout stays its own batch");
        check(g_batches.size() == 3 && g_batches[0].mode == 1 &&
                  g_batches[0].reservedVertices == 96 && g_batches[0].vertices.size() == 96,
              "twelve thick edges emit 96 camera-facing quad vertices");
        const auto vertices = allVertices();
        float widest = 0.0f;
        for (const auto& vertex : vertices) {
            widest = std::max(widest, std::fabs(vertex[0]));
        }
        check(widest > 0.0f, "the beams are emitted in camera-relative space");
        esp.boxThickness = 1.0f;
    }

    // --- Filled box ---------------------------------------------------------
    {
        esp.boxFilled = true;
        renderFrame();
        const int faces = static_cast<int>(std::count_if(
            g_batches.begin(), g_batches.end(), [](const MeshBatch& batch) {
                return batch.mode == 1;
            }));
        check(faces == 2, "the fill is a quad batch besides the distance readout");
        const bool hasFill = faces == 2 && g_batches[0].vertices.size() == 48;
        check(hasFill, "six box faces emit 48 vertices (both windings)");
        check(filledRectCount() == 0, "the fill is geometry, not a screen rectangle");
        esp.boxFilled = false;
    }

    // --- Colors -------------------------------------------------------------
    {
        esp.boxColor = 0xFF00FF00u;
        renderFrame();
        const MeshBatch* wire = lastLineBatch();
        check(wire != nullptr && near(wire->color[1], 1.0f) &&
                  near(wire->color[0], 0.0f),
              "the box color reaches the tessellator");
        check(wire != nullptr && near(wire->color[3], 1.0f),
              "the wireframe is opaque at every thickness");

        esp.rgb = true;
        renderFrame();
        const auto rgbColor = lastLineBatch() == nullptr ? std::array<float, 4>{}
                                                         : lastLineBatch()->color;
        renderFrame();
        const auto rgbColorLater = lastLineBatch() == nullptr ? std::array<float, 4>{}
                                                              : lastLineBatch()->color;
        check(near(rgbColor[3], 1.0f) && near(rgbColorLater[3], 1.0f),
              "the rainbow overlay stays opaque");
        esp.rgb = false;
        esp.boxColor = 0xFFFFFFFFu;
    }

    // --- Health bar + value -------------------------------------------------
    {
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue, 10.0f);
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue + 8, 20.0f);
        writeAt(mob, Mob::mHealthAttribute, static_cast<void*>(mobHealthAttribute.data()));

        renderFrame();
        check(textCommandCount() == 1,
              "the health value is the one HUD text left (the distance is geometry)");
        check(filledRectCount() == 2, "the health bar draws a track and a fill");
        check(distanceBatch() != nullptr,
              "the distance readout keeps drawing beside the health stack");

        // Dead actors lose the bar instead of drawing a negative fraction.
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue, -4.0f);
        renderFrame();
        check(filledRectCount() == 0, "a non-positive health value draws no bar");
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue, 20.0f);
        writeAt(mob, Mob::mHealthAttribute, static_cast<void*>(nullptr));
    }

    // --- Tracers -------------------------------------------------------------
    // Both origins aim at the world center of the entity's own box, so the
    // line lands mid-hitbox and cannot slide off while the view moves. Only
    // the feet origin can be *geometry* though: a world-space segment that
    // starts at the camera lies on a single view ray, which the game collapses
    // onto one pixel -- the "selecting Crosshair removes the tracer" this used
    // to be. The crosshair line is therefore HUD furniture, like the nametags.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.0f, 0.0f, 4.0f}, {4.0f, 1.8f, 6.0f});
        esp.tracer = true;
        esp.tracerOrigin = EspModule::TracerOrigin::Bottom;
        renderFrame();
        check(lineBatchCount() == 2,
              "the feet-origin tracer is a second line batch the game places");
        const MeshBatch* tracer = lastLineBatch();
        const Vec3 hitboxCenter{3.0f, 0.9f, 5.0f};
        const Vec3 playerFeet{0.0f, 0.0f, 0.0f};
        check(tracer != nullptr && tracer->vertices.size() == 2 &&
                  anyVertexCloseTo(tracer->vertices, hitboxCenter, cameraPosition) &&
                  anyVertexCloseTo(tracer->vertices, playerFeet, cameraPosition),
              "the bottom tracer runs from the player's feet to mid-hitbox");
        check(lineCommands().empty(),
              "and no HUD line is submitted while the line is geometry");

        esp.tracerOrigin = EspModule::TracerOrigin::Crosshair;
        renderFrame();
        check(lineBatchCount() == 1,
              "a crosshair tracer is not emitted as a line through the camera");
        std::vector<pl::modmenu::DrawCommand> lines = lineCommands();
        check(lines.size() == 1, "the crosshair tracer is a HUD line");
        if (lines.size() == 1) {
            const auto& line = lines[0];
            check(near(line.x, 500.0f) && near(line.y, 500.0f),
                  "it starts dead on the crosshair, at the middle of the surface");
            check(near(line.x + line.w, 200.0f, 0.5f) &&
                      near(line.y + line.h, 572.0f, 0.5f),
                  "and ends on the hitbox center the projection puts there");
            check(std::isfinite(line.x) && std::isfinite(line.y) &&
                      std::isfinite(line.w) && std::isfinite(line.h) &&
                      line.size > 0.0f,
                  "with a finite, non-zero stroke the launcher can draw");
        }

        // An entity that is already off the surface keeps a snapline pointing
        // the way: the direction is the information there, so the end is pushed
        // out to the edge of the screen instead of to a projected point a
        // thousand pixels out -- and never past it, which is what would hand
        // the launcher coordinates it rejects the whole batch for.
        setMobBox({40.0f, 0.0f, 4.0f}, {41.0f, 1.8f, 5.0f});
        renderFrame();
        lines = lineCommands();
        check(lines.size() == 1, "an off-screen entity keeps its crosshair snapline");
        if (lines.size() == 1) {
            const float dx = lines[0].x + lines[0].w - 500.0f;
            const float dy = lines[0].y + lines[0].h - 500.0f;
            check(lines[0].x + lines[0].w < 500.0f,
                  "which points east, i.e. left of a south-facing camera");
            check(std::sqrt(dx * dx + dy * dy) <= 708.0f,
                  "and stops at the edge of the screen, not at infinity");
        }
        check(textCommandCount() == 0 && quadBatchCount() == 1,
              "while its HUD furniture is culled, the readout still clips in-world");
        esp.tracer = false;
    }

    // --- Nametag sits above the head --------------------------------------
    // The name is centered on the projected head point (top-center of the
    // entity's own box), not on the 2D box's top-middle, which perspective
    // shifts away from the head -- see the geometry test. The off-axis box
    // from the tracer test makes the two anchors disagree by tens of pixels,
    // so this fails if the name ever goes back to the 2D average.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.0f, 0.0f, 4.0f}, {4.0f, 1.8f, 6.0f});
        auto* nameField = new (mob.data() + Player::mName) std::string("Steve");
        fake::g_actorIsPlayer = true;
        renderFrame();

        // Head anchor (3, 1.8, 5) through the first-person camera at
        // (0, 1.62, 0) on the 1000x1000, 90-degree surface: ndc (-0.6, 0.036).
        // The 14px name sits 4px above it, centered on its own glyph width
        // ("Steve" is S+e+v+e at 0.6 and a thin t at 0.32 em, so 2.72 em =
        // 38.08px): x = 200 - 38.08 / 2.
        const pl::modmenu::DrawCommand* nameCmd = nullptr;
        for (const auto& cmd : g_commands) {
            if (cmd.type == pl::modmenu::DrawCommandType::Text && cmd.text == "Steve") {
                nameCmd = &cmd;
            }
        }
        check(nameCmd != nullptr && near(nameCmd->x, 180.96f, 0.5f) &&
                  near(nameCmd->y, 464.0f, 1.0f),
              "the nametag is centered above the head, not the 2D box");
        check(nameCmd != nullptr && near(nameCmd->w, 38.08f, 0.5f),
              "and its own measured width is what centers it");

        fake::g_actorIsPlayer = false;
        std::destroy_at(nameField);
        renderFrame();
        check(textCommandCount() == 0,
              "without a player name no HUD text is left (the distance is geometry)");
    }

    // --- The label is measured in the font that draws it -------------------
    // Centering a nametag means knowing how wide it will come out, and of the
    // two faces the launcher can draw HUD text with, only one has metrics this
    // module can know: the packaged pixel font. So the module asks for that face
    // when it is registered and the name fits inside it, and leaves the text --
    // and its measurement -- with the launcher's own font otherwise. That is not
    // only about looks: a script the pixel font has no glyph for is drawn as a
    // row of replacement boxes, which is the other half of the "extra characters
    // beside the nametag" report, and the launcher's font is also the only one
    // that shapes right-to-left names.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.0f, 0.0f, 4.0f}, {4.0f, 1.8f, 6.0f});
        auto* nameField = new (mob.data() + Player::mName) std::string("Steve");
        fake::g_actorIsPlayer = true;

        // No font registered (the state on a host with no package to read it
        // from): the launcher's own face draws the label and the estimate that
        // goes with it centers it -- exactly the case above.
        renderFrame();
        const pl::modmenu::DrawCommand* nameCmd = findText("Steve");
        check(nameCmd != nullptr && nameCmd->fontId.empty(),
              "without the pixel font the label stays on the launcher's own face");
        check(nameCmd != nullptr && near(nameCmd->w, 38.08f, 0.5f),
              "measured with the estimate that goes with it");

        // With it, the same name is measured on the font's real cells: 't' is
        // two thirds of a cell and not the half-ish stroke the estimate charges,
        // so the 14px name is 39.2 wide. Because the launcher draws the label
        // with that very font, the centering lands on the head instead of a few
        // pixels to one side of it.
        s_pixelFontReady = true;
        renderFrame();
        nameCmd = findText("Steve");
        check(nameCmd != nullptr && nameCmd->fontId == "minecraft",
              "the pixel font is asked for once the launcher has it");
        check(nameCmd != nullptr && near(nameCmd->w, 39.2f, 0.01f) &&
                  near(nameCmd->x, 200.0f - 39.2f * 0.5f, 0.5f),
              "and the name is centered on the cell widths of that font");

        // A name the pixel font cannot draw does not get it anyway, and neither
        // does a name that only partly fits: the whole label switches face.
        std::string arabic;
        arabic += "\xD9\x84\xD8\xA7\xD9\x84\xD8\xA8"; // "لاعب"
        std::destroy_at(nameField);
        nameField = new (mob.data() + Player::mName) std::string(arabic + " X");
        renderFrame();
        nameCmd = findText(arabic + " X");
        check(nameCmd != nullptr && nameCmd->fontId.empty(),
              "a name outside the pixel font keeps the launcher's font");
        check(nameCmd != nullptr && near(nameCmd->w, (4.0f * 0.6f + 0.3f + 0.6f) * 14.0f, 0.5f),
              "and is measured in code points on that font's estimate");

        s_pixelFontReady = false;
        fake::g_actorIsPlayer = false;
        std::destroy_at(nameField);
        renderFrame();
    }

    // --- The nametag shows the name, and only the name ---------------------
    // The game's own font swallows the section-sign markup a server or a nick
    // add-on pads a name with, plus the invisible format characters a
    // right-to-left name arrives wrapped in. A plain HUD font paints them,
    // which is the "extra characters beside the nametag", and their bytes also
    // fed the width the label is centered on -- so a two-bytes-per-character
    // name sat half a label to the left of the head it belongs to.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.0f, 0.0f, 4.0f}, {4.0f, 1.8f, 6.0f});

        // "§r§a" + the U+200E left-to-right marker + the Arabic name "لاعب".
        // Every escape ends its own literal: "\xA7a" would swallow the code
        // character into the escape instead of leaving it to the markup.
        std::string kRaw;
        kRaw += "\xC2\xA7";
        kRaw += "r";
        kRaw += "\xC2\xA7";
        kRaw += "a";
        kRaw += "\xE2\x80\x8E";
        kRaw += "\xD9\x84\xD8\xA7\xD9\x84\xD8\xA8";
        const std::string kClean = "\xD9\x84\xD8\xA7\xD9\x84\xD8\xA8";

        auto* nameField = new (mob.data() + Player::mName) std::string(kRaw);
        fake::g_actorIsPlayer = true;
        renderFrame();

        const pl::modmenu::DrawCommand* nameCmd = nullptr;
        for (const auto& cmd : g_commands) {
            if (cmd.type == pl::modmenu::DrawCommandType::Text) nameCmd = &cmd;
        }
        check(nameCmd != nullptr && nameCmd->text == kClean,
              "the markup codes and the bidi marker never reach the label");
        // Four glyphs of 0.6 em at 14px is 33.6px of name, centered on the head
        // column at x = 200. Counting the eight bytes instead would have asked
        // the launcher for a 112px-wide label and drawn it from x = 144.
        check(nameCmd != nullptr && near(nameCmd->x, 200.0f - 33.6f * 0.5f, 0.5f),
              "a multi-byte name is centered on its glyphs, not its bytes");
        check(nameCmd != nullptr && near(nameCmd->w, 33.6f, 0.5f),
              "and its reported width is the same measurement");

        // A name that is nothing but markup has no label at all -- an empty
        // string would otherwise reserve a line of the stack above the box.
        std::destroy_at(nameField);
        std::string codesOnly;
        codesOnly += "\xC2\xA7";
        codesOnly += "r";
        codesOnly += "\xC2\xA7";
        codesOnly += "l";
        codesOnly += "   ";
        new (mob.data() + Player::mName) std::string(codesOnly);
        renderFrame();
        check(textCommandCount() == 0, "a name of nothing but codes draws nothing");

        std::destroy_at(nameField);
        fake::g_actorIsPlayer = false;
    }

    // --- One broken actor cannot take the frame's overlay down -------------
    // The launcher validates a module's whole batch before it draws any of it,
    // so a non-finite coordinate anywhere in it used to cost every nametag on
    // screen -- which is what made the labels blink out around corrupted or
    // half-loaded actors. Bad data now loses its own overlay instead.
    {
        PlayerNameGuard name(mob.data(), "Steve");
        const float kNan = std::nanf("");
        setMobBox({kNan, 0.0f, 4.0f}, {4.0f, 1.8f, 6.0f});
        renderFrame();
        check(g_batches.empty() && g_commands.empty(),
              "an actor whose collision box is not a number draws nothing");

        setMobBox({2.0f, 0.0f, 4.0f}, {4.0f, 1.8f, 6.0f});
        renderFrame();
        check(textCommandCount() == 1 && !g_batches.empty(),
              "and the next frame's nametag is there again");
    }

    // --- Handendness of the label anchor ----------------------------------
    // The HUD labels are still a projection, so the camera basis has to stay
    // right-handed: a mob east of a south-facing camera belongs on the LEFT.
    {
        PlayerNameGuard name(mob.data(), "Steve");
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.7f, 0.0f, 9.7f}, {3.3f, 1.8f, 10.3f});
        renderFrame();
        const pl::modmenu::DrawCommand* nameCmd = nullptr;
        for (const auto& cmd : g_commands) {
            if (cmd.type == pl::modmenu::DrawCommandType::Text && cmd.text == "Steve") {
                nameCmd = &cmd;
            }
        }
        check(nameCmd != nullptr && nameCmd->x < centerX,
              "a mob to the east gets its labels left of center");

        setCameraRotation(0.0f, 30.0f); // turn right
        renderFrame();
        nameCmd = nullptr;
        for (const auto& cmd : g_commands) {
            if (cmd.type == pl::modmenu::DrawCommandType::Text && cmd.text == "Steve") {
                nameCmd = &cmd;
            }
        }
        check(nameCmd != nullptr && nameCmd->x < centerX,
              "turning right keeps sweeping the label further left");
    }

    // --- Culling and filters ------------------------------------------------
    {
        setCameraRotation(0.0f, 0.0f);

        setMobBox({-0.3f, 0.0f, -10.3f}, {0.3f, 1.8f, -9.7f}); // behind the camera
        renderFrame();
        check(allVertices().empty() == false,
              "an actor behind the camera still gets its 3D box (the GPU clips it)");
        check(g_commands.empty(), "but nothing is drawn on the HUD for it");
        check(quadBatchCount() == 0,
              "and its distance readout is skipped, not mirrored across the screen");

        setMobBox({9.7f, 0.0f, 2.7f}, {10.3f, 1.8f, 3.3f}); // far outside the frustum
        renderFrame();
        check(g_commands.empty(), "an off-screen actor has no labels");

        // Show Mobs off: the same mob is not drawn at all.
        setMobBox({0.7f, 0.0f, 9.7f}, {1.3f, 1.8f, 10.3f});
        renderFrame();
        check(allVertices().size() == kWireVertsPerActor + kReadoutVertsForTenMeters,
              "an on-screen mob is drawn before the filter test");

        esp.showMobs = false;
        renderFrame();
        check(g_batches.empty() && g_commands.empty(),
              "disabling Show Mobs stops the overlay");
        esp.showMobs = true;

        fake::g_actorIsInvisible = true;
        renderFrame();
        check(g_batches.empty() && g_commands.empty(), "invisible actors stay skipped");
        fake::g_actorIsInvisible = false;
    }

    // --- The per-frame cap --------------------------------------------------
    // fetchNearbyActorsSorted returns the actors nearest-first, so drawing the
    // first N keeps the worst case bounded on a busy server without dropping
    // what the player is looking at.
    {
        fake::g_fetchedCount = fake::kFetchedCapacity;
        for (int i = 0; i < fake::kFetchedCapacity; ++i) {
            fake::g_fetched[i] = {mob.data(), static_cast<float>(i), 0.0f};
        }
        renderFrame();
        const auto vertices = allVertices();
        // kMaxDrawnActors boxes (24 wire vertices) with their readouts (each
        // mob here is ten blocks away, so 120 text vertices per actor).
        check(vertices.size() ==
                  static_cast<std::size_t>(kMaxDrawnActors) *
                      (kWireVertsPerActor + kReadoutVertsForTenMeters),
              "only the nearest kMaxDrawnActors overlays are emitted");
        fake::g_fetchedCount = 1;
        renderFrame();
        check(allVertices().size() == kWireVertsPerActor + kReadoutVertsForTenMeters,
              "and the cap leaves a normal crowd alone");
    }

    // --- Renderer state is restored ----------------------------------------
    {
        colorHolder = {0.2f, 0.3f, 0.4f, 0.5f};
        renderFrame();
        check(near(colorHolder[0], 0.2f) && near(colorHolder[1], 0.3f) &&
                  near(colorHolder[2], 0.4f) && near(colorHolder[3], 0.5f),
              "the overlay restores ScreenContext color state");
    }

    // --- Degraded renderer: labels survive, the mesh pass is skipped -------
    // A build whose Tessellator cannot be resolved must not lose the whole
    // overlay, and must not leave the game's color holder overwritten either.
    // The distance readout is mesh-bound now, so a nametag stands in for the
    // "HUD keeps working" half of the test.
    {
        PlayerNameGuard name(mob.data(), "Steve");
        auto savedBegin = s_mesh.begin;
        s_mesh.begin = nullptr;
        colorHolder = {0.2f, 0.3f, 0.4f, 0.5f};
        renderFrame();
        check(g_batches.empty(), "no mesh entry points means no geometry is emitted");
        check(textCommandCount() == 1, "the labels keep working without the mesh pass");
        check(near(colorHolder[0], 0.2f) && near(colorHolder[1], 0.3f) &&
                  near(colorHolder[2], 0.4f) && near(colorHolder[3], 0.5f),
              "a skipped mesh pass leaves the color holder alone");
        s_mesh.begin = savedBegin;
    }

    // --- Frames without the render hook drop the overlay -------------------
    // renderOverlay publishes the HUD labels itself (so they describe the same
    // frame as the geometry); onFrame only has to clear them once the world
    // stops rendering, and must not touch them while it is being drawn.
    {
        PlayerNameGuard name(mob.data(), "Steve");
        setMobBox({0.7f, 0.0f, 9.7f}, {1.3f, 1.8f, 10.3f});
        renderFrame();
        const int before = textCommandCount();
        check(before > 0, "a rendered frame leaves labels on screen");

        // The swap that follows a rendered frame keeps that frame's overlay.
        esp.onFrame();
        check(textCommandCount() == before,
              "onFrame keeps the frame the hook just published");

        // The world stopped rendering (menu screen / a level that is not
        // drawn), so the leftovers are dropped instead of freezing on screen.
        esp.onFrame();
        check(g_commands.empty(), "a frame without the render hook clears the labels");
        esp.onFrame();
        check(g_commands.empty(), "and the clear is submitted once, not every frame");
    }

    // --- Disabling the module clears it -------------------------------------
    {
        PlayerNameGuard name(mob.data(), "Steve");
        renderFrame();
        check(!g_commands.empty(), "the overlay is up before the disable test");
        esp.setMasterEnabled(false);
        check(g_commands.empty(), "disabling the module clears the HUD layer");
        renderFrame();
        check(g_batches.empty() && g_commands.empty(),
              "a disabled module draws nothing even if the hook runs");
        esp.setMasterEnabled(true);
    }

    // --- Configuration ------------------------------------------------------
    {
        EspModule source;
        source.throughWalls = false;
        source.boxStyle = EspModule::BoxStyle::Corner;
        source.boxThickness = 4.0f;
        source.boxColor = 0xFF123456u;
        source.tracer = true;
        source.tracerOrigin = EspModule::TracerOrigin::Crosshair;
        source.rgb = true;
        source.rgbSpeed = 0.75f;
        source.fov = 88.0f;

        nlohmann::json saved;
        source.saveConfig(saved);
        check(saved["boxStyle"].get<std::string>() == "1,Box,Corner",
              "the box radio value carries the wireframe labels");
        check(saved["boxColor"].get<std::string>() == "#123456",
              "the box color saves as a menu-compatible RGB string");

        EspModule loaded;
        loaded.loadConfig(saved);
        check(loaded.boxStyle == EspModule::BoxStyle::Corner && !loaded.throughWalls,
              "style and depth settings round-trip");
        check(near(loaded.boxThickness, 4.0f) && near(loaded.fov, 88.0f) &&
                  near(loaded.rgbSpeed, 0.75f) && loaded.rgb && loaded.tracer &&
                  loaded.tracerOrigin == EspModule::TracerOrigin::Crosshair,
              "numeric and toggle settings round-trip");
        check((loaded.boxColor & 0x00FFFFFFu) == 0x123456u, "the box color round-trips");

        // A config saved while style 0 was the screen-space rectangle keeps
        // index 0, which is now the world-space wireframe: the same feature,
        // drawn where it cannot drift.
        nlohmann::json legacy;
        legacy["boxStyle"] = "0,2D,Corner";
        legacy["throughWalls"] = false;
        EspModule migrated;
        migrated.loadConfig(legacy);
        check(migrated.boxStyle == EspModule::BoxStyle::Box,
              "a saved 2D style index resolves to the wireframe box");
        check(!migrated.throughWalls,
              "a config that explicitly asked for the occlusion cull keeps it");

        // Only a config that never mentioned Through Walls picks up the new
        // default.
        nlohmann::json empty;
        EspModule fresh;
        fresh.loadConfig(empty);
        check(fresh.throughWalls, "a config without Through Walls gets the new default");
        check(near(fresh.fov, 70.0f),
              "and the label projection starts at Bedrock's own default FOV");

        // Radio values. The launcher's picker may report the index, the index
        // with the option list appended (what saveConfig writes), or the label
        // on its own -- and every one of those has to select the option, or
        // choosing "Crosshair" silently leaves the module on the old origin.
        nlohmann::json bareIndex;
        bareIndex["tracerOrigin"] = "1";
        bareIndex["boxStyle"] = 1;
        EspModule byIndex;
        byIndex.loadConfig(bareIndex);
        check(byIndex.tracerOrigin == EspModule::TracerOrigin::Crosshair &&
                  byIndex.boxStyle == EspModule::BoxStyle::Corner,
              "a bare radio index selects its option, as text or as a number");

        nlohmann::json byLabel;
        byLabel["tracerOrigin"] = "Crosshair";
        byLabel["boxStyle"] = "corner";
        EspModule labelPicked;
        labelPicked.loadConfig(byLabel);
        check(labelPicked.tracerOrigin == EspModule::TracerOrigin::Crosshair &&
                  labelPicked.boxStyle == EspModule::BoxStyle::Corner,
              "an option picked by its label is selected too, case aside");

        nlohmann::json unknown;
        unknown["tracerOrigin"] = "Over my shoulder";
        EspModule keepsCurrent;
        keepsCurrent.tracerOrigin = EspModule::TracerOrigin::Bottom;
        keepsCurrent.loadConfig(unknown);
        check(keepsCurrent.tracerOrigin == EspModule::TracerOrigin::Bottom,
              "a value that names no option keeps the one the module has");
    }

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
