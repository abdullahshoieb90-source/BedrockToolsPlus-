#pragma once

// Finding a mangled C++ symbol inside an already loaded library.
//
// dlsym only sees the dynamic symbols the linker exported: on a stripped,
// version-scripted library like libminecraftpe.so most of the C++ surface is
// not visible through it. The image's own dynamic symbol table is still mapped
// in memory though, together with the hash tables that count its entries, so an
// accessor such as ItemStackBase::getDamageValue() can be found by name without
// any byte pattern at all.
//
// Every read is bounds checked against the image's PT_LOAD segments (the
// Android linker maps them with gaps), so a corrupt or unexpected table cannot
// make the resolver crash the game.
//
// The header is dependency free (dlfcn / link / elf only) so the host tests can
// cover the ELF walking against a real library.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <dlfcn.h>
#include <elf.h>
#include <link.h>

namespace bedrocktools::memory::library {
namespace detail {

// Sanity limit: a hash table whose chains run past this is treated as
// unreadable instead of being trusted.
inline constexpr std::uint32_t MaxSymbolIndex = 1u << 22;

inline bool readableRange(const dl_phdr_info& info, std::uintptr_t begin, std::size_t size) {
    if (!begin || size == 0) return false;
    const std::uintptr_t end = begin + size;
    if (end < begin) return false;
    for (int i = 0; i < info.dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info.dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;
        const std::uintptr_t start = static_cast<std::uintptr_t>(info.dlpi_addr) + phdr.p_vaddr;
        const std::uintptr_t stop = start + phdr.p_memsz;
        if (begin >= start && end <= stop) return true;
    }
    return false;
}

// End of the PT_LOAD segment holding `address`, 0 when it is not mapped.
inline std::uintptr_t segmentEnd(const dl_phdr_info& info, std::uintptr_t address) {
    for (int i = 0; i < info.dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info.dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;
        const std::uintptr_t start = static_cast<std::uintptr_t>(info.dlpi_addr) + phdr.p_vaddr;
        const std::uintptr_t stop = start + phdr.p_memsz;
        if (address >= start && address < stop) return stop;
    }
    return 0;
}

template <class T>
inline const T* readable(const dl_phdr_info& info, std::uintptr_t address) {
    if (!readableRange(info, address, sizeof(T))) return nullptr;
    return reinterpret_cast<const T*>(address);
}

// Dynamic table entries are virtual addresses in the file. glibc relocates them
// to absolute addresses in memory, bionic (and the Android linker) keeps the
// vaddrs, so both forms are accepted and the one that actually points into the
// image wins.
inline std::uintptr_t dynamicPointer(const dl_phdr_info& info, std::uintptr_t value, std::size_t size) {
    if (readableRange(info, value, size)) return value;
    const std::uintptr_t base = static_cast<std::uintptr_t>(info.dlpi_addr);
    if (value && readableRange(info, base + value, size)) return base + value;
    return 0;
}

// DT_HASH (SysV) stores [nbucket][nchain][...]: nchain is the symbol count.
inline std::size_t sysvSymbolCount(const dl_phdr_info& info, std::uintptr_t hash) {
    const auto* words = readable<Elf32_Word>(info, hash);
    if (!words || !readableRange(info, hash, 2 * sizeof(Elf32_Word))) return 0;
    const std::size_t count = static_cast<std::size_t>(words[1]);
    return count <= MaxSymbolIndex ? count : 0;
}

// DT_GNU_HASH carries no count: derive it from the buckets and the terminated
// chain of the highest used bucket.
inline std::size_t gnuSymbolCount(const dl_phdr_info& info, std::uintptr_t hash) {
    struct Header {
        Elf32_Word nbuckets;
        Elf32_Word symoffset;
        Elf32_Word bloomSize;
        Elf32_Word bloomShift;
    };
    const auto* header = readable<Header>(info, hash);
    if (!header || header->nbuckets == 0 || header->nbuckets > MaxSymbolIndex) return 0;

    const std::uintptr_t bloom = hash + sizeof(Header);
    if (!readableRange(info, bloom, static_cast<std::size_t>(header->bloomSize) * sizeof(ElfW(Addr)))) return 0;
    const std::uintptr_t buckets = bloom + static_cast<std::size_t>(header->bloomSize) * sizeof(ElfW(Addr));
    if (!readableRange(info, buckets, static_cast<std::size_t>(header->nbuckets) * sizeof(Elf32_Word))) return 0;
    const std::uintptr_t chain = buckets + static_cast<std::size_t>(header->nbuckets) * sizeof(Elf32_Word);

    const auto* bucketEntries = reinterpret_cast<const Elf32_Word*>(buckets);
    std::uint32_t last = header->symoffset > 0 ? header->symoffset - 1 : 0;
    for (Elf32_Word i = 0; i < header->nbuckets; ++i) {
        std::uint32_t index = bucketEntries[i];
        if (index < header->symoffset) continue; // empty bucket
        while (index < MaxSymbolIndex) {
            const auto* entry = readable<Elf32_Word>(
                info, chain + static_cast<std::size_t>(index - header->symoffset) * sizeof(Elf32_Word));
            if (!entry) return 0;
            if (*entry & 1u) break; // the LSB marks the end of the chain
            ++index;
        }
        if (index >= MaxSymbolIndex) return 0;
        if (index > last) last = index;
    }
    return static_cast<std::size_t>(last) + 1;
}

// Compares the symbol table's string against `wanted`, bounded so a corrupt
// offset cannot walk out of the string table's segment.
inline bool sameName(const char* table, std::size_t tableSize, std::uintptr_t nameOffset, const char* wanted) {
    if (nameOffset >= tableSize) return false;
    const std::size_t remaining = tableSize - nameOffset;
    const std::size_t wantedLength = std::strlen(wanted);
    if (wantedLength >= remaining) return false;
    return std::memcmp(table + nameOffset, wanted, wantedLength + 1) == 0;
}

inline std::uintptr_t lookupInImage(const dl_phdr_info& info, const char* symbol) {
    const std::uintptr_t base = static_cast<std::uintptr_t>(info.dlpi_addr);
    const ElfW(Dyn)* dynamic = nullptr;
    for (int i = 0; i < info.dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info.dlpi_phdr[i];
        if (phdr.p_type == PT_DYNAMIC) dynamic = reinterpret_cast<const ElfW(Dyn)*>(base + phdr.p_vaddr);
    }
    if (!dynamic || !readableRange(info, reinterpret_cast<std::uintptr_t>(dynamic), sizeof(ElfW(Dyn)))) return 0;

    std::uintptr_t symtab = 0;
    std::uintptr_t strtab = 0;
    std::uintptr_t hash = 0;
    std::uintptr_t gnuHash = 0;
    for (const ElfW(Dyn)* entry = dynamic;; ++entry) {
        if (!readableRange(info, reinterpret_cast<std::uintptr_t>(entry), sizeof(ElfW(Dyn)))) return 0;
        if (entry->d_tag == DT_NULL) break;
        switch (entry->d_tag) {
            case DT_SYMTAB: symtab = dynamicPointer(info, entry->d_un.d_ptr, sizeof(ElfW(Sym))); break;
            case DT_STRTAB: strtab = dynamicPointer(info, entry->d_un.d_ptr, 1); break;
            case DT_HASH: hash = dynamicPointer(info, entry->d_un.d_ptr, 2 * sizeof(Elf32_Word)); break;
            case DT_GNU_HASH: gnuHash = dynamicPointer(info, entry->d_un.d_ptr, sizeof(Elf32_Word)); break;
            default: break;
        }
    }
    if (!symtab || !strtab) return 0;

    const std::size_t strtabSize = [&] {
        const std::uintptr_t end = segmentEnd(info, strtab);
        return end > strtab ? end - strtab : 0;
    }();
    if (strtabSize == 0) return 0;

    const std::size_t count = hash ? sysvSymbolCount(info, hash) : gnuSymbolCount(info, gnuHash);
    if (count == 0) return 0;

    const char* strings = reinterpret_cast<const char*>(strtab);
    for (std::size_t index = 1; index < count; ++index) {
        const auto* symbolEntry = readable<ElfW(Sym)>(info, symtab + index * sizeof(ElfW(Sym)));
        if (!symbolEntry || symbolEntry->st_value == 0) continue;
        // Only real functions: an IFUNC entry holds the resolver (its address
        // is not the function to call), and data symbols are not wanted here.
        if ((symbolEntry->st_info & 0xf) != STT_FUNC) continue;
        if (!sameName(strings, strtabSize, symbolEntry->st_name, symbol)) continue;
        return base + symbolEntry->st_value;
    }
    return 0;
}

struct ScanState {
    const char* module;
    const char* symbol;
    std::uintptr_t result;
};

inline int scanImages(struct dl_phdr_info* info, std::size_t, void* data) {
    auto* state = static_cast<ScanState*>(data);
    if (!state || state->result) return 1;
    if (!info || !info->dlpi_name || !*info->dlpi_name) return 0;
    if (!std::strstr(info->dlpi_name, state->module)) return 0;
    state->result = lookupInImage(*info, state->symbol);
    return state->result ? 1 : 0;
}

} // namespace detail

// Looks `mangledSymbol` up in `moduleName` — a substring of the loaded path,
// the same matching the signature resolver uses for /proc/self/maps. dlsym is
// tried first because it is exact and cheap; the dynamic symbol table scan then
// covers the symbols the linker did not export.
inline std::uintptr_t findMangled(std::string_view moduleName, std::string_view mangledSymbol) {
    if (moduleName.empty() || mangledSymbol.empty()) return 0;
    const std::string module(moduleName);
    const std::string name(mangledSymbol);

    if (void* handle = dlopen(module.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
        void* address = dlsym(handle, name.c_str());
        dlclose(handle);
        if (address) return reinterpret_cast<std::uintptr_t>(address);
    }

    detail::ScanState state{module.c_str(), name.c_str(), 0};
    dl_iterate_phdr(detail::scanImages, &state);
    return state.result;
}

} // namespace bedrocktools::memory::library
