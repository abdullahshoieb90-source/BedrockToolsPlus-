#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk::offsets::Inventory {

inline constexpr std::size_t PlayerInventory = 0x570;
inline constexpr std::size_t PlayerInventoryContainer = 0xB8;
inline constexpr std::size_t ItemStackItemCounter = 0x8;
inline constexpr std::size_t ItemStackCount = 0x22;
inline constexpr std::size_t ItemStackValid = 0x23;
// ItemStackBase::mAuxValue — the damage of a damageable stack. The reversed
// ItemStackBase keeps it directly in front of mCount (mItem 0x0, mUserData
// 0x10, mBlock 0x18, mAuxValue 0x20, mCount 0x22, mValid 0x23), which is the
// same ordering the two offsets above already rely on. huditems::stackDamage
// uses it as the fallback when ItemStackBase::getDamageValue cannot be
// resolved, so a busted signature can no longer hide the durability bars.
inline constexpr std::size_t ItemStackDamage = ItemStackCount - sizeof(std::int16_t);
inline constexpr std::size_t ItemGetDescriptionIdVtableIndex = 5;
inline constexpr std::size_t FillingContainerItems = 0x140;
inline constexpr std::size_t ItemStackSize = 0x98;

}
