#include "inventoryhud.hpp"

#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/memory/Vtable.hpp>
#include <pl/ModMenuConfig.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t SlotCount = InventoryHudModule::TotalInventorySlots;
constexpr std::size_t FillingContainerItemsOffset = bedrocktools::sdk::offsets::Inventory::FillingContainerItems;
constexpr std::size_t ItemStackSize = bedrocktools::sdk::offsets::Inventory::ItemStackSize;
constexpr std::size_t MaxContainerSlots = 64;
constexpr float VanillaItemSize = 16.0f;
constexpr const char* MinecraftLibrary = "libminecraftpe.so";

#pragma pack(push, 4)
struct RectangleArea {
    float x0;
    float x1;
    float y0;
    float y1;
};

struct UiVec2 {
    float x;
    float y;
};
#pragma pack(pop)

namespace mce {
struct Color {
    float r;
    float g;
    float b;
    float a;
};

struct ClientTexture {
    std::byte storage[24]{};
};

class TexturePtr {
public:
    std::shared_ptr<const struct BedrockTextureData> clientTexture;
    std::shared_ptr<class ResourceLocation> resourceLocation;

    const ClientTexture& getClientTexture() const {
        static const ClientTexture empty{};
        return clientTexture ? *reinterpret_cast<const ClientTexture*>(clientTexture.get()) : empty;
    }
};
}

enum class ResourceFileSystem : int {
    UserPackage = 0
};

class ResourceLocation {
public:
    ResourceFileSystem fileSystem;
    std::string path;
    std::uint64_t pathHash;
    std::uint64_t fullHash;

    explicit ResourceLocation(const char* value)
        : fileSystem(ResourceFileSystem::UserPackage),
          path(value ? value : ""),
          pathHash(computeHash(path)),
          fullHash(pathHash ^ static_cast<std::uint64_t>(fileSystem)) {}

private:
    static std::uint64_t computeHash(std::string_view value) {
        constexpr std::uint64_t offset = 1469598103934665603ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;
        std::uint64_t hash = offset;
        for (unsigned char ch : value) hash = static_cast<std::uint64_t>(ch) ^ (prime * hash);
        return hash;
    }
};

class HashedString {
public:
    std::uint64_t hash;
    std::string value;
    mutable const HashedString* lastMatch;

    explicit HashedString(const char* text)
        : hash(computeHash(text ? std::string_view(text) : std::string_view())),
          value(text ? text : ""),
          lastMatch(nullptr) {}

private:
    static std::uint64_t computeHash(std::string_view text) {
        if (text.empty()) return 0;
        constexpr std::uint64_t offset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t prime = 0x100000001B3ULL;
        std::uint64_t result = offset;
        for (char character : text) {
            result = static_cast<std::uint64_t>(static_cast<unsigned char>(character)) ^ (prime * result);
        }
        return result;
    }
};

struct CachedUiTextures {
    bool loaded = false;
    mce::TexturePtr panel;
    mce::TexturePtr slot;
};

struct NbtTreeKey {
    const char* data;
    std::size_t len;
};

using HudCameraRendererFn = void (*)(void*, void*, void*, void*, int);
using BaseActorRenderContextCtorFn = void (*)(void*, void*, void*, void*);
using ItemStackBaseGetDamageValueFn = int (*)(void*);
using ItemStackBaseGetRawNameIdFn = std::string (*)(void*);
using ItemRendererRenderGuiItemNewFn = std::uint64_t (*)(
    void*, void*, void*, unsigned int, unsigned char, std::uint64_t,
    float, float, float, float, float);
using NbtTreeFindFn = void* (*)(void*, const NbtTreeKey*);

HudCameraRendererFn hudCameraRendererOriginal = nullptr;
BaseActorRenderContextCtorFn baseActorRenderContextCtor = nullptr;
ItemStackBaseGetDamageValueFn itemStackBaseGetDamageValue = nullptr;
ItemStackBaseGetRawNameIdFn itemStackBaseGetRawNameId = nullptr;
ItemRendererRenderGuiItemNewFn itemRendererRenderGuiItemNew = nullptr;
NbtTreeFindFn nbtTreeFind = nullptr;

InventoryHudModule* moduleInstance = nullptr;
bedrocktools::hooks::Handle hudRendererHook = nullptr;

void** getVtable(void* object) {
    return object ? *reinterpret_cast<void***>(object) : nullptr;
}

void* getLocalPlayer(void* client) {
    void** vtable = getVtable(client);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer])(client);
}

void* getMinecraftGame(void* client) {
    if (!client) return nullptr;
    void** vtable = getVtable(client);
    if (vtable && vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetMinecraftGame]) {
        void* game = reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetMinecraftGame])(client);
        if (game) return game;
    }
    return *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(client) + bedrocktools::sdk::offsets::ShulkerPreview::ClientInstanceMinecraftGame);
}

void* getStackItem(void* stack) {
    if (!stack) return nullptr;
    void* counter = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(stack) + bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseItem);
    if (!counter) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(counter) + bedrocktools::sdk::offsets::ShulkerPreview::SharedCounterPointer);
}

void* getStackUserData(void* stack) {
    if (!stack) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(stack) + bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseUserData);
}

short getMaxDamage(void* item) {
    void** vtable = getVtable(item);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ItemGetMaxDamage]) return 0;
    return reinterpret_cast<short (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ItemGetMaxDamage])(item);
}

unsigned int getItemAnimationFrame(void* item, void* localPlayer, void* stack) {
    if (!item || !localPlayer || !stack) return 0;
    void** vtable = getVtable(item);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor]) return 0;
    using Fn = unsigned int (*)(void*, void*, int, void*, int);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor])(item, localPlayer, 0, stack, 1);
}

RectangleArea getFullClippingRectangle(void* context) {
    RectangleArea result{};
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle]) return result;
    using Fn = RectangleArea (*)(void*);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle])(context);
}

void uiFlushImages(void* context, const mce::Color& color, float alpha, const HashedString& material) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages]) return;
    using Fn = void (*)(void*, const mce::Color&, float, const HashedString&);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages])(context, color, alpha, material);
}

void uiDrawImage(void* context, const mce::ClientTexture& texture, const UiVec2& position, const UiVec2& size, const UiVec2& uv, const UiVec2& uvSize, bool tiled) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage]) return;
    using Fn = void (*)(void*, const mce::ClientTexture&, const UiVec2&, const UiVec2&, const UiVec2&, const UiVec2&, bool);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage])(context, texture, position, size, uv, uvSize, tiled);
}

void uiFillRectangle(void* context, const RectangleArea& rectangle, const mce::Color& color, float alpha) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle]) return;
    using Fn = void (*)(void*, const RectangleArea&, const mce::Color&, float);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle])(context, rectangle, color, alpha);
}

mce::TexturePtr uiGetTexture(void* context, const ResourceLocation& location, bool forceReload) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture]) return {};
    using Fn = mce::TexturePtr (*)(void*, const ResourceLocation&, bool);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture])(context, location, forceReload);
}

bool hasTexture(const mce::TexturePtr& texture) {
    return static_cast<bool>(texture.clientTexture);
}

CachedUiTextures& getTextures(void* context) {
    static CachedUiTextures textures;
    if (!textures.loaded) {
        textures.panel = uiGetTexture(context, ResourceLocation("textures/ui/dialog_background_opaque"), false);
        textures.slot = uiGetTexture(context, ResourceLocation("textures/ui/item_cell"), false);
        textures.loaded = true;
    }
    return textures;
}

void setHudOpacity(void* context, float opacity) {
    if (!context) return;
    void* screenContext = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(context) + bedrocktools::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext);
    if (!screenContext) return;
    auto* constantBuffers = *reinterpret_cast<std::byte**>(reinterpret_cast<std::byte*>(screenContext) + 0x20);
    if (!constantBuffers) return;
    auto* shaderConstantBuffer = *reinterpret_cast<std::byte**>(constantBuffers + 0x150);
    if (!shaderConstantBuffer) return;
    auto* opacityPtr = *reinterpret_cast<float**>(shaderConstantBuffer + 0x30);
    if (!opacityPtr) return;
    if (*opacityPtr != opacity) {
        *opacityPtr = opacity;
        *reinterpret_cast<std::uint8_t*>(shaderConstantBuffer + 0x29) = 1;
    }
}

bool needsTextureOpacityPass(void* stack) {
    if (!stack || !itemStackBaseGetRawNameId) return false;
    const std::string name = itemStackBaseGetRawNameId(stack);
    return name == "leather_helmet" ||
           name == "leather_chestplate" ||
           name == "leather_leggings" ||
           name == "leather_boots" ||
           name == "firework_star" ||
           name == "leather_horse_armor" ||
           name == "potion" ||
           name == "splash_potion" ||
           name == "lingering_potion";
}

bool validRectangle(const RectangleArea& area) {
    return std::isfinite(area.x0) && std::isfinite(area.x1) && std::isfinite(area.y0) && std::isfinite(area.y1) &&
           area.x1 > area.x0 && area.y1 > area.y0;
}

void destroyBaseActorRenderContext(void* context) {
    void** vtable = getVtable(context);
    if (vtable && vtable[0]) reinterpret_cast<void (*)(void*)>(vtable[0])(context);
}

void* treeFindNode(void* compound, const char* key, std::size_t length) {
    if (!compound || !nbtTreeFind) return nullptr;
    NbtTreeKey searchKey{key, length};
    auto* base = reinterpret_cast<std::byte*>(compound);
    void* treeRoot = base + bedrocktools::sdk::offsets::ShulkerPreview::CompoundTagTreeRoot;
    void* treeEnd = base + bedrocktools::sdk::offsets::ShulkerPreview::CompoundTagTreeEnd;
    void* node = nbtTreeFind(treeRoot, &searchKey);
    return node == treeEnd ? nullptr : node;
}

std::uint32_t nodeType(void* node) {
    return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(node) + bedrocktools::sdk::offsets::ShulkerPreview::NbtNodeType);
}

void* nodePayload(void* node) {
    return reinterpret_cast<std::byte*>(node) + bedrocktools::sdk::offsets::ShulkerPreview::NbtNodePayload;
}

template <std::size_t N>
bool containsTag(void* compound, const char (&key)[N]) {
    return treeFindNode(compound, key, N - 1) != nullptr;
}

template <std::size_t N>
void* getCompoundTag(void* compound, const char (&key)[N]) {
    void* node = treeFindNode(compound, key, N - 1);
    if (!node || nodeType(node) != 10) return nullptr;
    return nodePayload(node);
}

template <std::size_t N>
bool readIntTag(void* compound, const char (&key)[N], int& output) {
    void* node = treeFindNode(compound, key, N - 1);
    if (!node) return false;
    auto* value = reinterpret_cast<std::byte*>(node) + bedrocktools::sdk::offsets::ShulkerPreview::NbtNodeNumericValue;
    switch (nodeType(node)) {
        case 1: output = *reinterpret_cast<std::uint8_t*>(value); return true;
        case 2: output = *reinterpret_cast<std::uint16_t*>(value); return true;
        case 3: output = *reinterpret_cast<std::int32_t*>(value); return true;
        default: return false;
    }
}

int readBundleWeight(void* stack) {
    void* userData = getStackUserData(stack);
    if (!userData) return -1;
    void* tagCompound = getCompoundTag(userData, "tag");
    if (!tagCompound) return -1;
    int weight = 0;
    return readIntTag(tagCompound, "bundle_weight", weight) ? weight : -1;
}

bool hasEnchantmentData(void* compound) {
    if (!compound || !nbtTreeFind) return false;
    return containsTag(compound, "ench") ||
           containsTag(compound, "Enchantments") ||
           containsTag(compound, "StoredEnchantments") ||
           containsTag(compound, "minecraft:enchantments") ||
           containsTag(compound, "minecraft:stored_enchantments");
}

template <class T>
T read(const void* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object) + offset);
}

bool inventorySlots(const void* player, std::uintptr_t& begin, std::size_t& size) {
    begin = 0;
    size = 0;
    if (!player) return false;
    const void* proxy = read<const void*>(player, bedrocktools::sdk::offsets::Inventory::PlayerInventory);
    if (!proxy) return false;
    const void* inventory = read<const void*>(proxy, bedrocktools::sdk::offsets::Inventory::PlayerInventoryContainer);
    if (!inventory) return false;
    begin = read<std::uintptr_t>(inventory, FillingContainerItemsOffset);
    const auto end = read<std::uintptr_t>(inventory, FillingContainerItemsOffset + sizeof(void*));
    if (!begin && !end) return true;
    if (!begin || end < begin || begin % alignof(void*) != 0) return false;
    const auto bytes = end - begin;
    if (bytes % ItemStackSize != 0) return false;
    size = bytes / ItemStackSize;
    return size <= MaxContainerSlots;
}

bool isStackValid(void* stack) {
    if (!stack) return false;
    return *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<const std::byte*>(stack) + bedrocktools::sdk::offsets::Inventory::ItemStackValid) != 0;
}

std::uint8_t getStackCount(void* stack) {
    if (!stack) return 0;
    return *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<const std::byte*>(stack) + bedrocktools::sdk::offsets::Inventory::ItemStackCount);
}

int getStackDamage(void* stack) {
    if (!stack || !itemStackBaseGetDamageValue) return 0;
    return std::max(0, itemStackBaseGetDamageValue(stack));
}

std::uint32_t parseColor(const std::string& value) {
    if (value.empty()) return 0xFFFFFFFFu;
    const std::string hex = value[0] == '#' ? value.substr(1) : value;
    try {
        if (hex.size() == 6) return 0xFF000000u | static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        if (hex.size() == 8) return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {
    }
    return 0xFFFFFFFFu;
}

mce::Color colorToMce(std::uint32_t argb, float opacity = 1.0f) {
    const float a = ((argb >> 24) & 0xFF) / 255.0f * std::clamp(opacity, 0.0f, 1.0f);
    const float r = ((argb >> 16) & 0xFF) / 255.0f;
    const float g = ((argb >> 8) & 0xFF) / 255.0f;
    const float b = (argb & 0xFF) / 255.0f;
    return {r, g, b, a};
}

void drawNineSlice(void* context, const mce::ClientTexture& texture, const RectangleArea& rectangle) {
    constexpr float textureSize = 16.0f;
    constexpr float slice = 4.0f;
    const float width = std::max(0.0f, rectangle.x1 - rectangle.x0);
    const float height = std::max(0.0f, rectangle.y1 - rectangle.y0);
    const float middleWidth = std::max(0.0f, width - slice * 2.0f);
    const float middleHeight = std::max(0.0f, height - slice * 2.0f);
    const float textureMiddle = textureSize - slice * 2.0f;
    const UiVec2 positions[9] = {
        {rectangle.x0, rectangle.y0},
        {rectangle.x0 + slice, rectangle.y0},
        {rectangle.x1 - slice, rectangle.y0},
        {rectangle.x0, rectangle.y0 + slice},
        {rectangle.x0 + slice, rectangle.y0 + slice},
        {rectangle.x1 - slice, rectangle.y0 + slice},
        {rectangle.x0, rectangle.y1 - slice},
        {rectangle.x0 + slice, rectangle.y1 - slice},
        {rectangle.x1 - slice, rectangle.y1 - slice}
    };
    const UiVec2 sizes[9] = {
        {slice, slice}, {middleWidth, slice}, {slice, slice},
        {slice, middleHeight}, {middleWidth, middleHeight}, {slice, middleHeight},
        {slice, slice}, {middleWidth, slice}, {slice, slice}
    };
    const UiVec2 uvPositions[9] = {
        {0.0f, 0.0f},
        {slice / textureSize, 0.0f},
        {(textureSize - slice) / textureSize, 0.0f},
        {0.0f, slice / textureSize},
        {slice / textureSize, slice / textureSize},
        {(textureSize - slice) / textureSize, slice / textureSize},
        {0.0f, (textureSize - slice) / textureSize},
        {slice / textureSize, (textureSize - slice) / textureSize},
        {(textureSize - slice) / textureSize, (textureSize - slice) / textureSize}
    };
    const UiVec2 uvSizes[9] = {
        {slice / textureSize, slice / textureSize},
        {textureMiddle / textureSize, slice / textureSize},
        {slice / textureSize, slice / textureSize},
        {slice / textureSize, textureMiddle / textureSize},
        {textureMiddle / textureSize, textureMiddle / textureSize},
        {slice / textureSize, textureMiddle / textureSize},
        {slice / textureSize, slice / textureSize},
        {textureMiddle / textureSize, slice / textureSize},
        {slice / textureSize, slice / textureSize}
    };
    for (int index = 0; index < 9; ++index) {
        if (sizes[index].x <= 0.0f || sizes[index].y <= 0.0f) continue;
        uiDrawImage(context, texture, positions[index], sizes[index], uvPositions[index], uvSizes[index], false);
    }
}

void drawDurabilityBar(void* context, float x, float y, float size, int damage, int maxDamage) {
    if (maxDamage <= 0 || damage <= 0) return;
    float ratio = static_cast<float>(maxDamage - damage) / static_cast<float>(maxDamage);
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    const float barHeight = std::max(1.0f, size * (1.5f / 16.0f));
    const float barWidth = size * (13.0f / 16.0f);
    const float barX = x + size * (1.5f / 16.0f);
    const float barY = y + size * (13.0f / 16.0f);
    uiFillRectangle(context, {barX, barX + barWidth, barY, barY + barHeight + 0.5f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f);
    uiFillRectangle(context, {barX, barX + barWidth * ratio, barY, barY + barHeight}, {1.0f - ratio, ratio, 0.0f, 1.0f}, 1.0f);
}

void drawBundleFullnessBar(void* context, float x, float y, float size, int weight) {
    if (weight < 0) return;
    const float ratio = std::clamp(static_cast<float>(weight) / 64.0f, 0.0f, 1.0f);
    const float barHeight = std::max(1.0f, size * (1.5f / 16.0f));
    const float barWidth = size * (13.0f / 16.0f);
    const float barX = x + size * (1.5f / 16.0f);
    const float barY = y + size * (13.0f / 16.0f);
    const mce::Color fill = weight >= 64 ? mce::Color{1.0f, 0.40f, 0.40f, 1.0f} : mce::Color{0.40f, 0.40f, 1.0f, 1.0f};
    uiFillRectangle(context, {barX, barX + barWidth, barY, barY + barHeight + 0.5f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f);
    uiFillRectangle(context, {barX, barX + barWidth * ratio, barY, barY + barHeight}, fill, 1.0f);
}

inline std::size_t getInventorySlotIndex(int row, int col) {
    if (row < 3) {
        return 9 + row * 9 + col;
    }
    return col; // row == 3 is hotbar (0..8)
}

void hudCameraRendererDetour(void* self, void* context, void* client, void* value, int pass) {
    if (hudCameraRendererOriginal) hudCameraRendererOriginal(self, context, client, value, pass);
    if (moduleInstance && moduleInstance->enabled) moduleInstance->renderNative(context, client);
}

}

InventoryHudModule::InventoryHudModule()
    : Module("Inventory HUD", "Displays your inventory and hotbar items directly on your HUD.") {
    moduleInstance = this;
}

InventoryHudModule::~InventoryHudModule() {
    if (hudRendererHook) {
        bedrocktools::hooks::remove(hudRendererHook);
        hudRendererHook = nullptr;
        hudCameraRendererOriginal = nullptr;
    }
    if (moduleInstance == this) moduleInstance = nullptr;
}

void InventoryHudModule::onInit() {
    baseActorRenderContextCtor = reinterpret_cast<BaseActorRenderContextCtorFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BaseActorRenderContextCtor));
    itemStackBaseGetDamageValue = reinterpret_cast<ItemStackBaseGetDamageValueFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseGetDamageValue));
    itemStackBaseGetRawNameId = reinterpret_cast<ItemStackBaseGetRawNameIdFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseGetRawNameId));
    itemRendererRenderGuiItemNew = reinterpret_cast<ItemRendererRenderGuiItemNewFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemRendererRenderGuiItemNew));
    nbtTreeFind = reinterpret_cast<NbtTreeFindFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::NbtTreeFind));

    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>([this](auto& event) {
        if (event.screen == bedrocktools::events::ScreenKind::Container) {
            m_inContainer.store(event.phase == bedrocktools::events::ScreenPhase::Opened, std::memory_order_relaxed);
        } else if (event.screen == bedrocktools::events::ScreenKind::Chat) {
            m_inChat.store(event.phase == bedrocktools::events::ScreenPhase::Opened, std::memory_order_relaxed);
        }
    });

    const std::uintptr_t hudRenderer = pl::memory::resolveVtableFunction(
        "17HudCameraRenderer",
        bedrocktools::sdk::offsets::VTable::HudCameraRendererRender,
        MinecraftLibrary);
    if (hudRenderer && !hudRendererHook) {
        hudRendererHook = bedrocktools::hooks::install(
            reinterpret_cast<void*>(hudRenderer),
            reinterpret_cast<void*>(hudCameraRendererDetour),
            reinterpret_cast<void**>(&hudCameraRendererOriginal));
    }
}

void InventoryHudModule::onDisable() {
    clearRuntime();
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>{});
}

InventoryHudModule::ConfigSnapshot InventoryHudModule::snapshotConfig() const {
    std::lock_guard lock(m_configMutex);
    return {
        m_showHotbar,
        m_hideInContainer,
        m_hideInChat,
        m_showEmptySlots,
        m_showStackCount,
        m_showDurability,
        m_showBundleWeight,
        m_showGlint,
        m_showSlotBackgrounds,
        static_cast<BackgroundStyle>(m_backgroundStyle),
        hudPosX,
        hudPosY,
        m_slotSize,
        m_slotGap,
        m_hotbarGap,
        m_padding,
        m_cornerRadius,
        m_countTextSize,
        parseColor(m_countTextColor),
        parseColor(m_backgroundColor),
        m_backgroundOpacity,
        parseColor(m_slotColor),
        m_slotOpacity,
        m_gridSize,
        m_gridGap,
        m_snapThreshold,
        (m_snapToGrid ? pl::modmenu::HudSnapGrid : pl::modmenu::HudSnapNone) |
            (m_snapToElements ? pl::modmenu::HudSnapElements : pl::modmenu::HudSnapNone) |
            (m_snapToScreenCenter ? pl::modmenu::HudSnapScreenCenter : pl::modmenu::HudSnapNone)
    };
}

void InventoryHudModule::clearRuntime() {
    for (auto& slot : m_runtime) {
        slot.hasItem.store(false, std::memory_order_release);
        slot.count.store(0, std::memory_order_release);
        slot.damage.store(0, std::memory_order_release);
        slot.maxDamage.store(0, std::memory_order_release);
        slot.enchanted.store(false, std::memory_order_release);
        slot.bundleWeight.store(-1, std::memory_order_release);
    }
}

void InventoryHudModule::submitEditorElements(const ConfigSnapshot& config, float totalWidth, float totalHeight) {
    pl::modmenu::HudEditorElement element;
    element.elementId = "bedrocktools.inventoryhud";
    element.displayName = "Inventory HUD";
    element.positionKeyX = "hudPosX";
    element.positionKeyY = "hudPosY";
    element.x = config.hudPosX;
    element.y = config.hudPosY;
    element.width = std::max(1.0f, totalWidth);
    element.height = std::max(1.0f, totalHeight);
    element.gridSize = config.gridSize;
    element.snapThreshold = config.snapThreshold;
    element.gridGap = config.gridGap;
    element.snapFlags = config.snapFlags;

    const std::array<pl::modmenu::HudEditorElement, 1> elements{std::move(element)};
    pl::modmenu::submitHudEditorElements(moduleId, elements);
}

void InventoryHudModule::renderNative(void* context, void* client) {
    if (!context || !client || !baseActorRenderContextCtor || !itemRendererRenderGuiItemNew) {
        clearRuntime();
        return;
    }

    const ConfigSnapshot config = snapshotConfig();
    if ((config.hideInContainer && m_inContainer.load(std::memory_order_relaxed)) ||
        (config.hideInChat && m_inChat.load(std::memory_order_relaxed))) {
        clearRuntime();
        return;
    }

    void* localPlayer = getLocalPlayer(client);
    if (!localPlayer) {
        clearRuntime();
        return;
    }

    std::uintptr_t begin = 0;
    std::size_t size = 0;
    if (!inventorySlots(localPlayer, begin, size) || size == 0) {
        clearRuntime();
        return;
    }

    const pl::modmenu::HudSurfaceSize surface = pl::modmenu::getHudSurfaceSize();
    const RectangleArea full = getFullClippingRectangle(context);
    const bool canRender = surface.width > 0.0f && surface.height > 0.0f && validRectangle(full);
    if (!canRender) {
        clearRuntime();
        return;
    }

    const int totalRows = config.showHotbar ? 4 : 3;
    const float totalWidth = config.padding * 2.0f + Columns * config.slotSize + (Columns - 1) * config.slotGap;
    const float totalHeight = config.padding * 2.0f + totalRows * config.slotSize + (totalRows - 1) * config.slotGap +
                              (config.showHotbar ? config.hotbarGap : 0.0f);

    const float uiWidth = full.x1 - full.x0;
    const float uiHeight = full.y1 - full.y0;
    const float scaleX = uiWidth / surface.width;
    const float scaleY = uiHeight / surface.height;

    const float panelX0 = full.x0 + config.hudPosX * scaleX;
    const float panelY0 = full.y0 + config.hudPosY * scaleY;
    const float panelX1 = panelX0 + totalWidth * scaleX;
    const float panelY1 = panelY0 + totalHeight * scaleY;

    static const HashedString flushMaterial("ui_flush");

    // 1. Render Background
    if (config.backgroundStyle == BackgroundStyle::Textured) {
        CachedUiTextures& textures = getTextures(context);
        const mce::Color tint = colorToMce(config.backgroundColor, config.backgroundOpacity);
        if (hasTexture(textures.panel)) {
            drawNineSlice(context, textures.panel.getClientTexture(), {panelX0, panelX1, panelY0, panelY1});
            uiFlushImages(context, tint, 1.0f, flushMaterial);
        }
        if (config.showSlotBackgrounds && hasTexture(textures.slot)) {
            for (int r = 0; r < totalRows; ++r) {
                const float rowExtraY = (r == 3 ? config.hotbarGap : 0.0f);
                for (int c = 0; c < Columns; ++c) {
                    const std::size_t slotIndex = getInventorySlotIndex(r, c);
                    void* stack = (slotIndex < size) ? reinterpret_cast<void*>(begin + slotIndex * ItemStackSize) : nullptr;
                    const bool hasItem = isStackValid(stack) && getStackItem(stack) != nullptr;
                    if (!config.showEmptySlots && !hasItem) continue;

                    const float slotVirtX = config.hudPosX + config.padding + c * (config.slotSize + config.slotGap);
                    const float slotVirtY = config.hudPosY + config.padding + r * (config.slotSize + config.slotGap) + rowExtraY;
                    const float slotX0 = full.x0 + slotVirtX * scaleX;
                    const float slotY0 = full.y0 + slotVirtY * scaleY;
                    const float slotW = config.slotSize * scaleX;
                    const float slotH = config.slotSize * scaleY;
                    uiDrawImage(context, textures.slot.getClientTexture(), {slotX0, slotY0}, {slotW, slotH}, {0.0f, 0.0f}, {1.0f, 1.0f}, false);
                }
            }
            uiFlushImages(context, tint, 1.0f, flushMaterial);
        }
    } else if (config.backgroundStyle == BackgroundStyle::Flat) {
        const mce::Color bgMce = colorToMce(config.backgroundColor, config.backgroundOpacity);
        uiFillRectangle(context, {panelX0, panelX1, panelY0, panelY1}, bgMce, bgMce.a);
        if (config.showSlotBackgrounds) {
            const mce::Color slotMce = colorToMce(config.slotColor, config.slotOpacity);
            for (int r = 0; r < totalRows; ++r) {
                const float rowExtraY = (r == 3 ? config.hotbarGap : 0.0f);
                for (int c = 0; c < Columns; ++c) {
                    const std::size_t slotIndex = getInventorySlotIndex(r, c);
                    void* stack = (slotIndex < size) ? reinterpret_cast<void*>(begin + slotIndex * ItemStackSize) : nullptr;
                    const bool hasItem = isStackValid(stack) && getStackItem(stack) != nullptr;
                    if (!config.showEmptySlots && !hasItem) continue;

                    const float slotVirtX = config.hudPosX + config.padding + c * (config.slotSize + config.slotGap);
                    const float slotVirtY = config.hudPosY + config.padding + r * (config.slotSize + config.slotGap) + rowExtraY;
                    const float slotX0 = full.x0 + slotVirtX * scaleX;
                    const float slotY0 = full.y0 + slotVirtY * scaleY;
                    const float slotX1 = slotX0 + config.slotSize * scaleX;
                    const float slotY1 = slotY0 + config.slotSize * scaleY;
                    uiFillRectangle(context, {slotX0, slotX1, slotY0, slotY1}, slotMce, slotMce.a);
                }
            }
        }
        uiFlushImages(context, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, flushMaterial);
    } else if (config.backgroundStyle == BackgroundStyle::Clean && config.showSlotBackgrounds) {
        const mce::Color slotMce = colorToMce(config.slotColor, config.slotOpacity);
        for (int r = 0; r < totalRows; ++r) {
            const float rowExtraY = (r == 3 ? config.hotbarGap : 0.0f);
            for (int c = 0; c < Columns; ++c) {
                const std::size_t slotIndex = getInventorySlotIndex(r, c);
                void* stack = (slotIndex < size) ? reinterpret_cast<void*>(begin + slotIndex * ItemStackSize) : nullptr;
                const bool hasItem = isStackValid(stack) && getStackItem(stack) != nullptr;
                if (!config.showEmptySlots && !hasItem) continue;

                const float slotVirtX = config.hudPosX + config.padding + c * (config.slotSize + config.slotGap);
                const float slotVirtY = config.hudPosY + config.padding + r * (config.slotSize + config.slotGap) + rowExtraY;
                const float slotX0 = full.x0 + slotVirtX * scaleX;
                const float slotY0 = full.y0 + slotVirtY * scaleY;
                const float slotX1 = slotX0 + config.slotSize * scaleX;
                const float slotY1 = slotY0 + config.slotSize * scaleY;
                uiFillRectangle(context, {slotX0, slotX1, slotY0, slotY1}, slotMce, slotMce.a);
            }
        }
        uiFlushImages(context, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, flushMaterial);
    }

    // 2. Setup Item Renderer & Scan Slots
    alignas(16) std::byte baseActorRenderContext[bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextStorageSize]{};
    void* itemRenderer = nullptr;
    void* screenContext = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(context) + bedrocktools::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext);
    void* game = getMinecraftGame(client);
    if (screenContext && game) {
        baseActorRenderContextCtor(baseActorRenderContext, screenContext, client, game);
        itemRenderer = *reinterpret_cast<void**>(baseActorRenderContext + bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextItemRenderer);
    }

    bool hasAnyEnchanted = false;
    for (int r = 0; r < totalRows; ++r) {
        for (int c = 0; c < Columns; ++c) {
            const std::size_t slotIndex = getInventorySlotIndex(r, c);
            void* stack = (slotIndex < size) ? reinterpret_cast<void*>(begin + slotIndex * ItemStackSize) : nullptr;
            void* item = getStackItem(stack);
            const bool valid = item != nullptr && isStackValid(stack);
            const std::uint8_t count = valid ? getStackCount(stack) : 0;
            const int damage = valid ? getStackDamage(stack) : 0;
            const int maxDamage = (valid && item) ? static_cast<int>(getMaxDamage(item)) : 0;
            const bool enchanted = valid && (hasEnchantmentData(stack) || hasEnchantmentData(getStackUserData(stack)));
            const int bundleWeight = valid ? readBundleWeight(stack) : -1;

            if (enchanted) hasAnyEnchanted = true;
            m_runtime[slotIndex].hasItem.store(valid && count > 0, std::memory_order_release);
            m_runtime[slotIndex].count.store(count, std::memory_order_release);
            m_runtime[slotIndex].damage.store(damage, std::memory_order_release);
            m_runtime[slotIndex].maxDamage.store(maxDamage, std::memory_order_release);
            m_runtime[slotIndex].enchanted.store(enchanted, std::memory_order_release);
            m_runtime[slotIndex].bundleWeight.store(bundleWeight, std::memory_order_release);
        }
    }

    if (itemRenderer) {
        const float iconSize = std::max(1.0f, std::min(config.slotSize * scaleX, config.slotSize * scaleY));
        const float iconScale = iconSize / VanillaItemSize;

        // Pass 1: Texture Opacity Pass (leather armor / potions)
        if (itemStackBaseGetRawNameId) {
            bool renderedTexturePass = false;
            setHudOpacity(context, 90.0f);
            for (int r = 0; r < totalRows; ++r) {
                const float rowExtraY = (r == 3 ? config.hotbarGap : 0.0f);
                for (int c = 0; c < Columns; ++c) {
                    const std::size_t slotIndex = getInventorySlotIndex(r, c);
                    if (!m_runtime[slotIndex].hasItem.load(std::memory_order_acquire)) continue;
                    void* stack = reinterpret_cast<void*>(begin + slotIndex * ItemStackSize);
                    void* item = getStackItem(stack);
                    if (!item || !needsTextureOpacityPass(stack)) continue;

                    const float slotVirtX = config.hudPosX + config.padding + c * (config.slotSize + config.slotGap);
                    const float slotVirtY = config.hudPosY + config.padding + r * (config.slotSize + config.slotGap) + rowExtraY;
                    const float x = full.x0 + slotVirtX * scaleX;
                    const float y = full.y0 + slotVirtY * scaleY;
                    const unsigned int animationFrame = getItemAnimationFrame(item, localPlayer, stack);

                    itemRendererRenderGuiItemNew(
                        itemRenderer,
                        baseActorRenderContext,
                        stack,
                        animationFrame,
                        0,
                        0,
                        x,
                        y,
                        1.0f,
                        20.0f,
                        iconScale);
                    renderedTexturePass = true;
                }
            }
            if (renderedTexturePass) uiFlushImages(context, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, flushMaterial);
            setHudOpacity(context, 1.0f);
        }

        // Pass 2: Regular Item Rendering
        bool renderedItems = false;
        for (int r = 0; r < totalRows; ++r) {
            const float rowExtraY = (r == 3 ? config.hotbarGap : 0.0f);
            for (int c = 0; c < Columns; ++c) {
                const std::size_t slotIndex = getInventorySlotIndex(r, c);
                if (!m_runtime[slotIndex].hasItem.load(std::memory_order_acquire)) continue;
                void* stack = reinterpret_cast<void*>(begin + slotIndex * ItemStackSize);
                void* item = getStackItem(stack);
                if (!item) continue;

                const float slotVirtX = config.hudPosX + config.padding + c * (config.slotSize + config.slotGap);
                const float slotVirtY = config.hudPosY + config.padding + r * (config.slotSize + config.slotGap) + rowExtraY;
                const float x = full.x0 + slotVirtX * scaleX;
                const float y = full.y0 + slotVirtY * scaleY;
                const unsigned int animationFrame = getItemAnimationFrame(item, localPlayer, stack);

                itemRendererRenderGuiItemNew(
                    itemRenderer,
                    baseActorRenderContext,
                    stack,
                    animationFrame,
                    0,
                    0,
                    x,
                    y,
                    1.0f,
                    1.0f,
                    iconScale);
                renderedItems = true;
            }
        }
        if (renderedItems) uiFlushImages(context, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, flushMaterial);

        // Pass 3: Enchantment Glint Pass
        if (config.showGlint && hasAnyEnchanted) {
            bool renderedGlint = false;
            for (int r = 0; r < totalRows; ++r) {
                const float rowExtraY = (r == 3 ? config.hotbarGap : 0.0f);
                for (int c = 0; c < Columns; ++c) {
                    const std::size_t slotIndex = getInventorySlotIndex(r, c);
                    if (!m_runtime[slotIndex].hasItem.load(std::memory_order_acquire) ||
                        !m_runtime[slotIndex].enchanted.load(std::memory_order_acquire)) continue;
                    void* stack = reinterpret_cast<void*>(begin + slotIndex * ItemStackSize);
                    void* item = getStackItem(stack);
                    if (!item) continue;

                    const float slotVirtX = config.hudPosX + config.padding + c * (config.slotSize + config.slotGap);
                    const float slotVirtY = config.hudPosY + config.padding + r * (config.slotSize + config.slotGap) + rowExtraY;
                    const float x = full.x0 + slotVirtX * scaleX;
                    const float y = full.y0 + slotVirtY * scaleY;
                    const unsigned int animationFrame = getItemAnimationFrame(item, localPlayer, stack);

                    itemRendererRenderGuiItemNew(
                        itemRenderer,
                        baseActorRenderContext,
                        stack,
                        animationFrame,
                        1,
                        1,
                        x,
                        y,
                        1.0f,
                        1.0f,
                        iconScale);
                    renderedGlint = true;
                }
            }
            if (renderedGlint) uiFlushImages(context, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, flushMaterial);
        }

        // Pass 4: Durability & Bundle Fullness Bars
        bool renderedBars = false;
        for (int r = 0; r < totalRows; ++r) {
            const float rowExtraY = (r == 3 ? config.hotbarGap : 0.0f);
            for (int c = 0; c < Columns; ++c) {
                const std::size_t slotIndex = getInventorySlotIndex(r, c);
                if (!m_runtime[slotIndex].hasItem.load(std::memory_order_acquire)) continue;

                const float slotVirtX = config.hudPosX + config.padding + c * (config.slotSize + config.slotGap);
                const float slotVirtY = config.hudPosY + config.padding + r * (config.slotSize + config.slotGap) + rowExtraY;
                const float x = full.x0 + slotVirtX * scaleX;
                const float y = full.y0 + slotVirtY * scaleY;

                const int damage = m_runtime[slotIndex].damage.load(std::memory_order_acquire);
                const int maxDamage = m_runtime[slotIndex].maxDamage.load(std::memory_order_acquire);
                const int bundleWeight = m_runtime[slotIndex].bundleWeight.load(std::memory_order_acquire);

                if (config.showDurability && maxDamage > 0 && damage > 0) {
                    drawDurabilityBar(context, x, y, iconSize, damage, maxDamage);
                    renderedBars = true;
                }
                if (config.showBundleWeight && bundleWeight >= 0) {
                    drawBundleFullnessBar(context, x, y, iconSize, bundleWeight);
                    renderedBars = true;
                }
            }
        }
        if (renderedBars) uiFlushImages(context, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, flushMaterial);

        destroyBaseActorRenderContext(baseActorRenderContext);
    }
}

void InventoryHudModule::onFrame() {
    if (!enabled) return;

    const ConfigSnapshot config = snapshotConfig();
    const int totalRows = config.showHotbar ? 4 : 3;
    const float totalWidth = config.padding * 2.0f + Columns * config.slotSize + (Columns - 1) * config.slotGap;
    const float totalHeight = config.padding * 2.0f + totalRows * config.slotSize + (totalRows - 1) * config.slotGap +
                              (config.showHotbar ? config.hotbarGap : 0.0f);

    submitEditorElements(config, totalWidth, totalHeight);

    if ((config.hideInContainer && m_inContainer.load(std::memory_order_relaxed)) ||
        (config.hideInChat && m_inChat.load(std::memory_order_relaxed))) {
        pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
        return;
    }

    std::vector<pl::modmenu::DrawCommand> commands;
    commands.reserve(SlotCount);

    if (config.showStackCount) {
        for (int r = 0; r < totalRows; ++r) {
            const float rowExtraY = (r == 3 ? config.hotbarGap : 0.0f);
            for (int c = 0; c < Columns; ++c) {
                const std::size_t slotIndex = getInventorySlotIndex(r, c);
                const bool hasItem = m_runtime[slotIndex].hasItem.load(std::memory_order_acquire);
                const std::uint8_t count = m_runtime[slotIndex].count.load(std::memory_order_acquire);
                if (!hasItem || count <= 1) continue;

                const float slotVirtX = config.hudPosX + config.padding + c * (config.slotSize + config.slotGap);
                const float slotVirtY = config.hudPosY + config.padding + r * (config.slotSize + config.slotGap) + rowExtraY;

                pl::modmenu::DrawCommand countCmd;
                countCmd.type = pl::modmenu::DrawCommandType::Text;
                countCmd.x = slotVirtX + config.slotSize - 1.0f;
                countCmd.y = slotVirtY + config.slotSize - 1.0f;
                countCmd.w = -1.0f; // Right-aligned
                countCmd.color = config.countTextColor;
                countCmd.size = config.countTextSize;
                countCmd.text = std::to_string(count);
                commands.push_back(std::move(countCmd));
            }
        }
    }

    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void InventoryHudModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("display")
        .category("display", "Display", "Inventory layout and visibility")
        .category("appearance", "Appearance", "Panel style, slot size, and colors")
        .category("items", "Items & Labels", "Stack counts, durability, and glint")
        .category("editor", "HUD Editor", "Snapping and positioning settings");

    auto node = [](std::string key, std::string title, std::string category, ConfigControlTypeV2 type) {
        ConfigNodeV2 value;
        value.id = key;
        value.key = std::move(key);
        value.title = std::move(title);
        value.category = std::move(category);
        value.type = type;
        return value;
    };
    auto section = [&](const char* id, const char* title, const char* category) {
        auto value = node(id, title, category, ConfigControlTypeV2::Section);
        value.key.clear();
        schema.node(std::move(value));
    };
    auto slider = [&](const char* key, std::string title, const char* category,
                      const char* sectionId, const char* min, const char* max,
                      const char* step = "1", const char* unit = " px",
                      const char* enabledKey = nullptr) {
        auto value = node(key, std::move(title), category, ConfigControlTypeV2::SliderFloat);
        value.section = sectionId;
        value.minValue = min;
        value.maxValue = max;
        value.step = step;
        value.unit = unit;
        if (enabledKey) value.visibleWhen = {{enabledKey, ConfigConditionOpV2::Truthy, {}}};
        schema.node(std::move(value));
    };

    // Category: display
    section("layout_section", "Layout", "display");
    schema.node(node("m_showHotbar", "Show Hotbar", "display", ConfigControlTypeV2::Toggle));
    schema.node(node("m_showEmptySlots", "Show Empty Slots", "display", ConfigControlTypeV2::Toggle));

    section("visibility_section", "Visibility", "display");
    schema.node(node("m_hideInContainer", "Hide In Inventory / Chests", "display", ConfigControlTypeV2::Toggle));
    schema.node(node("m_hideInChat", "Hide In Chat", "display", ConfigControlTypeV2::Toggle));

    section("activation_section", "Shortcut", "display");
    auto keybind = node("keybind", "Toggle Keybind", "display", ConfigControlTypeV2::Keybind);
    keybind.section = "activation_section";
    schema.node(std::move(keybind));

    // Category: appearance
    section("panel_style", "Background", "appearance");
    auto bgStyle = node("m_backgroundStyle", "Background Style", "appearance", ConfigControlTypeV2::Choice);
    bgStyle.section = "panel_style";
    bgStyle.choiceStyle = ConfigChoiceStyleV2::Segmented;
    bgStyle.options = {{"0", "Textured"}, {"1", "Flat Color"}, {"2", "Clean"}};
    bgStyle.defaultValue = "0";
    schema.node(std::move(bgStyle));

    auto bgColor = node("m_backgroundColor", "Background Color", "appearance", ConfigControlTypeV2::Color);
    bgColor.section = "panel_style";
    bgColor.defaultValue = "#000000";
    bgColor.colorAlpha = false;
    schema.node(std::move(bgColor));

    slider("m_backgroundOpacity", "Background Opacity", "appearance", "panel_style", "0", "1", "0.05", "");

    section("slots_style", "Slot Styling", "appearance");
    schema.node(node("m_showSlotBackgrounds", "Show Slot Cells", "appearance", ConfigControlTypeV2::Toggle));
    auto slotColor = node("m_slotColor", "Slot Cell Color", "appearance", ConfigControlTypeV2::Color);
    slotColor.section = "slots_style";
    slotColor.defaultValue = "#FFFFFF";
    slotColor.colorAlpha = false;
    slotColor.visibleWhen = {{"m_showSlotBackgrounds", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(slotColor));
    slider("m_slotOpacity", "Slot Cell Opacity", "appearance", "slots_style", "0", "1", "0.05", "", "m_showSlotBackgrounds");

    section("sizing_section", "Dimensions & Spacing", "appearance");
    slider("m_slotSize", "Slot Size", "appearance", "sizing_section", "12", "48");
    slider("m_slotGap", "Slot Spacing", "appearance", "sizing_section", "0", "16");
    slider("m_hotbarGap", "Hotbar Gap", "appearance", "sizing_section", "0", "20", "1", " px", "m_showHotbar");
    slider("m_padding", "Panel Padding", "appearance", "sizing_section", "0", "20");
    slider("m_cornerRadius", "Corner Radius", "appearance", "sizing_section", "0", "16");

    // Category: items
    section("counts_section", "Stack Counts", "items");
    schema.node(node("m_showStackCount", "Show Stack Counts", "items", ConfigControlTypeV2::Toggle));
    slider("m_countTextSize", "Count Text Size", "items", "counts_section", "6", "24", "1", " px", "m_showStackCount");
    auto countColor = node("m_countTextColor", "Count Text Color", "items", ConfigControlTypeV2::Color);
    countColor.section = "counts_section";
    countColor.defaultValue = "#FFFFFF";
    countColor.visibleWhen = {{"m_showStackCount", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(countColor));

    section("bars_section", "Indicators", "items");
    schema.node(node("m_showDurability", "Show Durability Bars", "items", ConfigControlTypeV2::Toggle));
    schema.node(node("m_showBundleWeight", "Show Bundle Fullness", "items", ConfigControlTypeV2::Toggle));
    schema.node(node("m_showGlint", "Show Enchantment Glint", "items", ConfigControlTypeV2::Toggle));

    // Category: editor
    auto help = node("editor_help", "Move In HUD Editor", "editor", ConfigControlTypeV2::Info);
    help.key.clear();
    help.description = "Open the HUD Editor to drag and position the Inventory HUD on screen.";
    schema.node(std::move(help));

    section("snapping_section", "Snapping", "editor");
    auto snapping = node("snap_targets", "Snap To", "editor", ConfigControlTypeV2::ToggleGroup);
    snapping.key.clear();
    snapping.section = "snapping_section";
    snapping.choiceStyle = ConfigChoiceStyleV2::Chips;
    snapping.options = {
        {"grid", "Grid", {}, "m_snapToGrid"},
        {"items", "Other Items", {}, "m_snapToElements"},
        {"center", "Screen Center", {}, "m_snapToScreenCenter"}
    };
    schema.node(std::move(snapping));
    slider("m_gridSize", "Grid Size", "editor", "snapping_section", "1", "100", "1", " px", "m_snapToGrid");
    slider("m_gridGap", "Gap Between Items", "editor", "snapping_section", "0", "100", "1", " px", "m_snapToElements");
    slider("m_snapThreshold", "Snap Distance", "editor", "snapping_section", "1", "100");

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

void InventoryHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard lock(m_configMutex);

    if (j.contains("hudPosX")) hudPosX = std::clamp(j["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudPosY")) hudPosY = std::clamp(j["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();

    if (j.contains("m_showHotbar")) m_showHotbar = j["m_showHotbar"].get<bool>();
    if (j.contains("m_hideInContainer")) m_hideInContainer = j["m_hideInContainer"].get<bool>();
    if (j.contains("m_hideInChat")) m_hideInChat = j["m_hideInChat"].get<bool>();
    if (j.contains("m_showEmptySlots")) m_showEmptySlots = j["m_showEmptySlots"].get<bool>();

    if (j.contains("m_showStackCount")) m_showStackCount = j["m_showStackCount"].get<bool>();
    if (j.contains("m_countTextSize")) m_countTextSize = std::clamp(j["m_countTextSize"].get<float>(), 6.0f, 40.0f);
    if (j.contains("m_countTextColor")) m_countTextColor = j["m_countTextColor"].get<std::string>();
    if (j.contains("m_showDurability")) m_showDurability = j["m_showDurability"].get<bool>();
    if (j.contains("m_showBundleWeight")) m_showBundleWeight = j["m_showBundleWeight"].get<bool>();
    if (j.contains("m_showGlint")) m_showGlint = j["m_showGlint"].get<bool>();

    if (j.contains("m_backgroundStyle")) {
        if (j["m_backgroundStyle"].is_number_integer()) {
            m_backgroundStyle = std::clamp(j["m_backgroundStyle"].get<int>(), 0, 2);
        } else if (j["m_backgroundStyle"].is_string()) {
            try {
                std::string val = j["m_backgroundStyle"].get<std::string>();
                const std::size_t comma = val.find(',');
                if (comma != std::string::npos) val.resize(comma);
                m_backgroundStyle = std::clamp(std::stoi(val), 0, 2);
            } catch (...) {
            }
        }
    }

    if (j.contains("m_showSlotBackgrounds")) m_showSlotBackgrounds = j["m_showSlotBackgrounds"].get<bool>();
    if (j.contains("m_backgroundColor")) m_backgroundColor = j["m_backgroundColor"].get<std::string>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = std::clamp(j["m_backgroundOpacity"].get<float>(), 0.0f, 1.0f);
    if (j.contains("m_slotColor")) m_slotColor = j["m_slotColor"].get<std::string>();
    if (j.contains("m_slotOpacity")) m_slotOpacity = std::clamp(j["m_slotOpacity"].get<float>(), 0.0f, 1.0f);
    if (j.contains("m_slotSize")) m_slotSize = std::clamp(j["m_slotSize"].get<float>(), 10.0f, 100.0f);
    if (j.contains("m_slotGap")) m_slotGap = std::clamp(j["m_slotGap"].get<float>(), 0.0f, 50.0f);
    if (j.contains("m_hotbarGap")) m_hotbarGap = std::clamp(j["m_hotbarGap"].get<float>(), 0.0f, 50.0f);
    if (j.contains("m_padding")) m_padding = std::clamp(j["m_padding"].get<float>(), 0.0f, 50.0f);
    if (j.contains("m_cornerRadius")) m_cornerRadius = std::clamp(j["m_cornerRadius"].get<float>(), 0.0f, 50.0f);

    if (j.contains("m_gridSize")) m_gridSize = std::clamp(j["m_gridSize"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_gridGap")) m_gridGap = std::clamp(j["m_gridGap"].get<float>(), 0.0f, 100.0f);
    if (j.contains("m_snapThreshold")) m_snapThreshold = std::clamp(j["m_snapThreshold"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_snapToGrid")) m_snapToGrid = j["m_snapToGrid"].get<bool>();
    if (j.contains("m_snapToElements")) m_snapToElements = j["m_snapToElements"].get<bool>();
    if (j.contains("m_snapToScreenCenter")) m_snapToScreenCenter = j["m_snapToScreenCenter"].get<bool>();
}

void InventoryHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);

    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;

    j["m_showHotbar"] = m_showHotbar;
    j["m_hideInContainer"] = m_hideInContainer;
    j["m_hideInChat"] = m_hideInChat;
    j["m_showEmptySlots"] = m_showEmptySlots;

    j["m_showStackCount"] = m_showStackCount;
    j["m_countTextSize"] = m_countTextSize;
    j["m_countTextColor"] = m_countTextColor;
    j["m_showDurability"] = m_showDurability;
    j["m_showBundleWeight"] = m_showBundleWeight;
    j["m_showGlint"] = m_showGlint;

    j["m_backgroundStyle"] = std::to_string(m_backgroundStyle) + ",Textured,Flat Color,Clean";
    j["m_showSlotBackgrounds"] = m_showSlotBackgrounds;
    j["m_backgroundColor"] = m_backgroundColor;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_slotColor"] = m_slotColor;
    j["m_slotOpacity"] = m_slotOpacity;
    j["m_slotSize"] = m_slotSize;
    j["m_slotGap"] = m_slotGap;
    j["m_hotbarGap"] = m_hotbarGap;
    j["m_padding"] = m_padding;
    j["m_cornerRadius"] = m_cornerRadius;

    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
}
