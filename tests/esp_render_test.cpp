// Integration-style host test for the Esp overlay render path.
//
// It drives the real EspModule::onFrame with small in-memory Minecraft
// stand-ins (a local player, a mob, a LevelRendererPlayer camera position) and
// inspects the draw commands the module submits, so the screen-space overlay
// is verified end to end: camera basis -> projection -> draw commands.
//
// The cases mirror the bug reports the projection has to get right: a box has
// to sit on its entity, on the correct side of the screen, and keep sitting
// there while the view turns -- including when the entity is pressed right up
// against the camera.
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

std::vector<pl::modmenu::DrawCommand> g_commands;

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

// ---- Fake game functions ---------------------------------------------------
// The ESP module resolves these by signature; the pointers it calls go here.

bool fakeActorIsPlayer(void*) { return false; }
bool fakeActorIsInvisible(void*) { return false; }

// Layout-compatible stand-ins for the module's own DistanceSortedActor /
// ActorVec (it re-declares them in its own anonymous namespace, which is this
// one once the module is included below).
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

FetchedActor g_fetched[4];
int g_fetchedCount = 0;

FetchedActorVec fakeFetchNearby(void*, void*, int) {
    FetchedActorVec out{};
    out.begin = g_fetched;
    out.end = g_fetched + g_fetchedCount;
    out.cap = g_fetched + 4;
    return out;
}

void* g_clientInstance = nullptr;

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
            return reinterpret_cast<std::uintptr_t>(&fakeActorIsPlayer);
        case SignatureId::ActorIsInvisible:
            return reinterpret_cast<std::uintptr_t>(&fakeActorIsInvisible);
        case SignatureId::ActorFetchNearbyActorsSorted:
            return reinterpret_cast<std::uintptr_t>(&fakeFetchNearby);
        default:
            // No BlockSource (occlusion off) and no perspective hook (the
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

namespace bedrocktools::core::gamehooks {
void* clientInstance() { return g_clientInstance; }
} // namespace bedrocktools::core::gamehooks

// Include the production module so the test drives the real render path.
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

// The box outline is four Line commands; the launcher treats w/h as the
// end-point delta, so each command spans x .. x + w.
struct LineBounds {
    float minX = 1e30f;
    float maxX = -1e30f;
    float minY = 1e30f;
    float maxY = -1e30f;
    int count = 0;
};

LineBounds boxBounds() {
    LineBounds out;
    for (const auto& cmd : g_commands) {
        if (cmd.type != pl::modmenu::DrawCommandType::Line) continue;
        ++out.count;
        out.minX = std::min(out.minX, std::min(cmd.x, cmd.x + cmd.w));
        out.maxX = std::max(out.maxX, std::max(cmd.x, cmd.x + cmd.w));
        out.minY = std::min(out.minY, std::min(cmd.y, cmd.y + cmd.h));
        out.maxY = std::max(out.maxY, std::max(cmd.y, cmd.y + cmd.h));
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

    alignas(std::max_align_t) std::array<std::byte, 0x800> mob{};
    alignas(std::max_align_t) std::array<std::byte, 64> mobAabbComponent{};

    alignas(std::max_align_t) std::array<std::byte, 0x500> levelRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> playerRenderer{};

    // Local player: only its rotation (the camera orientation) is read.
    writeAt(player, Actor::mActorRotationComponent, static_cast<void*>(playerRotation.data()));
    writeAt(player, Actor::mStateVectorComponent, static_cast<void*>(playerState.data()));
    writeAt(player, Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent,
            static_cast<void*>(playerAabbComponent.data()));
    writeAt(player, Actor::mCategories, static_cast<std::uint32_t>(ActorCategories::IsPlayer));
    // Mob::mHealthAttribute stays null, so readHealth reports "no health".

    // Mob: an AABB shape component plus the mob category bit.
    writeAt(mob, Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent,
            static_cast<void*>(mobAabbComponent.data()));
    writeAt(mob, Actor::mCategories, static_cast<std::uint32_t>(ActorCategories::IsMob));

    // ClientInstance -> LevelRenderer -> LevelRendererPlayer::mCamPos.
    writeAt(levelRenderer, LevelRenderer::mLevelRendererPlayer, static_cast<void*>(playerRenderer.data()));
    // Camera at eye height so a mob standing on the ground straddles the
    // horizon, the way it does in game.
    const Vec3 cameraPosition{0.0f, 1.62f, 0.0f};
    writeAt(playerRenderer, LevelRendererPlayer::mCamPos, cameraPosition);
    std::array<std::byte, 0x800> client{};
    writeAt(client, ClientInstance::mLevelRenderer, static_cast<void*>(levelRenderer.data()));
    g_clientInstance = client.data();

    auto setCameraRotation = [&](float pitch, float yaw) {
        const Vec2 rot{pitch, yaw};
        writeAt(playerRotation, 0, rot);
    };
    auto setMobBox = [&](const Vec3& min, const Vec3& max) {
        const bedrocktools::sdk::AABB bounds{min, max};
        writeAt(mobAabbComponent, AABBShapeComponent::mAABB, bounds);
    };

    // --- Module ------------------------------------------------------------
    EspModule esp;
    esp.onInit();
    esp.setMasterEnabled(true);
    // 90 degrees vertical FOV on a square surface: tan(half fov) == 1 and the
    // aspect is 1, so 45 degrees off-axis lands exactly on the screen edge and
    // the expected coordinates below stay readable.
    esp.fov = 90.0f;

    bedrocktools::events::LocalPlayerTickEvent tick{
        reinterpret_cast<bedrocktools::sdk::Player*>(player.data())};
    bedrocktools::events::bus().publish(tick);

    g_fetched[0] = {mob.data(), 14.0f, 0.0f};
    g_fetchedCount = 1;

    const float centerX = kSurfaceWidth * 0.5f;
    const float centerY = kSurfaceHeight * 0.5f;

    // --- Handedness: the box lands on the correct side of the screen --------
    // Camera at the origin facing south (yaw 0). The mob sits east (+X) and
    // ahead, so its box belongs on the LEFT half of the screen. A camera basis
    // built as cross(worldUp, forward) draws it on the right instead.
    {
        setCameraRotation(0.0f, 0.0f);
        // ~17 degrees east of the view axis: inside the frustum, clearly to
        // the left of center (a mirrored basis puts it at x = 630..670).
        setMobBox({2.7f, 0.0f, 9.7f}, {3.3f, 1.8f, 10.3f});
        g_commands.clear();
        esp.onFrame();

        const LineBounds box = boxBounds();
        check(box.count == 4, "the 2D box is four line commands");
        check(box.count == 4 && box.maxX < centerX,
              "a mob to the east gets a box left of center");
        check(box.count == 4 && box.minY < centerY && box.maxY > centerY,
              "the box straddles the horizon of a level camera");
        check(textCommandCount() == 1, "the distance readout is still drawn");
    }

    // --- Turning the view ---------------------------------------------------
    // Increasing yaw turns right (south -> west). The mob is fixed in the
    // world, so its box has to slide LEFT as the view turns right, and RIGHT
    // as the view turns left. A mirrored basis sweeps it the wrong way, which
    // reads in game as "the hitbox moves when I move the screen".
    {
        setMobBox({0.7f, 0.0f, 9.7f}, {1.3f, 1.8f, 10.3f}); // just east of ahead

        setCameraRotation(0.0f, 0.0f);
        g_commands.clear();
        esp.onFrame();
        const LineBounds level = boxBounds();

        setCameraRotation(0.0f, 30.0f); // turn right
        g_commands.clear();
        esp.onFrame();
        const LineBounds turnedRight = boxBounds();

        setCameraRotation(0.0f, 330.0f); // turn left
        g_commands.clear();
        esp.onFrame();
        const LineBounds turnedLeft = boxBounds();

        check(level.count == 4 && turnedRight.count == 4 && turnedLeft.count == 4,
              "the box is drawn in all three view angles");
        check(level.count == 4 && turnedRight.count == 4 && turnedRight.maxX < level.maxX,
              "turning right moves the box left");
        check(level.count == 4 && turnedLeft.count == 4 && turnedLeft.maxX > level.maxX,
              "turning left moves the box right");
    }

    // --- Looking up and down ------------------------------------------------
    {
        setMobBox({-0.3f, 5.0f, 9.7f}, {0.3f, 6.8f, 10.3f}); // above the camera

        setCameraRotation(0.0f, 0.0f);
        g_commands.clear();
        esp.onFrame();
        const LineBounds level = boxBounds();

        setCameraRotation(-45.0f, 0.0f); // look up towards the mob
        g_commands.clear();
        esp.onFrame();
        const LineBounds lookingUp = boxBounds();

        check(level.count == 4 && lookingUp.count == 4,
              "the elevated box is drawn from both pitches");
        check(level.count == 4 && level.minY < centerY,
              "a mob above the horizon draws above center");
        check(level.count == 4 && lookingUp.count == 4 && lookingUp.minY > level.minY,
              "looking up brings that box down the screen");
    }

    // --- Entity pressed against the camera ----------------------------------
    // Corners of this box are behind the near plane. Dropping them collapses
    // the 2D box to a fraction of its real size, so the overlay detaches from
    // the entity; clipping the box against the plane keeps it covering the
    // screen, which is what a mob in your face should do.
    {
        setCameraRotation(0.0f, 0.0f);
        setMobBox({-0.3f, 0.0f, -3.0f}, {0.3f, 1.8f, 0.3f});
        g_commands.clear();
        esp.onFrame();

        const LineBounds box = boxBounds();
        check(box.count == 4, "the close mob still gets a box");
        check(box.count == 4 && box.minX <= 0.0f && box.maxX >= kSurfaceWidth,
              "the close mob's box spans the whole width");
        check(box.count == 4 && std::isfinite(box.minX) && std::isfinite(box.maxX) &&
                  box.minX >= -4.0f * kSurfaceWidth && box.maxX <= 5.0f * kSurfaceWidth,
              "off-screen box coordinates stay clamped and finite");
    }

    // --- Culling ------------------------------------------------------------
    {
        setCameraRotation(0.0f, 0.0f);

        setMobBox({-0.3f, 0.0f, -10.3f}, {0.3f, 1.8f, -9.7f}); // behind the camera
        g_commands.clear();
        esp.onFrame();
        check(boxBounds().count == 0 && g_commands.empty(),
              "a mob behind the camera draws nothing");

        setMobBox({9.7f, 0.0f, 2.7f}, {10.3f, 1.8f, 3.3f}); // far outside the frustum
        g_commands.clear();
        esp.onFrame();
        check(boxBounds().count == 0 && g_commands.empty(),
              "a mob outside the frustum draws nothing");

        // Show Mobs off: the same mob is not drawn at all.
        setMobBox({0.7f, 0.0f, 9.7f}, {1.3f, 1.8f, 10.3f});
        g_commands.clear();
        esp.onFrame();
        check(boxBounds().count == 4, "an on-screen mob is drawn before the filter test");

        esp.showMobs = false;
        g_commands.clear();
        esp.onFrame();
        check(g_commands.empty(), "disabling Show Mobs stops the overlay");
        esp.showMobs = true;
    }

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
