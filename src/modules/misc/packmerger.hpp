#pragma once

#include "../Module.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// Pack Merger
//
// Lets conflicting resource packs work together. Minecraft Bedrock resolves a
// resource file by walking the pack stack top-down and taking the FIRST pack
// that contains the path; two packs that both ship, say,
// `entity/player.entity.json` (a Java-animations pack and a Cape-Physics
// pack) can therefore never coexist — the lower pack's bindings silently
// vanish and it looks "broken".
//
// This module automates the fix with no manual file editing:
//
//   1. it reads the user's global pack stack
//      (`<com.mojang>/global_resource_packs.json`) and maps each entry to its
//      folder under `resource_packs/`;
//   2. every file path that exists in more than one of those packs and is a
//      `.json` or `.lang` file is deep-merged (top pack wins conflicts, keys
//      that only exist in lower packs are copied in, `scripts.animate` arrays
//      are unioned so every pack's animation bindings survive — see
//      packmerger_merge.hpp);
//   3. the merged copies are written into a small generated pack
//      `resource_packs/bedrocktoolsplus-merged/` with its own fixed UUID;
//   4. the generated pack is appended to the global stack (and, optionally,
//      to every world stack that uses one of the participating packs), which
//      puts it above the user's packs so the merged files win;
//   5. every other path still resolves through the user's own packs exactly
//      as before, so normal texture overrides keep their vanilla priority.
//
// Disabling the module (or "None"-equivalent states) removes the generated
// pack's entries from the stack files again; nothing the user installed is
// ever modified — only the generated pack folder and the stack JSONs are
// touched, and a `.btp-backup` copy of each modified stack file is kept.
//
// Threading: the scan/merge/write runs on this module's own worker thread
// (file IO over FUSE can take a while) and never on the game thread. The
// stack files are only ever rewritten atomically (temp file + rename) and
// only when their current content parses — a malformed stack file is left
// untouched.
class PackMergerModule : public Module {
public:
    PackMergerModule();
    ~PackMergerModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Menu-visible options (the launcher UI is generated from saveConfig).
    bool m_addToWorlds = true;     // also splice the merged pack into world stacks that use a participant pack
    bool m_rebuildNow = false;     // menu button state (never persisted true)
    std::string m_statusText = "Not built yet";

private:
    // Queue a scan+merge pass on the worker thread. `apply` selects whether
    // the generated pack's entry is added to (true) or removed from (false)
    // the pack stack files.
    void kickJob(bool apply);
    void workerLoop();
    void runMergeJob(bool apply);
    void setStatus(const std::string& text);

    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::thread m_worker;
    bool m_workerStarted = false;
    bool m_hasJob = false;
    bool m_pendingApply = false;
    bool m_stop = false;

    std::mutex m_statusMutex;
    std::string m_statusTextWorker; // written by the worker, read via saveConfig
};

extern PackMergerModule* g_packMerger;
