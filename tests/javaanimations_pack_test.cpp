// Host tests for the Java Animations third-person pack builder.
//
// The player model is posed by resource-pack animations, and a resource pack
// overrides a client entity by identifier *absolutely*: a partial
// player.entity.json drops every vanilla animation it does not list, which is
// why packs that ship one break swimming, attacking or the spear. So the
// builder has to emit a complete entity file. These tests pin exactly that:
// every vanilla binding is still present, every override points at an id the
// pack actually defines, and the manifest is a valid resources pack.
//
// Needs the real nlohmann_json headers (the module builds its JSON with them);
// the host fake in tests/fakejson only covers the config slice of the API.
// Point JSON_INCLUDE at them, or let scripts/run_tests.sh find the xmake cache.
//
// Build: g++ -std=c++20 -I src -I <nlohmann include> tests/javaanimations_pack_test.cpp -o /tmp/javaanimations_pack_test
// Run:   /tmp/javaanimations_pack_test

#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "modules/player/javaanimations_pack.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

const javaanim::pack::File* findFile(const std::vector<javaanim::pack::File>& files, const std::string& path) {
    for (const auto& file : files) {
        if (file.path == path) return &file;
    }
    return nullptr;
}

nlohmann::json parse(const std::string& text) {
    return nlohmann::json::parse(text);
}

}  // namespace

int main() {
    std::printf("Java Animations - third-person pack\n");

    const javaanim::pack::ModelSettings all;
    const auto files = javaanim::pack::buildPack(all);

    // --- the pack is complete ----------------------------------------------
    {
        check(files.size() == 3, "a pack is a manifest, an animation file and the player entity");
        check(findFile(files, "manifest.json") != nullptr, "manifest.json is written");
        check(findFile(files, "animations/java17_player.animation.json") != nullptr, "the animations are written");
        check(findFile(files, "entity/player.entity.json") != nullptr, "the player entity is written");
        bool allNonEmpty = true;
        for (const auto& file : files) {
            if (file.content.empty()) allNonEmpty = false;
        }
        check(allNonEmpty, "no file in the pack is empty");
    }

    // --- manifest -----------------------------------------------------------
    {
        const auto manifest = parse(findFile(files, "manifest.json")->content);
        check(manifest["format_version"].get<int>() == 2, "the manifest uses format version 2");
        const auto& header = manifest["header"];
        check(header["uuid"].get<std::string>() == javaanim::pack::kHeaderUuid, "the header uuid is stable");
        check(header["min_engine_version"].is_array() && header["min_engine_version"].size() == 3,
              "the manifest pins a min engine version");
        const auto& modules = manifest["modules"];
        check(modules.size() == 1 && modules[0]["type"].get<std::string>() == "resources",
              "the pack declares exactly one resources module");
        check(modules[0]["uuid"].get<std::string>() == javaanim::pack::kModuleUuid, "the module uuid is stable");
        check(header["uuid"].get<std::string>() != modules[0]["uuid"].get<std::string>(),
              "the two uuids differ (the game rejects a pack with duplicate uuids)");
    }

    // --- entity: nothing vanilla goes missing -------------------------------
    {
        const auto entity = parse(findFile(files, "entity/player.entity.json")->content);
        const auto& description = entity["minecraft:client_entity"]["description"];
        check(description["identifier"].get<std::string>() == "minecraft:player", "the pack targets the player");
        check(description.contains("geometry") && description.contains("textures") &&
                  description.contains("materials") && description.contains("render_controllers"),
              "the entity file keeps the geometry, textures, materials and render controllers");

        const auto& bindings = description["animations"];
        // The table has to stay complete: a missing row drops that vanilla
        // animation on the floor instead of reporting an error.
        check(javaanim::pack::vanillaBindings().size() == 72,
              "the binding table has all 72 vanilla rows");
        check(bindings.contains("root") && bindings["root"].get<std::string>() == "controller.animation.player.root",
              "the root controller is bound (without it the player does not animate at all)");
        check(bindings.contains("move.legs") && bindings.contains("swimming") && bindings.contains("crawling") &&
                  bindings.contains("riding.arms") && bindings.contains("sleeping") &&
                  bindings.contains("first_person_base_pose") && bindings.contains("base_controller") &&
                  bindings.contains("bow_and_arrow"),
              "the legs, swim, crawl, ride, sleep, first-person and controller rows survive");

        std::set<std::string> declared;
        for (const auto& entry : bindings.items()) declared.insert(entry.key());

        bool allPresent = true;
        int overridden = 0;
        for (const auto& entry : javaanim::pack::vanillaBindings()) {
            if (!declared.count(entry.first)) allPresent = false;
            if (bindings[entry.first].get<std::string>() != entry.second) ++overridden;
        }
        check(allPresent, "every vanilla animation binding is still listed (a partial file breaks the player)");
        check(declared.size() == javaanim::pack::vanillaBindings().size(),
              "the entity file lists no binding the vanilla table does not have");
        check(overridden == 5, "the default settings override five animations");

        const auto& scripts = description["scripts"];
        check(scripts["animate"].is_array() && scripts["animate"].size() == 1 &&
                  scripts["animate"][0].get<std::string>() == "root",
              "the vanilla root controller still drives everything");
        const std::string pre = scripts["pre_animation"].dump();
        check(pre.find("variable.tcos0") != std::string::npos,
              "the pre-animation still computes the arm-swing variable our Molang reads");
        check(pre.find("variable.attack_body_rot_y") != std::string::npos,
              "the pre-animation still computes the attack body rotation");
    }

    // --- entity: no dangling references --------------------------------------
    {
        const auto animations = parse(findFile(files, "animations/java17_player.animation.json")->content);
        const auto entity = parse(findFile(files, "entity/player.entity.json")->content);
        const auto& bindings = entity["minecraft:client_entity"]["description"]["animations"];
        const auto& defined = animations["animations"];

        bool dangling = false;
        for (const auto& entry : bindings.items()) {
            const std::string id = entry.value().get<std::string>();
            if (id.rfind("animation.java17.", 0) != 0) continue;
            if (!defined.contains(id)) dangling = true;
        }
        check(!dangling, "every Java animation the entity points at is defined in the pack");

        bool orphan = false;
        for (const auto& entry : defined.items()) {
            bool referenced = false;
            for (const auto& binding : bindings.items()) {
                if (binding.value().get<std::string>() == entry.key()) referenced = true;
            }
            if (!referenced) orphan = true;
        }
        check(!orphan, "the pack defines no animation the entity never uses");

        bool malformed = false;
        for (const auto& entry : defined.items()) {
            if (!entry.value().contains("bones") || !entry.value()["bones"].is_object()) malformed = true;
        }
        check(!malformed, "every animation has a bones object");
    }

    // --- toggles --------------------------------------------------------------
    {
        javaanim::pack::ModelSettings none;
        none.moveArms = false;
        none.attack = false;
        none.bob = false;
        none.sneak = false;
        none.useItem = false;
        none.holding = false;
        const auto vanilla = javaanim::pack::buildPack(none);
        const auto entity = parse(findFile(vanilla, "entity/player.entity.json")->content);
        const auto& bindings = entity["minecraft:client_entity"]["description"]["animations"];

        bool identical = true;
        for (const auto& entry : javaanim::pack::vanillaBindings()) {
            if (bindings[entry.first].get<std::string>() != entry.second) identical = false;
        }
        check(identical, "with every animation off the pack is a byte-for-byte vanilla binding table");
        check(parse(findFile(vanilla, "animations/java17_player.animation.json")->content)["animations"].empty(),
              "with every animation off the animation file is empty");

        javaanim::pack::ModelSettings attackOnly;
        attackOnly.moveArms = false;
        attackOnly.bob = false;
        attackOnly.sneak = false;
        attackOnly.useItem = false;
        attackOnly.holding = false;
        const auto single = javaanim::pack::buildPack(attackOnly);
        const auto singleEntity = parse(findFile(single, "entity/player.entity.json")->content);
        const auto& singleBindings = singleEntity["minecraft:client_entity"]["description"]["animations"];
        check(singleBindings["attack.rotations"].get<std::string>() == javaanim::pack::kAnimAttack,
              "the attack override can be enabled on its own");
        check(singleBindings["move.arms"].get<std::string>() == "animation.player.move.arms",
              "the animations left off keep their vanilla binding");
        check(parse(findFile(single, "animations/java17_player.animation.json")->content)["animations"].size() == 1,
              "only the enabled animation is emitted");
    }

    // --- the 1.7 content -------------------------------------------------------
    {
        const auto animations = parse(findFile(files, "animations/java17_player.animation.json")->content);
        const auto& defined = animations["animations"];

        const auto attack = defined[javaanim::pack::kAnimAttack]["bones"]["rightarm"]["rotation"];
        check(attack[0].get<std::string>().find("68.75") != std::string::npos,
              "the 1.7 attack lifts the arm by the Java 1.7 angle (1.2 rad), not Bedrock's 30 degrees");
        check(attack[2].get<std::string>().find("-22.9") != std::string::npos,
              "the 1.7 attack rolls the arm like Java 1.7 (0.4 rad)");

        const auto& bob = defined[javaanim::pack::kAnimBob]["bones"];
        check(bob.contains("body") && !bob.contains("rightarm"),
              "the 1.7 idle breathes with the body and leaves the arms to hang straight");

        const auto& sneak = defined[javaanim::pack::kAnimSneak]["bones"];
        check(sneak["rightarm"]["rotation"][0].get<double>() == -2.0,
              "the 1.7 crouch keeps the arms almost straight (Bedrock pushes them to -5.7)");

        const auto& use = defined[javaanim::pack::kAnimUseItem]["bones"];
        check(use["rightarm"]["rotation"][0].get<std::string>().find("-95.0") != std::string::npos,
              "the 1.7 eat/drink lifts the arm to the mouth (Bedrock lifts 60 degrees)");

        const auto& move = defined[javaanim::pack::kAnimMoveArms]["bones"];
        check(move["rightarm"]["rotation"][0].get<std::string>() == "-variable.tcos0 * 1.15",
              "the 1.7 walk swings the arms a little wider than Bedrock");
    }

    // --- determinism and the default install path -------------------------------
    {
        const auto again = javaanim::pack::buildPack(all);
        bool stable = again.size() == files.size();
        for (std::size_t i = 0; i < files.size() && stable; ++i) {
            if (again[i].path != files[i].path || again[i].content != files[i].content) stable = false;
        }
        check(stable, "building the same pack twice produces identical files");

        const std::string path = javaanim::pack::defaultPackPath();
        check(path.rfind("/sdcard/games/com.mojang/resource_packs/", 0) == 0,
              "the default install folder is Minecraft's own resource-pack folder");
        check(path.find(javaanim::pack::kPackFolder) != std::string::npos,
              "the default install folder is named after the pack");
    }

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
