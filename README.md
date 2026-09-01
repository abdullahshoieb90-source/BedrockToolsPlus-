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

**Misc:** No Disconnect, Chat Timestamps, No Touch Border, CPS Limiter, Pack Merger

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

The **Custom Capes** module lets you wear any PNG as a classic cape.

1. Put cape images (`.png`, ideally 64x32 — any other size is scaled automatically) into the `capes` folder next to your `config.json` (`<mod config dir>/capes`, created automatically on first launch along with a sample cape).
2. (Re)launch the game, open the BedrockToolsPlus mod menu and enable **Custom Capes**.
3. Pick a file in the module's **Cape** selector — the cape updates in-game immediately. Choose `None` to bring your vanilla cape back.

Images that are not exactly 64x32 are scaled onto the cape's outer back face (`x=1..11, y=1..17` of the 64x32 cape canvas); the inner front face gets a flat lining color instead of a repeat of the image, and the top/bottom/side edge strips pick up the image's edge colors so the cape keeps its visible thickness. Exact 64x32 images are used pixel-for-pixel with no processing.

The change is fully client-side and visual only; it does not affect servers, accounts, or other players. Persona skins are not affected (capes are persona pieces there).

## Pack Merger

The **Pack Merger** module lets conflicting resource packs work together. Minecraft Bedrock loads a resource file from the highest pack on the stack that contains it, so two packs that both edit the same file — for example a Java-animations pack and a Cape-Physics pack both shipping `entity/player.entity.json` — can never coexist: only the top pack's version is loaded and the other pack silently stops working.

When enabled, the module automates the usual manual fix with no file editing:

1. It reads your global pack stack (`<com.mojang>/global_resource_packs.json`).
2. Every `.json` or `.lang` file that exists in **more than one** of those packs is deep-merged: the top pack wins every conflicting key, keys that only exist in lower packs are copied in recursively, and `scripts.animate` binding lists are unioned so every pack's animations stay bound.
3. The merged files are written to a small generated pack `resource_packs/bedrocktoolsplus-merged/` (fixed UUID, shows up as **BTP Merged Packs**).
4. That pack is appended to the global stack — and to any world's stack that uses one of the participating packs — so the merged files load above your packs. Everything else (textures, sounds, unique files) still resolves through your own packs exactly like vanilla.
5. Disabling the module removes the generated pack from the stacks again. Your packs are never modified, and a one-time `.btp-backup` copy is kept next to each stack file before it is first rewritten.

Usage: enable **Pack Merger**, restart Minecraft once, done. Add or remove resource packs whenever you like — the merged pack is rebuilt automatically on the next launch. The module's menu entry shows the last build status, an **Add To Worlds** toggle (on by default) and a **Rebuild Now** button.

Notes and limits:

- Only packs enabled in the **global** resource pack stack participate; packs enabled per world count only through the world-stack update in step 4.
- Binary conflicts (both packs shipping the same `.png`) are not merged — the top pack's file wins, like vanilla.
- A pack whose JSON is invalid keeps vanilla behavior for that file (the top pack's bytes are used instead of a merge).
- Files inside `subpacks/` are ignored.

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
