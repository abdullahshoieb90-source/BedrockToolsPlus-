#pragma once

#include "../Module.hpp"

#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/client/ContainerScreenController.hpp>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bedrocktools::events {
struct ClientInstanceUpdateEvent;
struct GameModeActionEvent;
struct ScreenStateEvent;
}

// How the best-armor rule recognizes and compares armor pieces. It lives in this
// header because two modules work with it and both must agree on what "a better
// piece" is: ChestStealer compares the armor a container holds, and AutoArmor
// compares the armor the player carries against the piece that is worn.
namespace cheststealer {

// The four pieces the player wears, in the order a container screen lists them.
enum class ArmorSlot : int {
    None = -1,
    Helmet = 0,
    Chestplate,
    Leggings,
    Boots,
};

inline constexpr int kArmorSlotCount = 4;

inline constexpr bool isArmorSlot(ArmorSlot slot) { return slot != ArmorSlot::None; }

// Index into one of the per-slot arrays below; only valid for a slot that
// isArmorSlot() accepts.
inline constexpr std::size_t armorIndex(ArmorSlot slot) {
    return static_cast<std::size_t>(static_cast<int>(slot));
}

// How strong one armor piece is, compared in this order:
//
//   1. the material (leather ... netherite; an identifier this module does not
//      know - a behavior pack's own armor - ranks above the vanilla sets),
//   2. the durability the piece can take in total, which separates two
//      materials of the same rank and is the only measure an unknown one has,
//   3. the durability it has left, so the least damaged one of two otherwise
//      identical pieces wins.
//
// Enchantments are not part of the comparison: a plain diamond piece beats an
// enchanted leather one.
struct ArmorScore {
    int material = 0;
    int maxDurability = 0;
    int durability = 0;

    bool betterThan(const ArmorScore& other) const {
        if (material != other.material) return material > other.material;
        if (maxDurability != other.maxDurability) return maxDurability > other.maxDurability;
        return durability > other.durability;
    }

    bool operator==(const ArmorScore& other) const = default;
};

// The strongest piece of every armor slot seen somewhere: the container during
// this session, or the armor the player actually wears.
struct ArmorSet {
    ArmorScore score[kArmorSlotCount];
    bool known[kArmorSlotCount]{};

    void reset() {
        for (int index = 0; index < kArmorSlotCount; ++index) known[index] = false;
    }

    // Keeps the piece only while it beats what the slot holds so far.
    void remember(ArmorSlot slot, const ArmorScore& candidate) {
        if (!isArmorSlot(slot)) return;
        const std::size_t index = armorIndex(slot);
        if (known[index] && !candidate.betterThan(score[index])) return;
        known[index] = true;
        score[index] = candidate;
    }

    // Is `candidate` stronger than the piece this set holds for that slot? An
    // empty slot counts as weaker than anything.
    bool wouldImprove(ArmorSlot slot, const ArmorScore& candidate) const {
        if (!isArmorSlot(slot)) return true;
        const std::size_t index = armorIndex(slot);
        return !known[index] || candidate.betterThan(score[index]);
    }
};

// What the container showed during one session, and what was already moved out
// of it.
struct ArmorTargets {
    // The strongest piece of every armor slot the container has shown.
    ArmorSet strongest;
    // The strongest piece of every armor slot that was taken out. A piece is
    // only taken again when it beats the one already in the inventory, so a
    // spare of equal strength stays in the container while a genuinely stronger
    // find replaces it.
    ArmorSet taken;

    void reset() {
        strongest.reset();
        taken.reset();
    }
};

// The armor settings in the shape the container scan reads them.
struct ArmorPolicy {
    bool bestOnly = false;
    bool upgradeOnly = false;
};

// --- How one piece is identified and measured -------------------------------

// Lower-cased copy of a string. Identifiers are compared in one casing so a
// behavior pack that capitalizes its own names cannot slip past a check.
inline std::string toLower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

// An item identifier without its namespace, lower-cased: "minecraft:Diamond_Helmet"
// becomes "diamond_helmet", which is what the suffix checks below expect.
inline std::string unqualifiedItemName(std::string_view rawName) {
    const auto separator = rawName.rfind(':');
    if (separator != std::string_view::npos) rawName.remove_prefix(separator + 1);
    return toLower(rawName);
}

// The armor slot a piece belongs to. Everything else the game files under armor
// - the elytra, horse and wolf armor, an addon item that only ends in "_armor" -
// has no slot here, because the player never wears it in one of the four armor
// slots.
inline ArmorSlot armorSlotOf(std::string_view name) {
    if (name.ends_with("_helmet")) return ArmorSlot::Helmet;
    if (name.ends_with("_chestplate")) return ArmorSlot::Chestplate;
    if (name.ends_with("_leggings")) return ArmorSlot::Leggings;
    if (name.ends_with("_boots")) return ArmorSlot::Boots;
    return ArmorSlot::None;
}

// The vanilla armor materials, weakest first, matched by the prefix of the base
// identifier so every piece of a material gets the same rank. Copper shipped
// with the copper equipment set and protects less than gold; a turtle shell
// protects like the other mid-tier helmets and adds water breathing, so it sits
// directly below diamond.
struct ArmorMaterial {
    std::string_view prefix;
    int rank;
};

inline constexpr ArmorMaterial kArmorMaterials[] = {
    {"leather_", 1},
    {"copper_", 2},
    {"golden_", 3},
    {"gold_", 3},
    {"chainmail_", 4},
    {"iron_", 5},
    {"turtle_", 6},
    {"diamond_", 7},
    {"netherite_", 8},
};

// Armor a behavior pack adds under its own identifier: such a set is built to be
// an upgrade over the vanilla one, so it ranks above them. The durability that
// follows in the comparison still separates the pack's own tiers.
inline constexpr int kCustomArmorRank = 9;

inline int armorMaterialRank(std::string_view name) {
    for (const ArmorMaterial& material : kArmorMaterials) {
        if (name.starts_with(material.prefix)) return material.rank;
    }
    return kCustomArmorRank;
}

// How strong one stack is. The identifier is handed in because the caller had
// to read it anyway to tell what the stack is.
inline ArmorScore armorScoreOf(const bedrocktools::sdk::ItemStack& stack, std::string_view name) {
    ArmorScore score;
    score.material = armorMaterialRank(name);
    score.maxDurability = stack.maxDamage();
    const int damage = stack.damage();
    score.durability = score.maxDurability > damage ? score.maxDurability - damage : 0;
    return score;
}

} // namespace cheststealer

// Empties the storage container you open into your inventory.
//
// The port of the ChestStealer module of the LuminaClient project. In addition
// to Auto Close and Delay, independent item-type toggles can select any
// combination of blocks, armor, tools/weapons and other items. While a
// container's screen is open, one matching stack after another moves into the
// player's inventory until no matching stack remains or the inventory cannot
// take more.
//
// Armor is the one category that can be narrowed down further:
//
//   * "Best Armor Only" (on by default) compares the armor pieces a container
//     holds per slot and moves only the strongest one of each slot out. The
//     comparison is the material (leather ... netherite, an addon's own set
//     ranking above them), then the total durability, then the durability that
//     is left. Everything weaker than the strongest piece of its slot stays in
//     the container, and so does a spare of a slot that already sent a piece to
//     the inventory - unless it beats that piece, in which case the stronger
//     find is taken too.
//
//     What a container showed and what it already sent to the inventory is
//     remembered for the container itself: closing it and opening the very same
//     block again picks that memory up, so the weaker pieces the first visit
//     left behind stay behind instead of the module working its way down the
//     container one visit at a time. A piece that is genuinely stronger than
//     what that container already gave is still taken. Opening any other
//     container starts from a blank slate, and the coordinates and the
//     dimension are what identifies a container.
//   * "Armor Upgrade Only" (off by default) additionally compares the piece
//     with the one the player wears in that slot and leaves anything that is
//     not an upgrade behind.
//
// Only the four slots the player wears take part in that search. Equipment the
// game files under armor but the player never wears in one of them - the
// elytra, horse and wolf armor, an addon item that only calls itself armor -
// has no strength to compare and keeps being taken by the Armor toggle, the way
// it was before the search existed.
//
// Where the original drives the move through the protocol (it owns the packets
// the mod sits on), this module runs inside the game and uses the game's own
// quick-move: the same "auto place" a shift-click runs, which merges the stack
// onto matching inventory stacks, picks the destination slot itself and sends
// the matching inventory transaction to the server. Nothing is built by hand,
// so nothing can desync the container.
//
// A session starts only when the player uses a storage block - chest, trapped
// chest, copper chest, ender chest, barrel, shulker box, hopper, dispenser or
// dropper - and the container screen that follows belongs to it. The player's
// own inventory screen, a crafting table, an anvil or a villager trade screen
// is never probed. Entity containers (chest boats, minecart chests) do not go
// through a block interaction and are left alone.
class ChestStealerModule final : public Module {
public:
    ChestStealerModule();
    ~ChestStealerModule() override;

    void onInit() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void onGameModeAction(const bedrocktools::events::GameModeActionEvent& event);
    void onScreenState(const bedrocktools::events::ScreenStateEvent& event);

    // One stealing step, called from the game's own update (main thread).
    void onUpdate();
    void step();

    // Reads the armor the player wears, for the upgrade rule.
    void refreshWornArmor();

    // Asks the game to leave the container screen; the close event it sends
    // ends the session.
    void requestClose();
    void endSession();

    int clampedDelayMs() const;

    // --- Settings -----------------------------------------------------------

    // Close the container once no item matching the enabled types remains.
    bool m_autoClose = true;
    // Independent category toggles. All are enabled by default to preserve the
    // original ChestStealer behavior and old configurations.
    bool m_takeBlocks = true;
    bool m_takeArmor = true;
    bool m_takeTools = true;
    bool m_takeOther = true;
    // Move only the strongest piece of each armor slot out of the container
    // instead of every piece of armor it holds.
    bool m_bestArmorOnly = true;
    // ... and only when that piece beats the one the player wears in the same
    // slot.
    bool m_armorUpgradeOnly = false;
    // Pause between two moves. 50 ms is one game tick, the rate the original
    // uses; higher values are gentler on servers that rate-limit.
    int m_delayMs = 50;

    // --- Session state ------------------------------------------------------

    // The player used a storage block; the container screen that follows
    // belongs to it. The position is kept so the screen is only accepted while
    // that block still is a storage block, and the slots the game binds for it
    // are the only ones ever looked at (a chest merges with its neighbour, so
    // both halves are covered). The whole thing expires after a moment, so an
    // interaction that never opens a screen cannot leak into a later one.
    bool m_armed = false;
    TimePoint m_armedAt{};
    bedrocktools::sdk::BlockPos m_armedPosition{};

    // The container screen we are emptying.
    bool m_active = false;
    void* m_controller = nullptr;
    int m_slots = 0;

    // The screen is being closed; the module waits for the game's own close
    // event instead of injecting the key twice.
    bool m_closing = false;
    bool m_closeFallbackSent = false;
    TimePoint m_closingAt{};

    TimePoint m_nextStepAt{};

    // The move that was requested but not yet judged. The game may apply it
    // right away or a moment later, so the step after the request compares the
    // container against these numbers.
    bool m_pending = false;
    int m_pendingSlot = -1;
    int m_pendingStacks = 0;
    int m_pendingItems = 0;
    // The armor slot and the strength of that move (ArmorSlot::None for every
    // other move), so a completed one is remembered as what the slot already
    // holds.
    cheststealer::ArmorSlot m_pendingArmorSlot = cheststealer::ArmorSlot::None;
    cheststealer::ArmorScore m_pendingArmorScore;

    // Where the next attempt starts, and how many slots refused in a row.
    int m_preferredSlot = 0;
    int m_refusedSlots = 0;

    // The strongest piece each armor slot has shown in this session, and the
    // slots whose strongest piece is already in the inventory.
    cheststealer::ArmorTargets m_armorTargets;
    // What the player wears, refreshed while the upgrade rule runs.
    cheststealer::ArmorSet m_wornArmor;

    // The container the armor memory belongs to. It is the block the session was
    // opened on, together with the dimension the player was in (two containers
    // can share coordinates across dimensions), and the armor memory itself:
    // what that container has shown and what it already sent to the inventory.
    // Reopening the same block picks it up again, while opening any other
    // container replaces it - the module only ever remembers one container, the
    // one it worked on last.
    bool m_hasRememberedChest = false;
    bedrocktools::sdk::BlockPos m_rememberedChestPosition{};
    const void* m_rememberedChestDimension = nullptr;
    cheststealer::ArmorTargets m_rememberedArmor;
    // This session was opened on a container it could identify, so what it
    // learns is worth keeping for the next visit to that container.
    bool m_sessionIdentified = false;

    // The container held an item matching the enabled types at least once
    // during this session, so the module worked on it and may close it once
    // those matching items are gone.
    bool m_sawItems = false;
};
