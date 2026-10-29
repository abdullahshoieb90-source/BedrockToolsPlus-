#pragma once

// Small fake of the compound-tag tree used by the real HUD item reader. The
// tag storage lives at stable addresses so ItemStack user-data pointers remain
// valid while the integration tests update equipment in place.

#include <bedrocktools/sdk/Offsets.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string_view>

namespace fake_damage_nbt {
namespace offsets = bedrocktools::sdk::offsets::ShulkerPreview;

struct Tag {
    alignas(void*) std::array<std::byte, offsets::CompoundTagTreeEnd + sizeof(void*)> compound{};
    alignas(void*) std::array<std::byte, offsets::NbtNodeType + sizeof(std::uint32_t)> node{};
    bool present = true;
    bool returnNull = false;
};

inline std::map<void*, Tag> tags;
inline int findCalls = 0;

template<class T>
void put(void* object, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(value));
}

inline void setDamage(void* stack, int damage) {
    if (damage == 0) {
        put(stack, offsets::ItemStackBaseUserData, static_cast<void*>(nullptr));
        return;
    }
    Tag& tag = tags[stack];
    tag.present = true;
    tag.returnNull = false;
    put(tag.node.data(), offsets::NbtNodeType, std::uint32_t{3});
    put(tag.node.data(), offsets::NbtNodeNumericValue, static_cast<std::int32_t>(damage));
    put(stack, offsets::ItemStackBaseUserData, static_cast<void*>(tag.compound.data()));
}

inline void* find(void* treeRoot, const char* key, std::size_t length) {
    ++findCalls;
    if (!key || std::string_view(key, length) != "Damage") return nullptr;
    for (auto& [stack, tag] : tags) {
        (void)stack;
        if (treeRoot != tag.compound.data() + offsets::CompoundTagTreeRoot) continue;
        if (tag.returnNull) return nullptr;
        return tag.present ? static_cast<void*>(tag.node.data())
                           : static_cast<void*>(tag.compound.data() + offsets::CompoundTagTreeEnd);
    }
    return nullptr;
}
} // namespace fake_damage_nbt
