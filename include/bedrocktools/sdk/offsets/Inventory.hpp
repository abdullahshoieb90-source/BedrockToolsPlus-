#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk::offsets::Inventory {

inline constexpr std::size_t PlayerInventory = 0x570;
inline constexpr std::size_t PlayerInventoryContainer = 0xB8;
inline constexpr std::size_t ItemStackItemCounter = 0x8;
inline constexpr std::size_t ItemStackCount = 0x22;
inline constexpr std::size_t ItemStackValid = 0x23;
// ItemStackBase::mUserData — the stack's std::unique_ptr<CompoundTag>. The
// damage of a damageable stack lives in this tag ("Damage"), which is what
// ItemStackBase::getDamageValue() reads; a null pointer means the stack has no
// tag at all and therefore no damage.
inline constexpr std::size_t ItemStackUserData = 0x10;
// ItemStackBase::mAuxValue — the item's auxiliary value (block data, dye
// colour), *not* the durability of an armor piece: that one lives in the
// stack's tag. The reversed ItemStackBase keeps it directly in front of mCount
// (mItem 0x0, mUserData 0x10, mBlock 0x18, mAuxValue 0x20, mCount 0x22,
// mValid 0x23), the ordering the offsets above rely on. The name stays for the
// callers that once took it for the damage: huditems probes this offset and its
// neighbours as a last-resort fallback and keeps a value only when it is
// plausible, so a moved field cannot report garbage either.
inline constexpr std::size_t ItemStackDamage = ItemStackCount - sizeof(std::int16_t);
inline constexpr std::size_t ItemGetDescriptionIdVtableIndex = 5;
inline constexpr std::size_t FillingContainerItems = 0x140;
inline constexpr std::size_t ItemStackSize = 0x98;

}
