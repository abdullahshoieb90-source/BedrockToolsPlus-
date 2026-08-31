# BedrockToolsPlus

## Introduction

BedrockToolsPlus is an open-source native mod for Minecraft Bedrock on Android, made for [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid). It adds a collection of visual, HUD, player, and utility modules while also providing a small C++ SDK and event system for native mod development.

The source is public so people can study how a real LeviLauncher mod is structured, learn from it, and use the SDK as a starting point for their own mods.

## Features

- Native C++20 mod built for LeviLauncher and Preloader
- 45 configurable modules
- Public headers for Minecraft wrappers, offsets, signatures, and utilities
- Typed event system with runtime subscriptions for other native mods
- LeviLauncher mod-menu integration and persistent configuration
- Open-source and designed to be practical to extend

## Modules

**Visual:** Fullbright, Motion Blur, Fog Color, Glint Color, TNT Timer, NoFog, View Model, Third Person Nametag, Chunk Border, Block Outline, Hitbox, Zoom, Breadcrumbs, FPS Unlocker, Light Overlay, ShulkerPreview, Connected Glass

**HUD:** Ping Counter, Reach Counter, Combo Display, Break Indicator, Player Coords, Compass, Speed Display, Effect Display, Debug Menu, Keystrokes, Tablist, Crosshair

**Player:** Time Changer, Weather Changer, Nick, Skin Stealer, AutoGG, AutoReQ, Custom Capes

**Misc:** No Disconnect, Chat Timestamps, No Touch Border, CPS Limiter

## Wings

The **Wings** module renders animated 3D wings on your back that flap, idle and glide with your movement. Open the module's **Wing Style** selector to choose a shape:

- **Dragon** — the default articulated membrane wing
- **Angel** — white feathered blades with gold tips
- **Demon** — deep-red spiky membrane
- **Bat** — small dark membrane
- **Butterfly** — pink/orange panels with blue accents
- **Phoenix** — fiery orange feathers
- **Fairy** — small translucent cyan/pink wings

The wings are a world-space overlay (a `RenderLevel` hook + tessellator); they never touch skin memory and only appear from a third-person point of view. Each style is drawn as closed, tapered feather prisms with a rest-pose fan, a backwards sweep and per-face shading, so the wings read as real 3D volume. Developers can preview every style offline (and compare against the legacy renderer) with `./scripts/gen_wings_preview.sh`, which writes PNGs to `build/wings-preview/`.

## Custom Capes

The **Custom Capes** module lets you swap your character's cape for your own PNG files. Drop any number of PNG capes into the `capes` folder next to `config.json` (the folder is created automatically; on first run it is seeded with a small default cape and a `README.txt` that explains the folder). The module then shows a **Cape** selector in the launcher menu listing every PNG by file name, and a **Refresh Capes** button that re-scans the folder.

How it works:

- While enabled, the selected PNG is decoded and written into `SerializedSkinImpl::mCapeImage` of the local player's skin, so the engine renders it exactly like a vanilla cape (RGBA8Unorm, sRGB, depth 1 — the same image description real capes carry).
- The module also writes a synthetic non-empty `SerializedSkinImpl::mCapeId` (`bedrocktoolsplus-N`) next to the image. Modern game versions only render the classic cape when that id is non-empty, and the engine caches cape textures keyed by it — this is the piece a bare `mCapeImage` patch was missing. The id is regenerated on every cape (re)load so switching files shows immediately without rejoining; the original id bytes are restored on disable.
- The engine bakes your cape mesh — and decides whether you have a cape at all — from your skin **once**, when your renderer data is created at world entry. Patching the skin after that moment never rebuilds the mesh, which is why a cape patched only on the per-tick path would never show. The module therefore also runs the patch from `ClientInstanceUpdateEvent`, which fires once per frame *before* the level render pass: on the first frame after you join, the cape is already in the skin when the mesh is built, so the engine creates a real cape mesh with your PNG (vanilla cape, waving included).
- Because of the above, **enable the module before entering the world** (from the launcher menu) — or re-enter the world after enabling it mid-game — for the cape to appear.
- The original cape image is backed up on first application and restored when the module is disabled; on respawn the game rebuilds the skin and the module simply re-applies the cape.
- Capes are **client-side only** (other players still see your original cape) and require a **classic (non-persona) skin** — persona skins render their cape from persona pieces rather than `mCapeImage`.
- 64x32 or 128x64 PNGs with full alpha work best; the pixel buffer handed to the engine is owned by the game once installed (freed through the image's deleter slot), so nothing is leaked or double-freed across skin rebuilds.

## System Requirements

- Android 9 or newer
- 64-bit ARM device (`arm64-v8a`)
- [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid)
- A Minecraft Bedrock version supported by the BedrockToolsPlus release you are using

## Installation

1. Install LeviLauncher.
2. Download the latest `BedrockToolsPlus.levipack` release.
3. Import the package from LeviLauncher's mod manager and enable it.
4. Launch Minecraft through LeviLauncher.

## Development Setup

Requirements:

- Android NDK r28c
- xmake
- Python 3

Build for Android ARM64:

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

The release build produces `libBedrockToolsPlus.so` and `BedrockToolsPlus.levipack` in the xmake target directory.

Public SDK headers are under `include/bedrocktools`. Shared runtime code lives under `src/core`, while features are kept under `src/modules` by category. Minecraft signatures and offsets are version-specific, so those are the main pieces that normally need updating for a new game build.

Example event subscription from another native mod:

```cpp
#include <bedrocktools/BedrockToolsPlus.hpp>

bedrocktools::events::RuntimeListener<bedrocktools::events::LocalPlayerTickEvent> listener(
    [](auto& event) {
        if (!event.player) return;
        auto position = event.player->position();
    }
);
```

## Contributing

Pull requests are highly appreciated. Keep changes focused, preserve the existing project structure, and test changes against the intended Minecraft version before submitting them.

## Usage Guidelines

Do not use LeviLauncher or BedrockToolsPlus to violate Mojang or Microsoft's user agreements.

**Disclaimer:** The authors and contributors of BedrockToolsPlus and LeviLauncher are not responsible for bans, damages, or issues arising from the use of this software. Use it at your own risk and in accordance with Minecraft's terms of service.

## Credits & Acknowledgements

BedrockToolsPlus is made by [RadiantByte](https://github.com/RadiantByte) and maintained by VENOM P2 GM.

Special thanks to [dreamguxiang](https://github.com/dreamguxiang) for helping make this mod possible.

Motion blur module based on [mcpelauncher-motion-blur](https://github.com/CrackedMatter/mcpelauncher-motion-blur) by [CrackedMatter](https://github.com/CrackedMatter).

Thanks to [Kashifro](https://github.com/Kashifro) for the Shulker Preview and Tablist modules.

Built for [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid).

## Contact

Discord: [discord.gg/rMgdpTFFVg](https://discord.gg/rMgdpTFFVg)

**Report Issues:** Open an issue in this GitHub repository.

## License

BedrockToolsPlus is licensed under the [GNU General Public License v3.0](LICENSE).
