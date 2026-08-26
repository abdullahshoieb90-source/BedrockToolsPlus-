#pragma once

#include <cstdint>

namespace bedrocktools::sdk {

struct Vec2 { float x; float y; constexpr bool operator==(const Vec2&) const = default; };
struct Vec3 { float x; float y; float z; constexpr bool operator==(const Vec3&) const = default; };
struct BlockPos { int x; int y; int z; };
struct AABB { Vec3 min; Vec3 max; };
struct Color { float r; float g; float b; float a; };

}
