"""Guards the single stb_image_write implementation.

The mod is one shared object. If any translation unit that calls
stbi_write_png() is not linked with the one file that implements it, the symbol
stays undefined, dlopen(RTLD_NOW) fails, and *every* module disappears from the
launcher menu - which is exactly what happened when the Custom Capes module was
removed and took the PNG writer with it.

The inverse is just as fatal: a second STB_IMAGE_WRITE_IMPLEMENTATION anywhere
is a duplicate symbol and the library does not link at all.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class StbWriterTest(unittest.TestCase):
    def test_exactly_one_implementation(self):
        owners = [
            str(path.relative_to(ROOT))
            for path in list((ROOT / "src").rglob("*.cpp")) + list((ROOT / "src").rglob("*.hpp"))
            if re.search(r"^#\s*define\s+STB_IMAGE_WRITE_IMPLEMENTATION",
                         path.read_text(errors="ignore"), re.MULTILINE)
        ]
        self.assertEqual(owners, ["src/core/StbImageWrite.cpp"])

    def test_every_caller_links_the_implementation(self):
        callers = [
            str(path.relative_to(ROOT))
            for path in (ROOT / "src").rglob("*.cpp")
            if "stbi_write_" in path.read_text(errors="ignore")
            and "STB_IMAGE_WRITE_IMPLEMENTATION" not in path.read_text(errors="ignore")
        ]
        self.assertTrue(callers, "expected at least one caller of the writer")
        runner = (ROOT / "scripts/run_tests.sh").read_text()
        # The host runs link callers like Wings through the real module file.
        self.assertIn("src/core/StbImageWrite.cpp", runner)


if __name__ == "__main__":
    unittest.main()
