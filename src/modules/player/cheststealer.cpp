#include "cheststealer.hpp"

#include "core/GameHooks.hpp"
#include "launcher/KeyInjection.hpp"
#include "modules/hud/huditems.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/client/ContainerScreenController.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/Dimension.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

namespace off = bedrocktools::sdk::offsets;

using bedrocktools::sdk::BlockPos;
using bedrocktools::sdk::ContainerScreenController;
using bedrocktools::sdk::ItemStack;

constexpr int kMinDelayMs = 1;
constexpr int kMaxDelayMs = 1000;

// The container screen arrives one server round trip after the interaction that
// opened it, so the arming flag lives for a moment instead of being consumed by
// the next event.
constexpr auto kArmWindow = std::chrono::milliseconds(1000);

// After asking the game to close the screen, try the native Escape bridge if
// Android Back did not close it promptly. If neither path works, the session
// ends quietly instead of injecting a key again and again.
constexpr auto kCloseFallbackDelay = std::chrono::milliseconds(350);
constexpr auto kCloseTimeout = std::chrono::milliseconds(1500);

// How often an emptied container is looked at again while its screen stays open
// with Auto Close turned off.
constexpr auto kEmptyInterval = std::chrono::milliseconds(250);

// A block name longer than this is not a block name; the check keeps a garbage
// read out of the string comparison below.
constexpr std::size_t kMaxNameLength = 64;

const std::string& containerCollection() {
    static const std::string collection(ContainerScreenController::ContainerCollection);
    return collection;
}

bool isArmorName(std::string_view name) {
    // All vanilla material variants (including turtle/copper and future
    // materials that follow the same identifiers) share these suffixes.
    // Horse/wolf armor and elytra are equipment too, even though they do not
    // occupy one of the player's four ordinary armor slots.
    return name.ends_with("_helmet") ||
           name.ends_with("_chestplate") ||
           name.ends_with("_leggings") ||
           name.ends_with("_boots") ||
           name.ends_with("_armor") ||
           name == "elytra";
}

bool isKnownToolName(std::string_view name) {
    // maxDamage() below catches normal damageable tools and weapons. These
    // identifier checks are a fallback for a behavior pack (or future build)
    // that reports one of the familiar tool kinds as non-damageable.
    if (name.ends_with("_sword") ||
        name.ends_with("_pickaxe") ||
        name.ends_with("_axe") ||
        name.ends_with("_shovel") ||
        name.ends_with("_hoe") ||
        name.ends_with("_spear")) {
        return true;
    }

    constexpr std::string_view names[] = {
        "bow", "crossbow", "trident", "mace", "shield", "shears",
        "fishing_rod", "flint_and_steel", "brush", "carrot_on_a_stick",
        "warped_fungus_on_a_stick",
    };
    for (const std::string_view known : names) {
        if (name == known) return true;
    }
    return false;
}

enum class ItemCategory {
    Unknown,
    Block,
    Armor,
    Tool,
    Other,
};

// --- Armor strength ---------------------------------------------------------

using cheststealer::ArmorPolicy;
using cheststealer::ArmorScore;
using cheststealer::ArmorSet;
using cheststealer::ArmorSlot;
using cheststealer::ArmorTargets;
using cheststealer::armorIndex;
using cheststealer::armorScoreOf;
using cheststealer::armorSlotOf;
using cheststealer::isArmorSlot;
using cheststealer::toLower;
using cheststealer::unqualifiedItemName;

// Is this piece one the module moves out of the container?
//
// With the best-armor search on, a piece weaker than the strongest one its slot
// showed stays where it is, and so does a piece that is no better than the one
// that slot already sent to the inventory - so exactly one piece per slot moves
// out, unless a stronger one turns up later and replaces it. With the upgrade
// rule on, a piece that does not beat the one the player wears stays as well.
bool armorIsWanted(const ArmorPolicy& policy,
                   const ArmorTargets& targets,
                   const ArmorSet& worn,
                   ArmorSlot slot,
                   const ArmorScore& score) {
    // Not one of the four slots the player wears (the elytra, horse and wolf
    // armor, an addon item that only calls itself armor): there is no strength
    // to compare it by, so it is left to the Armor toggle as before.
    if (!isArmorSlot(slot)) return true;

    if (policy.bestOnly) {
        const std::size_t index = armorIndex(slot);
        if (targets.strongest.known[index] && targets.strongest.score[index].betterThan(score)) {
            return false;
        }
        if (targets.taken.known[index] && !score.betterThan(targets.taken.score[index])) {
            return false;
        }
    }

    if (policy.upgradeOnly && !worn.wouldImprove(slot, score)) return false;
    return true;
}

// Is the container the player just opened the one the armor memory was left in?
// The block and the dimension are what identifies a container; a null dimension
// means the game did not hand one over, so nothing can be matched and the
// session starts from a blank slate.
bool isRememberedChest(bool hasRemembered,
                       const BlockPos& rememberedPosition,
                       const void* rememberedDimension,
                       const BlockPos& position,
                       const void* dimension) {
    if (!hasRemembered || !dimension) return false;
    if (dimension != rememberedDimension) return false;
    return position.x == rememberedPosition.x &&
           position.y == rememberedPosition.y &&
           position.z == rememberedPosition.z;
}

// What a stack is, and - for armor - the slot it would be worn in.
struct ItemClassification {
    ItemCategory category = ItemCategory::Unknown;
    ArmorSlot armorSlot = ArmorSlot::None;
    // The identifier the category was derived from; empty when the item-name
    // lookup is unavailable in this build.
    std::string name;
};

ItemClassification classifyItem(const ItemStack& stack) {
    ItemClassification result;
    if (stack.empty()) return result;

    // mBlock is authoritative and supports custom/future placeable blocks
    // without maintaining a list of hundreds of identifiers.
    if (stack.isBlock()) {
        result.category = ItemCategory::Block;
        return result;
    }

    // Armor must be recognized before maxDamage(): ordinary armor is
    // damageable too and would otherwise be grouped with tools.
    result.name = unqualifiedItemName(stack.rawNameId());
    if (result.name.empty()) return result;
    if (isArmorName(result.name)) {
        // The armor search only compares the four slots the player wears, so
        // this is None for the elytra, horse and wolf armor and any addon item
        // that only calls itself armor. Those keep being taken by the Armor
        // toggle, exactly as they were before the search existed.
        result.armorSlot = armorSlotOf(result.name);
        result.category = ItemCategory::Armor;
        return result;
    }
    if (stack.maxDamage() > 0 || isKnownToolName(result.name)) {
        result.category = ItemCategory::Tool;
        return result;
    }
    result.category = ItemCategory::Other;
    return result;
}

struct ItemFilters {
    bool blocks = true;
    bool armor = true;
    bool tools = true;
    bool other = true;

    bool all() const { return blocks && armor && tools && other; }
};

bool shouldTake(const ItemFilters& filters,
                const ItemStack& stack,
                const ArmorPolicy& armorPolicy,
                ItemClassification& classification) {
    // Preserve the original module behavior, including on a build where
    // item-name resolution is unavailable, when every category is selected and
    // the armor rules are off: nothing has to be classified then.
    if (filters.all() && !armorPolicy.bestOnly && !armorPolicy.upgradeOnly) return !stack.empty();
    if (stack.empty()) return false;

    classification = classifyItem(stack);
    switch (classification.category) {
        case ItemCategory::Block:
            return filters.blocks;
        case ItemCategory::Armor:
            return filters.armor;
        case ItemCategory::Tool:
            return filters.tools;
        case ItemCategory::Other:
            return filters.other;
        case ItemCategory::Unknown:
            // The name lookup is missing in this build, so the stack cannot be
            // classified and - just as importantly - cannot be compared with
            // other armor. With every category selected it is taken the way the
            // original module took it; a selective filter never moves an item it
            // cannot identify.
            return filters.all();
    }
    return false;
}

// What a block name means to this module: a container to empty, and the slots
// the game binds for it.
struct StorageBlock {
    bool storage = false;
    int slots = 0;
    // Chests merge with the chest beside them, which is the one container whose
    // grid is larger than the block's own.
    bool merges = false;
};

// Storage blocks the module empties, with the slot count of their screen. The
// names are the ones the game reports for a block (Storage ESP classifies the
// same set), with the namespace stripped, so a new container kind stays
// harmless until it is listed here.
StorageBlock classifyStorageBlock(std::string_view rawName) {
    constexpr std::string_view kNamespace = "minecraft:";
    std::string_view name = rawName;
    if (name.starts_with(kNamespace)) name.remove_prefix(kNamespace.size());
    if (name.empty()) return {};

    // Copper chests ship as one block per oxidation stage and again per waxed
    // stage ("copper_chest", "exposed_copper_chest", ...).
    if (name == "chest" || name == "trapped_chest" || name == "copper_chest" || name.ends_with("_copper_chest")) {
        return {true, 27, true};
    }
    if (name == "ender_chest" || name == "barrel") return {true, 27, false};
    // Colored boxes are "<color>_shulker_box"; the undyed one has shipped under
    // two names across versions.
    if (name == "shulker_box" || name == "undyed_shulker_box" || name.ends_with("_shulker_box")) {
        return {true, 27, false};
    }
    if (name == "hopper") return {true, 5, false};
    if (name == "dispenser" || name == "dropper") return {true, 9, false};
    return {};
}

// ClientInstance::getLocalPlayer through the vtable. The launcher modules run
// inside the mod library itself, where the BedrockToolsPlus API lookup
// (sdk::ClientInstance::current) resolves nothing, so the same direct call the
// HUD modules make is used here.
void* localPlayer() {
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) return nullptr;
    auto** vtable = *reinterpret_cast<void***>(client);
    if (!vtable) return nullptr;
    const auto getLocalPlayer = vtable[off::VTable::ClientInstanceGetLocalPlayer];
    if (!getLocalPlayer) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(getLocalPlayer)(client);
}

using BlockSourceGetBlockFn = const void* (*)(void*, const BlockPos*, int);

// The address is cached by the memory module, so the lookup is a table read;
// the static keeps the cast out of the hot path anyway.
BlockSourceGetBlockFn getBlockFunction() {
    static BlockSourceGetBlockFn function = reinterpret_cast<BlockSourceGetBlockFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock));
    return function;
}

// The block at one position, or null when the position sits in a region that is
// not loaded (unloaded chunks must not be read).
const bedrocktools::sdk::Block* blockAt(bedrocktools::sdk::Actor* player, const BlockPos& position) {
    const auto getBlock = getBlockFunction();
    if (!player || !getBlock) return nullptr;
    auto* dimension = player->dimension();
    if (!dimension) return nullptr;
    auto* region = dimension->blockSource();
    if (!region) return nullptr;
    const void* block = getBlock(region, &position, 0);
    if (!block) return nullptr;
    const auto* named = static_cast<const bedrocktools::sdk::Block*>(block);
    if (reinterpret_cast<std::uintptr_t>(named->blockType()) < 0x1000) return nullptr;
    return named;
}

// The name of the block at one position, or an empty string when it cannot be
// read.
std::string blockNameAt(bedrocktools::sdk::Actor* player, const BlockPos& position) {
    const auto* block = blockAt(player, position);
    if (!block) return {};
    const std::string* fullName = block->fullName();
    if (!fullName || !fullName->data() || fullName->empty() || fullName->size() > kMaxNameLength) return {};
    return toLower(*fullName);
}

// Is the block at this position a container this module empties? On the way out
// `slots` holds the number of slots its screen binds - a single chest 27, the
// double chest two blocks make 54, a hopper 5, a dispenser 9.
bool storageBlockAt(bedrocktools::sdk::Actor* player, const BlockPos& position, int& slots) {
    const std::string name = blockNameAt(player, position);
    if (name.empty()) return false;

    const StorageBlock kind = classifyStorageBlock(name);
    if (!kind.storage) return false;
    slots = kind.slots;
    if (!kind.merges) return true;

    constexpr BlockPos kNeighbours[] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& offset : kNeighbours) {
        const BlockPos neighbour{position.x + offset.x, position.y + offset.y, position.z + offset.z};
        if (blockNameAt(player, neighbour) != name) continue;
        slots = 54;
        break;
    }
    return true;
}

// Is the block at this position a container this module empties?
bool isStorageBlock(bedrocktools::sdk::Actor* player, const BlockPos& position) {
    int slots = 0;
    return storageBlockAt(player, position, slots);
}

bool isBefore(const BlockPos& left, const BlockPos& right) {
    if (left.x != right.x) return left.x < right.x;
    if (left.y != right.y) return left.y < right.y;
    return left.z < right.z;
}

// The identity of the container a storage block belongs to. A chest that merges
// with the block beside it is one container with two blocks, and the player may
// open either half, so the two positions are reduced to the lower one and both
// halves name the same container. Every other storage block keeps its position.
BlockPos containerIdentity(bedrocktools::sdk::Actor* player, const BlockPos& position) {
    BlockPos identity = position;
    const std::string name = blockNameAt(player, position);
    if (name.empty()) return identity;

    const StorageBlock kind = classifyStorageBlock(name);
    if (!kind.storage || !kind.merges) return identity;

    constexpr BlockPos kNeighbours[] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& offset : kNeighbours) {
        const BlockPos neighbour{position.x + offset.x, position.y + offset.y, position.z + offset.z};
        if (blockNameAt(player, neighbour) != name) continue;
        if (isBefore(neighbour, identity)) identity = neighbour;
    }
    return identity;
}

// How many slots does an open container screen bind? If the screen belongs
// to a block interaction that was armed and verified in the local world, the
// count comes from that block. On dedicated servers (such as CubeCraft) where
// chests open without a local UseItemOn event, or where server-side plugins
// serve virtual/custom container screens, scan the bound container collection
// up to MaxContainerSlots to detect the accessible range.
int detectContainerSlots(const ContainerScreenController& controller) {
    const std::string& collection = containerCollection();
    int lastOccupied = -1;
    for (int slot = 0; slot < ContainerScreenController::MaxContainerSlots; ++slot) {
        if (!controller.stack(collection, slot).empty()) {
            lastOccupied = slot;
        }
    }
    // If items were found, choose the smallest standard container size that covers them.
    if (lastOccupied >= 27) return 54;
    if (lastOccupied >= 9) return 27;
    if (lastOccupied >= 5) return 9;
    if (lastOccupied >= 0) return 5;
    // Empty container: fall back to a single chest size (27 slots).
    return 27;
}

// One piece of armor on a container slot: the slot it would be worn in and how
// strong it is. A slot of ArmorSlot::None marks an item that is not armor.
struct ArmorPiece {
    ArmorSlot slot = ArmorSlot::None;
    ArmorScore score;
};

// What one look at the container found: which slots hold an item matching the
// enabled item types (and passing the armor rules) and how much is in them.
struct ContainerSnapshot {
    int occupied[ContainerScreenController::MaxContainerSlots]{};
    ArmorPiece armor[ContainerScreenController::MaxContainerSlots]{};
    int stacks = 0;
    int items = 0;

    // The matching slot the next attempt starts at: the first one at or after
    // `from`, wrapping around to the front of the container. A slot that
    // refused a move is skipped that way instead of being retried forever.
    int slotFrom(int from) const {
        if (stacks == 0) return -1;
        for (int index = 0; index < stacks; ++index) {
            if (occupied[index] >= from) return occupied[index];
        }
        return occupied[0];
    }

    // The armor piece that sits at one container slot, for a move that is in
    // flight and has to be remembered once it lands.
    ArmorPiece armorAt(int containerSlot) const {
        for (int index = 0; index < stacks; ++index) {
            if (occupied[index] == containerSlot) return armor[index];
        }
        return {};
    }
};

// Everything one scan of the container needs besides the container itself: the
// item types, the armor rules, and the session's own armor memory.
struct ScanContext {
    ItemFilters filters;
    ArmorPolicy armorPolicy;
    ArmorTargets& targets;
    const ArmorSet& worn;
};

// One slot the item filters accept, before the armor rules are applied. The
// strength of an armor piece travels with it, so the whole container can be
// compared before anything is taken: the strongest piece of a slot wins no
// matter where in the grid it sits.
struct Candidate {
    int slot = -1;
    int count = 0;
    ArmorPiece armor;
};

ContainerSnapshot scanContainer(
    const ContainerScreenController& controller,
    int slots,
    const ScanContext& context) {
    const std::string& collection = containerCollection();
    const int limit = std::clamp(slots, 0, ContainerScreenController::MaxContainerSlots);

    Candidate candidates[ContainerScreenController::MaxContainerSlots];
    int candidateCount = 0;
    for (int slot = 0; slot < limit; ++slot) {
        const ItemStack stack = controller.stack(collection, slot);
        ItemClassification classification;
        if (!shouldTake(context.filters, stack, context.armorPolicy, classification)) continue;
        Candidate& candidate = candidates[candidateCount++];
        candidate.slot = slot;
        candidate.count = stack.count();
        if (classification.category == ItemCategory::Armor) {
            candidate.armor.slot = classification.armorSlot;
            // Only a piece that goes into one of the four armor slots has a
            // strength worth reading; the rest is taken without being compared.
            if (isArmorSlot(candidate.armor.slot)) {
                candidate.armor.score = armorScoreOf(stack, classification.name);
            }
        }
    }

    // The container is known in full now, so what each armor slot holds at its
    // strongest is remembered before the weaker pieces are judged against it.
    if (context.armorPolicy.bestOnly) {
        for (int index = 0; index < candidateCount; ++index) {
            const Candidate& candidate = candidates[index];
            if (isArmorSlot(candidate.armor.slot)) {
                context.targets.strongest.remember(candidate.armor.slot, candidate.armor.score);
            }
        }
    }

    ContainerSnapshot snapshot;
    for (int index = 0; index < candidateCount; ++index) {
        const Candidate& candidate = candidates[index];
        if (!armorIsWanted(context.armorPolicy, context.targets, context.worn,
                           candidate.armor.slot, candidate.armor.score)) {
            continue;
        }
        snapshot.occupied[snapshot.stacks] = candidate.slot;
        snapshot.armor[snapshot.stacks] = candidate.armor;
        ++snapshot.stacks;
        snapshot.items += candidate.count;
    }
    return snapshot;
}

}

ChestStealerModule::ChestStealerModule()
    : Module("ChestStealer",
             "Quick-moves selected item categories from the container you open, with an "
             "option to take only the strongest armor.") {}

ChestStealerModule::~ChestStealerModule() = default;

void ChestStealerModule::onInit() {
    using namespace bedrocktools::events;

    // Arming: an interaction with a storage block is the only thing that may
    // start a session.
    bus().subscribe<GameModeActionEvent>([this](auto& event) { onGameModeAction(event); });

    // The container screen itself: the game tells us when it opens and closes,
    // and hands over the screen controller that owns the container.
    bus().subscribe<ScreenStateEvent>([this](auto& event) { onScreenState(event); });

    // Stealing: paced on the game's own update, which runs every frame on the
    // main thread, the thread the container screen is touched on.
    bus().subscribe<ClientInstanceUpdateEvent>([this](auto&) { onUpdate(); });
}

void ChestStealerModule::onDisable() {
    endSession();
}

void ChestStealerModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    if (json.contains("autoClose")) m_autoClose = json["autoClose"].get<bool>();
    if (json.contains("delay")) m_delayMs = std::clamp(json["delay"].get<int>(), kMinDelayMs, kMaxDelayMs);

    bool takeBlocks = m_takeBlocks;
    bool takeArmor = m_takeArmor;
    bool takeTools = m_takeTools;
    bool takeOther = m_takeOther;
    bool bestArmorOnly = m_bestArmorOnly;
    bool armorUpgradeOnly = m_armorUpgradeOnly;
    const bool hasTypeToggles = json.contains("takeBlocks") ||
                                json.contains("takeArmor") ||
                                json.contains("takeTools") ||
                                json.contains("takeOther");

    if (hasTypeToggles) {
        // Each category is independent, so combinations such as Blocks + Armor
        // are possible. Missing keys retain their current/default value.
        if (json.contains("takeBlocks")) takeBlocks = json["takeBlocks"].get<bool>();
        if (json.contains("takeArmor")) takeArmor = json["takeArmor"].get<bool>();
        if (json.contains("takeTools")) takeTools = json["takeTools"].get<bool>();
        if (json.contains("takeOther")) takeOther = json["takeOther"].get<bool>();
    } else if (json.contains("stealMode")) {
        // Migrate the radio value written by the first filtered version. Once
        // saved again, the four independent toggle keys replace stealMode.
        int selected = -1;
        const auto& value = json["stealMode"];
        try {
            if (value.is_number_integer()) {
                selected = value.get<int>();
            } else if (value.is_string()) {
                const std::string text = value.get<std::string>();
                selected = std::stoi(text.substr(0, text.find(',')));
            }
        } catch (...) {
            selected = -1;
        }

        if (selected >= 0 && selected <= 4) {
            takeBlocks = selected == 0 || selected == 1;
            takeArmor = selected == 0 || selected == 2;
            takeTools = selected == 0 || selected == 3;
            takeOther = selected == 0 || selected == 4;
        }
    }

    if (json.contains("bestArmorOnly")) bestArmorOnly = json["bestArmorOnly"].get<bool>();
    if (json.contains("armorUpgradeOnly")) armorUpgradeOnly = json["armorUpgradeOnly"].get<bool>();

    const bool filtersChanged = takeBlocks != m_takeBlocks ||
                                takeArmor != m_takeArmor ||
                                takeTools != m_takeTools ||
                                takeOther != m_takeOther ||
                                bestArmorOnly != m_bestArmorOnly ||
                                armorUpgradeOnly != m_armorUpgradeOnly;
    if (filtersChanged) {
        // A pending move is judged against a snapshot of the previous filter,
        // and the armor rules come with a memory of what the container showed.
        // End that session rather than mixing two selections.
        endSession();
        m_takeBlocks = takeBlocks;
        m_takeArmor = takeArmor;
        m_takeTools = takeTools;
        m_takeOther = takeOther;
        m_bestArmorOnly = bestArmorOnly;
        m_armorUpgradeOnly = armorUpgradeOnly;
    }
}

void ChestStealerModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    json["autoClose"] = m_autoClose;
    json["delay"] = clampedDelayMs();
    json["takeBlocks"] = m_takeBlocks;
    json["takeArmor"] = m_takeArmor;
    json["takeTools"] = m_takeTools;
    json["takeOther"] = m_takeOther;
    json["bestArmorOnly"] = m_bestArmorOnly;
    json["armorUpgradeOnly"] = m_armorUpgradeOnly;
}

int ChestStealerModule::clampedDelayMs() const {
    return std::clamp(m_delayMs, kMinDelayMs, kMaxDelayMs);
}

void ChestStealerModule::onGameModeAction(const bedrocktools::events::GameModeActionEvent& event) {
    if (!enabled || m_active) return;
    if (!ContainerScreenController::available()) return;

    // Only the block the player interacts with can open a container screen, and
    // only the ones that hold something to take are of interest: using a tool,
    // placing a block or feeding an animal is not a container.
    if (event.action != bedrocktools::events::GameModeAction::UseItemOn) return;
    if (!event.blockPosition) return;

    auto* player = reinterpret_cast<bedrocktools::sdk::Actor*>(localPlayer());
    if (!player) return;

    const BlockPos position = *static_cast<const BlockPos*>(event.blockPosition);
    if (!isStorageBlock(player, position)) return;

    m_armed = true;
    m_armedAt = Clock::now();
    // The container, not the block that was clicked: both halves of a double
    // chest open the same inventory and are remembered as one container.
    m_armedPosition = containerIdentity(player, position);
}

void ChestStealerModule::onScreenState(const bedrocktools::events::ScreenStateEvent& event) {
    using namespace bedrocktools::events;
    if (event.screen != ScreenKind::Container) return;

    if (event.phase == ScreenPhase::Closed) {
        // Ignore a screen that is not ours; its own close event ends nothing.
        if (m_active && (event.controller == m_controller || !event.controller)) endSession();
        return;
    }

    if (m_active || !enabled) {
        m_armed = false;
        return;
    }
    if (!event.controller) return;

    // Determine the slot count:
    // 1. If armed by a local UseItemOn on a storage block within the arm window,
    //    read the storage block definition directly.
    // 2. Otherwise (e.g. on dedicated servers like CubeCraft where chests are
    //    opened by server packets, latency exceeds kArmWindow, or custom virtual
    //    containers are shown), accept the container screen directly and detect
    //    the slot count from the screen controller.
    int slots = 0;
    const bool armedValid = m_armed && (Clock::now() - m_armedAt <= kArmWindow);
    m_armed = false;

    // The dimension the container sits in, for telling it apart from a container
    // that happens to share its coordinates somewhere else.
    const void* dimension = nullptr;
    if (armedValid) {
        auto* player = reinterpret_cast<bedrocktools::sdk::Actor*>(localPlayer());
        if (player) {
            dimension = player->dimension();
            storageBlockAt(player, m_armedPosition, slots);
        }
    }

    if (slots <= 0) {
        const ContainerScreenController controller(event.controller);
        slots = detectContainerSlots(controller);
    }
    if (slots <= 0) return;

    m_controller = event.controller;
    m_active = true;
    m_slots = slots;
    m_closing = false;
    m_closeFallbackSent = false;
    m_sawItems = false;
    m_preferredSlot = 0;
    m_refusedSlots = 0;
    m_pending = false;
    m_pendingArmorSlot = ArmorSlot::None;

    // The armor rules remember what a container showed and what it already sent
    // to the inventory. Reopening that very container picks the memory up, so
    // the weaker pieces the earlier visit left behind stay behind instead of the
    // module working its way down the container one visit at a time; a piece
    // that beats what the container already gave is still taken. Any other
    // container - and a screen the module cannot tie to a block, such as a
    // server-side virtual one - starts from a blank slate.
    const bool sameChest = isRememberedChest(m_hasRememberedChest,
                                             m_rememberedChestPosition,
                                             m_rememberedChestDimension,
                                             m_armedPosition,
                                             dimension);
    m_armorTargets = sameChest ? m_rememberedArmor : ArmorTargets{};
    m_wornArmor.reset();

    // From here on this container is the one the module remembers, whether it
    // was already known or is being seen for the first time.
    m_sessionIdentified = armedValid && dimension != nullptr;
    if (m_sessionIdentified) {
        m_hasRememberedChest = true;
        m_rememberedChestPosition = m_armedPosition;
        m_rememberedChestDimension = dimension;
        m_rememberedArmor = m_armorTargets;
    }

    m_nextStepAt = Clock::now();
}

void ChestStealerModule::onUpdate() {
    if (!enabled || !m_active || !m_controller) return;

    const TimePoint now = Clock::now();
    if (m_closing) {
        // The game closes the screen and reports it; that event ends the
        // session. If Android Back was accepted but did not close the game UI,
        // make one delayed native-Escape attempt before timing out.
        if (!m_closeFallbackSent && now - m_closingAt > kCloseFallbackDelay) {
            m_closeFallbackSent = true;
            bedrocktools::launcher::injectGameKey(bedrocktools::launcher::GameKeyEscape);
        }
        if (now - m_closingAt > kCloseTimeout) endSession();
        return;
    }
    if (now < m_nextStepAt) return;
    step();
}

// Reads the armor the player wears, so the upgrade rule can compare a piece in
// the container against the piece it would replace. The four slots come from the
// game's own equipment component through the same accessor the Armor HUD draws
// from, and every piece is matched to its slot by identifier, so the order the
// game keeps them in does not matter. When the game exposes no equipment, the
// rule simply finds no worn piece and the best armor is taken - it never blocks
// a take on a missing read.
void ChestStealerModule::refreshWornArmor() {
    m_wornArmor.reset();
    if (!m_armorUpgradeOnly) return;

    auto* player = localPlayer();
    if (!player) return;

    const bedrocktools::huditems::EquipmentStacks equipment =
        bedrocktools::huditems::getEquipmentStacks(player);
    for (void* stack : equipment.armor) {
        const ItemStack worn(stack);
        if (worn.empty()) continue;
        const std::string name = unqualifiedItemName(worn.rawNameId());
        const ArmorSlot slot = armorSlotOf(name);
        if (!isArmorSlot(slot)) continue;
        m_wornArmor.remember(slot, armorScoreOf(worn, name));
    }
}

void ChestStealerModule::step() {
    const ContainerScreenController controller(m_controller);
    refreshWornArmor();

    const ItemFilters filters{m_takeBlocks, m_takeArmor, m_takeTools, m_takeOther};
    const ScanContext context{
        filters,
        ArmorPolicy{m_bestArmorOnly, m_armorUpgradeOnly},
        m_armorTargets,
        m_wornArmor,
    };
    const ContainerSnapshot before = scanContainer(controller, m_slots, context);
    if (before.stacks > 0) m_sawItems = true;

    // Judge the previous move first: the client may apply it right away or a
    // moment later, so its result is only read now.
    if (m_pending) {
        m_pending = false;
        const bool progressed = before.items < m_pendingItems || before.stacks < m_pendingStacks;
        if (progressed) {
            m_refusedSlots = 0;
            m_preferredSlot = m_pendingSlot;
            if (m_pendingArmorSlot != ArmorSlot::None) {
                // What that slot sent to the inventory is remembered now, so
                // anything weaker - and a spare that is exactly as strong -
                // stays in the container. The snapshot above was read while the
                // piece was still in flight, so the container is looked at once
                // more before the next piece is picked.
                m_armorTargets.taken.remember(m_pendingArmorSlot, m_pendingArmorScore);
                m_pendingArmorSlot = ArmorSlot::None;
                m_nextStepAt = Clock::now() + std::chrono::milliseconds(clampedDelayMs());
                return;
            }
        } else if (before.stacks > 0) {
            ++m_refusedSlots;
            if (m_refusedSlots >= before.stacks) {
                // Every matching slot refused the quick-move: the inventory
                // cannot take any more of the selected category, the same
                // "full" condition the original stops on.
                if (m_autoClose) requestClose();
                else endSession();
                return;
            }
            m_preferredSlot = before.slotFrom(m_pendingSlot + 1);
        }
        // Whatever this move was, it is judged now; the armor slot it may have
        // belonged to is carried over below when a new move starts.
        m_pendingArmorSlot = ArmorSlot::None;
    }

    if (before.stacks == 0) {
        if (!m_sawItems) {
            // The container had no item matching the enabled types when its
            // screen opened. Leave a screen the module never worked on exactly
            // as the player opened it, even if non-matching items are present.
            endSession();
            return;
        }
        if (m_autoClose) {
            // Every matching item was taken. Non-matching stacks deliberately
            // remain in the container and do not prevent Auto Close.
            requestClose();
            return;
        }
        // Auto Close off: keep looking at the open container instead of ending
        // the session, so matching items the player puts in afterwards are
        // taken too.
        m_refusedSlots = 0;
        m_nextStepAt = Clock::now() + kEmptyInterval;
        return;
    }

    const int slot = before.slotFrom(m_preferredSlot);
    if (slot < 0) {
        endSession();
        return;
    }

    if (!controller.autoPlace(containerCollection(), slot)) {
        // The quick-move is not resolvable in this build: stop instead of
        // spinning on the same slot, and leave the screen as it is.
        endSession();
        return;
    }

    const ArmorPiece piece = before.armorAt(slot);
    m_pending = true;
    m_pendingSlot = slot;
    m_pendingStacks = before.stacks;
    m_pendingItems = before.items;
    m_pendingArmorSlot = piece.slot;
    m_pendingArmorScore = piece.score;
    m_nextStepAt = Clock::now() + std::chrono::milliseconds(clampedDelayMs());
}

void ChestStealerModule::requestClose() {
    m_closing = true;
    m_closeFallbackSent = false;
    m_closingAt = Clock::now();

    // Android Back is the game's normal "leave this screen" path: the
    // container closes client side and the matching packet is sent by the
    // game. The launcher bridge falls back to native Escape on older builds.
    // If neither path is available, stop and leave the screen open.
    if (!bedrocktools::launcher::closeGameScreen()) endSession();
}

void ChestStealerModule::endSession() {
    m_active = false;
    m_controller = nullptr;
    m_slots = 0;
    m_closing = false;
    m_closeFallbackSent = false;
    m_armed = false;
    m_sawItems = false;
    m_pending = false;
    m_pendingSlot = -1;
    m_pendingStacks = 0;
    m_pendingItems = 0;
    m_pendingArmorSlot = ArmorSlot::None;
    m_preferredSlot = 0;
    m_refusedSlots = 0;

    // What this visit learned about the container it worked on is kept for the
    // next visit to that same container.
    if (m_sessionIdentified) m_rememberedArmor = m_armorTargets;

    m_sessionIdentified = false;
    m_armorTargets.reset();
    m_wornArmor.reset();
    m_nextStepAt = TimePoint{};
}
