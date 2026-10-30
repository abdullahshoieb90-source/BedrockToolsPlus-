#pragma once

// Minimal model of the supported game's shader-color state. The CPU holder
// and the GPU's cached value are deliberately separate: changing the floats
// without setting the dirty byte does not change the value used by a draw.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fake_render {

using Color = std::array<float, 4>;

// These are fixed test-fixture values for Bedrock 1.26.60, rather than the
// production offset constants; otherwise a wrong production offset would make
// both the test setup and the renderer agree on the same incorrect address.
inline constexpr std::size_t kColorHolderOffset = 0x30;
inline constexpr std::size_t kOldColorHolderOffset = 0x28;
inline constexpr std::size_t kDirtyOffset = 0x10;

struct ShaderColor {
    Color rgba{};
    std::uint8_t dirty = 0;
};
static_assert(offsetof(ShaderColor, dirty) == kDirtyOffset);

inline ShaderColor* colorHolder(void* screenContext) {
    ShaderColor* result = nullptr;
    std::memcpy(&result, static_cast<std::byte*>(screenContext) + kColorHolderOffset, sizeof(result));
    return result;
}

struct GpuColor {
    Color rgba{1.0f, 1.0f, 1.0f, 1.0f};

    void upload(ShaderColor& holder) {
        if (holder.dirty) {
            rgba = holder.rgba;
            holder.dirty = 0;
        }
    }

    Color draw(ShaderColor& holder, const Color& vertex) {
        upload(holder);
        return {rgba[0] * vertex[0], rgba[1] * vertex[1],
                rgba[2] * vertex[2], rgba[3] * vertex[3]};
    }
};

} // namespace fake_render
