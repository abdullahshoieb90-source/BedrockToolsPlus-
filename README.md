# BedrockToolsPlus

## Introduction

BedrockToolsPlus is an open-source native mod for Minecraft Bedrock on Android, made for [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid). It adds a collection of visual, HUD, player, and utility modules while also providing a small C++ SDK and event system for native mod development.

The source is public so people can study how a real LeviLauncher mod is structured, learn from it, and use the SDK as a starting point for their own mods.

## Features

- Native C++20 mod built for LeviLauncher and Preloader
- 47 configurable modules
- Public headers for Minecraft wrappers, offsets, signatures, and utilities
- Typed event system with runtime subscriptions for other native mods
- LeviLauncher mod-menu integration and persistent configuration
- Open-source and designed to be practical to extend

## Modules

**Visual:** Fullbright, Motion Blur, Fog Color, Glint Color, TNT Timer, NoFog, View Model, Third Person Nametag, Chunk Border, Block Outline, Hitbox, Zoom, Free Look, Breadcrumbs, FPS Unlocker, Light Overlay, ShulkerPreview, Connected Glass

**HUD:** Ping Counter, Reach Counter, Combo Display, Break Indicator, Player Coords, Compass, Speed Display, Effect Display, Debug Menu, Keystrokes, Tablist, Crosshair

**Player:** Time Changer, Weather Changer, Nick, Skin Stealer, Custom Capes, AutoGG, AutoReQ

**Misc:** No Disconnect, Chat Timestamps, No Touch Border, CPS Limiter

## Custom Capes

The **Custom Capes** module lets you wear any PNG as a classic cape.

1. Put cape images (`.png`, ideally 64x32 — any other size is scaled automatically) into the `capes` folder next to your `config.json` (`<mod config dir>/capes`, created automatically on first launch along with a sample cape).
2. (Re)launch the game, open the BedrockToolsPlus mod menu and enable **Custom Capes**.
3. Pick a file in the module's **Cape** selector — the cape updates in-game immediately. Choose `None` to bring your vanilla cape back.

Images that are not exactly 64x32 are scaled onto the cape's outer back face (`x=1..11, y=1..17` of the 64x32 cape canvas); the inner front face gets a flat lining color instead of a repeat of the image, and the top/bottom/side edge strips pick up the image's edge colors so the cape keeps its visible thickness. Exact 64x32 images are used pixel-for-pixel with no processing.

The change is fully client-side and visual only; it does not affect servers, accounts, or other players. Persona skins are not affected (capes are persona pieces there).

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

## Free Look

The **Free Look** module decouples the camera from your body: while it is active, dragging the look controls turns only the camera, while your movement, attacks and the rotation other players see stay locked in the direction you were facing when you engaged it. Sprint straight ahead while looking around — like the free look known from Java clients.

How to use it:

- Enable the module, then set its **Keybind**. In the default **Hold Mode** Free Look is active while the key is held; turn **Hold Mode** off to make the key toggle it instead.
- On touch (or in addition to the key) there is a **Free Look** overlay button you can place anywhere, next to the Zoom button.
- **Max Yaw / Max Pitch** limit how far the camera may swing away from the locked direction (defaults 180°/90° = effectively unlimited).
- On release the camera glides back to your body direction (**Smooth Return**, **Return Speed**); turn **Smooth Return** off for an instant snap.

Implementation notes, for anyone extending it. On modern Bedrock the camera and the body are two separate things, and Free Look uses both:

- `LocalPlayer::applyTurnDelta` drives the **camera** (the new camera system); it does not touch the actor rotation. The module hooks it — the same hook Zoom uses, and the two chain, so Zoom's sensitivity scaling still applies — and lets the look input through untouched. It only trims a delta where the camera would swing past **Max Yaw / Max Pitch**, and zeroes it while a release animation owns the camera. The camera angle is tracked by summing the deltas that were allowed through (the argument is `{x = pitch, y = yaw}`, the same layout as the rotation component).
- The actor rotation component (`Actor::mActorRotationComponent`) is the **body**: the player model, the movement direction and the rotation that is sent to the server. The locked angle is written into it at `LocalPlayerPreTickEvent` (so the tick, the movement and the `MovePlayerPacket` use it) and again at `LocalPlayerTickEvent` (so the player model renders locked).
- On release the accumulated swing is undone with compensating deltas sent back through the original `applyTurnDelta` from the post-tick — one full step with **Smooth Return** off, an exponential lerp at **Return Speed** otherwise. The body is unlocked only once the camera has arrived back on it, so the two never separate permanently.

The yaw limit is accumulated incrementally rather than re-derived from the wrapped camera-minus-locked difference: that difference flips sign at 180°, so a flick into the limit would otherwise teleport the camera to the far side of the lock. The module also re-locks automatically on respawn or dimension change, dropping the swing instead of compensating into a camera the game has already reset.

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
