#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {

// Offsets describing how to reach the player's movement-input state.
//
// On the supported arm64 client build the LocalPlayer owns an input component
// that carries the per-tick RawInputState. The keystrokes module reads it every
// game tick to mirror the player's WASD / jump / sneak state and the analog
// movement vector coming from touch / controller input.
namespace MoveInput {
// LocalPlayer -> IMoveInputComponent (the component that stores the raw state).
inline constexpr std::size_t mComponent = 0x2A0;
// IMoveInputComponent -> MoveInputState mRawInputState.
inline constexpr std::size_t mRawInputState = 0x0;
}

// Layout of MoveInputState. The button state is packed as a bitset while the
// analog stick contribution is stored as a Vec2.
namespace MoveInputState {
inline constexpr std::size_t mAnalogMoveVector = 0x0;
inline constexpr std::size_t mInputBits = 0x8;
}

}
