#include "debugscan.hpp"

#include <android/log.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace {

constexpr const char* kTag = "BTScan";
constexpr std::size_t kMaxHits = 24;

void logLine(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", buf);
}

struct LinkMap {
    std::uintptr_t l_addr;
    char* l_name;
    void* l_ld;
    LinkMap* l_next;
    LinkMap* l_prev;
};

struct Segment {
    std::uintptr_t vaddr = 0;
    std::uintptr_t filesz = 0;
    std::uint32_t flags = 0;
};

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

void dumpHex(std::uintptr_t addr, int words) {
    for (int i = 0; i < words; ++i) {
        const std::uint32_t insn = *reinterpret_cast<const volatile std::uint32_t*>(addr + 4 * i);
        __android_log_print(ANDROID_LOG_INFO, kTag, "     +0x%02x: %08x", i * 4, insn);
    }
}

bool isPrologue(std::uint32_t insn) {
    return insn == 0xD503237Fu  // paciasp
        || insn == 0xD50317BFu  // pacibsp
        || insn == 0xD503245Fu  // bti c
        || insn == 0xD503249Fu; // bti j
}

void scanText(std::uintptr_t textStart, std::uintptr_t textEnd, std::uintptr_t base,
              std::uintptr_t target) {
    int found = 0;
    for (std::uintptr_t pc = textStart; pc + 4 <= textEnd && found < 12; pc += 4) {
        const std::uint32_t insn = *reinterpret_cast<const volatile std::uint32_t*>(pc);
        if ((insn & 0x9F000000u) != 0x90000000u) continue; // ADRP
        const std::int64_t immhi = static_cast<std::int64_t>((insn >> 5) & 0x7FFFF);
        const std::int64_t immlo = static_cast<std::int64_t>((insn >> 29) & 0x3);
        std::int64_t imm21 = (immhi << 2) | immlo;
        if (imm21 & (1 << 20)) imm21 |= ~((static_cast<std::int64_t>(1) << 21) - 1);
        const std::uintptr_t page = (pc & ~0xFFFULL) + static_cast<std::uintptr_t>(imm21 << 12);
        const std::uint32_t rd = insn & 0x1F;
        for (int j = 1; j <= 8; ++j) {
            const std::uintptr_t at = pc + 4 * j;
            if (at + 4 > textEnd) break;
            const std::uint32_t add = *reinterpret_cast<const volatile std::uint32_t*>(at);
            if ((add & 0x7F800000u) != 0x11000000u) continue;             // ADD (64-bit imm)
            if (((add >> 5) & 0x1F) != rd || (add & 0x1F) != rd) continue; // ADD Xd, Xd, #imm
            std::uint32_t imm12 = (add >> 10) & 0xFFF;
            if (add & (1u << 22)) imm12 <<= 12;
            if (page + imm12 != static_cast<std::uintptr_t>(base + target)) continue;

            __android_log_print(ANDROID_LOG_INFO, kTag, "   xref @ +0x%llx (adrp insn at distance %d)",
                                static_cast<unsigned long long>(pc - base), j);
            // Walk backwards for a prologue marker.
            std::uintptr_t prologue = 0;
            for (std::uintptr_t p = pc; p >= textStart && pc - p <= 0x800; p -= 4) {
                const std::uint32_t back = *reinterpret_cast<const volatile std::uint32_t*>(p);
                if (isPrologue(back)) { prologue = p; break; }
            }
            if (prologue) {
                __android_log_print(ANDROID_LOG_INFO, kTag, "   prologue @ +0x%llx, first 20 words:",
                                    static_cast<unsigned long long>(prologue - base));
                for (int k = 0; k < 20; ++k) {
                    const std::uint32_t w = *reinterpret_cast<const volatile std::uint32_t*>(prologue + 4 * k);
                    __android_log_print(ANDROID_LOG_INFO, kTag, "     +0x%02x: %08x", k * 4, w);
                }
            } else {
                __android_log_print(ANDROID_LOG_INFO, kTag, "   no prologue in 2KB window; dumping xref bytes:");
                dumpHex(pc - 0x40, 10);
                dumpHex(pc, 24);
            }
            ++found;
            break;
        }
    }
    if (!found) __android_log_print(ANDROID_LOG_INFO, kTag, "   no adrp+add xref found");
}

} // namespace

DebugScanModule::DebugScanModule()
    : Module("Debug Scan", "Temporary reverse-engineering probe: dumps the overlay-color plumbing to logcat.") {
    showInMenu = false;
    hideInHudEditor = true;
}

void DebugScanModule::onInit() {
    __android_log_print(ANDROID_LOG_INFO, kTag, "BTScan: begin");

    void* handle = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "libminecraftpe.so is not loaded");
        __android_log_print(ANDROID_LOG_INFO, kTag, "BTScan: end");
        return;
    }

    const auto base = reinterpret_cast<LinkMap*>(handle)->l_addr;
    if (!base) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "link_map has no base address");
        __android_log_print(ANDROID_LOG_INFO, kTag, "BTScan: end");
        return;
    }
    const auto* eh = reinterpret_cast<const std::uint8_t*>(base);
    if (eh[0] != 0x7F || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F') {
        __android_log_print(ANDROID_LOG_INFO, kTag, "not an ELF at the reported base");
        __android_log_print(ANDROID_LOG_INFO, kTag, "BTScan: end");
        return;
    }

    const std::uint64_t phoff = *reinterpret_cast<const std::uint64_t*>(eh + 0x20);
    const std::uint16_t phentsize = *reinterpret_cast<const std::uint16_t*>(eh + 0x36);
    const std::uint16_t phnum = *reinterpret_cast<const std::uint16_t*>(eh + 0x38);
    __android_log_print(ANDROID_LOG_INFO, kTag, "base=+0x%llx phnum=%u",
                        static_cast<unsigned long long>(base), phnum);

    std::vector<Segment> loads;
    std::uintptr_t textStart = ~static_cast<std::uintptr_t>(0);
    std::uintptr_t textEnd = 0;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::uint8_t* ph = eh + phoff + static_cast<std::uint64_t>(i) * phentsize;
        if (*reinterpret_cast<const std::uint32_t*>(ph) != 1) continue; // PT_LOAD
        Segment seg;
        seg.flags = *reinterpret_cast<const std::uint32_t*>(ph + 0x04);
        seg.filesz = *reinterpret_cast<const std::uint64_t*>(ph + 0x20);
        const std::uint64_t vaddr = *reinterpret_cast<const std::uint64_t*>(ph + 0x10);
        seg.vaddr = static_cast<std::uintptr_t>(vaddr);
        __android_log_print(ANDROID_LOG_INFO, kTag, "PT_LOAD vaddr=+0x%llx filesz=+0x%llx flags=0x%x",
                            static_cast<unsigned long long>(seg.vaddr),
                            static_cast<unsigned long long>(seg.filesz), seg.flags);
        if (seg.flags & 1u) {
            textStart = std::min(textStart, base + seg.vaddr);
            textEnd = std::max(textEnd, base + seg.vaddr + seg.filesz);
        }
        loads.push_back(seg);
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "text range +0x%llx..+0x%llx",
                        static_cast<unsigned long long>(textStart - base),
                        static_cast<unsigned long long>(textEnd - base));

    // 0) Are the client RenderController methods exported as dynamic symbols?
    //    (pl::memory::resolveSignature tries dlsym first, then byte patterns.)
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
        scanText(textStart, textEnd, base, hit);
    }

    __android_log_print(ANDROID_LOG_INFO, kTag, "BTScan: end");
}
