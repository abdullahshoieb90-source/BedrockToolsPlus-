#include "debugscan.hpp"

#include <android/log.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace {

constexpr const char* kTag = "BTScan";
constexpr std::size_t kMaxHits = 24;

struct Segment {
    std::uintptr_t vaddr = 0;   // relative to base (absolute start when stored in `loads`)
    std::uintptr_t filesz = 0;  // mapping size
    std::uint32_t flags = 0;    // ELF PF_R=4 / PF_W=2 / PF_X=1
};

// True when [addr, addr+4) lies entirely inside one of the mapped segments.
bool wordInSegments(const std::vector<Segment>& loads, std::uintptr_t base,
                    std::uintptr_t addr) {
    for (const auto& seg : loads) {
        const std::uintptr_t start = base + seg.vaddr;
        if (addr >= start && addr + 4 <= start + seg.filesz) return true;
    }
    return false;
}

// memmem does not exist in bionic, so locate `needle` manually.
const std::uint8_t* findBytes(const std::uint8_t* hay, std::size_t hayLen,
                              const char* needle, std::size_t needleLen) {
    if (needleLen == 0 || hayLen < needleLen) return nullptr;
    const std::uint8_t* end = hay + hayLen - needleLen;
    for (const std::uint8_t* p = hay; p <= end; ++p) {
        if (p[0] != static_cast<std::uint8_t>(needle[0])) continue;
        if (std::memcmp(p, needle, needleLen) == 0) return p;
    }
    return nullptr;
}

std::vector<std::uintptr_t> findOccurrences(const std::uint8_t* base,
                                            const std::vector<Segment>& segments,
                                            const char* needle) {
    std::vector<std::uintptr_t> hits;
    const std::size_t len = std::strlen(needle);
    for (const auto& seg : segments) {
        const std::uint8_t* hay = base + seg.vaddr;
        const std::size_t size = seg.filesz;
        const std::uint8_t* cursor = hay;
        const std::uint8_t* end = hay + size;
        while (cursor < end && hits.size() < kMaxHits) {
            const std::uint8_t* found = findBytes(cursor, static_cast<std::size_t>(end - cursor), needle, len);
            if (!found) break;
            hits.push_back(reinterpret_cast<std::uintptr_t>(found) - reinterpret_cast<std::uintptr_t>(base));
            cursor = found + 1;
        }
    }
    return hits;
}

// Every dump goes through this bounded reader — no read may cross a mapping end.
void dumpHex(const std::vector<Segment>& loads, std::uintptr_t base,
             std::uintptr_t addr, int words) {
    for (int i = 0; i < words; ++i) {
        const std::uintptr_t at = addr + 4 * static_cast<std::uintptr_t>(i);
        if (!wordInSegments(loads, base, at)) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "     +0x%02x: <out of range>", i * 4);
            break;
        }
        const std::uint32_t insn = *reinterpret_cast<const volatile std::uint32_t*>(at);
        __android_log_print(ANDROID_LOG_INFO, kTag, "     +0x%02x: %08x", i * 4, insn);
    }
}

bool isPrologue(std::uint32_t insn) {
    return insn == 0xD503237Fu  // paciasp
        || insn == 0xD50317BFu  // pacibsp
        || insn == 0xD503245Fu  // bti c
        || insn == 0xD503249Fu; // bti j
}

void scanText(const std::vector<Segment>& loads, std::uintptr_t base,
              std::uintptr_t target) {
    int found = 0;
    for (const auto& seg : loads) {
        if ((seg.flags & 1u) == 0) continue;  // executable mappings only
        const std::uintptr_t segStart = base + seg.vaddr;
        const std::uintptr_t segEnd = segStart + seg.filesz;
        for (std::uintptr_t pc = segStart; pc + 4 <= segEnd && found < 12; pc += 4) {
            const std::uint32_t insn = *reinterpret_cast<const volatile std::uint32_t*>(pc);
            if ((insn & 0x9F000000u) != 0x90000000u) continue; // ADRP
            const std::int64_t immhi = static_cast<std::int64_t>((insn >> 5) & 0x7FFFF);
            const std::int64_t immlo = static_cast<std::int64_t>((insn >> 29) & 0x3);
            std::int64_t imm21 = (immhi << 2) | immlo;
            if (imm21 & (1 << 20)) imm21 |= ~((static_cast<std::int64_t>(1) << 21) - 1);
            const std::uintptr_t page = (pc & ~0xFFFULL) + static_cast<std::uintptr_t>(imm21 << 12);
            const std::uint32_t rd = insn & 0x1F;
            for (int j = 1; j <= 8; ++j) {
                const std::uintptr_t at = pc + 4 * static_cast<std::uintptr_t>(j);
                if (at + 4 > segEnd) break;  // never leave the mapping
                const std::uint32_t add = *reinterpret_cast<const volatile std::uint32_t*>(at);
                if ((add & 0x7F800000u) != 0x11000000u) continue;             // ADD (64-bit imm)
                if (((add >> 5) & 0x1F) != rd || (add & 0x1F) != rd) continue; // ADD Xd, Xd, #imm
                std::uint32_t imm12 = (add >> 10) & 0xFFF;
                if (add & (1u << 22)) imm12 <<= 12;
                if (page + imm12 != static_cast<std::uintptr_t>(base + target)) continue;

                __android_log_print(ANDROID_LOG_INFO, kTag, "   xref @ +0x%llx (adrp insn at distance %d)",
                                    static_cast<unsigned long long>(pc - base), j);
                // Walk backwards for a prologue marker (bounded by the segment).
                std::uintptr_t prologue = 0;
                for (std::uintptr_t p = pc; p >= segStart && pc - p <= 0x800; p -= 4) {
                    const std::uint32_t back = *reinterpret_cast<const volatile std::uint32_t*>(p);
                    if (isPrologue(back)) { prologue = p; break; }
                }
                if (prologue) {
                    __android_log_print(ANDROID_LOG_INFO, kTag, "   prologue @ +0x%llx, first 20 words:",
                                        static_cast<unsigned long long>(prologue - base));
                    dumpHex(loads, base, prologue, 20);
                } else {
                    __android_log_print(ANDROID_LOG_INFO, kTag, "   no prologue in 2KB window; dumping xref bytes:");
                    dumpHex(loads, base, pc >= segStart + 0x40 ? pc - 0x40 : segStart, 10);
                    dumpHex(loads, base, pc, 24);
                }
                ++found;
                break;
            }
        }
    }
    if (!found) __android_log_print(ANDROID_LOG_INFO, kTag, "   no adrp+add xref found");
}

// Parse /proc/self/maps for the mappings of `module`. Fills `base` (minimum
// mapping start) and a list of readable segments relative to it. This mirrors
// what the LeviLauncher preloader does — bionic's dlopen() does NOT return a
// link_map, so we never dereference the handle it gives us (we only feed it
// to dlsym).
bool parseMapsFor(const char* module, std::uintptr_t& base, std::vector<Segment>& loads) {
    base = ~static_cast<std::uintptr_t>(0);
    loads.clear();
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;

    char line[4096];
    while (std::fgets(line, sizeof(line), maps)) {
        if (std::strstr(line, module) == nullptr) continue;
        std::uintptr_t start = 0, end = 0;
        char perms[5] = {};
        if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, perms) != 3) continue;
        if (end <= start || perms[0] != 'r') continue;
        Segment seg;
        seg.vaddr = start;
        seg.filesz = end - start;
        seg.flags = 4;                               // PF_R
        if (perms[1] == 'w') seg.flags |= 2;         // PF_W
        if (perms[2] == 'x') seg.flags |= 1;         // PF_X
        loads.push_back(seg);
    }
    std::fclose(maps);

    if (loads.empty()) return false;
    for (const auto& seg : loads) base = std::min(base, seg.vaddr);
    for (auto& seg : loads) seg.vaddr -= base;
    return true;
}

} // namespace

DebugScanModule::DebugScanModule()
    : Module("Debug Scan", "Temporary reverse-engineering probe: dumps the overlay-color plumbing to logcat.") {
    showInMenu = false;
    hideInHudEditor = true;
}

void DebugScanModule::onInit() {
    __android_log_print(ANDROID_LOG_INFO, kTag, "BTScan: begin");

    // Kept for dlsym only; the handle is opaque on bionic and never dereferenced.
    void* handle = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);

    std::uintptr_t base = 0;
    std::vector<Segment> loads;
    if (!parseMapsFor("libminecraftpe.so", base, loads)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "libminecraftpe.so not found in /proc/self/maps");
        __android_log_print(ANDROID_LOG_INFO, kTag, "BTScan: end");
        return;
    }
    const auto* eh = reinterpret_cast<const std::uint8_t*>(base);

    std::uintptr_t textStart = ~static_cast<std::uintptr_t>(0);
    std::uintptr_t textEnd = 0;
    for (const auto& seg : loads) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "mapping vaddr=+0x%llx size=+0x%llx flags=0x%x",
                            static_cast<unsigned long long>(seg.vaddr),
                            static_cast<unsigned long long>(seg.filesz), seg.flags);
        if (seg.flags & 1u) {
            textStart = std::min(textStart, base + seg.vaddr);
            textEnd = std::max(textEnd, base + seg.vaddr + seg.filesz);
        }
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "base=+0x%llx text range +0x%llx..+0x%llx",
                        static_cast<unsigned long long>(base),
                        static_cast<unsigned long long>(textStart - base),
                        static_cast<unsigned long long>(textEnd - base));

    // 0) Are the client RenderController methods exported as dynamic symbols?
    //    (pl::memory::resolveSignature tries dlsym first, then byte patterns.)
    if (handle) {
        const char* candidates[] = {
            "_ZNK16RenderController15getOverlayColorER12RenderParams",
            "_ZN16RenderController15getOverlayColorER12RenderParams",
            "_ZNK16RenderController12getTintColorER12RenderParams",
            "_ZN16RenderController12getTintColorER12RenderParams",
            "_ZNK16RenderController14getOnFireColorER12RenderParams",
            "_ZN16RenderController14getOnFireColorER12RenderParams",
            "_ZNK16RenderController9getUVAnimER12RenderParamsRK4Vec4",
            "_ZTI16RenderController",
            "_ZTS16RenderController",
            "_ZTV16RenderController",
        };
        for (const char* name : candidates) {
            void* sym = dlsym(handle, name);
            __android_log_print(ANDROID_LOG_INFO, kTag, "dlsym %s -> %p", name, sym);
        }
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "dlopen(RTLD_NOLOAD) returned null; skipping dlsym checks");
    }

    // 1) Sanity: does the binary contain the symbol string at all?
    const char* probeStrings[] = {"getOverlayColor", "OverlayColor", "RenderController"};
    for (const char* needle : probeStrings) {
        const auto hits = findOccurrences(eh, loads, needle);
        __android_log_print(ANDROID_LOG_INFO, kTag, "'%s' occurrences: %zu", needle, hits.size());
        for (const auto off : hits) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "   str @ +0x%llx",
                                static_cast<unsigned long long>(off));
        }
    }

    // 2) Cross-reference every "OverlayColor" string from executable code.
    const auto overlayHits = findOccurrences(eh, loads, "OverlayColor");
    for (const std::uintptr_t hit : overlayHits) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "xrefs to string @ +0x%llx:",
                            static_cast<unsigned long long>(hit));
        scanText(loads, base, hit);
    }

    if (handle) dlclose(handle);

    __android_log_print(ANDROID_LOG_INFO, kTag, "BTScan: end");
}
