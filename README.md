# BedrockToolsPlus

## Introduction

BedrockToolsPlus is an open-source native mod for Minecraft Bedrock on Android, made for [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid). It adds a collection of visual, HUD, player, and utility modules while also providing a small C++ SDK and event system for native mod development.

The source is public so people can study how a real LeviLauncher mod is structured, learn from it, and use the SDK as a starting point for their own mods.

## Features

- Native C++20 mod built for LeviLauncher and Preloader
- 56 configurable modules
- Public headers for Minecraft wrappers, offsets, signatures, and utilities
- Typed event system with runtime subscriptions for other native mods
- LeviLauncher mod-menu integration and persistent configuration
- Open-source and designed to be practical to extend

## Modules

**Visual:** Fullbright, Motion Blur, Fog Color, Glint Color, TNT Timer, NoFog, View Model, Third Person Nametag, Chunk Border, Hitbox, Block Outline, ESP, Zoom, Breadcrumbs, FPS Unlocker, Light Overlay, ShulkerPreview, Connected Glass, Swing Modifier, Wings

**HUD:** Ping Counter, Reach Counter, Combo Display, Break Indicator, Player Coords, Compass, Speed Display, Effect Display, Debug Menu, Keystrokes, Tablist, Crosshair, ArmorHUD, Armor, Hotbar Slots, Inventory HUD, World Time, Arrow Counter, Totem Counter

**Player:** Time Changer, Weather Changer, Nick, Skin Stealer, AutoGG, AutoReQ, AutoSprint, Quick Loot, Custom Capes

**Misc:** No Disconnect, Chat Timestamps, No Touch Border, CPS Limiter, Hit Sound, ForceGlobalRP, CommentKey, Command Hotkey, Hive Utils

## Block Outline

**Block Outline** draws a configurable world-space overlay over the hard-to-see selected-block border. It only follows the block currently under the crosshair and is entirely client-side.

- **Outline**, **Outline Color**, **Outline Opacity** and **Line Thickness** control the 12 block edges. Thickness uses real camera-facing geometry above the hairline setting, so it works on Android GLES drivers that ignore native line width.
- **Fill** adds a translucent block overlay. **Fill Face Only** limits it to the face under the crosshair; otherwise all six faces are drawn. **Fill Color** and **Fill Opacity** are independent from the outline.
- **Rainbow** animates both passes, with **Rainbow Speed** controlling the cycle. **Pulse** smoothly animates their opacity, with a separate **Pulse Speed**.
- **Through Walls** switches to a no-depth material when the current game build provides it. It is off by default, so terrain normally occludes the overlay.

The target is sampled on the client tick instead of resolving the hit result from the render thread, avoiding frame stalls. Settings and keybind state are persisted in `config.json`.

## Inventory HUD

Shows the 27 slots of your inventory grid on the HUD without opening the inventory. Under **Details** you can toggle **Stack Count** and **Durability Bar**, **Number Text** changes the size and color of the stack counts, and **Slot Background** (on by default) draws a cell behind every slot — empty ones included — so the grid reads like the inventory screen. **Background Color** and **Background Opacity** style those cells.

The module owns a single HUD Editor element, **Inventory Grid**. Armor and the offhand are no longer part of it — they live in the separate **Armor** module below.

## Armor

**Armor** is its own module: it draws your helmet, chestplate, leggings, boots and the offhand item on the HUD, with or without Inventory HUD enabled. It has its own toggle, keybind, settings and HUD Editor element (**Armor & Offhand**), so it can be placed anywhere on screen independently of the inventory grid.

- **Offhand Slot**, **Stack Count** and **Durability Bar** under **Details** control what is drawn on each slot.
- **Armor Durability Numbers** is on by default and displays each piece's remaining/maximum durability (for example, `220/363`), including fully repaired armor. It works independently of the durability bars, and its labels are part of the armor element.
- **Slot Background** (on by default) draws a cell behind every armor and offhand slot — empty ones included — so the column reads like the inventory screen. **Background Color** and **Background Opacity** style those cells.
- **Horizontal Layout** lays the five slots out in a row instead of a column.

Configs saved while armor was still an Inventory HUD option are migrated automatically: the new module inherits the position, size and style you had, and it starts enabled only if **Armor & Offhand** was on.

## Crosshair

**Crosshair** replaces the game's own crosshair with one of 29 shapes drawn at the exact screen center. The **Style** picker is grouped by how the shape reads on screen:

- **Marks** — Dot, Plus, X, T Shape, T Shape Down, Chevron, Arrow, Star
- **Gapped crosses** — Cross, Cross Dot, Cross X, Vertical, Horizontal
- **Rings** — Circle, Circle Dot, Circle Cross, Ring Ticks, Broken Ring, Target
- **Boxes** — Square, Square Dot, Diamond, Triangle, Brackets, Brackets Dot, Grid
- **Reticles** — Scope, Mil Dots, Converge

Every shape shares the same controls: **Scale** and **Thickness** set the size and the line weight, **Color** plus **Opacity** style it, and **Outline** adds a dark back-pass so bright skies and sand stay readable. **Rgb** animates the hue through the whole wheel with **Rgb Speed** controlling the cycle. **Indicator** recolors the crosshair (with **Indicator Color**) while you are aiming at a mob or another player — the hit test lives in this module, so it works without enabling Hitbox. **Show Third Person** also draws the overlay while the camera is behind or in front of you; it is off by default, like vanilla.

Selecting **Vanilla** gives the crosshair back to the game: the module then only tints the game's own crosshair when the indicator fires (and, on builds that cannot be tinted in place, briefly swaps it for a same-shaped overlay), so exactly one crosshair is ever on screen. New styles are always appended to the picker, so configs saved by an older version keep drawing the same shape.

## ESP

**ESP** draws its boxes, tracers and labels as *world-space geometry inside the game's own render pass* — the exact path the Hitbox module uses (a `LevelRenderer::renderLevel` detour feeding the game's Tessellator), with the one difference that the overlay is **not** depth clipped, so entities stay visible **through walls**. Everything is client-side and read-only.

Because the game transforms that geometry with the same matrices it rendered the level with, a box can only ever land where its entity is: there is no camera model of the module's own to get wrong, no frame of look latency between the game and the launcher's overlay, and sprint FOV changes or view bob cannot pull a box off its target. That was the whole problem with projecting ESP to the HUD surface, which is why everything that has to sit *on* an entity moved out of the projection: the boxes, both tracers, the distance readout, and — since the module can build text out of the game's own pixel face — the nametag and the health stack too. They are billboarded quads hung on the entity's hitbox now, and the projection is only ever asked how big a pixel is at that depth, never where a label goes.

What is left on the HUD is what geometry cannot reach: a name written in a script the pixel face has no cells for (Arabic, Hebrew, CJK, emoji), which keeps the launcher's font and takes its whole label column with it, and a snapline for an entity behind the eye, which has no pixels for anything to be pinned to.

- **Show Players**, **Show Mobs** and **Show Items** control which actor categories are drawn. Invisible actors are always skipped, and **Show Local Player** adds your own ESP while the camera is in third person (it is off by default, matching the Hitbox behavior).
- **Range** sets how far out actors are fetched (8–256 blocks). **Through Walls** is on by default: no occlusion test runs and the material ignores the depth buffer, so a walled-off actor keeps drawing. Switching it off culls actors that are fully hidden behind solid blocks with the same voxel raycast the Hitbox module uses (a tall mob whose head pokes over a low wall stays visible).
- **Box** draws the twelve-edge wireframe around the entity's hitbox. **Box Style** picks between the full **Box** and **Corner** (the same edges trimmed to even brackets at the corners), **Box Thickness** sets the line weight — 1.0 is the game's hairline, above that every edge also becomes a camera-facing beam `boxThickness * 0.01` blocks wide, because mobile GLES drivers ignore line width — and **Box Color** styles it. **Rgb** (with **Rgb Speed**) animates the outline through the whole color wheel instead.
- **Filled Box** paints the six faces of the box in **Filled Box Color** at **Filled Box Opacity**, behind the wireframe and with both windings so it reads from any side.
- **Tracers** draws a line that ends inside the entity's hitbox, so it cannot slide off it while you turn the camera or while the **Fov** slider disagrees with the game. Both origins are world-space geometry handed to the game in the same render pass as the boxes, and only where they start differs: **Bottom** from your own feet, **Crosshair** from a point on the view axis. That detour is the whole trick — a line that *starts at the eye* lies on a single view ray, so the game projects all of it onto one pixel and the tracer used to disappear when **Crosshair** was selected, while every point of the axis lands on the middle of the screen at any depth, which is what lets the crosshair line be geometry and still read as drawn from the crosshair. Both are styled by **Tracer Color** and stay hairline (a thickness beam that starts at the camera fills the screen). An entity at or behind the eye is the one case with nothing to pin to, and it keeps a screen-space snapline pointing the way; an entity off the edge of the screen needs no such line, since the game clips the real one.
- **Distance** prints the range in meters right under the entity's hitbox, also as world-space geometry: a small blocky, billboarded readout that the game pins to the entity's feet. It keeps a constant on-screen size at any range, and the measurement is feet-to-feet between the two collision boxes, so it reads the same in first and third person. The **Fov** setting only influences how large the digits appear, never where.
- **Nametag** and **Health** are a column above the entity's head — the name, the health value, and a green → yellow → red bar as wide as the hitbox it sits on, styled by **Nametag Color** and **Nametag Scale** — and it is geometry like the wireframe under it: the module builds the glyphs as billboarded quads on the top-center of the entity's own AABB, so a name follows its head through sprint FOV, view bob and a wrong **Fov** alike. The bar's track and its fill are two color groups of one mesh. Only a label the pixel face cannot spell falls back to the launcher's HUD text, and then the *whole* column falls back with it rather than being split into a pinned name over a projected bar.
  - The face the column is spelled with is generated from the game's own `resources/minecraft.ttf` by `scripts/gen_esp_pixel_font.py` and committed as `esp_pixel_font.hpp` (`scripts/run_tests.sh` re-checks it against the font): every printable ASCII code point is rasterized on the font's 192-unit pixel grid and merged into rectangles, so an `i` costs a third of an `M` and a `g` hangs below the baseline exactly as the game draws them. Basic Latin is all the face covers, which is also what decides which path a name takes.
  - A name that goes to the launcher instead is measured there — and that is the one case where the module needs the font for itself: it registers the same `minecraft.ttf` under the launcher (once per package, shared with Effect Display) and asks for it whenever the text fits inside it, because a centered label has to be centered on widths it can read, and the pixel font's are the only ones known here. The launcher's default font takes a script the pixel font has no glyph for, which is also the only one that shapes right-to-left names instead of drawing a row of replacement boxes.
  - The name is cleaned before it is drawn either way: the section-sign markup codes a server or a nick add-on pads it with, and the invisible zero-width and bidi characters a right-to-left name arrives wrapped in, are exactly what a plain HUD font would paint as extra characters beside the label; a doubled sign, which is the game's own escape for a literal **§**, survives as that one character. A name is also cut short before it is drawn — one oversized text makes the launcher reject the whole frame's batch, which would blank every other entity's nametag — and a line longer than the label column can afford to spell out as quads goes to the HUD instead of inflating the mesh. The HUD path centers what it draws on a width measured in *glyphs* rather than bytes, so a multi-byte name sits above the head instead of to the left of it.

**Fov** matches the projection to your in-game field of view (default 70, Bedrock's own `gfx_fov`, a *vertical* FOV widened by the surface aspect). It sizes the billboarded labels — how many blocks a surface pixel is at that depth — and places the fallback ones, so nudge it if a fallback name sits too close to the middle of the screen or overshoots its target. The boxes, both tracers and every pinned label do not depend on it for their position.

The overlay is published from inside the render hook, so the geometry and the labels always describe the same frame; `onFrame` only clears the HUD layer again once the world stops being rendered, which keeps a frozen overlay from hanging over menus.

The world-space builders (box edges, corner brackets, faces, both tracers, the label plane and its billboarded glyphs), the camera basis, the projection, the nametag text cleanup, which font ends up drawing a name and the glyph-width measurement that centers it are plain math in `esp_geometry.hpp`, and the tessellator/material plumbing lives in `overlay_mesh.hpp`; both are pure enough to run on the host, so `tests/esp_geometry_test.cpp` covers the math — including that a camera-origin line really is degenerate, that an axis point is not, and where the snapline is still needed — and `tests/esp_render_test.cpp` drives the real render hook with stand-in actors and checks the emitted vertices and draw commands, including that turning the view leaves the box geometry bit-identical, cannot slide the readout or the nametag off the hitbox, and never blanks the labels of the frame around one actor with a broken collision box.

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

## Hit Sound

The **Hit Sound** module plays a custom sound of your choice every time you land a melee hit on a mob or another player.

1. Put sound files (`.wav`, `.ogg`, `.mp3`, `.m4a` or `.flac` — ideally short one-shot effects; a `Sample Hit.wav` is generated for you on first launch) into the `hitsounds` folder next to your `config.json` (`<mod config dir>/hitsounds`, created automatically on first launch).
2. (Re)launch the game, open the BedrockToolsPlus mod menu and enable **Hit Sound**.
3. Pick a file in the module's **Sound** selector — every audio file in the folder shows up there. Choose `None` to keep the vanilla behavior. The **Volume** slider sets how loud the sound plays.

The sound is a purely client-side overlay: the victim's own hurt sound is not cancelled or replaced, and nothing is sent to the server. Files whose names contain a comma are ignored (the menu picker cannot represent them), and sounds that fail to decode on your device are simply skipped.

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
