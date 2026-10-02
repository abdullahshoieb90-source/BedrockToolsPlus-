#pragma once

// Shared plumbing for the HUD modules that paint inventory items with the
// game's own ItemRenderer (ArmorHUD, Hotbar Slots, Inventory HUD).
//
// Those modules only differ in *which* stacks they show and *where*. Everything
// else lives here so it is written (and fixed for a new game version) once:
//
//   * the HudCameraRenderer::render hook that gives them a render pass,
//   * walking Player -> Inventory::PlayerInventory -> proxy ->
//     PlayerInventoryContainer -> FillingContainer::mItems in ItemStackSize
//     steps, plus the armor container and the equipment hand container
//     (offhand = slot 1, with the Actor::getOffhandSlot accessor as fallback),
//   * constructing a BaseActorRenderContext to reach the ItemRenderer,
//   * mapping launcher HUD units onto the MinecraftUIRenderContext,
//   * ItemRenderer::renderGuiItemNew for one icon and the final flushImages.
//
// The header stays free of preloader / entt includes; the heavy lifting is in
// huditems.cpp.

#include <bedrocktools/sdk/Offsets.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace bedrocktools::huditems {

// Resolves the ItemRenderer / ItemStackBase / BaseActorRenderContext
// functions from the signature table. Idempotent; call it from onInit().
void initialize();

// One HudCameraRenderer::render hook shared by every item HUD module. The
// listener runs on the render thread right after the vanilla HUD was drawn.
using RenderListener = void (*)(void* context, void* client, void* user);
bool addRenderListener(RenderListener listener, void* user);
void removeRenderListener(RenderListener listener, void* user);

// ---- Player / container access --------------------------------------------

void* getLocalPlayer(void* client);
void* getCarriedItem(void* player);

// The contiguous ItemStack array of a FillingContainer.
struct ContainerSlots {
    std::uintptr_t begin = 0; // first ItemStack, 0 when the container is unusable
    std::size_t count = 0;    // number of ItemStackSize-strided slots

    // ItemStack of a slot, nullptr when the index is out of range.
    void* stack(std::size_t index) const {
        if (!begin || index >= count) return nullptr;
        return reinterpret_cast<void*>(begin + index * sdk::offsets::Inventory::ItemStackSize);
    }
    explicit operator bool() const { return begin != 0 && count != 0; }
};

ContainerSlots containerSlots(void* container);

// The player's own inventory: slots 0-8 are the hotbar, 9-35 the 9x3 grid of
// the inventory screen.
ContainerSlots playerInventory(void* player);

struct EquipmentStacks {
    void* armor[4] = {}; // helmet, chestplate, leggings, boots
    void* offhand = nullptr;
    void* mainhand = nullptr;
};
EquipmentStacks getEquipmentStacks(void* player);

// ---- ItemStack inspection ---------------------------------------------------

void* stackItem(void* stack);         // Item*, nullptr for an empty slot
std::uint8_t stackCount(void* stack); // ItemStackBase::mCount
// Candidate offsets the damage probe reads, most likely first: the
// layout-derived mAuxValue (see Inventory::ItemStackDamage) and its neighbours,
// so a build that moved the field by a word still reports the right damage.
inline constexpr std::size_t DamageCandidateCount = 4;
inline constexpr std::size_t DamageCandidateOffsets[DamageCandidateCount] = {
    sdk::offsets::Inventory::ItemStackDamage,     // 0x20
    sdk::offsets::Inventory::ItemStackDamage - 4, // 0x1C
    sdk::offsets::Inventory::ItemStackDamage + 4, // 0x24
    sdk::offsets::Inventory::ItemStackDamage + 8, // 0x28
};

// The stack header dumped as 32-bit words for the diagnostics line: mUserData,
// mBlock, mAuxValue/mCount/mValid and the words around them, so the exact
// offset a future game build moved the damage to can be read straight off the
// HUD instead of guessed.
inline constexpr std::size_t DamageWindowBase = 0x10;
inline constexpr std::size_t DamageWindowCount = 8;

// One durability reading of a stack: the value the bars use, where it came from
// and the raw value of every candidate offset (for the diagnostics readout).
struct DamageProbe {
    enum class Source : std::uint8_t {
        None = 0,
        Accessor = 1, // ItemStackBase::getDamageValue
        Field = 2,    // the stack's own field at DamageCandidateOffsets[index]
    };

    int value = 0;           // damage kept for the bars, >= 0
    Source source = Source::None;
    int candidateIndex = -1; // set when source == Field
    int accessor = -1;       // what the accessor answered, -1 when unresolved
    int raw[DamageCandidateCount] = {-1, -1, -1, -1};
    int window[DamageWindowCount] = {}; // see DamageWindowBase below
};

// Reads the durability of one stack: the accessor when it is resolved and
// answers a plausible value, otherwise the damage field itself. Keeping the
// field as a fallback means a signature that landed on the wrong function can
// no longer freeze every item at full durability.
DamageProbe probeDamage(void* stack, int maxDamage);

// Damage (>= 0) of a stack: probeDamage() with the stack's own maximum.
int stackDamage(void* stack);
// Maximum damage of a stack's item (0 when unbreakable):
// ItemStackBase::getMaxDamage when it is resolved, otherwise the Item vtable.
int stackMaxDamage(void* stack);
int itemMaxDamage(void* item);        // Item::getMaxDamage, 0 when unbreakable
// Items whose icon needs the HUD-opacity pass (dyed leather etc., see
// IconPainter::beginOpacityFixPass).
bool needsTextureOpacityPass(void* stack);

// ---- Durability diagnostics --------------------------------------------------

// What the durability reader saw the last time it ran, so a HUD module can
// report it on screen when a game build stops producing damage values. The
// snapshot is global (the last probed stack wins) and cheap to read.
struct DurabilityDiagnostics {
    bool symbolFound = false;  // ItemStackBase::getDamageValue via its symbol
    bool patternFound = false; // ...via its byte pattern
    int accessorAnswer = -1;   // what that accessor returned (-1 = unresolved)
    int maxDamage = 0;
    int value = 0;
    int source = 0;            // DamageProbe::Source as an int
    int candidateIndex = -1;
    int raw[DamageCandidateCount] = {-1, -1, -1, -1};
    int window[DamageWindowCount] = {};
    // Durability bars painted since the last resetBarDiagnostics() call, so a
    // module can tell "no damage was read" apart from "nothing was drawn".
    int barsDrawn = 0;
};
DurabilityDiagnostics durabilityDiagnostics();
// Clears barsDrawn; HUD modules call it at the start of their native pass.
void resetBarDiagnostics();

// One HUD line: "BT+ 1.5.6 dmg[+0x20] 143/363 sym0 pat1 acc0 raw 0,143,0,0
// hex 0,0,8F,0,1,0 bars 3".
std::string durabilityDiagnosticsText(const DurabilityDiagnostics& diagnostics);

// ---- Drawing ----------------------------------------------------------------

// Launcher HUD units (pl::modmenu::getHudSurfaceSize) -> UI render context
// coordinates (MinecraftUIRenderContext::getFullClippingRectangle).
struct HudMapping {
    float originX = 0.0f;
    float originY = 0.0f;
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    bool valid = false;

    float x(float hudX) const { return originX + hudX * scaleX; }
    float y(float hudY) const { return originY + hudY * scaleY; }
};
HudMapping hudMapping(void* context);

// Scope that owns a BaseActorRenderContext for one HUD render pass. Icons are
// batched into the UI image mesh and flushed when the painter goes away.
class IconPainter {
public:
    // `wantRender == false` skips all render setup (the module still gets the
    // player for bookkeeping); ready() is then false.
    IconPainter(void* context, void* client, bool wantRender = true);
    ~IconPainter();

    IconPainter(const IconPainter&) = delete;
    IconPainter& operator=(const IconPainter&) = delete;

    bool ready() const { return mItemRenderer != nullptr; }
    void* player() const { return mPlayer; }
    const HudMapping& mapping() const { return mMapping; }

    // Paints the icon of `stack` into the HUD-space square (hudX, hudY, hudSize).
    bool draw(void* stack, void* item, float hudX, float hudY, float hudSize);

    // Fills a HUD-space rectangle (hudX, hudY, hudW, hudH) with an ARGB color
    // through the same UI context the icons use. Call it before draw() for a
    // slot: fills are submitted first, so the icons of the same pass always
    // land on top of them (slot backgrounds).
    bool fillRect(float hudX, float hudY, float hudW, float hudH, std::uint32_t color);

    // The vanilla durability bar (black track + green-to-red fill) of a
    // HUD-space slot square, drawn with the game's own fillRectangle. Call it
    // after draw() so the bar covers the bottom of the icon.
    bool drawDurabilityBar(float hudX, float hudY, float hudSize, int damage, int maxDamage);

    // Dyed leather armor loses its tinted pixels when the HUD opacity shader
    // constant is at its default; the fix is an extra pass at a high opacity
    // with the renderer's "20" mode for just those stacks. Only stacks for
    // which needsTextureOpacityPass() is true should be drawn in the pass.
    bool supportsOpacityFix() const;
    void beginOpacityFixPass();
    bool drawOpacityFix(void* stack, void* item, float hudX, float hudY, float hudSize);
    void endOpacityFixPass();

private:
    bool paint(void* stack, void* item, float hudX, float hudY, float hudSize, float mode);

    void* mContext = nullptr;
    void* mClient = nullptr;
    void* mPlayer = nullptr;
    HudMapping mMapping{};
    alignas(16) std::byte mStorage[sdk::offsets::ShulkerPreview::BaseActorRenderContextStorageSize]{};
    void* mItemRenderer = nullptr;
    bool mConstructed = false;
    bool mDrewAny = false;
    bool mDrewFix = false;
    bool mInFixPass = false;
};

// ---- Colors -----------------------------------------------------------------

// "#RRGGBB" / "#AARRGGBB" -> ARGB, `fallback` when unparsable.
std::uint32_t parseColor(const std::string& value, std::uint32_t fallback = 0xFFFFFFFFu);
std::uint32_t withOpacity(std::uint32_t color, float opacity);

} // namespace bedrocktools::huditems
