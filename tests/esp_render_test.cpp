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
// edges * two vertices; the label geometry is one quad per merged rectangle of
// every glyph of the line, twice wound -- see billboardVertsFor() below, which
// counts them off the same generated face the module draws from.
constexpr std::size_t kWireVertsPerActor = 24;

// "لاعب" -- a name the packaged pixel face has no cells for, so the Esp module
// leaves it (and the rest of that entity's label column) on the launcher's HUD
// font. The cases that care about the HUD path steer by it; the ones that care
// about the mesh path use a plain ASCII name, which now goes to the geometry.
const std::string kHudName = "\xD9\x84\xD8\xA7\xD9\x84\xD8\xA8";

// Captured HUD-layer commands (nametags, distance, tracers, health bar).
std::vector<pl::modmenu::DrawCommand> g_commands;

// Captured world-space geometry: one entry per Tessellator flush, with the
// vertices (already camera-relative) and the primitive mode.
struct MeshBatch {
    int mode = -1;
    int reservedVertices = 0;
    std::vector<std::array<float, 3>> vertices;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 0.0f};
    // Every color the batch was told, in order: a grouped flush (the health
    // bars) sets one per group, so `color` alone is only the last of them.
    std::vector<std::array<float, 4>> colors;
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
    g_currentBatch.colors.push_back({r, g, b, a});
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

// The vertices one billboarded line costs the mesh: a quad per merged
// rectangle of every glyph, both windings each. Counted off the generated face
// the module spells its labels with (esp_pixel_font.hpp), so the numbers below
// say "this text and nothing else" instead of hard-coding a count that changes
// whenever the font is regenerated.
std::size_t billboardVertsFor(std::string_view text) {
    std::size_t rects = 0;
    for (const char c : text) {
        const esp::world::PixelGlyph* glyph =
            esp::world::pixelGlyph(static_cast<unsigned char>(c));
        if (glyph) rects += glyph->rectCount;
    }
    return rects * 8;
}

// "10.0m" is what the mob ten blocks away reads, and the number the batch-count
// assertions below share.
std::size_t readoutVertsForTenMeters() { return billboardVertsFor("10.0m"); }

// What a billboarded line occupies, in the font's own pixel cells: the left edge
// of the first glyph's ink, the right edge of the last, and the total advance.
// The pinned labels are laid out on exactly these numbers, so recomputing them
// here checks "the label is as wide as the face says and no wider" without
// trusting a constant nobody can read off a font.
std::array<float, 3> billboardInkFor(std::string_view text) {
    float pen = 0.0f, left = 1e30f, right = -1e30f;
    for (const char c : text) {
        const esp::world::PixelGlyph* glyph =
            esp::world::pixelGlyph(static_cast<unsigned char>(c));
        if (!glyph) continue;
        for (std::size_t i = 0; i < glyph->rectCount; ++i) {
            const esp::world::PixelRect& rect = esp::world::kPixelRects[glyph->firstRect + i];
            left = std::min(left, pen + static_cast<float>(rect.x));
            right = std::max(right, pen + static_cast<float>(rect.x + rect.width));
        }
        pen += static_cast<float>(glyph->advance);
    }
    return {left, right, pen};
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

// The last quad batch (GL_QUADS): the billboarded text -- the distance readout,
// the nametag and the health value, all one color and all flushed last so a
// label stays readable over the wireframe it crosses (see the flush order in
// drawWorldOverlay).
const MeshBatch* labelBatch() {
    const MeshBatch* out = nullptr;
    for (const auto& batch : g_batches) {
        if (batch.mode == 1) out = &batch;
    }
    return out;
}

// The health bars of the frame: the one quad batch that opens with the track's
// translucent black and is followed by a fill of another color. Recognizing it
// by its colors is the point -- a bar that is not two color groups in one mesh
// is a bar the module did not group.
const MeshBatch* barBatch() {
    const MeshBatch* out = nullptr;
    for (const auto& batch : g_batches) {
        if (batch.mode != 1 || batch.colors.size() < 2) continue;
        if (near(batch.colors[0][3], 0.5f) && near(batch.colors[0][0], 0.0f) &&
            near(batch.colors[0][1], 0.0f) && near(batch.colors[0][2], 0.0f)) {
            out = &batch;
        }
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

    explicit PlayerNameGuard(std::byte* actor, const std::string& name)
        : field(new (actor + bedrocktools::sdk::offsets::Player::mName)
                    std::string(name)) {
        fake::g_actorIsPlayer = true;
    }

    // Swapping the name is how one case checks both label paths on the same
    // actor: a name the pixel face can spell goes into the mesh, one it cannot
    // spell stays on the launcher's font.
    void rename(std::byte* actor, const std::string& name) {
        std::destroy_at(field);
        field = new (actor + bedrocktools::sdk::offsets::Player::mName) std::string(name);
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
        // under the entity's feet now, pinned by the game like the box. The mob
        // is sqrt(109) = 10.4 blocks away, so the batch has to be exactly the
        // glyphs of "10.4m" -- which is also the only way to tell that the
        // readout still says what it measured.
        const MeshBatch* readout = labelBatch();
        check(readout != nullptr &&
                  readout->vertices.size() == billboardVertsFor("10.4m"),
              "the distance readout is world geometry, spelled out of \"10.4m\"");
        check(readout != nullptr &&
                  allVerticesBelow(*readout, 0.0f, cameraPosition),
              "and it hangs below the entity's feet, not next to the hitbox");
        check(textCommandCount() == 0,
              "no HUD text is left for a nameless, healthless mob");
    }

    // --- Turning the view cannot move the geometry --------------------------
    // The old screen-space box re-projected every frame from the module's own
    // camera model, so a mismatch there read as "the box moves when I move the
    // screen". World-space edges are the same numbers whatever the view does --
    // and that is now true of the tracers, the readout and the label column too,
    // which is the whole reason the nametag and the health stack moved into the
    // mesh. The one label that still has to be projected is a name the pixel
    // face cannot spell, so the pair of cases below is the difference between
    // the two paths, on the same entity at the same angles.
    {
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

        // --- a spellable name: geometry, and therefore still there ---------
        PlayerNameGuard name(mob.data(), "Steve");
        setMobBox({0.7f, 0.0f, 9.7f}, {1.3f, 1.8f, 10.3f});

        setCameraRotation(0.0f, 0.0f);
        renderFrame();
        const auto levelWire = lineBatchVertices();
        const auto levelReadout = quadBatchVertices();

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

        // The readout and the name stay on the hitbox: their world anchors are
        // the entity's feet (x=1, z=10, just under y=0) and the top-center of its
        // box, so turning the view may only tilt and resize a billboard, never
        // move it off the box. (The HUD projection this replaced moved the readout
        // by whole box widths whenever its camera model disagreed with the
        // game's -- which is exactly what a nametag used to do, one batch later.)
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
        check(g_commands.empty() && textCommandCount() == 0,
              "and none of it is on the HUD at all");

        // The name is in the same batch as the readout -- one color, one flush --
        // so what separates them is the head: the rows above it are the nametag's.
        // Its centroid has to stay on the head column however the billboard tilts.
        auto nameVertices = [&](const std::vector<std::array<float, 3>>& quads) {
            std::vector<std::array<float, 3>> out;
            for (const auto& vertex : quads) {
                if (vertex[1] + cameraPosition.y > 1.8f) out.push_back(vertex);
            }
            return out;
        };
        const auto levelName = nameVertices(levelReadout);
        const auto turnedName = nameVertices(turnedReadout);
        check(levelName.size() == billboardVertsFor("Steve") &&
                  turnedName.size() == levelName.size(),
              "the nametag is spelled in both frames, one vertex pair per rectangle");
        const auto levelNameCenter = centroid(levelName);
        const auto turnedNameCenter = centroid(turnedName);
        check(near(levelNameCenter[0], 1.0f, 0.05f) &&
                  near(levelNameCenter[2], 10.0f, 0.05f) &&
                  levelNameCenter[1] + cameraPosition.y > 1.8f,
              "and it hangs over the head of the box, not beside it");
        check(near(turnedNameCenter[0], levelNameCenter[0], 0.05f) &&
                  near(turnedNameCenter[1], levelNameCenter[1], 0.05f) &&
                  near(turnedNameCenter[2], levelNameCenter[2], 0.05f),
              "turning the view cannot slide the nametag off the head either");

        // --- an unspellable name: still a projection, and it does move ------
        // The fallback is not a bug to fix here but a trade the module makes on
        // purpose (Arabic and CJK have no cells in the pixel face), so what has
        // to hold is that its label keeps up with the view instead of sticking to
        // where the last frame put it.
        const std::string arabic = "\xD9\x84\xD8\xA7\xD9\x84\xD8\xA8"; // "لاعب"
        name.rename(mob.data(), arabic);
        setCameraRotation(0.0f, 0.0f);
        renderFrame();
        const auto levelLabels = g_commands;
        setCameraRotation(-20.0f, 15.0f);
        renderFrame();
        check(g_commands.size() == levelLabels.size() && !g_commands.empty(),
              "the fallback label keeps being drawn while the view turns");
        check(std::fabs(g_commands.back().x - levelLabels.back().x) > 1.0f,
              "and follows the view, which is what a projection is for");
        setCameraRotation(0.0f, 0.0f);
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
        check(allVertices().size() == kWireVertsPerActor + readoutVertsForTenMeters(),
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
        check(allVertices().size() == kWireVertsPerActor + readoutVertsForTenMeters(),
              "and it comes back as soon as the raycast is clear");

        fake::g_solidBlocks = true;
        esp.throughWalls = true;
        renderFrame();
        check(allVertices().size() == kWireVertsPerActor + readoutVertsForTenMeters(),
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
        const MeshBatch* readout = labelBatch();
        check(readout != nullptr &&
                  readout->vertices.size() == readoutVertsForTenMeters(),
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
        check(labelBatch() != nullptr &&
                  labelBatch()->vertices.size() == readoutVertsForTenMeters(),
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
        check(labelBatch() != nullptr &&
                  labelBatch()->vertices.size() == readoutVertsForTenMeters(),
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
    // The value and the bar are part of the pinned column now: the bar is two
    // boxes in the label plane (a translucent track, then a fill on top of it)
    // and the number is spelled out of the same face as the name, so the stack
    // cannot come apart from the head it was measured against.
    {
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue, 15.0f);
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue + 8, 20.0f);
        writeAt(mob, Mob::mHealthAttribute, static_cast<void*>(mobHealthAttribute.data()));

        renderFrame();
        const MeshBatch* bars = barBatch();
        check(bars != nullptr && bars->vertices.size() == 16,
              "the bar is a track and a fill, in one grouped mesh");
        check(bars != nullptr && bars->colors.size() == 2 &&
                  near(bars->colors[0][3], 0.5f) && near(bars->colors[1][3], 1.0f),
              "which is how one batch carries two colors: the track's half alpha, "
              "the fill opaque");
        // 15 of 20 is three quarters, and green is what that fraction is worth.
        check(bars != nullptr && near(bars->colors[1][0], 0x22 / 255.0f, 0.01f) &&
                  near(bars->colors[1][1], 0xC5 / 255.0f, 0.01f),
              "a healthy entity's fill is the green one");
        check(labelBatch() != nullptr &&
                  labelBatch()->vertices.size() ==
                      readoutVertsForTenMeters() + billboardVertsFor("15"),
              "and the value is a line of the label batch, over the readout");
        check(filledRectCount() == 0 && textCommandCount() == 0,
              "nothing of it is left on the HUD layer");

        // The column switches paths whole: an entity whose name the face cannot
        // spell keeps bar, value and name on the launcher's font, because a
        // pinned name over a projected bar is two labels that disagree.
        PlayerNameGuard hudName(mob.data(), kHudName);
        renderFrame();
        check(filledRectCount() == 2 && textCommandCount() == 2,
              "an unspellable name takes the bar and the value back to the HUD with it");
        check(barBatch() == nullptr, "and leaves the mesh nothing to group");
        fake::g_actorIsPlayer = false;

        // Dead actors lose the bar instead of drawing a negative fraction.
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue, -4.0f);
        renderFrame();
        check(filledRectCount() == 0 && barBatch() == nullptr,
              "a non-positive health value draws no bar");
        writeAt(mobHealthAttribute, AttributeInstance::mCurrentValue, 20.0f);
        writeAt(mob, Mob::mHealthAttribute, static_cast<void*>(nullptr));
    }

    // --- Tracers -------------------------------------------------------------
    // Both origins aim at the world center of the entity's own box and both are
    // handed to the mesh now, so the line ends mid-hitbox at any FOV and cannot
    // slide off while the view moves. Only where they start differs: the feet
    // origin at the local player's own feet, the crosshair origin on the view
    // axis -- where every point projects to the middle of the screen, which is
    // what makes a crosshair line drawable as geometry at all. A line that
    // started *at the eye* lay on one single ray, the game collapsed it onto one
    // pixel, and the tracer vanished: the report this whole block exists for.
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
        check(lineBatchCount() == 2,
              "the crosshair origin gets its own line batch, exactly like Bottom");
        tracer = lastLineBatch();
        // Camera at (0, 1.62, 0) facing south, the hitbox five blocks out: the
        // segment starts half a block along the view axis, the far end of the
        // entity's own box.
        const Vec3 axisStart{0.0f, 1.62f, 0.5f};
        check(tracer != nullptr && tracer->vertices.size() == 2 &&
                  anyVertexCloseTo(tracer->vertices, hitboxCenter, cameraPosition),
              "which ends inside the hitbox, at the same world point Bottom aims at");
        check(tracer != nullptr &&
                  anyVertexCloseTo(tracer->vertices, axisStart, cameraPosition),
              "and starts on the view axis, in front of the eye instead of at it");
        check(lineCommands().empty(),
              "so nothing about it is left for the module's projection to place");

        // The pixel it starts on is a property of the axis, not of the module's
        // model of the camera: any depth along it lands on the surface center, so
        // the start is where the drift used to be visible and is now gone with it.
        const auto axisVertices = tracer->vertices;
        const float savedFov = esp.fov;
        esp.fov = 110.0f; // sprint FOV: the number the HUD path used to slide on
        renderFrame();
        check(lineBatchCount() == 2 && lastLineBatch() != nullptr &&
                  lastLineBatch()->vertices == axisVertices,
              "and an FOV the module was not told about cannot move the line at all");
        esp.fov = savedFov;

        // An entity off the edge of the surface is not a special case any more:
        // the game clips the line at the viewport, which is both cheaper than
        // clamping it here and exact -- the visible part points at the entity
        // because the whole of it does.
        setMobBox({40.0f, 0.0f, 4.0f}, {41.0f, 1.8f, 5.0f});
        renderFrame();
        check(lineBatchCount() == 2 && lineCommands().empty(),
              "an off-screen entity in front of the camera keeps geometry, not a snapline");
        check(lastLineBatch()->vertices.size() == 2 &&
                  anyVertexCloseTo(lastLineBatch()->vertices, {40.5f, 0.9f, 4.5f},
                                   cameraPosition),
              "whose far end is still the entity's own box center, unclamped");
        check(textCommandCount() == 0 && quadBatchCount() == 1,
              "while its HUD furniture is culled, the readout still clips in-world");

        // Behind the eye there is nothing for geometry to be pinned to, and that
        // one case keeps the screen-space snapline: the direction is the whole
        // message, so its end is pushed out to the edge of the screen rather than
        // to a projected point a thousand pixels out -- and never past it, which
        // is what would hand the launcher coordinates it rejects the batch for.
        setMobBox({-3.3f, 0.0f, -10.3f}, {-2.7f, 1.8f, -9.7f}); // behind the camera
        renderFrame();
        std::vector<pl::modmenu::DrawCommand> lines = lineCommands();
        check(lines.size() == 1, "an entity over the shoulder falls back to the snapline");
        check(lineBatchCount() == 1,
              "and the mesh gets no segment that could not be seen anyway");
        if (lines.size() == 1) {
            const float dx = lines[0].x + lines[0].w - 500.0f;
            const float dy = lines[0].y + lines[0].h - 500.0f;
            check(near(lines[0].x, 500.0f) && near(lines[0].y, 500.0f),
                  "which still starts on the middle of the surface");
            check(dx * dx + dy * dy <= 708.0f * 708.0f,
                  "and stops at the edge of the screen, not at infinity");
        }
        esp.tracer = false;
    }

    // --- Nametag sits above the head --------------------------------------
    // The name hangs off the world top-center of the entity's own box, measured
    // on the cell widths of the face that spells it. The 2D box's top-middle is
    // what the HUD path centers on -- an average of eight projected corners that
    // perspective slides away from the head, by tens of pixels on the off-axis
    // box below -- and it is also what an FOV the module has not been told about
    // used to move. Neither applies to a billboard: the numbers here are the
    // entity's own, so they are all the test has to say about them.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.0f, 0.0f, 4.0f}, {4.0f, 1.8f, 6.0f});
        PlayerNameGuard name(mob.data(), "Steve");
        renderFrame();

        const MeshBatch* labels = labelBatch();
        check(labels != nullptr, "the name is spelled into the label batch");
        check(textCommandCount() == 0,
              "and that is all of it: no HUD text is left to drift");

        // The readout shares the batch (one color, one flush), and what
        // separates the two is the head itself: the rows above it are the name's.
        std::vector<std::array<float, 3>> ink;
        for (const auto& vertex : labels->vertices) {
            if (vertex[1] + cameraPosition.y > 1.8f) ink.push_back(vertex);
        }
        check(ink.size() == billboardVertsFor("Steve"),
              "the whole name, one quad per merged glyph rectangle");

        // Head (3, 1.8, 5) at depth 5 on the 1000x1000, 90-degree surface: one
        // pixel is 2*tan(45)*5/1000 = 0.01 blocks, and a row of the 14px em is an
        // eighth of that cell -- the only two numbers the projection has left.
        const float px = 0.01f;
        const float em = 14.0f / static_cast<float>(esp::world::kPixelEm) * px;
        const auto advance = billboardInkFor("Steve");
        float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f, minZ = 1e30f,
              maxZ = -1e30f;
        for (const auto& vertex : ink) {
            minX = std::min(minX, vertex[0]);
            maxX = std::max(maxX, vertex[0]);
            minY = std::min(minY, vertex[1]);
            maxY = std::max(maxY, vertex[1]);
            minZ = std::min(minZ, vertex[2]);
            maxZ = std::max(maxZ, vertex[2]);
        }
        check(near((minX + maxX) * 0.5f + cameraPosition.x, 3.0f, 0.02f) &&
                  near((minZ + maxZ) * 0.5f + cameraPosition.z, 5.0f, 0.02f),
              "centered on the head of the hitbox, which no projection can move");
        check(minY + cameraPosition.y > 1.8f &&
                  maxY + cameraPosition.y < 1.8f + (14.0f + 4.0f) * px + em,
              "a line's height above the head, under the gap the column is stacked on");
        check(near(maxX - minX, (advance[1] - advance[0]) * em, 0.001f),
              "and as wide as the face measures itself, so the center means something");
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
    //
    // Since the label column went to the mesh, this is the face the *fallback*
    // is measured with: a name the pixel cells cannot spell, or a build whose
    // Tessellator did not resolve. The second is what the ASCII case below runs
    // with, because an ASCII name the mesh can spell never reaches the HUD.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.0f, 0.0f, 4.0f}, {4.0f, 1.8f, 6.0f});
        auto* nameField = new (mob.data() + Player::mName) std::string("Steve");
        fake::g_actorIsPlayer = true;
        auto savedBegin = s_mesh.begin;
        s_mesh.begin = nullptr;

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
        // With the mesh back, the same ASCII name is geometry and the question
        // of which font the launcher would have drawn it with does not arise.
        s_mesh.begin = savedBegin;
        renderFrame();
        // The mob is 5.83 blocks from the player's feet here, so its own batch is
        // the readout plus the name -- and the HUD has neither.
        check(textCommandCount() == 0 && labelBatch() != nullptr &&
                  labelBatch()->vertices.size() ==
                      billboardVertsFor("5.8m") + billboardVertsFor("Steve"),
              "with a mesh to draw on, the spellable name is not text at all");
        s_mesh.begin = nullptr;

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

        // That Arabic name is the case the mesh pass refuses on its own, with a
        // mesh to spend: it stays on the launcher's font while a Latin one next to
        // it goes into the level.
        s_mesh.begin = savedBegin;
        renderFrame();
        check(findText(arabic + " X") != nullptr,
              "and it is the HUD path even with a working mesh pass");
        check(quadBatchCount() == 1 &&
                  labelBatch()->vertices.size() == billboardVertsFor("5.8m"),
              "while the readout next to it stays pinned, and the bar and the value "
              "came up onto the HUD with the name rather than being split in two");

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
        PlayerNameGuard name(mob.data(), kHudName);
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
    // The HUD fallback is still a projection, so the camera basis has to stay
    // right-handed: a mob east of a south-facing camera belongs on the LEFT. (The
    // pinned column has no screen side to get wrong -- the game places it.)
    {
        PlayerNameGuard name(mob.data(), kHudName);
        setCameraRotation(0.0f, 0.0f);
        setMobBox({2.7f, 0.0f, 9.7f}, {3.3f, 1.8f, 10.3f});
        renderFrame();
        const pl::modmenu::DrawCommand* nameCmd = findText(kHudName);
        check(nameCmd != nullptr && nameCmd->x < centerX,
              "a mob to the east gets its labels left of center");
        // Copied, not kept as a pointer: renderFrame() below rebuilds the command
        // list the pointer looks into.
        const float xAhead = nameCmd != nullptr ? nameCmd->x : centerX;

        // Turned right, but not so far that the label leaves the surface: past
        // that the early-out drops it, which is a different assertion.
        setCameraRotation(0.0f, 15.0f);
        renderFrame();
        const pl::modmenu::DrawCommand* turnedCmd = findText(kHudName);
        check(turnedCmd != nullptr && turnedCmd->x < xAhead,
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
        check(allVertices().size() == kWireVertsPerActor + readoutVertsForTenMeters(),
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
                      (kWireVertsPerActor + readoutVertsForTenMeters()),
              "only the nearest kMaxDrawnActors overlays are emitted");
        fake::g_fetchedCount = 1;
        renderFrame();
        check(allVertices().size() == kWireVertsPerActor + readoutVertsForTenMeters(),
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
        PlayerNameGuard name(mob.data(), kHudName);
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
        PlayerNameGuard name(mob.data(), kHudName);
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
