#include "packmerger.hpp"
#include "packmerger_merge.hpp"

#include "modules/ModuleRegistry.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

PackMergerModule* g_packMerger = nullptr;

namespace {

using namespace bedrocktools::modules::packmerger;

constexpr const char* kMergedFolderName = "bedrocktoolsplus-merged";
// Fixed identity of the generated pack. Hardcoded (instead of random) so the
// module can find and replace its own stack entries across sessions.
constexpr const char* kMergedHeaderUuid = "c1f4a8e2-6b3d-4f97-9a52-8d0e71c4b3a6";
constexpr const char* kMergedModuleUuid = "9e6d2b7a-41c8-4e05-b3fa-27d9c815e4f1";

// Known locations of the com.mojang directory on Android. The mod runs inside
// the Minecraft process (see Runtime::launcherContext), so the game's own
// external files dir is readable and writable here. Newer Android versions
// scope the game into Android/data; older installs and some launchers use the
// legacy top-level games folder.
std::vector<std::string> comMojangCandidates() {
    return {
        "/storage/emulated/0/Android/data/com.mojang.minecraftpe/files/games/com.mojang",
        "/sdcard/Android/data/com.mojang.minecraftpe/files/games/com.mojang",
        "/storage/emulated/0/games/com.mojang",
        "/sdcard/games/com.mojang",
    };
}

std::string findComMojangDir() {
    std::string firstExisting;
    for (const std::string& candidate : comMojangCandidates()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(candidate, ec) || ec) continue;
        std::string withPacks = candidate + "/resource_packs";
        if (std::filesystem::is_directory(withPacks, ec) && !ec) return candidate;
        if (firstExisting.empty()) firstExisting = candidate;
    }
    return firstExisting;
}

bool readTextFile(const std::filesystem::path& path, std::string& out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeTextFileAtomically(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::path temp = path;
    temp += ".btp-tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        // Some filesystems refuse rename-over-existing; fall back to a
        // direct replace (window is tiny and the file is recreated fully).
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    return true;
}

// Writes only when the content on disk differs, to keep repeated launches
// from churning the pack folder (and to keep the game's file watchers calm).
bool writeTextFileIfChanged(const std::filesystem::path& path, const std::string& content) {
    std::string existing;
    if (readTextFile(path, existing) && existing == content) return true;
    return writeTextFileAtomically(path, content);
}

bool writeBinaryFileIfChanged(const std::filesystem::path& path, const std::string& content) {
    return writeTextFileIfChanged(path, content); // same bytes-in, bytes-out path
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct PackManifestInfo {
    std::string name;
    std::string uuid;
    JsonValue minEngineVersion; // array or null
};

PackManifestInfo readPackManifest(const std::string& root) {
    PackManifestInfo info;
    std::string text;
    if (!readTextFile(std::filesystem::path(root) / "manifest.json", text)) return info;
    JsonParseResult parsed = parseJsonLenient(text);
    if (!parsed.ok) return info;
    const JsonValue* header = parsed.value.find("header");
    if (!header) return info;
    if (const JsonValue* name = header->find("name"); name && name->isString()) {
        info.name = name->stringValue;
    }
    if (const JsonValue* uuid = header->find("uuid"); uuid && uuid->isString()) {
        info.uuid = toLower(uuid->stringValue);
    }
    if (const JsonValue* minEngine = header->find("min_engine_version");
        minEngine && minEngine->isArray()) {
        info.minEngineVersion = *minEngine;
    }
    return info;
}

// --- pack stack files ------------------------------------------------------

// Reads a `world_resource_packs.json` / `global_resource_packs.json` style
// file into an ordered list of lowercase pack uuids (lowest priority first).
// Returns false when the file exists but cannot be parsed (the caller must
// then never rewrite it).
bool readStackFile(const std::filesystem::path& path, std::vector<std::string>& ids) {
    std::string text;
    if (!readTextFile(path, text)) return true; // missing file == empty stack
    JsonParseResult parsed = parseJsonLenient(text);
    if (!parsed.ok || !parsed.value.isArray()) return false;
    for (const JsonValue& entry : parsed.value.arrayValue) {
        if (const JsonValue* id = entry.find("pack_id"); id && id->isString()) {
            ids.push_back(toLower(id->stringValue));
        }
    }
    return true;
}

// Adds or removes `{ pack_id, version }` for one uuid, preserving every other
// entry and its order. New entries are APPENDED: stack entries are applied in
// array order and the last one overrides the earlier ones, so appending puts
// the generated pack above the user's packs. Returns:
//   0 = nothing to do / file untouched, 1 = written, -1 = error (untouched).
int upsertStackEntry(const std::filesystem::path& path, const char* uuid, bool add) {
    std::string text;
    bool exists = readTextFile(path, text);
    if (!exists && !add) return 0;

    JsonValue arr = JsonValue::makeArray();
    if (exists) {
        JsonParseResult parsed = parseJsonLenient(text);
        if (!parsed.ok || !parsed.value.isArray()) return -1; // never break a foreign file
        arr = std::move(parsed.value);
    }

    JsonValue filtered = JsonValue::makeArray();
    for (JsonValue& entry : arr.arrayValue) {
        const JsonValue* id = entry.find("pack_id");
        const bool ours = id && id->isString() && toLower(id->stringValue) == uuid;
        if (!ours) filtered.arrayValue.push_back(std::move(entry));
    }
    if (add) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("pack_id", JsonValue::makeString(uuid));
        JsonValue version = JsonValue::makeArray();
        version.push(JsonValue::makeNumber(1));
        version.push(JsonValue::makeNumber(0));
        version.push(JsonValue::makeNumber(0));
        entry.set("version", std::move(version));
        filtered.arrayValue.push_back(std::move(entry));
    }
    if (filtered.arrayValue.size() == arr.arrayValue.size() && !add) return 0; // unchanged
    if (filtered.arrayValue.size() == arr.arrayValue.size()) {
        // same size: only a rewrite when an entry actually moved in
        if (dumpJson(filtered) == dumpJson(arr)) return 0;
    }

    // Keep a one-time backup of each stack file before the first modification.
    std::error_code ec;
    std::filesystem::path backup = path;
    backup += ".btp-backup";
    if (!std::filesystem::exists(backup, ec)) {
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::none, ec);
    }

    return writeTextFileAtomically(path, dumpJson(filtered)) ? 1 : -1;
}

} // namespace

PackMergerModule::PackMergerModule()
    : Module("Pack Merger",
             "Lets conflicting resource packs work together: deep-merges shared entity/animation/controller JSON and .lang files "
             "between your enabled global resource packs into one generated pack, so combos like Java Animations + Cape Physics "
             "both work. Never modifies your packs; restart Minecraft after the first enable.") {
    g_packMerger = this;
    showInMenu = true;
    hideInHudEditor = true; // background file utility, not HUD
}

PackMergerModule::~PackMergerModule() {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_stop = true;
    }
    m_jobCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    if (g_packMerger == this) g_packMerger = nullptr;
}

void PackMergerModule::onInit() {
    // The enabled state (restored from config) drives the first job through
    // onEnable/onDisable; nothing to hook into the game here.
}

void PackMergerModule::onEnable() {
    kickJob(true);
}

void PackMergerModule::onDisable() {
    // Remove the generated pack from the stacks again (the worker keeps the
    // merged folder on disk; only stack membership changes).
    kickJob(false);
}

void PackMergerModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("addToWorlds")) m_addToWorlds = j["addToWorlds"].get<bool>();
    if (j.contains("statusText") && j["statusText"].is_string()) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_statusTextWorker = j["statusText"].get<std::string>();
    }
    if (j.contains("rebuildNowButton") && j["rebuildNowButton"].is_boolean() &&
        j["rebuildNowButton"].get<bool>()) {
        m_rebuildNow = false; // it is a button press, not a stored state
        kickJob(enabled);
    }
}

void PackMergerModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["addToWorlds"] = m_addToWorlds;
    j["rebuildNowButton"] = false; // always render the menu button unpressed
    std::lock_guard<std::mutex> lock(m_statusMutex);
    j["statusText"] = m_statusTextWorker;
}

void PackMergerModule::setStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_statusTextWorker = text;
}

void PackMergerModule::kickJob(bool apply) {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (m_stop) return;
        m_pendingApply = apply;
        m_hasJob = true;
        if (!m_workerStarted) {
            m_workerStarted = true;
            m_worker = std::thread(&PackMergerModule::workerLoop, this);
        }
    }
    m_jobCv.notify_all();
}

void PackMergerModule::workerLoop() {
    std::unique_lock<std::mutex> lock(m_jobMutex);
    while (true) {
        m_jobCv.wait(lock, [this] { return m_stop || m_hasJob; });
        if (m_stop) return;
        const bool apply = m_pendingApply;
        m_pendingApply = false;
        m_hasJob = false;
        lock.unlock();

        runMergeJob(apply);

        lock.lock();
    }
}

void PackMergerModule::runMergeJob(bool apply) {
    if (m_stop) return;

    const std::string mojang = findComMojangDir();
    if (mojang.empty()) {
        setStatus("com.mojang folder not found (install Minecraft / check storage)");
        return;
    }
    if (m_stop) return;

    const std::filesystem::path resourcePacksDir =
        std::filesystem::path(mojang) / "resource_packs";

    // Enumerate installed packs and index them by uuid.
    std::vector<PackFolder> allPacks;
    {
        std::error_code ec;
        std::filesystem::directory_iterator it(resourcePacksDir, ec);
        std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            if (it->is_directory()) {
                PackManifestInfo manifest = readPackManifest(it->path().generic_string());
                PackFolder folder;
                folder.root = it->path().generic_string();
                folder.name = manifest.name.empty() ? it->path().filename().generic_string()
                                                    : manifest.name;
                folder.uuid = manifest.uuid;
                allPacks.push_back(std::move(folder));
            }
            it.increment(ec);
        }
    }

    // Ordered global stack (lowest priority first).
    const std::filesystem::path globalStackPath =
        std::filesystem::path(mojang) / "global_resource_packs.json";
    std::vector<std::string> stackIds;
    if (!readStackFile(globalStackPath, stackIds)) {
        setStatus("global_resource_packs.json is not valid JSON; left untouched");
        return;
    }

    // Participants: enabled packs that exist on disk (the generated pack
    // itself is never a participant).
    std::vector<PackFolder> participants;
    {
        std::vector<bool> used(allPacks.size(), false);
        for (const std::string& id : stackIds) {
            if (id == kMergedHeaderUuid) continue;
            for (size_t i = 0; i < allPacks.size(); ++i) {
                if (!used[i] && allPacks[i].uuid == id) {
                    used[i] = true;
                    participants.push_back(allPacks[i]);
                    break;
                }
            }
        }
    }

    if (m_stop) return;

    const std::filesystem::path mergedDir = resourcePacksDir / kMergedFolderName;

    if (participants.size() < 2) {
        // Nothing to merge; make sure we are not polluting the stacks.
        upsertStackEntry(globalStackPath, kMergedHeaderUuid, false);
        setStatus(participants.empty()
                      ? "No resource packs enabled globally; nothing to merge"
                      : "Only 1 pack enabled globally; nothing to merge");
        return;
    }

    MergePlan plan = buildMergePlan(participants);
    if (m_stop) return;

    if (plan.files.empty()) {
        upsertStackEntry(globalStackPath, kMergedHeaderUuid, false);
        setStatus("Packs have no conflicting files; the normal stack already "
                  "handles them (" + plan.note + ")");
        return;
    }

    // Materialize the generated pack.
    std::error_code ec;
    std::filesystem::create_directories(mergedDir, ec);
    if (ec) {
        setStatus("Cannot create " + mergedDir.generic_string());
        return;
    }

    JsonValue manifest = JsonValue::makeObject();
    manifest.set("format_version", JsonValue::makeNumber(2));
    {
        JsonValue header = JsonValue::makeObject();
        header.set("name", JsonValue::makeString("BTP Merged Packs"));
        header.set("description",
                   JsonValue::makeString("Auto-generated by BedrockToolsPlus Pack Merger: " +
                                         std::to_string(participants.size()) +
                                         " packs deep-merged. Do not edit; disable the module to remove."));
        header.set("uuid", JsonValue::makeString(kMergedHeaderUuid));
        JsonValue version = JsonValue::makeArray();
        version.push(JsonValue::makeNumber(1));
        version.push(JsonValue::makeNumber(0));
        version.push(JsonValue::makeNumber(0));
        header.set("version", std::move(version));
        // Inherit the top pack's engine requirement so the game never treats
        // the merged pack as newer than the packs it was built from.
        const PackFolder& topPack = participants.back();
        PackManifestInfo topManifest = readPackManifest(topPack.root);
        if (topManifest.minEngineVersion.isArray()) {
            header.set("min_engine_version", topManifest.minEngineVersion);
        } else {
            JsonValue minEngine = JsonValue::makeArray();
            minEngine.push(JsonValue::makeNumber(1));
            minEngine.push(JsonValue::makeNumber(16));
            minEngine.push(JsonValue::makeNumber(0));
            header.set("min_engine_version", std::move(minEngine));
        }
        manifest.set("header", std::move(header));
    }
    {
        JsonValue modules = JsonValue::makeArray();
        JsonValue module = JsonValue::makeObject();
        module.set("type", JsonValue::makeString("resources"));
        module.set("uuid", JsonValue::makeString(kMergedModuleUuid));
        JsonValue version = JsonValue::makeArray();
        version.push(JsonValue::makeNumber(1));
        version.push(JsonValue::makeNumber(0));
        version.push(JsonValue::makeNumber(0));
        module.set("version", std::move(version));
        modules.push(std::move(module));
        manifest.set("modules", std::move(modules));
    }
    if (!writeTextFileIfChanged(mergedDir / "manifest.json", dumpJson(manifest))) {
        setStatus("Cannot write the merged pack manifest");
        return;
    }

    int written = 0;
    for (const MergedFile& file : plan.files) {
        if (m_stop) return;
        std::filesystem::path target = mergedDir / std::filesystem::path(file.relPath);
        std::filesystem::path parent = target.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) continue;
        }
        if (writeTextFileIfChanged(target, file.content)) ++written;
    }

    // Cosmetic: reuse the top pack's icon so the generated pack is
    // recognizable in the game's pack list.
    const std::filesystem::path iconTarget = mergedDir / "pack_icon.png";
    if (!std::filesystem::exists(iconTarget, ec)) {
        std::string icon;
        if (readTextFile(std::filesystem::path(participants.back().root) / "pack_icon.png", icon)) {
            writeBinaryFileIfChanged(iconTarget, icon);
        }
    }

    // Stack membership follows the module state.
    int stackResult = upsertStackEntry(globalStackPath, kMergedHeaderUuid, apply);

    int worldCount = 0;
    if (apply && m_addToWorlds) {
        const std::filesystem::path worldsDir = std::filesystem::path(mojang) / "worlds";
        std::filesystem::directory_iterator it(worldsDir, ec);
        std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            std::error_code innerEc;
            if (it->is_directory(innerEc) && !innerEc) {
                std::filesystem::path stackFile = it->path() / "world_resource_packs.json";
                std::vector<std::string> ids;
                if (readStackFile(stackFile, ids)) {
                    bool usesParticipant = false;
                    bool usesOurs = false;
                    for (const std::string& id : ids) {
                        if (id == kMergedHeaderUuid) usesOurs = true;
                        for (const PackFolder& pack : participants) {
                            if (pack.uuid == id) { usesParticipant = true; break; }
                        }
                    }
                    // World stacks sit ABOVE the global stack: a world that
                    // uses any participating pack must also carry the merged
                    // pack, or its own copy would win over the merged files.
                    if (usesParticipant || usesOurs) {
                        if (upsertStackEntry(stackFile, kMergedHeaderUuid, usesParticipant) == 1) {
                            ++worldCount;
                        }
                    }
                }
            }
            it.increment(ec);
        }
    } else if (!apply) {
        // Also clean world entries when the module is off.
        const std::filesystem::path worldsDir = std::filesystem::path(mojang) / "worlds";
        std::filesystem::directory_iterator it(worldsDir, ec);
        std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            std::error_code innerEc;
            if (it->is_directory(innerEc) && !innerEc) {
                std::filesystem::path stackFile = it->path() / "world_resource_packs.json";
                if (upsertStackEntry(stackFile, kMergedHeaderUuid, false) == 1) ++worldCount;
            }
            it.increment(ec);
        }
    }

    if (stackResult < 0) {
        setStatus("Merged pack built (" + std::to_string(written) +
                  " file(s)) but global_resource_packs.json could not be updated; enable the pack manually");
        return;
    }

    std::string status = apply ? "OK: " : "OFF: ";
    status += plan.note;
    if (apply) {
        status += ". Restart Minecraft to apply";
        if (worldCount > 0) status += " (updated " + std::to_string(worldCount) + " world(s))";
        status += ".";
    } else {
        status += ". Pack removed from the stacks.";
    }
    setStatus(status);
}
