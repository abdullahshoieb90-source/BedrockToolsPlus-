#pragma once

#include <bedrocktools/sdk/offsets/Render.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bedrocktools::core {

// World overlays supply their own vertex colors. The game's shader-color
// multiplier, however, may still contain a tint from drawing the player (in
// particular in third person). Temporarily use white and tell the renderer to
// upload it; writing the four floats alone leaves the GPU's cached tint intact.
// Restore the game's original color and mark it dirty again for the next draw.
class ScopedShaderColor {
public:
    explicit ScopedShaderColor(void* screenContext) noexcept {
        if (reinterpret_cast<std::uintptr_t>(screenContext) < 0x1000) return;

        std::byte* holder = nullptr;
        std::memcpy(&holder,
                    static_cast<std::byte*>(screenContext) +
                        bedrocktools::sdk::offsets::ScreenContext::mColorHolder,
                    sizeof(holder));
        if (reinterpret_cast<std::uintptr_t>(holder) < 0x1000) return;

        mHolder = holder;
        std::memcpy(mSaved.data(), mHolder, sizeof(mSaved));
        constexpr std::array<float, 4> white{1.0f, 1.0f, 1.0f, 1.0f};
        std::memcpy(mHolder, white.data(), sizeof(white));
        markDirty();
    }

    ~ScopedShaderColor() {
        if (!mHolder) return;
        std::memcpy(mHolder, mSaved.data(), sizeof(mSaved));
        markDirty();
    }

    explicit operator bool() const noexcept { return mHolder != nullptr; }
    ScopedShaderColor(const ScopedShaderColor&) = delete;
    ScopedShaderColor& operator=(const ScopedShaderColor&) = delete;
    ScopedShaderColor(ScopedShaderColor&&) = delete;
    ScopedShaderColor& operator=(ScopedShaderColor&&) = delete;

private:
    void markDirty() noexcept {
        mHolder[bedrocktools::sdk::offsets::ShaderColor::mDirty] = std::byte{1};
    }

    std::byte* mHolder = nullptr;
    std::array<float, 4> mSaved{};
};

} // namespace bedrocktools::core
