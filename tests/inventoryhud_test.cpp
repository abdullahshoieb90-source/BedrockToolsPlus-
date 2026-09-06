// Host integration tests for Inventory HUD and the real shared item plumbing.
// Minecraft functions are replaced by fake vtables / signature targets; the
// real module still reads stacks, paints icons and submits overlay commands.
// Requires preloader, nlohmann_json and entt headers (see scripts/run_tests.sh).

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
std::vector<PaintedIcon> icons;
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
    Storage<offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext + sizeof(void*)> context;
    put(context.bytes, 0, contextVtable.data());
    put(context.bytes, offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext, &renderTag);

    InventoryHudModule module;
    module.onInit();
    module.setMasterEnabled(true);
    auto frame = [&] {
        icons.clear();
        module.renderNative(context.bytes, &client);
        module.onFrame();
    };
    frame();
    check(!findIcon(heldStack.bytes) && !findText("220/363"), "equipment is hidden by default");
    nlohmann::json config;
    config["m_showEquipment"] = true; // old config, without the new toggle
    module.loadConfig(config);
    frame();
    check(findIcon(heldStack.bytes) != nullptr, "offhand icon is painted when Armor & Offhand is enabled");
    check(findIcon(heldStack.bytes) && near(findIcon(heldStack.bytes)->y, 200.0f + 4.0f * 36.0f), "offhand stays below boots");
    check(findText("16") && findText("64"), "offhand and inventory stack counts remain visible");
    check(!findText("1"), "single items do not get redundant stack counts");
    check(findText("220/363") && findText("528/528") && findText("0/495") && findText("428/429"),
          "all four armor slots show clamped remaining/maximum durability by default");
    check(!findText("90/100"), "inventory items do not get armor labels");
    const auto* label = findText("220/363");
    const auto* firstIcon = findIcon(inventory.stack(9));
    check(label && near(label->h, 32.0f) && near(label->size, 12.0f), "armor label is centered within its row");

    // The grid and the armor + offhand column are two HUD editor elements with
    // separate position keys, which is what makes them draggable on their own.
    const auto* gridElement = findElement(GridElementId);
    const auto* armorElement = findElement(EquipmentElementId);
    check(elements.size() == 2 && gridElement && armorElement,
          "inventory grid and armor are separate HUD editor elements");
    check(gridElement && gridElement->positionKeyX == "hudPosX" && gridElement->positionKeyY == "hudPosY",
          "the grid element keeps the module position keys");
    check(armorElement && armorElement->positionKeyX == "hudEquipmentPosX" &&
              armorElement->positionKeyY == "hudEquipmentPosY",
          "the armor element owns its own position keys");
    check(gridElement && firstIcon && near(gridElement->x, firstIcon->x) &&
              near(gridElement->x + gridElement->width, firstIcon->x + 320.0f),
          "the grid element covers exactly the inventory grid");
    check(armorElement && near(armorElement->width, 32.0f + 4.0f + 84.0f) &&
              near(armorElement->height, 5 * 32.0f + 4 * 4.0f),
          "the armor element covers its icons and their durability labels");
    check(label && armorElement && label->x >= armorElement->x &&
              label->x + label->w <= armorElement->x + armorElement->width + 0.001f,
          "armor labels stay inside the armor element");
    // A column nobody positioned yet is placed beside the grid, not on top of it.
    check(armorElement && firstIcon && armorElement->x >= firstIcon->x + 320.0f,
          "an unplaced armor column does not overlap the inventory grid");
    const float labeledWidth = armorElement ? armorElement->width : 0.0f;

    setStack(heldStack.bytes, &counters[5], 1, 25); // a single damageable offhand item
    frame();
    check(findIcon(heldStack.bytes) && !findText("16") && !findText("75/100"),
          "single offhand item updates its icon without a stale count or armor label");
    const bool offhandBar = std::any_of(commands.begin(), commands.end(), [](const auto& command) {
        return command.type == pl::modmenu::DrawCommandType::RectFilled && near(command.y, 370.0f);
    });
    check(offhandBar, "damageable offhand items keep their durability bar");
    setStack(heldStack.bytes, &counters[4], 16);

    // Each element keeps the position the user gave it: moving one never moves
    // the other, in the overlay and in the HUD editor alike.
    nlohmann::json placed = config;
    placed["hudPosX"] = 120.0f;
    placed["hudPosY"] = 300.0f;
    placed["hudEquipmentPosX"] = 24.0f;
    placed["hudEquipmentPosY"] = 60.0f;
    module.loadConfig(placed);
    frame();
    firstIcon = findIcon(inventory.stack(9));
    check(firstIcon && near(firstIcon->x, 120.0f) && near(firstIcon->y, 300.0f),
          "the inventory grid renders at its own position");
    check(findIcon(heldStack.bytes) && near(findIcon(heldStack.bytes)->x, 24.0f) &&
              near(findIcon(heldStack.bytes)->y, 60.0f + 4.0f * 36.0f),
          "the armor column renders at its own position");
    check(findElement(GridElementId) && near(findElement(GridElementId)->x, 120.0f) &&
              near(findElement(GridElementId)->y, 300.0f),
          "the grid editor box sits on the grid");
    check(findElement(EquipmentElementId) && near(findElement(EquipmentElementId)->x, 24.0f) &&
              near(findElement(EquipmentElementId)->y, 60.0f),
          "the armor editor box sits on the armor column");

    placed["hudEquipmentPosX"] = 500.0f;
    placed["hudEquipmentPosY"] = 12.0f;
    module.loadConfig(placed);
    frame();
    check(findIcon(heldStack.bytes) && near(findIcon(heldStack.bytes)->x, 500.0f) &&
              near(findIcon(heldStack.bytes)->y, 12.0f + 4.0f * 36.0f),
          "the armor column can be moved on its own");
    firstIcon = findIcon(inventory.stack(9));
    check(firstIcon && near(firstIcon->x, 120.0f) && near(firstIcon->y, 300.0f),
          "moving the armor leaves the inventory grid where it was");

    placed["hudPosX"] = 60.0f;
    placed["hudPosY"] = 240.0f;
    module.loadConfig(placed);
    frame();
    firstIcon = findIcon(inventory.stack(9));
    check(firstIcon && near(firstIcon->x, 60.0f) && near(firstIcon->y, 240.0f),
          "the inventory grid can be moved on its own");
    check(findIcon(heldStack.bytes) && near(findIcon(heldStack.bytes)->x, 500.0f) &&
              near(findIcon(heldStack.bytes)->y, 12.0f + 4.0f * 36.0f),
          "moving the grid leaves the armor column where it was");

    // A column without a saved position is put beside the grid (here to its
    // right, the grid is too close to the left edge) and that resolved anchor is
    // stored, so it does not keep following the grid afterwards.
    nlohmann::json unplaced = placed;
    unplaced["hudEquipmentPosX"] = -1.0f;
    unplaced["hudEquipmentPosY"] = -1.0f;
    module.loadConfig(unplaced);
    frame();
    check(findIcon(heldStack.bytes) && near(findIcon(heldStack.bytes)->x, 60.0f + 320.0f + 4.0f) &&
              near(findIcon(heldStack.bytes)->y, 240.0f + 4.0f * 36.0f),
          "an armor column without a position is placed beside the grid");
    nlohmann::json resolved;
    module.saveConfig(resolved);
    check(near(resolved["hudEquipmentPosX"].get<float>(), 384.0f) &&
              near(resolved["hudEquipmentPosY"].get<float>(), 240.0f),
          "the resolved armor position is saved");
    resolved["hudPosX"] = 20.0f;
    module.loadConfig(resolved);
    frame();
    check(findIcon(heldStack.bytes) && near(findIcon(heldStack.bytes)->x, 384.0f),
          "the armor column keeps its own position once it has one");

    // Configs saved before the split carried a single anchor: the armor column at
    // it and the grid one slot plus separator to its right. Such a config is
    // split without moving anything the user had arranged.
    nlohmann::json legacy;
    legacy["hudPosX"] = 24.0f;
    legacy["hudPosY"] = 200.0f;
    legacy["m_showEquipment"] = true;
    module.loadConfig(legacy);
    frame();
    check(findIcon(heldStack.bytes) && near(findIcon(heldStack.bytes)->x, 24.0f) &&
              near(findIcon(heldStack.bytes)->y, 200.0f + 4.0f * 36.0f),
          "a legacy config keeps the armor column at the shared anchor");
    firstIcon = findIcon(inventory.stack(9));
    label = findText("220/363");
    check(firstIcon && near(firstIcon->x, 148.0f), "a legacy config keeps the grid where it was drawn");
    check(label && firstIcon && label->x + label->w < firstIcon->x,
          "a migrated legacy layout still keeps labels clear of the grid");
    module.loadConfig(legacy);
    frame();
    firstIcon = findIcon(inventory.stack(9));
    check(firstIcon && near(firstIcon->x, 148.0f), "loading a legacy config twice does not shift the grid");
    nlohmann::json migrated;
    module.saveConfig(migrated);
    check(near(migrated["hudPosX"].get<float>(), 148.0f) &&
              near(migrated["hudEquipmentPosX"].get<float>(), 24.0f) &&
              near(migrated["hudEquipmentPosY"].get<float>(), 200.0f),
          "a migrated legacy config saves both element positions");

    config["m_showStackCount"] = false;
    config["m_showDurability"] = false;
    module.loadConfig(config);
    frame();
    check(commands.size() == 4 && findText("528/528"), "armor numbers work with both bars and counts disabled");
    items[0].maxDamage = 0; // e.g. a carved pumpkin
    setStack(armor.stack(1), nullptr, 0);
    frame();
    check(commands.size() == 2 && !findText("220/363") && !findText("528/528"),
          "non-damageable and removed armor leave no stale labels");
    items[0].maxDamage = 363;
    setStack(armor.stack(1), &counters[1], 1);

    config["m_showArmorDurability"] = false;
    module.loadConfig(config);
    frame();
    check(commands.empty() && findIcon(heldStack.bytes), "numbers can be disabled without hiding equipment icons");
    check(findElement(EquipmentElementId) && findElement(EquipmentElementId)->width < labeledWidth,
          "disabling numbers reclaims label space in the armor element");
    nlohmann::json saved;
    module.saveConfig(saved);
    check(!saved["m_showArmorDurability"].get<bool>(), "armor-number toggle is saved");
    InventoryHudModule restored;
    restored.loadConfig(saved);
    nlohmann::json roundTrip;
    restored.saveConfig(roundTrip);
    check(!roundTrip["m_showArmorDurability"].get<bool>() && roundTrip["m_showEquipment"].get<bool>(),
          "equipment and numeric-durability options round-trip independently");
    check(near(roundTrip["hudPosX"].get<float>(), saved["hudPosX"].get<float>()) &&
              near(roundTrip["hudPosY"].get<float>(), saved["hudPosY"].get<float>()) &&
              near(roundTrip["hudEquipmentPosX"].get<float>(), saved["hudEquipmentPosX"].get<float>()) &&
              near(roundTrip["hudEquipmentPosY"].get<float>(), saved["hudEquipmentPosY"].get<float>()),
          "both element positions round-trip");

    config["m_showArmorDurability"] = true;
    config["m_slotSize"] = 8.0f;
    config["m_slotGap"] = 0.0f;
    config["m_countTextSize"] = 40.0f;
    config["m_countColor"] = "#12ABEF";
    module.loadConfig(config);
    frame();
    label = findText("220/363");
    firstIcon = findIcon(inventory.stack(9));
    check(label && near(label->size, 8.0f) && label->color == 0xFF12ABEFu, "number style is applied and fits tiny slots");
    const auto* tinyArmorElement = findElement(EquipmentElementId);
    check(label && tinyArmorElement && label->x >= tinyArmorElement->x &&
              label->x + label->w <= tinyArmorElement->x + tinyArmorElement->width + 0.001f,
          "large configured text and zero gap stay inside the armor element");
    module.onMenuRegistered();
    check(schemaJson.find("m_showArmorDurability") != std::string::npos && schemaJson.find("Number Text") != std::string::npos,
          "menu exposes armor numbers and shared text styling");

    using namespace bedrocktools::events;
    ScreenStateEvent screen{ScreenKind::Container, ScreenPhase::Opened, nullptr};
    bus().publish(screen);
    frame();
    check(commands.empty() && icons.empty(), "container screen hides both equipment icons and armor numbers");
    screen.phase = ScreenPhase::Closed;
    bus().publish(screen);
    frame();
    check(findText("220/363") && findIcon(heldStack.bytes), "closing container restores icons and numbers");

    config["m_showEquipment"] = false;
    module.loadConfig(config);
    frame();
    check(!findIcon(heldStack.bytes) && !findIcon(armor.stack(0)) && commands.empty(),
          "disabling equipment clears icons and numbers even while their own toggle is on");
    check(elements.size() == 1 && findElement(GridElementId) && !findElement(EquipmentElementId),
          "disabling equipment removes its HUD editor element and keeps the grid");
    config["m_showEquipment"] = true;
    module.loadConfig(config);
    offhand = nullptr;
    frame();
    check(!findIcon(heldStack.bytes) && findText("220/363"), "empty offhand does not hide armor or retain old icon");
    player = nullptr;
    frame();
    check(commands.empty() && icons.empty(), "world exit clears equipment data");
    module.setMasterEnabled(false);
    check(commands.empty() && elements.empty(), "disable clears overlay and editor elements");
    bus().clear();
    entityContext->~EntityContext();

    std::printf("inventoryhud_test: %s (%d failures)\n", failures ? "FAILED" : "all checks passed", failures);
    return failures ? 1 : 0;
}
