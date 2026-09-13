// Integration-style host test for the Esp overlay render path.
//
// The box overlay is world-space geometry handed to the game's renderer (the
// same pass the Hitbox module draws in), so this test drives the real render
// hook with small in-memory Minecraft stand-ins -- a local player, a mob, the
// camera the level is rendered with, a ScreenContext and its tessellator --
// and inspects both halves of the overlay: the tessellator calls the geometry
// turns into, and the draw commands the HUD layer submits.
//
// The cases mirror the bug reports the module has to keep getting right:
//
//   * the geometry sits on the entity, in world space, relative to the camera
//     the level was drawn with;
//   * turning the view cannot move it, because the module no longer projects
//     it (this is the regression the screen-space box used to have);
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
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <pl/ModMenu.hpp>

#include "bedrocktools/events/EventBus.hpp"
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/sdk/Offsets.hpp"
#include "bedrocktools/sdk/Types.hpp"

namespace {

constexpr float kSurfaceWidth = 1000.0f;
constexpr float kSurfaceHeight = 1000.0f;

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

int textCommandCount() {
    int count = 0;
    for (const auto& cmd : g_commands) {
        if (cmd.type == pl::modmenu::DrawCommandType::Text) ++count;
    }
    return count;
}

// The distance readout is the only text command with the trailing "m" suffix
// (the health value is a bare number and the fake actors are never players,
// so no nametag is ever drawn).
std::string findDistanceText() {
    for (const auto& cmd : g_commands) {
        if (cmd.type == pl::modmenu::DrawCommandType::Text &&
            !cmd.text.empty() && cmd.text.back() == 'm') {
            return cmd.text;
        }
    }
    return {};
}

int lineCommandCount() {
    int count = 0;
    for (const auto& cmd : g_commands) {
        if (cmd.type == pl::modmenu::DrawCommandType::Line) ++count;
    }
    return count;
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
    const float centerY = kSurfaceHeight * 0.5f;

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
        const auto vertices = allVertices();
        check(vertices.size() == 24, "twelve box edges emit twenty-four vertices");
        check(anyVertexCloseTo(vertices, mobMin, cameraPosition) &&
                  anyVertexCloseTo(vertices, mobMax, cameraPosition),
              "the box corners are the entity AABB corners, camera-relative");

        // Hairline by default: nothing but the line list.
        check(g_batches.size() == 1 && g_batches[0].mode == 4,
              "thickness 1 emits only the crisp line pass");

        // The labels are separate HUD furniture and still track the mob.
        check(textCommandCount() == 1, "the distance readout is still drawn");
    }

    // --- Turning the view cannot move the geometry --------------------------
    // The old screen-space box re-projected every frame from the module's own
    // camera model, so a mismatch there read as "the box moves when I move the
    // screen". World-space edges are the same numbers whatever the view does.
    {
        setMobBox({0.7f, 0.0f, 9.7f}, {1.3f, 1.8f, 10.3f});

        setCameraRotation(0.0f, 0.0f);
        renderFrame();
        const auto levelView = allVertices();
        const auto levelLabels = g_commands;

        setCameraRotation(-20.0f, 15.0f); // look up and turn right, mob still on screen
        renderFrame();
        const auto turnedView = allVertices();

        check(levelView.size() == turnedView.size() && !levelView.empty(),
              "the box is drawn at every view angle");
        check(levelView == turnedView,
              "the box geometry is identical while the view turns");
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
        check(allVertices().size() == 24, "an actor behind a wall is still drawn");
        check(textCommandCount() == 1, "and its labels stay up");
    }

    // --- Through Walls off: the occlusion cull takes over ------------------
    {
        esp.throughWalls = false;
        renderFrame();
        check(g_batches.empty() && g_commands.empty(),
              "with Through Walls off, a walled-off actor is culled entirely");

        fake::g_solidBlocks = false;
        renderFrame();
        check(allVertices().size() == 24,
              "and it comes back as soon as the raycast is clear");

        fake::g_solidBlocks = true;
        esp.throughWalls = true;
        renderFrame();
        check(allVertices().size() == 24,
              "the cull never runs while Through Walls is on, even with a region");
    }

    // --- Distance is measured from the player, not the camera ---------------
    // The readout is the feet-to-feet distance between the two collision
    // boxes, so it cannot depend on the perspective: pulling the camera back
    // into third person must not change what the same entity reports.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({-0.3f, 0.0f, 9.7f}, {0.3f, 1.8f, 10.3f});
        esp.throughWalls = true;
        fake::g_solidBlocks = false;

        renderFrame();
        check(findDistanceText() == "10.0m",
              "ten blocks away reads 10.0m in first person");

        // The readout sits just under the hitbox: 4px below the projected
        // box bottom, centered on it -- not floating in the stack above.
        {
            const esp::Camera cam = esp::computeCamera(cameraPosition, {0.0f, 0.0f});
            const esp::SurfaceProjection proj =
                esp::makeProjection(kSurfaceWidth, kSurfaceHeight, 90.0f);
            const esp::ScreenBox box = esp::projectBox(
                cam, proj, {-0.3f, 0.0f, 9.7f}, {0.3f, 1.8f, 10.3f}, 0.0f);
            const pl::modmenu::DrawCommand* distanceCmd = nullptr;
            for (const auto& cmd : g_commands) {
                if (cmd.type == pl::modmenu::DrawCommandType::Text &&
                    !cmd.text.empty() && cmd.text.back() == 'm') {
                    distanceCmd = &cmd;
                }
            }
            const float expectedX = (box.minX + box.maxX) * 0.5f - 16.0f;
            check(box.visible && distanceCmd != nullptr &&
                      near(distanceCmd->x, expectedX, 1.0f) &&
                      near(distanceCmd->y, box.maxY + 4.0f, 1.0f),
                  "the readout sits just under the hitbox");
        }

        // Third-person boom: the camera swings several blocks behind and
        // above the player while the player itself does not move.
        const Vec3 thirdPersonCam{0.0f, 3.0f, -4.0f};
        writeAt(playerRenderer, LevelRendererPlayer::mCamPos, thirdPersonCam);
        renderFrame();
        check(findDistanceText() == "10.0m",
              "the same entity still reads 10.0m in third person");

        // Degraded local box: with no feet anchor there is no second
        // measurement to fall back to, so the readout is hidden instead of
        // showing a differently-measured number.
        const AABB zeroBox{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
        writeAt(playerAabbComponent, AABBShapeComponent::mAABB, zeroBox);
        renderFrame();
        check(findDistanceText().empty() && textCommandCount() == 0,
              "without a local box no distance is shown, not a second one");
        writeAt(playerAabbComponent, AABBShapeComponent::mAABB, playerBounds);

        writeAt(playerRenderer, LevelRendererPlayer::mCamPos, cameraPosition);
        renderFrame();
        check(findDistanceText() == "10.0m",
              "the feet anchor is used again once the local box is back");
    }

    // --- Thick lines become real geometry ---------------------------------
    {
        esp.boxThickness = 6.0f;
        renderFrame();
        check(g_batches.size() == 2, "a thicker box adds a beam pass under the lines");
        check(g_batches.size() == 2 && g_batches[0].mode == 1 &&
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
        check(faces == 1, "the fill is one quad batch");
        const bool hasFill = faces == 1 && g_batches[0].vertices.size() == 48;
        check(hasFill, "six box faces emit 48 vertices (both windings)");
        check(filledRectCount() == 0, "the fill is geometry, not a screen rectangle");
        esp.boxFilled = false;
    }

    // --- Colors -------------------------------------------------------------
    {
        esp.boxColor = 0xFF00FF00u;
        renderFrame();
        check(!g_batches.empty() && near(g_batches.back().color[1], 1.0f) &&
                  near(g_batches.back().color[0], 0.0f),
              "the box color reaches the tessellator");
        check(!g_batches.empty() && near(g_batches.back().color[3], 1.0f),
              "the wireframe is opaque at every thickness");

        esp.rgb = true;
        renderFrame();
        const auto rgbColor = g_batches.empty() ? std::array<float, 4>{}
                                                : g_batches.back().color;
        renderFrame();
        const auto rgbColorLater = g_batches.empty() ? std::array<float, 4>{}
                                                     : g_batches.back().color;
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
        check(textCommandCount() == 2, "the health value is drawn next to the distance");
        check(filledRectCount() == 2, "the health bar draws a track and a fill");

        // Dead actors lose the bar instead of drawing a negative fraction.
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue, -4.0f);
        renderFrame();
        check(filledRectCount() == 0, "a non-positive health value draws no bar");
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue, 20.0f);
        writeAt(mob, Mob::mHealthAttribute, static_cast<void*>(nullptr));
    }

    // --- Tracers -------------------------------------------------------------
    // The line ends on the projected center of the entity's own box, so it
    // lands mid-hitbox instead of the 2D box's middle (which perspective
    // shifts off of it -- see the geometry test). The off-axis box below
    // makes the two endpoints disagree by tens of pixels, so this fails if
    // the line ever goes back to the 2D average.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.0f, 0.0f, 4.0f}, {4.0f, 1.8f, 6.0f});
        esp.tracer = true;
        esp.tracerOrigin = EspModule::TracerOrigin::Bottom;
        renderFrame();
        check(lineCommandCount() == 1, "the tracer is one screen line");
        const bool fromBottom = lineCommandCount() == 1 &&
                                near(g_commands.front().y, kSurfaceHeight);
        check(fromBottom, "the bottom origin starts at the screen edge");

        // Center anchor (3, 0.9, 5) through the first-person camera at (0, 1.62, 0)
        // on the 1000x1000, 90-degree surface: ndc (-0.6, -0.144).
        const auto& bottomLine = g_commands.front();
        const float bottomEndX = bottomLine.x + bottomLine.w;
        const float bottomEndY = bottomLine.y + bottomLine.h;
        check(lineCommandCount() == 1 && near(bottomEndX, 200.0f, 1.0f) &&
                  near(bottomEndY, 572.0f, 1.0f),
              "the tracer ends mid-hitbox, not on the 2D box middle");

        esp.tracerOrigin = EspModule::TracerOrigin::Crosshair;
        renderFrame();
        check(lineCommandCount() == 1 && near(g_commands.front().y, centerY),
              "the crosshair origin starts at the screen center");
        const auto& crossLine = g_commands.front();
        check(lineCommandCount() == 1 &&
                  near(crossLine.x + crossLine.w, 200.0f, 1.0f) &&
                  near(crossLine.y + crossLine.h, 572.0f, 1.0f),
              "the crosshair tracer ends on the same mid-hitbox anchor");
        esp.tracer = false;
    }

    // --- Nametag sits above the head --------------------------------------
    // The name is centered on the projected head point (top-center of the
    // entity's own box), not on the 2D box's top-middle, which perspective
    // shifts away from the head -- see the geometry test. The off-axis box
    // left over from the tracer test makes the two anchors disagree by tens
    // of pixels, so this fails if the name ever goes back to the 2D average.
    {
        auto* nameField = new (mob.data() + Player::mName) std::string("Steve");
        fake::g_actorIsPlayer = true;
        renderFrame();

        // Head anchor (3, 1.8, 5) through the first-person camera at
        // (0, 1.62, 0) on the 1000x1000, 90-degree surface: ndc (-0.6, 0.036).
        // The 14px name sits 4px above it, centered: x = 200 - 42 / 2.
        const pl::modmenu::DrawCommand* nameCmd = nullptr;
        for (const auto& cmd : g_commands) {
            if (cmd.type == pl::modmenu::DrawCommandType::Text && cmd.text == "Steve") {
                nameCmd = &cmd;
            }
        }
        check(nameCmd != nullptr && near(nameCmd->x, 179.0f, 1.0f) &&
                  near(nameCmd->y, 464.0f, 1.0f),
              "the nametag is centered above the head, not the 2D box");

        fake::g_actorIsPlayer = false;
        std::destroy_at(nameField);
        renderFrame();
        check(textCommandCount() == 1 && findDistanceText() == "5.8m",
              "without a player name only the distance is left");
    }

    // --- Handendness of the label anchor ----------------------------------
    // The labels are still a projection, so the camera basis has to stay
    // right-handed: a mob east of a south-facing camera belongs on the LEFT.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.7f, 0.0f, 9.7f}, {3.3f, 1.8f, 10.3f});
        renderFrame();
        check(textCommandCount() == 1 && !g_commands.empty() && g_commands.back().x < centerX,
              "a mob to the east gets its labels left of center");

        setCameraRotation(0.0f, 30.0f); // turn right
        renderFrame();
        check(textCommandCount() == 1 && !g_commands.empty() && g_commands.back().x < centerX,
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

        setMobBox({9.7f, 0.0f, 2.7f}, {10.3f, 1.8f, 3.3f}); // far outside the frustum
        renderFrame();
        check(g_commands.empty(), "an off-screen actor has no labels");

        // Show Mobs off: the same mob is not drawn at all.
        setMobBox({0.7f, 0.0f, 9.7f}, {1.3f, 1.8f, 10.3f});
        renderFrame();
        check(allVertices().size() == 24, "an on-screen mob is drawn before the filter test");

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
        // kMaxDrawnActors boxes, twelve edges, two vertices per edge.
        check(vertices.size() == static_cast<std::size_t>(kMaxDrawnActors) * 24,
              "only the nearest kMaxDrawnActors boxes are emitted");
        fake::g_fetchedCount = 1;
        renderFrame();
        check(allVertices().size() == 24, "and the cap leaves a normal crowd alone");
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
    {
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
    }

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
