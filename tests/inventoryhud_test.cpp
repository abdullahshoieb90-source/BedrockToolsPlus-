// Host integration tests for Inventory HUD and the real shared item plumbing.
// Minecraft functions are replaced by fake vtables / signature targets; the
// real module still reads stacks, paints icons and submits overlay commands.
// Requires preloader, nlohmann_json and entt headers (see scripts/run_tests.sh).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

// Include the implementations to let the fake engine use the exact render
// signatures, and to simulate an unavailable / later-resolved offhand accessor.
#include "modules/hud/huditems.cpp"
#include "modules/hud/inventoryhud.cpp"

class EntityRegistry {};

namespace {
namespace offsets = bedrocktools::sdk::offsets;
namespace hud = bedrocktools::huditems;

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("  FAIL %s\n", message);
        ++failures;
    }
}
bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

template<class T>
void put(void* object, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(value));
}

template<std::size_t Size>
struct alignas(void*) Storage {
    std::byte bytes[Size]{};
};

template<std::size_t Count>
struct Container {
    Storage<offsets::Inventory::FillingContainerItems + 2 * sizeof(void*)> data;
    Storage<offsets::Inventory::ItemStackSize> slots[Count];

    Container() {
        const auto begin = reinterpret_cast<std::uintptr_t>(slots);
        put(data.bytes, offsets::Inventory::FillingContainerItems, begin);
        put(data.bytes, offsets::Inventory::FillingContainerItems + sizeof(void*),
            begin + Count * offsets::Inventory::ItemStackSize);
    }
    void* stack(std::size_t index) { return slots[index].bytes; }
};

struct FakeItem {
    void** vtable;
    short maxDamage;
};

constexpr std::size_t FakeDamageOffset = 0x30;
void setStack(void* stack, void* counter, std::uint8_t count, int damage = 0) {
    put(stack, offsets::Inventory::ItemStackItemCounter, counter);
    put(stack, offsets::Inventory::ItemStackCount, count);
    put(stack, offsets::Inventory::ItemStackValid, static_cast<std::uint8_t>(counter != nullptr));
    put(stack, FakeDamageOffset, damage);
}

struct PaintedIcon {
    void* stack;
    float x;
    float y;
};
struct FilledCell {
    hud::RectangleArea area;
    hud::Color color;
    float alpha;
};
std::vector<PaintedIcon> icons;
std::vector<FilledCell> fills;
std::vector<pl::modmenu::DrawCommand> commands;
std::vector<pl::modmenu::HudEditorElement> elements;
std::string schemaJson;
void* player = nullptr;
void* mainhand = nullptr;
const void* offhand = nullptr;
const void* offhandPlayer = nullptr;
int offhandCalls = 0;
bool offhandAvailable = false;
int renderTag = 0;

void* fakePlayer(void*) { return player; }
void* fakeGame(void*) { return &renderTag; }
void* fakeCarriedItem(void*) { return mainhand; }
const void* fakeOffhand(const void* actor) {
    offhandPlayer = actor;
    ++offhandCalls;
    return offhand;
}
short fakeMaxDamage(void* item) { return static_cast<FakeItem*>(item)->maxDamage; }
int fakeDamage(void* stack) {
    int result;
    std::memcpy(&result, static_cast<std::byte*>(stack) + FakeDamageOffset, sizeof(result));
    return result;
}

hud::RectangleArea fakeClip(void*) { return {0.0f, 1000.0f, 0.0f, 1000.0f}; }
void fakeFlush(void*, const hud::Color&, float, const hud::HashedString&) {}
void fakeFill(void*, const hud::RectangleArea& area, const hud::Color& color, float alpha) {
    fills.push_back({area, color, alpha});
}
void fakeDestroyContext(void*) {}
void fakeCreateContext(void* context, void*, void*, void*) {
    static void* vtable[] = {reinterpret_cast<void*>(fakeDestroyContext)};
    put(context, 0, vtable);
    put(context, offsets::ShulkerPreview::BaseActorRenderContextItemRenderer, &renderTag);
}
std::uint64_t fakePaint(void*, void*, void* stack, unsigned int, unsigned char,
                        std::uint64_t, float x, float y, float, float, float) {
    icons.push_back({stack, x, y});
    return 0;
}

const PaintedIcon* findIcon(void* stack) {
    for (const auto& icon : icons) if (icon.stack == stack) return &icon;
    return nullptr;
}
const pl::modmenu::HudEditorElement* findElement(const char* elementId) {
    for (const auto& element : elements) if (element.elementId == elementId) return &element;
    return nullptr;
}
const pl::modmenu::DrawCommand* findText(const std::string& text) {
    for (const auto& command : commands) {
        if (command.type == pl::modmenu::DrawCommandType::Text && command.text == text) return &command;
    }
    return nullptr;
}
} // namespace

namespace pl::memory {
int hook(FuncPtr, FuncPtr, FuncPtr*, HookPriority) { return -1; }
bool unhook(FuncPtr, FuncPtr) { return true; }
std::uintptr_t resolveVtableFunction(std::string_view, std::size_t, std::string_view) { return 0; }
}
namespace pl::modmenu {
HudSurfaceSize getHudSurfaceSize() { return {1000.0f, 1000.0f}; }
void submitDrawCommands(std::string_view, std::span<const DrawCommand> submitted) {
    commands.assign(submitted.begin(), submitted.end());
}
void submitHudEditorElements(std::string_view, std::span<const HudEditorElement> submitted) {
    elements.assign(submitted.begin(), submitted.end());
}
bool setConfigSchemaJson(std::string_view, std::string_view schema) {
    schemaJson = schema;
    return true;
}
}
namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId id) {
    switch (id) {
        case SignatureId::ActorGetOffhandSlot:
            return offhandAvailable ? reinterpret_cast<std::uintptr_t>(fakeOffhand) : 0;
        case SignatureId::ItemStackBaseGetDamageValue:
            return reinterpret_cast<std::uintptr_t>(fakeDamage);
        case SignatureId::BaseActorRenderContextCtor:
            return reinterpret_cast<std::uintptr_t>(fakeCreateContext);
        case SignatureId::ItemRendererRenderGuiItemNew:
            return reinterpret_cast<std::uintptr_t>(fakePaint);
        default: return 0;
    }
}
}
namespace bedrocktools::events {
EventBus& bus() { static EventBus instance; return instance; }
}

int main() {
    std::array<void*, offsets::VTable::ItemGetAnimationFrameFor + 1> itemVtable{};
    itemVtable[offsets::VTable::ItemGetMaxDamage] = reinterpret_cast<void*>(fakeMaxDamage);
    FakeItem items[] = {{itemVtable.data(), 363}, {itemVtable.data(), 528},
                        {itemVtable.data(), 495}, {itemVtable.data(), 429},
                        {itemVtable.data(), 0}, {itemVtable.data(), 100}};
    void* counters[] = {&items[0], &items[1], &items[2], &items[3], &items[4], &items[5]};
    Container<4> armor;
    setStack(armor.stack(0), &counters[0], 1, 143);
    setStack(armor.stack(1), &counters[1], 1, 0);
    setStack(armor.stack(2), &counters[2], 1, 500);
    setStack(armor.stack(3), &counters[3], 1, 1);
    Container<36> inventory;
    setStack(inventory.stack(9), &counters[5], 1, 10);
    setStack(inventory.stack(10), &counters[4], 64);
    Storage<offsets::Inventory::ItemStackSize> heldStack;
    setStack(heldStack.bytes, &counters[4], 16);
    offhand = heldStack.bytes;
    mainhand = inventory.stack(0);

    // The armor component is valid, but its hand container is absent. The
    // offhand accessor is authoritative and must work independently of it.
    entt::basic_registry<EntityId> registry;
    EntityRegistry entityRegistry;
    const EntityId entity = registry.create();
    registry.emplace<ActorEquipmentComponent>(entity, nullptr, armor.data.bytes);
    Storage<offsets::Inventory::PlayerInventory + sizeof(void*)> actor;
    auto* entityContext = new (actor.bytes + offsets::Actor::mEntityContext)
        EntityContext{entityRegistry, registry, entity};
    std::array<void*, offsets::VTable::PlayerGetCarriedItem + 1> playerVtable{};
    playerVtable[offsets::VTable::PlayerGetCarriedItem] = reinterpret_cast<void*>(fakeCarriedItem);
    put(actor.bytes, 0, playerVtable.data());
    Storage<offsets::Inventory::PlayerInventoryContainer + sizeof(void*)> proxy;
    put(proxy.bytes, offsets::Inventory::PlayerInventoryContainer, inventory.data.bytes);
    put(actor.bytes, offsets::Inventory::PlayerInventory, proxy.bytes);
    player = actor.bytes;

    hud::initialize();
    auto equipment = hud::getEquipmentStacks(player);
    check(!equipment.offhand, "unresolved offhand signature safely returns no stack");
    check(equipment.armor[0] == armor.stack(0), "missing offhand accessor does not hide armor");
    offhandAvailable = true;
    hud::initialize(); // render functions were already resolved; retry offhand
    equipment = hud::getEquipmentStacks(player);
    check(equipment.offhand == offhand && offhandPlayer == player, "offhand comes from Actor::getOffhandSlot");
    check(equipment.mainhand == mainhand, "mainhand remains the carried item");
    for (std::size_t i = 0; i < 4; ++i) check(equipment.armor[i] == armor.stack(i), "armor order preserved");
    registry.remove<ActorEquipmentComponent>(entity);
    equipment = hud::getEquipmentStacks(player);
    check(equipment.offhand == offhand && equipment.mainhand == mainhand, "hands survive a missing armor component");
    check(!equipment.armor[0], "missing armor component produces empty armor slots");
    registry.emplace<ActorEquipmentComponent>(entity, nullptr, armor.data.bytes);
    const int calls = offhandCalls;
    check(!hud::getEquipmentStacks(nullptr).offhand && offhandCalls == calls, "null player never calls engine accessor");

    std::array<void*, offsets::VTable::ClientInstanceGetMinecraftGame + 1> clientVtable{};
    clientVtable[offsets::VTable::ClientInstanceGetLocalPlayer] = reinterpret_cast<void*>(fakePlayer);
    clientVtable[offsets::VTable::ClientInstanceGetMinecraftGame] = reinterpret_cast<void*>(fakeGame);
    void** client = clientVtable.data();
    std::array<void*, offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle + 1> contextVtable{};
    contextVtable[offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle] = reinterpret_cast<void*>(fakeClip);
    contextVtable[offsets::VTable::MinecraftUIRenderContextFlushImages] = reinterpret_cast<void*>(fakeFlush);
    contextVtable[offsets::VTable::MinecraftUIRenderContextFillRectangle] = reinterpret_cast<void*>(fakeFill);
    Storage<offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext + sizeof(void*)> context;
    put(context.bytes, 0, contextVtable.data());
    put(context.bytes, offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext, &renderTag);

    InventoryHudModule module;
    module.onInit();
    module.setMasterEnabled(true);
    auto frame = [&] {
        icons.clear();
        fills.clear();
        module.renderNative(context.bytes, &client);
        module.onFrame();
    };
    frame();

    // The module owns the inventory grid only: armor and offhand belong to the
    // separate Armor module now.
    check(!findIcon(heldStack.bytes) && !findText("220/363"),
          "the inventory grid never draws equipment or armor labels");
    check(!findIcon(armor.stack(0)), "armor icons are not painted by Inventory HUD");
    check(findIcon(inventory.stack(9)) && findIcon(inventory.stack(10)),
          "inventory items are painted");
    check(findText("64"), "inventory stack counts remain visible");
    check(!findText("1"), "single items do not get redundant stack counts");

    // Slot backgrounds are on by default and cover all 27 cells of the grid,
    // so empty slots keep their place.
    check(fills.size() == 27, "default slot backgrounds cover all 27 grid cells");
    check(near(fills[0].area.x0, 24.0f) && near(fills[0].area.x1, 56.0f) &&
              near(fills[0].area.y0, 200.0f) && near(fills[0].area.y1, 232.0f),
          "the first cell sits exactly under the first grid slot");
    check(near(fills[26].area.x0, 24.0f + 8.0f * 36.0f) && near(fills[26].area.y0, 200.0f + 2.0f * 36.0f),
          "the last cell sits at the bottom-right of the 9x3 grid");
    check(near(fills[0].color.r, 0.0f) && near(fills[0].color.g, 0.0f) && near(fills[0].color.b, 0.0f) &&
              static_cast<int>(fills[0].color.a * 255.0f + 0.5f) == 114,
          "default cells are black at the configured 45% opacity");

    const auto* firstIcon = findIcon(inventory.stack(9));
    const auto* gridElement = findElement(GridElementId);
    check(elements.size() == 1 && gridElement, "the module submits exactly one HUD editor element");
    check(gridElement && gridElement->positionKeyX == "hudPosX" && gridElement->positionKeyY == "hudPosY",
          "the grid element keeps the module position keys");
    check(gridElement && firstIcon && near(gridElement->x, firstIcon->x) &&
              near(gridElement->x + gridElement->width, firstIcon->x + 320.0f),
          "the grid element covers exactly the inventory grid");

    // Old configs may still carry the retired armor keys; they must be ignored
    // without disturbing the grid.
    nlohmann::json config;
    config["m_showEquipment"] = true;
    config["hudEquipmentPosX"] = 500.0f;
    config["hudEquipmentPosY"] = 12.0f;
    config["hudPosX"] = 120.0f;
    config["hudPosY"] = 300.0f;
    module.loadConfig(config);
    frame();
    firstIcon = findIcon(inventory.stack(9));
    check(firstIcon && near(firstIcon->x, 120.0f) && near(firstIcon->y, 300.0f),
          "the inventory grid renders at its own position");
    check(!findIcon(heldStack.bytes) && !findIcon(armor.stack(0)),
          "retired equipment keys no longer make the grid draw armor");
    check(elements.size() == 1 && findElement(GridElementId),
          "no equipment element is submitted any more");
    nlohmann::json saved;
    module.saveConfig(saved);
    check(!saved.contains("m_showEquipment") && !saved.contains("hudEquipmentPosX") &&
              !saved.contains("hudEquipmentPosY") && !saved.contains("m_showArmorDurability"),
          "armor options are gone from the Inventory HUD config");

    // A grid-only durability bar and a stack count for a damaged tool.
    config["m_showStackCount"] = false;
    module.loadConfig(config);
    frame();
    check(!findText("64"), "stack counts can be disabled");
    const bool bar = std::any_of(commands.begin(), commands.end(), [](const auto& command) {
        return command.type == pl::modmenu::DrawCommandType::RectFilled;
    });
    check(bar, "damaged inventory items keep their durability bar");
    config["m_showDurability"] = false;
    module.loadConfig(config);
    frame();
    check(commands.empty(), "both decorations can be turned off");
    config["m_showStackCount"] = true;
    config["m_showDurability"] = true;

    setStack(inventory.stack(13), &counters[4], 2);
    config["m_columns"] = 4;
    module.loadConfig(config);
    frame();
    check(findIcon(inventory.stack(13)) &&
              near(findIcon(inventory.stack(13))->y, 300.0f + 36.0f),
          "fewer columns wrap the grid into more rows");
    config["m_columns"] = 9;
    module.loadConfig(config);

    // Slot backgrounds follow the module's options.
    config["m_slotBackground"] = false;
    module.loadConfig(config);
    frame();
    check(fills.empty() && findIcon(inventory.stack(9)),
          "slot backgrounds can be switched off without hiding the grid");
    config["m_slotBackground"] = true;
    config["m_slotBgColor"] = "#0000FF";
    config["m_slotBgOpacity"] = 0.8f;
    module.loadConfig(config);
    frame();
    check(fills.size() == 27 && near(fills[13].area.x0, 120.0f + 4.0f * 36.0f) &&
              near(fills[13].area.y0, 300.0f + 36.0f),
          "styled backgrounds follow the grid geometry");
    check(near(fills[0].color.r, 0.0f) && near(fills[0].color.g, 0.0f) && near(fills[0].color.b, 1.0f) &&
              static_cast<int>(fills[0].color.a * 255.0f + 0.5f) == 204,
          "background color and opacity are applied");

    module.onMenuRegistered();
    check(schemaJson.find("m_showStackCount") != std::string::npos &&
              schemaJson.find("Number Text") != std::string::npos,
          "menu exposes the grid options");
    check(schemaJson.find("m_slotBackground") != std::string::npos &&
              schemaJson.find("m_slotBgOpacity") != std::string::npos &&
              schemaJson.find("m_slotBgColor") != std::string::npos &&
              schemaJson.find("Slot Background") != std::string::npos,
          "menu exposes the slot background option");
    check(schemaJson.find("m_showEquipment") == std::string::npos,
          "menu no longer offers the armor option");

    using namespace bedrocktools::events;
    ScreenStateEvent screen{ScreenKind::Container, ScreenPhase::Opened, nullptr};
    bus().publish(screen);
    frame();
    check(commands.empty() && icons.empty(), "container screen hides the grid");
    screen.phase = ScreenPhase::Closed;
    bus().publish(screen);
    frame();
    check(findIcon(inventory.stack(9)), "closing the container restores the grid");

    player = nullptr;
    frame();
    check(commands.empty() && icons.empty(), "world exit clears grid data");
    module.setMasterEnabled(false);
    check(commands.empty() && elements.empty(), "disable clears overlay and editor elements");
    bus().clear();
    entityContext->~EntityContext();

    std::printf("inventoryhud_test: %s (%d failures)\n", failures ? "FAILED" : "all checks passed", failures);
    return failures ? 1 : 0;
}
