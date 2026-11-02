// Regression tests for scoped world-overlay shader colors. The fake models
// the game's CPU holder and independently cached GPU tint/dirty flag.
#include "core/RenderState.hpp"
#include "fake_shader_color.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace {
using bedrocktools::core::ScopedShaderColor;
using fake_render::Color;
using fake_render::GpuColor;
using fake_render::ShaderColor;

int failures = 0;
void check(bool condition, const char* message) {
    std::printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

template <std::size_t N>
void putPointer(std::array<std::byte, N>& screen, std::size_t offset, void* pointer) {
    std::memcpy(screen.data() + offset, &pointer, sizeof(pointer));
}

bool earlyReturn(void* screen) {
    ScopedShaderColor color(screen);
    return static_cast<bool>(color);
}
} // namespace

int main() {
    namespace off = bedrocktools::sdk::offsets;
    static_assert(!std::is_copy_constructible_v<ScopedShaderColor>);
    static_assert(!std::is_move_constructible_v<ScopedShaderColor>);

    std::printf("scoped overlay shader color\n");
    check(off::ScreenContext::mColorHolder == fake_render::kColorHolderOffset &&
          off::ShaderColor::mDirty == fake_render::kDirtyOffset,
          "uses the supported game's holder and dirty-byte offsets");

    alignas(16) std::array<std::byte, 0xC0> screen{};
    const Color original{0.15f, 0.35f, 0.55f, 0.75f};
    const Color white{1, 1, 1, 1};
    const Color black{0, 0, 0, 1};
    const Color vertex{0.2f, 0.6f, 0.9f, 0.4f};
    ShaderColor holder{original, 0};
    ShaderColor oldSlot{{0.8f, 0.7f, 0.6f, 0.5f}, 0};
    putPointer(screen, fake_render::kColorHolderOffset, &holder);
    putPointer(screen, fake_render::kOldColorHolderOffset, &oldSlot);

    GpuColor gpu;
    gpu.rgba = black;
    {
        ScopedShaderColor outer(screen.data());
        check(static_cast<bool>(outer) && holder.rgba == white && holder.dirty == 1,
              "sets white and invalidates a previously black GPU tint");
        check(gpu.draw(holder, vertex) == vertex && holder.dirty == 0,
              "the next overlay draw receives unmodified vertex RGBA");
        {
            ScopedShaderColor inner(screen.data());
            check(static_cast<bool>(inner) && holder.rgba == white && holder.dirty == 1,
                  "nested overlays also invalidate the cached tint");
            gpu.draw(holder, vertex);
        }
        check(holder.rgba == white && holder.dirty == 1,
              "nested scope restores and re-invalidates the outer state");
        gpu.upload(holder);
    }
    check(holder.rgba == original && holder.dirty == 1,
          "scope exit restores all four original channels and marks them dirty");
    gpu.upload(holder);
    check(gpu.rgba == original && holder.dirty == 0,
          "the following game draw receives the restored tint");
    check((oldSlot.rgba == Color{0.8f, 0.7f, 0.6f, 0.5f}) && oldSlot.dirty == 0,
          "the unrelated object at the old offset remains untouched");

    // A scope that returns before submitting a mesh still restores state.
    holder = {original, 0};
    gpu.rgba = black;
    check(earlyReturn(screen.data()) && holder.rgba == original && holder.dirty == 1,
          "early return restores the holder without leaking white");
    gpu.upload(holder);
    check(gpu.rgba == original, "the next upload after an early return uses the original tint");

    check(!ScopedShaderColor(nullptr), "null screen context is rejected");
    check(!ScopedShaderColor(reinterpret_cast<void*>(0x10)), "invalid low screen pointer is rejected");
    putPointer(screen, fake_render::kColorHolderOffset, nullptr);
    check(!ScopedShaderColor(screen.data()), "null color holder is rejected");
    putPointer(screen, fake_render::kColorHolderOffset, reinterpret_cast<void*>(0x10));
    check(!ScopedShaderColor(screen.data()), "invalid low holder pointer is rejected");

    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all shader-color checks passed\n");
    return 0;
}
