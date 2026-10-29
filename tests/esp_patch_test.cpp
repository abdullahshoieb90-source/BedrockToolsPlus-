// Behavior test for ESP (src/modules/visual/esp.cpp).
//
// Drives the production RenderLevel hook with fake actor/renderer memory
// and captures every tessellator call, so the grouping rules (players /
// mobs / items, invisible actors), the per-group colors, the box / fill /
// tracer passes, the fetch extent and the thick-line path are all checked
// against what a frame actually emits - the same approach as
// hitbox_patch_test.cpp.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson -pthread
//            tests/esp_patch_test.cpp -o /tmp/esp_patch_test
// Run:   /tmp/esp_patch_test

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

#include "bedrocktools/events/EventBus.hpp"
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/sdk/Offsets.hpp"
#include "bedrocktools/sdk/Types.hpp"

// Host stubs. No signature resolves, so the module finds no materials and
// takes its embedded selection-overlay fallback, while the test installs
// fake tessellator/fetch functions into the module's statics directly.
namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
} // namespace bedrocktools::memory

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

#include "modules/visual/esp.cpp"

namespace {

using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Vec3;
namespace offs = bedrocktools::sdk::offsets;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

// --- capture of everything the frame emits ----------------------------------

struct BeginCall {
    int mode;
    int vertexCount;
};
struct ColorCall {
    float r, g, b, a;
    std::size_t atVertex;
    std::size_t atBegin;
};
std::vector<BeginCall> g_begins;
std::vector<ColorCall> g_colors;
std::vector<Vec3> g_vertices;
int g_flushCalls = 0;

void fakeTessBegin(void*, void*, int mode, int vertexCount, int) {
    g_begins.push_back(BeginCall{mode, vertexCount});
}
void fakeTessColor(void*, float r, float g, float b, float a) {
    g_colors.push_back(ColorCall{r, g, b, a, g_vertices.size(), g_begins.size() - 1});
}
void fakeTessVertex(void*, float x, float y, float z) {
    g_vertices.push_back(Vec3{x, y, z});
}
void fakeRenderMesh(void*, void*, void*, char*) { ++g_flushCalls; }

bool colorNear(const ColorCall& c, std::uint32_t rgb, float alpha = 1.0f) {
    return near(c.r, static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, 1e-6f) &&
           near(c.g, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f, 1e-6f) &&
           near(c.b, static_cast<float>(rgb & 0xFF) / 255.0f, 1e-6f) &&
           near(c.a, alpha, 1e-6f);
}

// --- fake actors --------------------------------------------------------------

struct FakeActor {
    struct Shape {
        AABB aabb{{0, 0, 0}, {0, 0, 0}};
    };

    FakeActor() {
        const auto stateAddr = reinterpret_cast<std::uintptr_t>(&state);
        const auto shapeAddr = reinterpret_cast<std::uintptr_t>(&shape);
        writeAt(bytes, offs::Actor::mStateVectorComponent, stateAddr);
        writeAt(bytes, offs::Actor::mStateVectorComponent + offs::BuiltInActorComponents::mAABBShapeComponent, shapeAddr);
    }

    void* handle() { return bytes.data(); }
    void setBox(float x, float y, float z, float w = 0.6f, float h = 1.8f) {
        shape.aabb = {{x, y, z}, {x + w, y + h, z + w}};
    }
    void setCategories(std::uint32_t categories) {
        writeAt(bytes, offs::Actor::mCategories, categories);
    }

    alignas(16) std::array<std::byte, 0x248> bytes{};
    Vec3 state{0, 0, 0};
    Shape shape{};
};

std::unordered_set<void*> g_players;
std::unordered_set<void*> g_invisible;
bool fakeIsPlayer(void* actor) { return g_players.count(actor) != 0; }
bool fakeIsInvisible(void* actor) { return g_invisible.count(actor) != 0; }

std::vector<DistanceSortedActor> g_fetchList;
float g_lastFetchExtent = 0.0f;
ActorVec fakeFetch(void*, void* extent, int) {
    if (extent) g_lastFetchExtent = static_cast<float*>(extent)[0];
    if (g_fetchList.empty()) return ActorVec{};
    return ActorVec{g_fetchList.data(), g_fetchList.data() + g_fetchList.size(),
                    g_fetchList.data() + g_fetchList.size()};
}

void setFetch(std::initializer_list<void*> actors) {
    g_fetchList.clear();
    for (void* actor : actors) g_fetchList.push_back(DistanceSortedActor{actor, 0.0f, 0.0f});
}

// Runs one frame through the production hook with the camera at the origin,
// so captured vertices are world coordinates.
void drawFrame() {
    alignas(16) std::array<std::byte, 0xC0> screenContext{};
    alignas(16) std::array<std::byte, 0x500> levelRenderer{};
    alignas(16) std::array<std::byte, 0x1100> rendererPlayer{};
    alignas(16) std::array<float, 4> colorHolder{0.25f, 0.5f, 0.75f, 1.0f};

    writeAt(screenContext, offs::ScreenContext::mTessellator,
            static_cast<std::uintptr_t>(0x1234));
    writeAt(screenContext, offs::ScreenContext::mColorHolder,
            reinterpret_cast<std::uintptr_t>(colorHolder.data()));
    writeAt(levelRenderer, offs::LevelRenderer::mLevelRendererPlayer,
            reinterpret_cast<std::uintptr_t>(rendererPlayer.data()));
    writeAt(rendererPlayer, offs::LevelRendererPlayer::mCamPos, Vec3{0.0f, 0.0f, 0.0f});
    // Populated-looking embedded MaterialPtr so the line-material fallback
    // engages (no RenderMaterialGroup on the host).
    writeAt(rendererPlayer, offs::LevelRendererPlayer::mSelectionOverlayMaterial,
            static_cast<std::uintptr_t>(0x2345));

    g_begins.clear();
    g_colors.clear();
    g_vertices.clear();
    g_flushCalls = 0;
    g_lastFetchExtent = 0.0f;

    renderLevelHook(levelRenderer.data(), screenContext.data(), nullptr);

    // The hook must hand the color holder back untouched.
    if (!(near(colorHolder[0], 0.25f) && near(colorHolder[1], 0.5f) &&
          near(colorHolder[2], 0.75f) && near(colorHolder[3], 1.0f))) {
        std::printf("  FAIL color holder restored after the frame\n");
        ++g_failures;
    }
}

struct Bounds {
    bool valid = false;
    float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
};
Bounds capturedBounds() {
    Bounds b;
    for (const Vec3& v : g_vertices) {
        if (!b.valid) {
            b = Bounds{true, v.x, v.y, v.z, v.x, v.y, v.z};
            continue;
        }
        b.minX = std::fmin(b.minX, v.x);
        b.minY = std::fmin(b.minY, v.y);
        b.minZ = std::fmin(b.minZ, v.z);
        b.maxX = std::fmax(b.maxX, v.x);
        b.maxY = std::fmax(b.maxY, v.y);
        b.maxZ = std::fmax(b.maxZ, v.z);
    }
    return b;
}

} // namespace

int main() {
    ESPModule mod;
    mod.onInit(); // resolve() == 0 everywhere; install fakes instead.
    s_tessBegin = &fakeTessBegin;
    s_tessColor = &fakeTessColor;
    s_tessVertex = &fakeTessVertex;
    s_renderMesh = &fakeRenderMesh;
    s_actorIsPlayer = &fakeIsPlayer;
    s_actorIsInvisible = &fakeIsInvisible;
    s_fetchNearby = &fakeFetch;
    mod.setMasterEnabled(true);
    mod.lineThickness = 1.0f; // hairline-only: deterministic pass counts

    // Actors stay alive for the whole run (the fake identity sets key on
    // raw addresses; a recycled address must never inherit an identity).
    std::vector<std::unique_ptr<FakeActor>> keepAlive;
    auto keep = [&](std::unique_ptr<FakeActor> actor) {
        keepAlive.push_back(std::move(actor));
        return keepAlive.back().get();
    };
    auto makeActor = [&](bool player, std::uint32_t categories, float x) {
        auto actor = std::make_unique<FakeActor>();
        actor->setBox(x, 64.0f, 0.0f);
        actor->setCategories(categories);
        FakeActor* raw = keep(std::move(actor));
        if (player) g_players.insert(raw->handle());
        return raw;
    };
    auto makePlayer = [&](float x) {
        return makeActor(true, offs::ActorCategories::IsPlayer | offs::ActorCategories::IsMob, x);
    };
    auto makeMob = [&](float x) { return makeActor(false, offs::ActorCategories::IsMob, x); };
    auto makeItem = [&](float x) { return makeActor(false, offs::ActorCategories::IsItem, x); };

    auto* local = keep(std::make_unique<FakeActor>());
    local->setBox(-0.3f, 64.0f, -0.3f);
    s_localPlayer = local->handle();

    std::printf("esp box pass\n");
    {
        auto* target = makePlayer(5.0f); // box (5,64,0)-(5.6,65.8,0.6)
        setFetch({target->handle()});
        drawFrame();
        check(g_begins.size() == 1 && g_begins[0].mode == 4 && g_begins[0].vertexCount == 24,
              "one player -> one hairline pass of 12 edges (24 vertices)");
        check(g_vertices.size() == 24 && g_flushCalls == 1, "pass flushed exactly once");
        const Bounds b = capturedBounds();
        check(b.valid && near(b.minX, 5.0f) && near(b.maxX, 5.6f) && near(b.minY, 64.0f) &&
                  near(b.maxY, 65.8f) && near(b.minZ, 0.0f) && near(b.maxZ, 0.6f),
              "box edges trace the actor's collision box");
        check(g_colors.size() == 1 && colorNear(g_colors[0], mod.showPlayersColor),
              "player box uses the players color");
        check(near(g_lastFetchExtent, mod.espRange), "fetch extent follows the ESP range setting");
    }
    {
        auto* p = makePlayer(5.0f);
        auto* m = makeMob(8.0f);
        auto* i = makeItem(11.0f);
        setFetch({p->handle(), m->handle(), i->handle(), local->handle(), nullptr});
        drawFrame();
        check(g_begins.size() == 2, "items are hidden until Show Items is enabled");
        check(g_colors.size() == 2 && colorNear(g_colors[0], mod.showPlayersColor) &&
                  colorNear(g_colors[1], mod.showMobsColor),
              "one pass per group, each with its own color");

        mod.showItems = true;
        drawFrame();
        check(g_begins.size() == 3 && colorNear(g_colors[2], mod.showItemsColor),
              "items get their own pass and color when shown");

        mod.showPlayers = false;
        drawFrame();
        check(g_begins.size() == 2 && colorNear(g_colors[0], mod.showMobsColor),
              "players disappear when their toggle is off (self/null never drawn)");
        mod.showPlayers = true;
        mod.showItems = false;
    }
    {
        auto* ghost = makePlayer(5.0f);
        g_invisible.insert(ghost->handle());
        setFetch({ghost->handle()});
        drawFrame();
        check(g_begins.size() == 1, "invisible actors are shown by default (the ESP point)");
        mod.showInvisible = false;
        drawFrame();
        check(g_begins.empty(), "invisible actors hide when Show Invisible is off");
        mod.showInvisible = true;
        g_invisible.clear();
    }
    {
        setFetch({});
        drawFrame();
        check(g_begins.empty() && g_vertices.empty(), "empty world -> nothing drawn");
    }

    std::printf("esp tracer and fill passes\n");
    {
        auto* target = makePlayer(5.0f);
        mod.box = false;
        mod.tracers = true;
        setFetch({target->handle()});
        drawFrame();
        check(g_begins.size() == 1 && g_begins[0].mode == 4 && g_vertices.size() == 2,
              "tracer is a single line segment");
        check(g_vertices.size() == 2 && near(g_vertices[1].x, 5.3f) &&
                  near(g_vertices[1].y, 64.9f) && near(g_vertices[1].z, 0.3f),
              "tracer ends at the box center");
        check(g_vertices.size() == 2 &&
                  std::sqrt(g_vertices[0].x * g_vertices[0].x + g_vertices[0].y * g_vertices[0].y +
                            g_vertices[0].z * g_vertices[0].z) < 0.2f,
              "tracer starts just in front of the camera");
        mod.tracers = false;
    }
    {
        auto* target = makePlayer(5.0f);
        mod.fill = true; // box still off: fill only
        setFetch({target->handle()});
        drawFrame();
        check(g_begins.size() == 1 && g_begins[0].mode == 1 && g_begins[0].vertexCount == 24,
              "fill is one quad pass (6 faces x 4 vertices)");
        check(g_colors.size() == 1 && colorNear(g_colors[0], mod.showPlayersColor, mod.fillOpacity),
              "fill uses the group color at the fill opacity");
        mod.box = true;
        drawFrame();
        check(g_begins.size() == 2 && g_begins[0].mode == 1 && g_begins[1].mode == 4,
              "fill renders before the box edges when both are on");
        mod.fill = false;
    }

    std::printf("esp thick lines and range\n");
    {
        auto* target = makePlayer(5.0f);
        mod.lineThickness = 3.0f;
        setFetch({target->handle()});
        drawFrame();
        check(g_begins.size() == 2 && g_begins[0].mode == 1 && g_begins[1].mode == 4,
              "thickness above 1 adds a geometry pass before the hairline pass");
        check(g_vertices.size() > 24, "thick pass emits real quad geometry");
        mod.lineThickness = 1.0f;

        mod.espRange = 32.0f;
        drawFrame();
        check(near(g_lastFetchExtent, 32.0f), "fetch extent tracks range changes");
        mod.espRange = 64.0f;
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all esp behavior checks passed\n");
    return 0;
}
