// Host tests for Custom Capes discovery and launcher/config round-trips.
#include "modules/visual/customcapes_files.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    std::printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}
}

int main() {
    using namespace customcapes;
    const std::filesystem::path dir = "/tmp/bt-custom-capes-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "zeta.png") << "png";
    std::ofstream(dir / "Alpha.PNG") << "png";
    std::ofstream(dir / "notes.txt") << "ignored";
    std::ofstream(dir / "bad,name.png") << "ignored";
    std::filesystem::create_directory(dir / "directory.png");

    const auto files = scanCapeFiles(dir.string());
    check(files.size() == 2, "only usable PNG files are listed");
    check(files.size() == 2 && files[0] == "Alpha.PNG" && files[1] == "zeta.png",
          "capes are sorted case-insensitively");
    check(hasPngExtension("cape.PnG"), "PNG extension is case-insensitive");
    check(!hasPngExtension("cape.png.txt"), "a PNG suffix in the middle is rejected");
    check(!isUsableCapeFileName("../cape.png"), "path traversal names are rejected");

    const std::string radio = makeRadioValue(2, files);
    check(radio == "2,Default Cape,No Cape,Alpha.PNG,zeta.png", "radio contains built-ins and every file");
    Selection selected = parseSelection(radio, files);
    check(selected.index == 2 && selected.id == "Alpha.PNG", "full radio selects the named PNG");
    selected = parseSelection("3", files);
    check(selected.index == 3 && selected.id == "zeta.png", "launcher bare index selects a PNG");
    selected = parseSelection("1", files);
    check(selected.index == 1 && selected.id == kNoneId, "No Cape survives a round-trip");
    selected = parseSelection("0", files);
    check(selected.index == 0 && selected.id == kDefaultId, "Default Cape survives a round-trip");
    selected = parseSelection("zeta.png", files);
    check(selected.index == 3 && selected.id == "zeta.png", "a hand-edited bare filename is accepted");

    const auto reordered = std::vector<std::string>{"a-new-cape.png", "Alpha.PNG", "zeta.png"};
    selected = parseSelection("2,Default Cape,No Cape,Alpha.PNG,zeta.png", reordered);
    check(selected.index == 3 && selected.id == "Alpha.PNG", "embedded name survives alphabetical reordering");
    selected = parseSelection("9,Default Cape,No Cape,Alpha.PNG,zeta.png", files);
    check(selected.index == 0, "an out-of-range selection safely falls back to vanilla");
    selected = parseSelection("deleted.png", files);
    check(selected.index == 0, "a deleted cape safely falls back to vanilla");

    std::filesystem::remove_all(dir);
    if (failures) {
        std::printf("%d custom capes check(s) failed\n", failures);
        return 1;
    }
    std::printf("all custom capes checks passed\n");
    return 0;
}
