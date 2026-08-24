#pragma once

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/offsets/Input.hpp>
#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk {

// Mirror of the game's MoveInputState. It exposes the movement bitset through a
// small set of named flags plus the analog movement vector. Only the members
// consumed by the tooling are modelled; the struct is used as a view over the
// live game object via reinterpret_cast, so it must stay a trivial layout that
// matches offsets::MoveInputState.
struct MoveInputState {
    enum class Flag : std::uint32_t {
        Up        = 1u << 0,
        Down      = 1u << 1,
        Left      = 1u << 2,
        Right     = 1u << 3,
        UpLeft    = 1u << 4,
        UpRight   = 1u << 5,
        DownLeft  = 1u << 6,
        DownRight = 1u << 7,
        JumpDown  = 1u << 8,
        SneakDown = 1u << 9,
    };

    Vec2 mAnalogMoveVector;
    std::uint32_t mInputBits;

    bool test(Flag flag) const {
        return (mInputBits & static_cast<std::uint32_t>(flag)) != 0;
    }
};

// View over the game's move-input component. The raw per-tick state lives at a
// fixed offset inside the component and is exposed as a reference member so call
// sites can read it as `component->mRawInputState`.
struct MoveInputComponent {
    const MoveInputState& rawInputState() const {
        return *reinterpret_cast<const MoveInputState*>(
            reinterpret_cast<std::uintptr_t>(this) + offsets::MoveInput::mRawInputState);
    }
};

// Resolve the movement-input component stored on the local player. Returns
// nullptr when the player pointer is null or the component is not present.
inline const MoveInputComponent* moveInputComponent(void* player) {
    if (!player) return nullptr;
    auto* component = field<void*>(player, offsets::MoveInput::mComponent);
    return reinterpret_cast<const MoveInputComponent*>(component);
}

}
