#pragma once

#include <cstdint>
#include <string_view>

namespace bedrocktools {

inline constexpr std::string_view Name = "BedrockToolsPlus";
inline constexpr std::string_view Author = "VENOM P2 GM";
inline constexpr std::string_view Description = "A collection of utility, HUD, player, and visual modules for Minecraft Bedrock.";
inline constexpr std::string_view Version = "1.5.3";
// Minecraft's year-based release name is 26.50; Android reports the full
// client version as 1.26.50.4. Keep this in one place so the package metadata
// and clients embedding the SDK advertise the same target build.
inline constexpr std::string_view MinecraftVersion = "1.26.50.4";
inline constexpr std::uint32_t SdkVersion = 1;
inline constexpr std::string_view RuntimeLibrary = "libBedrockToolsPlus.so";

}
