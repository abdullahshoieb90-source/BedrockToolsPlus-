// Host-side unit test for the Pack Merger merge engine.
//
// The Pack Merger module (src/modules/misc/packmerger.cpp) deep-merges the
// files that two or more enabled resource packs both ship, so packs that
// would normally shadow each other (Java Animations + Cape Physics editing
// the same entity/player.entity.json) work together. The merge engine in
// src/modules/misc/packmerger_merge.hpp is plain C++ so it can be verified
// without Minecraft. This test covers:
//
//   * the lenient JSON parser: strict documents, BOM, // and /* */ comments,
//     trailing commas, \u escapes (incl. surrogate pairs) and rejects broken
//     input instead of silently producing garbage
//   * the serializer: roundtrips parse(dump(x)) and escapes safely; integral
//     numbers print without a forced ".0"
//   * the deep merge: top pack wins conflicts, lower-pack-only keys are
//     copied in recursively, scripts.animate arrays are unioned with
//     de-duplication, and a 3-pack chain accumulates everything
//   * .lang merging: top file verbatim, unique lower keys appended
//   * path normalization + the skip list (manifests, icons, subpacks)
//   * buildMergePlan end-to-end over real folders: JSON/lang conflicts are
//     merged, binary conflicts and unique files are left to the stack,
//     subpacks are ignored, and unparseable files fall back to the top
//     pack's raw bytes (vanilla behavior for that file)
//
// Build and run standalone (no game, no third-party headers required):
//     g++ -std=c++20 -I src -I include tests/packmerger_merge_test.cpp -o /tmp/packmerger_merge_test
//     /tmp/packmerger_merge_test

#include "modules/misc/packmerger_merge.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace pm = bedrocktools::modules::packmerger;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

pm::JsonValue parseOk(const std::string& text) {
    pm::JsonParseResult r = pm::parseJsonLenient(text);
    if (!r.ok) {
        std::printf("  FAIL parse unexpectedly failed: %s (%s)\n", r.error.c_str(), text.c_str());
        ++g_failures;
        return pm::JsonValue{};
    }
    return std::move(r.value);
}

void writeAll(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

} // namespace

int main() {
    std::printf("== parser ==\n");

    {
        pm::JsonParseResult r = pm::parseJsonLenient(
            " { \"a\" : 1 , \"b\" : [true, false, null, \"s\", -1.5e2] } ");
        check(r.ok, "strict document parses");
        check(r.value.isObject() && r.value.find("a") && r.value.find("a")->isNumber() &&
                  r.value.find("a")->numberValue == 1.0,
              "object members accessible");
        const pm::JsonValue* b = r.value.find("b");
        check(b && b->isArray() && b->arrayValue.size() == 5, "array parsed");
        check(b && b->arrayValue[4].isNumber() && b->arrayValue[4].numberValue == -150.0,
              "exponent number parsed");
    }

    {
        std::string doc = "\xEF\xBB\xBF// leading comment\n{/* c1 */ \"a\": 1, /* mid */ \"b\": [1, 2,], }\n";
        pm::JsonParseResult r = pm::parseJsonLenient(doc);
        check(r.ok, "BOM + comments + trailing commas parse");
        check(r.ok && r.value.find("a") && r.value.find("a")->numberValue == 1.0 &&
                  r.value.find("b")->arrayValue.size() == 2,
              "lenient document content correct");
    }

    {
        pm::JsonParseResult r = pm::parseJsonLenient("\"\\u0041\\u00e9 \\ud83d\\ude00\\n\\t\\\\\"");
        check(r.ok && r.value.stringValue == "Aé \xF0\x9F\x98\x80\n\t\\",
              "string escapes incl. surrogate pair decode to UTF-8");
    }

    {
        check(!pm::parseJsonLenient("{'a':1}").ok, "single quotes rejected");
        check(!pm::parseJsonLenient("{\"a\":}").ok, "missing value rejected");
        check(!pm::parseJsonLenient("{").ok, "unterminated object rejected");
        check(!pm::parseJsonLenient("[1,2}").ok, "mismatched bracket rejected");
        check(!pm::parseJsonLenient("1 2").ok, "trailing garbage rejected");
        check(!pm::parseJsonLenient("").ok, "empty input rejected");
        check(!pm::parseJsonLenient("\xFF\xFE{\"a\":1}").ok, "UTF-16 BOM rejected with error");
    }

    std::printf("== serializer ==\n");

    {
        pm::JsonValue v = parseOk(
            "{\"s\":\"quote\\\" back\\\\ nl\\n ctrl\\u0001\",\"n\":2,\"f\":1.5,\"a\":[1,{\"b\":[]}],\"e\":{}}");
        std::string text = pm::dumpJson(v);
        pm::JsonValue back = parseOk(text);
        check(back.sameAs(v), "dump -> parse roundtrip is structurally equal");
        check(text.find("quote\\\" back\\\\ nl\\n ctrl\\u0001") != std::string::npos,
              "dangerous characters escaped");
        check(text.find("\"n\": 2") != std::string::npos && text.find("\"f\": 1.5") != std::string::npos,
              "integral numbers print without .0, fractions kept");
        check(text.find("\"a\": [") != std::string::npos, "arrays are indented");
    }

    std::printf("== deep merge ==\n");

    {
        pm::JsonValue top = parseOk(
            "{\"format_version\":\"1.10.0\","
            "\"animations\":{\"walk\":\"animation.player.walk\"},"
            "\"scripts\":{\"animate\":[\"walk\"]}}");
        pm::JsonValue lower = parseOk(
            "{\"format_version\":\"1.8.0\","
            "\"animations\":{\"cape\":\"animation.cape.move\",\"walk\":\"animation.other.walk\"},"
            "\"scripts\":{\"animate\":[\"cape\"]},"
            "\"extra\":true}");
        pm::mergeJsonInto(top, lower);

        check(top.find("format_version")->stringValue == "1.10.0", "top pack wins scalar conflicts");
        check(top.find("animations")->find("walk")->stringValue == "animation.player.walk",
              "top pack wins nested keys");
        check(top.find("animations")->find("cape") &&
                  top.find("animations")->find("cape")->stringValue == "animation.cape.move",
              "lower-pack-only animation copied in");
        check(top.find("scripts")->find("animate")->arrayValue.size() == 2 &&
                  top.find("scripts")->find("animate")->arrayValue[0].stringValue == "walk" &&
                  top.find("scripts")->find("animate")->arrayValue[1].stringValue == "cape",
              "scripts.animate arrays unioned (top first)");
        check(top.find("extra") && top.find("extra")->boolValue, "lower-pack-only key copied in");

        // Idempotence + union de-duplication.
        pm::mergeJsonInto(top, lower);
        check(top.find("scripts")->find("animate")->arrayValue.size() == 2,
              "union is de-duplicated on repeated merges");
    }

    {
        // Three-pack chain: every pack contributes its unique keys, the
        // highest pack still wins shared ones.
        pm::JsonValue base = parseOk("{\"a\":{\"x\":1,\"shared\":\"base\"},\"only_base\":1}");
        pm::JsonValue mid = parseOk("{\"a\":{\"y\":2,\"shared\":\"mid\"},\"only_mid\":2}");
        pm::JsonValue top = parseOk("{\"a\":{\"z\":3,\"shared\":\"top\"},\"only_top\":3}");
        pm::JsonValue acc = base;
        pm::mergeJsonInto(mid, acc);
        acc = std::move(mid);
        pm::mergeJsonInto(top, acc);
        acc = std::move(top);
        check(acc.find("only_base") && acc.find("only_mid") && acc.find("only_top"),
              "3-pack chain keeps every pack's unique keys");
        check(acc.find("a")->find("shared")->stringValue == "top" &&
                  acc.find("a")->find("x") && acc.find("a")->find("y") && acc.find("a")->find("z"),
              "3-pack chain conflict winner is the top pack");
    }

    std::printf("== lang merge ==\n");

    {
        std::string top = "a=Top A\nb=Top B\n# comment\nc=Top C\r\n";
        std::string lower = "b=Lower B\nd=Lower D\n\n[section]\ne=Lower E\n";
        pm::LangMergeResult r = pm::mergeLangText(top, lower);
        check(r.text.find("a=Top A") != std::string::npos, "lang keeps top lines");
        check(r.text.find("b=Top B") != std::string::npos &&
                  r.text.find("b=Lower B") == std::string::npos,
              "lang conflicts keep the top value only");
        check(r.text.find("d=Lower D") != std::string::npos && r.text.find("e=Lower E") != std::string::npos,
              "lang appends unique lower keys");
        check(r.addedKeys == 2, "lang reports added key count");
        check(pm::mergeLangText(top, top).addedKeys == 0, "lang merge with itself adds nothing");
    }

    std::printf("== paths ==\n");

    {
        check(pm::normalizeRelPath(".\\Entity\\Player.Entity.JSON") == "entity/player.entity.json",
              "backslashes + case normalized");
        check(pm::normalizeRelPath("a//b") == "a/b", "double slashes collapsed");
        check(pm::normalizeRelPath("./x.json") == "x.json", "leading ./ stripped");
        check(pm::normalizeRelPath("/abs/path.json") == "abs/path.json", "leading / stripped");
        check(pm::isJsonRelPath("entity/player.entity.json"), "json detection");
        check(pm::isLangRelPath("texts/en_US.lang"), "lang detection");
        check(!pm::isJsonRelPath("textures/a.png"), "non-json detection");
        check(pm::isSkippedRelPath("manifest.json") && pm::isSkippedRelPath("pack_icon.png") &&
                  pm::isSkippedRelPath("subpacks/high/entity/player.entity.json"),
              "skip list hits");
        check(!pm::isSkippedRelPath("entity/player.entity.json"), "skip list passes real content");
    }

    std::printf("== buildMergePlan end-to-end ==\n");

    {
        namespace fs = std::filesystem;
        fs::path root = fs::temp_directory_path() /
                        ("btp_packmerger_test_" + std::to_string(std::rand()));
        fs::create_directories(root);

        const fs::path low = root / "low_pack";
        const fs::path top = root / "top_pack";

        // Shared entity file: the exact Java-animations + Cape-Physics shape.
        writeAll(low / "entity" / "player.entity.json",
                 "{\"format_version\":\"1.10.0\","
                 "\"animations\":{\"cape\":\"animation.cape\"},"
                 "\"scripts\":{\"animate\":[\"cape\"]}}");
        writeAll(top / "entity" / "player.entity.json",
                 "{\"format_version\":\"1.10.0\","
                 "\"animations\":{\"walk\":\"animation.walk\"},"
                 "\"scripts\":{\"animate\":[\"walk\"]}}");

        // Shared lang file.
        writeAll(low / "texts" / "en_US.lang", "b=Low B\nc=Low C\n");
        writeAll(top / "texts" / "en_US.lang", "a=Top A\nb=Top B\n");

        // Shared binary: the stack must handle it (no merge attempt).
        writeAll(low / "textures" / "entity" / "cape.png", "PNGLOW");
        writeAll(top / "textures" / "entity" / "cape.png", "PNGTOP");

        // Unique files: no conflict, nothing to do.
        writeAll(low / "animations" / "cape.animation.json", "{\"animations\":{}}");
        writeAll(top / "render_controllers" / "x.json", "{\"render_controllers\":{}}");

        // Subpack copies are ignored entirely.
        writeAll(low / "subpacks" / "high" / "entity" / "player.entity.json", "{\"sub\":1}");
        writeAll(top / "subpacks" / "high" / "entity" / "player.entity.json", "{\"sub\":2}");

        // Both packs ship the same broken JSON: fall back to the top bytes.
        writeAll(low / "broken" / "thing.json", "{\"oops\"");
        writeAll(top / "broken" / "thing.json", "{\"oops\"");

        // Lower pack's copy is broken, top pack's is valid: fall back to top.
        writeAll(low / "entity" / "halfbroken.entity.json", "{not json at all");
        writeAll(top / "entity" / "halfbroken.entity.json", "{\"valid\":true}");

        std::vector<pm::PackFolder> packs = {
            {"Low Pack", low.generic_string(), "uuid-low"},
            {"Top Pack", top.generic_string(), "uuid-top"},
        };
        pm::MergePlan plan = pm::buildMergePlan(packs);

        check(plan.packNames.size() == 2 && plan.packNames[0] == "Low Pack" &&
                  plan.packNames[1] == "Top Pack",
              "plan records participating packs in stack order");

        int entityIdx = -1, langIdx = -1, brokenIdx = -1, halfIdx = -1;
        for (size_t i = 0; i < plan.files.size(); ++i) {
            if (plan.files[i].relPath == "entity/player.entity.json") entityIdx = static_cast<int>(i);
            if (plan.files[i].relPath == "texts/en_us.lang") langIdx = static_cast<int>(i);
            if (plan.files[i].relPath == "broken/thing.json") brokenIdx = static_cast<int>(i);
            if (plan.files[i].relPath == "entity/halfbroken.entity.json") halfIdx = static_cast<int>(i);
        }

        check(entityIdx >= 0, "entity conflict detected");
        if (entityIdx >= 0) {
            const pm::MergedFile& f = plan.files[entityIdx];
            pm::JsonParseResult parsed = pm::parseJsonLenient(f.content);
            check(parsed.ok, "merged entity parses");
            check(parsed.ok && parsed.value.find("animations")->find("walk") &&
                      parsed.value.find("animations")->find("cape"),
                  "merged entity carries both packs' animations");
            check(parsed.ok && parsed.value.find("scripts")->find("animate")->arrayValue.size() == 2,
                  "merged entity binds both packs' animate lists");
            check(f.topPack == 1 && f.mergedFrom == 2 && !f.copyOfTop,
                  "entity merge metadata correct");
        }

        check(langIdx >= 0, "lang conflict detected");
        if (langIdx >= 0) {
            const std::string& t = plan.files[langIdx].content;
            check(t.find("a=Top A") != std::string::npos && t.find("b=Top B") != std::string::npos &&
                      t.find("c=Low C") != std::string::npos,
                  "merged lang keeps top and adds unique lower keys");
        }

        check(brokenIdx >= 0 && plan.files[brokenIdx].copyOfTop &&
                  plan.files[brokenIdx].content == "{\"oops\"",
              "unparseable conflict falls back to top pack bytes");

        check(halfIdx >= 0 && plan.files[halfIdx].copyOfTop &&
                  plan.files[halfIdx].content == "{\"valid\":true}",
              "conflict with one broken side falls back to the valid top");

        bool binariesOrUniquesLeaked = false;
        for (const pm::MergedFile& f : plan.files) {
            if (f.relPath.find("cape.png") != std::string::npos ||
                f.relPath.find("cape.animation.json") != std::string::npos ||
                f.relPath.find("render_controllers") != std::string::npos ||
                f.relPath.find("subpacks") != std::string::npos) {
                binariesOrUniquesLeaked = true;
            }
        }
        check(!binariesOrUniquesLeaked,
              "binary conflicts, unique files and subpacks stay out of the plan");
        check(plan.files.size() == 4, "plan contains exactly the four conflicted files");
        check(plan.note.find("2 packs") != std::string::npos, "note summarizes the plan");

        std::error_code ec;
        fs::remove_all(root, ec);
        check(!ec, "temp workspace cleaned up");
    }

    std::printf("\n%s\n", g_failures == 0 ? "packmerger_merge_test: all checks passed"
                                          : "packmerger_merge_test: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
