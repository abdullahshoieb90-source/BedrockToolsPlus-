# BedrockToolsPlus 1.5.8 — Discord announcement

Copy one of the versions below into the `#plugins` **Pins** thread, then attach
`BedrockToolsPlus.levipack` to the message.

---

## Version 1 — short (same style as previous updates)

```text
New Update 1.5.8

• Added Jump Circle module
• Added Damage Circle module
• Support 26.60
• Fixed all known bugs and improved stability
```

---

## Version 2 — with a one-line description per new module

```text
New Update 1.5.8

• NEW — Jump Circle
  A soft effect on takeoff and landing (walking off a block counts too).
  13 shapes: Circle, Square, Star, Triangle, Diamond, Pentagon, Hexagon,
  Octagon, Four Point Star, Six Point Star, Heart, Flower, Gear.
  Effect Duration, Effect Color and Rainbow mode included.

• NEW — Damage Circle
  A ring appears on the entity you hit, then travels from the feet to the
  top of the hitbox and back down. Same 13 shapes, plus Effect Speed,
  Effect Size, Show Players / Show Mobs and Rainbow mode.
  Local-only: nothing is sent to the server.

• Support 26.60
• Fixed all known bugs and improved stability
• Added support 26.52 / 26.51 (for anyone still on older builds)
```

---

## Version 3 — full pinned post

```text
**BedrockToolsPlus — New Update 1.5.8**

Two brand-new modules and full support for Minecraft 1.26.60.

**▶ New module: Jump Circle**
Effect plays when you jump and when you land, including stepping off a block.
• 13 Effect Styles: Circle, Square, Star, Triangle, Diamond, Pentagon,
  Hexagon, Octagon, Four Point Star, Six Point Star, Heart, Flower, Gear
• Effect Duration (0.1–10s), Effect Color + opacity
• Rainbow mode with Rainbow Speed
• Persistent option keeps the effects visible instead of fading

**▶ New module: Damage Circle**
A ring marks every hit you land and travels up the hitbox, then back down.
• Same 13 Effect Styles
• Effect Duration, Effect Speed (0.25–4) and Effect Size (0.5–2.5)
• Effect Color + opacity, Rainbow mode with Rainbow Speed
• Show Players / Show Mobs toggles
• Your own damage effect is always shown; effects are local-only

**▶ Other changes**
• Support 26.60
• Fixed all known bugs and improved stability
• Existing modules unchanged — your config is kept

**▶ How to update**
1. Download BedrockToolsPlus.levipack below
2. Import it from LeviLauncher's mod manager (it replaces the old version)
3. Restart Minecraft through LeviLauncher

📦 BedrockToolsPlus.levipack
Minecraft Bedrock 1.26.60.X · Android arm64
```

---

## Notes

- Replace `26.60` / `1.26.60.X` with the exact build string you want to advertise if it changes.
- Keep the file name in the attachment as `BedrockToolsPlus.levipack` so the link preview looks the same as previous pins.
- Version bumped in `levimod.json`, `include/bedrocktools/Version.hpp` and `tests/test_module_scope.py` for this release.
