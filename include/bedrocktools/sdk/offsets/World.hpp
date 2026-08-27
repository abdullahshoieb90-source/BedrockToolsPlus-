#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk::offsets {

namespace Player {
// mInventory is the per-player PlayerInventory that owns both the hotbar and
// the main inventory (36 slots: 9 hotbar + 27 main). On 1.21+ Bedrock ARM64
// the field is a unique pointer wrapped in a Bedrock::NonOwnerPointer-style
// structure: the first pointer-sized slot is the reference control and the
// second holds the PlayerInventory object. The actual object is what
// InventoryHUD reads from; the wrapper is invisible to callers because the
// field stores the raw object pointer in practice.
// The default value below is the 1.26.44 ARM64-v8a offset that has been
// verified against the bundled signatures. If a future Bedrock update
// reshuffles Player, this is the single value the InventoryHUD module needs
// to be rebuilt against.
inline constexpr std::size_t mName = 2824;
inline constexpr std::size_t mSkin = 2552;
inline constexpr std::size_t mInventory = 0xA18;
}

namespace PlayerInventory {
// PlayerInventory stores its hotbar (9 slots) and main inventory (27 slots)
// in a flat ItemStack array. mItems is a Bedrock::BedrockVector<ItemStack>
// with the usual begin/end/capacity pointer triple; mInventorySize is the
// total number of slots (hotbar + main = 36 on vanilla). Both fields are
// laid out as documented in the 1.26.44 ARM64-v8a decompile.
inline constexpr std::size_t mItems = 0x10;
inline constexpr std::size_t mItemsSize = 0x800; // one ItemStack
inline constexpr std::size_t mInventorySize = 36;
inline constexpr std::size_t mHotbarSize = 9;
inline constexpr std::size_t mSelectedSlot = 0xE0; // current hotbar index
}

namespace ItemStack {
inline constexpr std::size_t mCount = 0x88;          // uint8: stack size
}

namespace Mob {
inline constexpr std::size_t mHealthAttribute = 0x1A0;
}

namespace AttributeInstance {
inline constexpr std::size_t mCurrentValue = 0x8;
}

namespace Actor {
inline constexpr std::size_t mEntityContext = 0x8;
inline constexpr std::size_t mEntityData = 0x120;
inline constexpr std::size_t mStateVectorComponent = 0x208;
inline constexpr std::size_t mActorRotationComponent = 0x218;
inline constexpr std::size_t mLevel = 464;
inline constexpr std::size_t mDimension = 448;
inline constexpr std::size_t mHurtTime = 0x194;
inline constexpr std::size_t mCategories = 512;
inline constexpr std::size_t mNameTagHash = 384;
inline constexpr std::size_t mFilteredNameTag = 712;
}

// The component pointed to by Actor::mStateVectorComponent. Its first member
// is the actor's current world position and its second member is the
// position from the previous tick; together they drive partial-tick
// interpolation (pos = prev + (cur - prev) * partialTicks).
namespace StateVectorComponent {
inline constexpr std::size_t mPosition = 0x0;
inline constexpr std::size_t mPreviousPosition = 0xC;
}

// Bitmask values for the actor category flags stored in Actor::mCategories,
// in the order of the ActorCategory enum of the 1.21+ decompile. The Hitbox
// module relies on IsMob (1 << 1) being the "mob" bit.
namespace ActorCategories {
inline constexpr std::uint32_t IsPlayer             = 1u << 0;
inline constexpr std::uint32_t IsMob                = 1u << 1;
inline constexpr std::uint32_t IsItem               = 1u << 2;
inline constexpr std::uint32_t IsProjectile         = 1u << 3;
inline constexpr std::uint32_t IsFireball           = 1u << 4;
inline constexpr std::uint32_t IsHangingObject      = 1u << 5;
inline constexpr std::uint32_t IsWaterCreature      = 1u << 6;
inline constexpr std::uint32_t IsMonster            = 1u << 7;
inline constexpr std::uint32_t IsCreature           = 1u << 8;
inline constexpr std::uint32_t IsWaterMob           = 1u << 9;
inline constexpr std::uint32_t IsAmbient            = 1u << 10;
inline constexpr std::uint32_t IsFlying             = 1u << 11;
inline constexpr std::uint32_t IsPowderSnowCreature = 1u << 12;
inline constexpr std::uint32_t IsMinecart           = 1u << 13;
inline constexpr std::uint32_t IsVehicle            = 1u << 14;
inline constexpr std::uint32_t IsTNT                = 1u << 15;
}

namespace ActorDataIds {
// Health is data id 1 in the synced actor data (the same numbering the wire
// protocol uses since 1.16.100: HEALTH = 1, FUSE_LENGTH = 55,
// ALWAYS_SHOW_NAMETAG = 81). It is an int on vanilla-style servers and a
// float on others, so readers must accept both DataItem types.
inline constexpr std::size_t Health = 1;
inline constexpr std::size_t FuseTime = 55;
inline constexpr std::size_t NametagAlwaysShow = 81;
}

namespace ActorFlags {
inline constexpr int CanShowName = 14;
inline constexpr int AlwaysShowName = 15;
}

namespace DataItem {
inline constexpr std::size_t mType = 0x8;
inline constexpr std::size_t mId = 0xA;
// +0xC is the dirty/header field; the payload starts at +0x10.
inline constexpr std::size_t mValue = 0x10;
inline constexpr std::size_t mMinimumSize = 0x18;
inline constexpr std::uint8_t ByteType = 0;
inline constexpr std::uint8_t ShortType = 1;
inline constexpr std::uint8_t IntType = 2;
inline constexpr std::uint8_t FloatType = 3;
}

namespace BuiltInActorComponents {
inline constexpr std::size_t mAABBShapeComponent = 8;
}

namespace AABBShapeComponent {
inline constexpr std::size_t mAABB = 0;
}

namespace Level {
inline constexpr std::size_t mActorManager = 0x470;
// Bedrock::UniqueOwnerPointer<HitResultWrapper>, not an embedded wrapper.
inline constexpr std::size_t mHitResultWrapper = 456;
}

namespace UniqueOwnerPointer {
// UniqueOwnerPointer stores its reference-control owner first and the owned
// value second. Both members are pointer-sized on the supported arm64 ABI.
inline constexpr std::size_t mValue = sizeof(std::uintptr_t);
}

namespace HitResult {
inline constexpr std::size_t mStartPos = 0;
inline constexpr std::size_t mRayDir = 12;
inline constexpr std::size_t mType = 24;
inline constexpr std::size_t mFacing = 28;
inline constexpr std::size_t mBlockPos = 32;
inline constexpr std::size_t mPos = 44;
inline constexpr int TypeBlock = 0;
inline constexpr int TypeEntity = 1;
inline constexpr int TypeEntityOutOfRange = 2;
inline constexpr int TypeNoHit = 3;
}

namespace HitResultWrapper {
inline constexpr std::size_t mHitResult = 0;
}

namespace Dimension {
inline constexpr std::size_t mBlockSource = 208;
inline constexpr std::size_t mWeather = 0x1B8;
}

namespace Biome {
inline constexpr std::size_t mHash = 400;
}

namespace Weather {
inline constexpr std::size_t mOldRainLevel = 0x34;
inline constexpr std::size_t mRainLevel = 0x38;
inline constexpr std::size_t mTargetRainLevel = 0x3C;
inline constexpr std::size_t mOldLightningLevel = 0x40;
inline constexpr std::size_t mLightningLevel = 0x44;
inline constexpr std::size_t mTargetLightningLevel = 0x48;
}

}
