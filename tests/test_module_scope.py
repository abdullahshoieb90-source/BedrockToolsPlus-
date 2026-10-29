"""Guard the module scope against silently gaining/losing modules.

Run with: python3 -m unittest discover -s tests -p 'test_*.py'
"""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ModuleScopeTest(unittest.TestCase):
    def test_original_plus_registry_is_preserved(self):
        registry = (ROOT / "src/modules/ModuleRegistry.cpp").read_text()
        registered = re.findall(r"registry\.emplace<(\w+)>\(\)", registry)
        self.assertEqual(registered, [
            "HitSoundModule", "EffectDisplayModule", "HitboxModule",
            "BlockOutlineModule", "CustomCapesModule", "CommentKey",
            "CommandHotkeyModule", "CrosshairModule", "WingsModule",
            "HotbarSlotsModule", "InventoryHudModule", "ArmorModule",
            "ESPModule", "KillAuraModule",
        ])

    def test_only_original_plus_module_implementations_are_built(self):
        # xmake compiles every src/modules/**/*.cpp, even unregistered ones.
        actual = {
            str(p.relative_to(ROOT / "src/modules"))
            for p in (ROOT / "src/modules").rglob("*.cpp")
        }
        self.assertEqual(actual, {
            "ModuleRegistry.cpp", "misc/hitsound.cpp", "misc/commentkey.cpp",
            "misc/commandhotkey.cpp", "hud/effectdisplay.cpp",
            "hud/crosshair.cpp", "hud/hotbarslots.cpp", "hud/huditems.cpp",
            "hud/inventoryhud.cpp", "hud/armorhud.cpp", "visual/hitbox.cpp",
            "visual/blockoutline.cpp", "visual/wings.cpp", "visual/esp.cpp",
            "player/customcapes.cpp", "combat/killaura.cpp",
        })
        self.assertFalse((ROOT / "src/launcher/OverlayBridge.cpp").exists())
        runtime = (ROOT / "src/core/Runtime.cpp").read_text()
        self.assertNotIn("OverlayBridge", runtime)

    def test_compatibility_metadata_is_retained(self):
        manifest = json.loads((ROOT / "levimod.json").read_text())
        self.assertEqual(manifest["id"], "bedrocktoolsplus")
        self.assertEqual(manifest["version"], "1.6.0")
        self.assertEqual(manifest["minecraft_versions"], ["1.26.60.X"])
        version = (ROOT / "include/bedrocktools/Version.hpp").read_text()
        self.assertIn('Version = "1.6.0"', version)


if __name__ == "__main__":
    unittest.main()
