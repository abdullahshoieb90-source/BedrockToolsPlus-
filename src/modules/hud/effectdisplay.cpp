// Effect Display module
//
// Data acquisition: the active effect list of the local player is resolved
// without any version-specific signature, using two self-validating scans:
//   (1) the player vtable is searched for a trivial getter (a few AArch64
//       instructions ending in RET) that returns a std::vector whose
//       elements all look like MobEffectInstance objects — Mob::getAllEffects
//       is a known virtual in modern builds, so this is the strongest signal;
//   (2) the player object's memory is scanned for a member vector with the
//       same property.
// Every candidate is checked against /proc/self/maps before being
// dereferenced and every element's id/duration/amplifier/flags are
// range-validated, so a wrong guess can never crash or show garbage — the
// worst case is an empty HUD while the resolver keeps retrying.

#include "effectdisplay.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace {

// The resolver deliberately reads foreign game memory (checked against
// /proc/self/maps first). AddressSanitizer cannot model those reads, so the
// raw accessors are excluded from instrumentation.
#if defined(__GNUC__) || defined(__clang__)
#define EFFECTDISPLAY_RAW_READ __attribute__((no_sanitize("address")))
#else
#define EFFECTDISPLAY_RAW_READ
#endif

// ---------------------------------------------------------------------------
// Effect table
// ---------------------------------------------------------------------------

struct EffectDef {
    const char* name;
    std::uint32_t color; // 0xRRGGBB tint used for the icon
    const char* glyph;   // 1-2 characters drawn inside the icon
};

// Bedrock Edition effect ids (client side). Index = effect id, entry 0 is a
// placeholder. Unknown-but-valid ids fall back to a generic style, so the
// module supports more effects than this table lists.
constexpr EffectDef kEffectDefs[] = {
    {"",                0x000000, ""  },
    {"Speed",           0x7CAFC6, "Sp"},
    {"Slowness",        0x5A6C81, "Sl"},
    {"Haste",           0xD9C043, "Ha"},
    {"Mining Fatigue",  0x4A4217, "MF"},
    {"Strength",        0x932423, "St"},
    {"Instant Health",  0xF82423, "IH"},
    {"Instant Damage",  0x430A09, "ID"},
    {"Jump Boost",      0x22FF4C, "JB"},
    {"Nausea",          0x551D4A, "Na"},
    {"Regeneration",    0xCD5CAB, "Rg"},
    {"Resistance",      0x99453A, "Rs"},
    {"Fire Resistance", 0xE49A3A, "FR"},
    {"Water Breathing", 0x2E5299, "WB"},
    {"Invisibility",    0x7F8392, "Iv"},
    {"Blindness",       0x1F1F23, "Bl"},
    {"Night Vision",    0x1F1FA1, "NV"},
    {"Hunger",          0x587653, "Hu"},
    {"Weakness",        0x484D48, "Wk"},
    {"Poison",          0x4E9331, "Ps"},
    {"Wither",          0x352A27, "Wi"},
    {"Health Boost",    0xF87D23, "HB"},
    {"Absorption",      0x2552A5, "Ab"},
    {"Saturation",      0xF82423, "Sa"},
    {"Levitation",      0xCEFFFF, "Lv"},
    {"Fatal Poison",    0x4E9331, "FP"},
    {"Conduit Power",   0x1DC2D1, "CP"},
    {"Slow Falling",    0xCEFFFF, "SF"},
    {"Bad Omen",        0x0B6138, "BO"},
    {"Village Hero",    0x44FF17, "VH"},
    {"Darkness",        0x292721, "Dk"},
    {"Trial Omen",      0x0E6262, "TO"},
    {"Raid Omen",       0x6B2E4D, "RO"},
    {"Wind Charged",    0x777777, "WC"},
    {"Weaving",         0x789892, "Wv"},
    {"Oozing",          0x8FD08B, "Oz"},
    {"Infested",        0x8C9C5C, "In"},
};

constexpr int kKnownEffectCount = static_cast<int>(sizeof(kEffectDefs) / sizeof(EffectDef)) - 1;

const EffectDef& effectDef(int id) {
    if (id >= 1 && id <= kKnownEffectCount) return kEffectDefs[id];
    static const EffectDef kUnknown{"Effect", 0x808080, "?"};
    return kUnknown;
}

// ---------------------------------------------------------------------------
// Memory safety: /proc/self/maps lookup cache
// ---------------------------------------------------------------------------

struct MemRegion {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
};

class MapsCache {
public:
    bool contains(std::uintptr_t addr, std::size_t len) {
        if (addr == 0 || len == 0) return false;
        const bool prevHit = m_prevHit;
        for (int attempt = 0; attempt < 2; ++attempt) {
            refreshIfNeeded();
            const bool hit = lookup(addr, len);
            m_prevHit = hit;
            if (hit) return true;

            // Miss: the cache may be missing a freshly created mapping.
            // Reload and retry once — but only when the previous lookup hit,
            // so scans over genuinely unmapped ranges stay cheap.
            if (attempt == 0 && prevHit) {
                reload();
                m_prevHit = false;
                continue;
            }
            return false;
        }
        return false;
    }

private:
    bool lookup(std::uintptr_t addr, std::size_t len) const {
        const std::uintptr_t end = addr + len;
        const auto it = std::lower_bound(m_regions.begin(), m_regions.end(), addr,
            [](const MemRegion& r, std::uintptr_t a) { return r.end <= a; });
        return it != m_regions.end() && it->start <= addr && it->end >= end;
    }

    void refreshIfNeeded() {
        const auto now = std::chrono::steady_clock::now();
        if (!m_regions.empty() && now - m_lastReload < std::chrono::seconds(2)) return;
        reload();
        m_lastReload = now;
    }

    void reload() {
        std::vector<MemRegion> regions;
        regions.reserve(256);
        std::ifstream in("/proc/self/maps");
        std::string line;
        while (std::getline(in, line)) {
            unsigned long long start = 0, end = 0;
            char perms[5] = {};
            // Format: start-end perms offset dev inode pathname
            if (std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms) != 3) continue;
            if (perms[0] != 'r') continue; // PROT_NONE / write-only regions must not be read
            if (end <= start) continue;
            regions.push_back({static_cast<std::uintptr_t>(start), static_cast<std::uintptr_t>(end)});
        }
        if (!regions.empty()) m_regions.swap(regions);
    }

    std::vector<MemRegion> m_regions;
    std::chrono::steady_clock::time_point m_lastReload{};
    bool m_prevHit = true;
};

MapsCache s_maps;

// ---------------------------------------------------------------------------
// AArch64 instruction decoding (only the shapes a trivial getter can use)
// ---------------------------------------------------------------------------

struct Insn {
    enum class Kind { Other, Ret, Nop, Mov, Add, Ldr } kind = Kind::Other;
    int rd = -1;
    int rn = -1;
    std::uint32_t imm = 0;
    bool shift12 = false;
};

Insn decodeInsn(std::uint32_t word) {
    Insn out;
    if (word == 0xD65F03C0u) { out.kind = Insn::Kind::Ret; return out; }        // RET
    if (word == 0xD503201Fu) { out.kind = Insn::Kind::Nop; return out; }        // NOP
    if ((word & 0xFFE0FC1Fu) == 0xAA000000u) {                                  // MOV rd, rn (ORR rd, XZR, rn)
        out.kind = Insn::Kind::Mov;
        out.rd = word & 0x1Fu;
        out.rn = (word >> 5) & 0x1Fu;
        return out;
    }
    if ((word & 0x7F800000u) == 0x11000000u && (word & 0x80000000u)) {         // ADD rd, rn, #imm (64-bit)
        out.kind = Insn::Kind::Add;
        out.rd = word & 0x1Fu;
        out.rn = (word >> 5) & 0x1Fu;
        out.imm = (word >> 10) & 0xFFFu;
        out.shift12 = (word >> 22) & 1u;
        return out;
    }
    if ((word & 0xFFC00000u) == 0xF9400000u) {                                  // LDR Xt, [Xn, #imm*8]
        out.kind = Insn::Kind::Ldr;
        out.rd = word & 0x1Fu;
        out.rn = (word >> 5) & 0x1Fu;
        out.imm = (word >> 10) & 0xFFFu;
        return out;
    }
    return out;
}

// Symbolic address expression produced by a tiny getter:
//   This      -> this
//   ThisAdd   -> this + disp
//   Load      -> *(this + base) + disp
//   LoadLoad  -> *(*(this + base) + base2) + disp
struct Expr {
    enum class Kind { Invalid, This, ThisAdd, Load, LoadLoad } kind = Kind::Invalid;
    std::uintptr_t base = 0;
    std::uintptr_t base2 = 0;
    std::uintptr_t disp = 0;

    bool bounded() const {
        constexpr std::uintptr_t kMax = 0x8000;
        switch (kind) {
            case Kind::ThisAdd: return disp <= kMax;
            case Kind::Load: return base <= kMax && disp <= kMax;
            case Kind::LoadLoad: return base <= kMax && base2 <= kMax && disp <= kMax;
            default: return false;
        }
    }
};

// Evaluate a <=5 instruction sequence ending in RET as a getter computing x0
// from the implicit "this". Recognized shapes include:
//   add x0, x0, #imm / ret                          -> this + imm
//   ldr x0, [x0, #imm] / ret                        -> *(this + imm)
//   ldr x8, [x0, #imm] / add x0, x8, #imm / ret     -> *(this + imm) + imm
//   ldr x8, [x0, #imm] / mov x0, x8 / ret           -> *(this + imm)
//   ldr x8, [x0, #i] / ldr x0, [x8, #j] / ret       -> *(*(this + i) + j)
EFFECTDISPLAY_RAW_READ
bool evalGetter(const std::uint32_t* code, std::size_t count, Expr& out) {
    Expr regs[32];
    regs[0] = {Expr::Kind::This, 0, 0, 0};
    const std::size_t limit = std::min<std::size_t>(count, 5);
    for (std::size_t i = 0; i < limit; ++i) {
        const Insn ins = decodeInsn(code[i]);
        switch (ins.kind) {
            case Insn::Kind::Ret: {
                if (regs[0].kind == Expr::Kind::This) return false;
                out = regs[0];
                return out.kind != Expr::Kind::Invalid && out.bounded();
            }
            case Insn::Kind::Nop:
                break;
            case Insn::Kind::Mov:
                regs[ins.rd] = regs[ins.rn];
                break;
            case Insn::Kind::Add: {
                const std::uintptr_t val = ins.imm << (ins.shift12 ? 12 : 0);
                const Expr& src = regs[ins.rn];
                switch (src.kind) {
                    case Expr::Kind::This: regs[ins.rd] = {Expr::Kind::ThisAdd, 0, 0, val}; break;
                    case Expr::Kind::ThisAdd: regs[ins.rd] = {Expr::Kind::ThisAdd, 0, 0, src.disp + val}; break;
                    case Expr::Kind::Load: regs[ins.rd] = {Expr::Kind::Load, src.base, 0, src.disp + val}; break;
                    case Expr::Kind::LoadLoad: regs[ins.rd] = {Expr::Kind::LoadLoad, src.base, src.base2, src.disp + val}; break;
                    default: regs[ins.rd] = {}; break;
                }
                break;
            }
            case Insn::Kind::Ldr: {
                const std::uintptr_t off = ins.imm * 8;
                const Expr& src = regs[ins.rn];
                switch (src.kind) {
                    case Expr::Kind::This: regs[ins.rd] = {Expr::Kind::Load, off, 0, 0}; break;
                    case Expr::Kind::ThisAdd: regs[ins.rd] = {Expr::Kind::Load, off + src.disp, 0, 0}; break;
                    case Expr::Kind::Load: regs[ins.rd] = {Expr::Kind::LoadLoad, src.base, off + src.disp, 0}; break;
                    default: regs[ins.rd] = {}; break;
                }
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

EFFECTDISPLAY_RAW_READ
bool exprAddr(std::uintptr_t self, const Expr& expr, std::uintptr_t& out) {
    switch (expr.kind) {
        case Expr::Kind::This: out = self; return true;
        case Expr::Kind::ThisAdd: out = self + expr.disp; return true;
        case Expr::Kind::Load: {
            const std::uintptr_t addr = self + expr.base;
            if (!s_maps.contains(addr, 8)) return false;
            out = *reinterpret_cast<const std::uintptr_t*>(addr) + expr.disp;
            return true;
        }
        case Expr::Kind::LoadLoad: {
            const std::uintptr_t addr = self + expr.base;
            if (!s_maps.contains(addr, 8)) return false;
            const std::uintptr_t mid = *reinterpret_cast<const std::uintptr_t*>(addr);
            const std::uintptr_t addr2 = mid + expr.base2;
            if (!s_maps.contains(addr2, 8)) return false;
            out = *reinterpret_cast<const std::uintptr_t*>(addr2) + expr.disp;
            return true;
        }
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// std::vector / MobEffectInstance validation
// ---------------------------------------------------------------------------

struct VectorInfo {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

struct RawEffect {
    int id = 0;
    int duration = 0;   // -1 = infinite
    int amplifier = 0;
    bool visible = true;
    bool ambient = false;
};

EFFECTDISPLAY_RAW_READ
bool readVector(std::uintptr_t addr, VectorInfo& out) {
    if (!s_maps.contains(addr, 24)) return false;
    out.begin = *reinterpret_cast<const std::uintptr_t*>(addr);
    out.end = *reinterpret_cast<const std::uintptr_t*>(addr + 8);
    const std::uintptr_t cap = *reinterpret_cast<const std::uintptr_t*>(addr + 16);
    if (out.end < out.begin) return false;
    const std::size_t bytes = out.end - out.begin;
    if (bytes > 40 * sizeof(std::uintptr_t)) return false;
    // The capacity field must be sane for the vector to be trusted.
    if (cap < out.end) return false;
    if ((cap - out.end) % sizeof(std::uintptr_t) != 0) return false;
    if (cap - out.end > (std::uintptr_t(1) << 20)) return false;
    if (!s_maps.contains(out.begin, bytes)) return false;
    return true;
}

// Validate one MobEffectInstance. Both the modern layout (>= 1.16, amplifier
// at 0x14, flags at 0x18/0x19) and the legacy layout (amplifier at 0x8,
// flags at 0xC/0xD) are detected per instance.
EFFECTDISPLAY_RAW_READ
bool validateEffectInstance(std::uintptr_t p, RawEffect& out) {
    if (!s_maps.contains(p, 0x1C)) return false;
    const int id = *reinterpret_cast<const int*>(p);
    const int duration = *reinterpret_cast<const int*>(p + 4);
    if (id < 1 || id > 64) return false;
    if (duration != -1 && (duration < 0 || duration > 2000000000)) return false;

    const int ampNew = *reinterpret_cast<const int*>(p + 0x14);
    const std::uint8_t visNew = *reinterpret_cast<const std::uint8_t*>(p + 0x18);
    const std::uint8_t ambNew = *reinterpret_cast<const std::uint8_t*>(p + 0x19);
    if (ampNew >= 0 && ampNew <= 255 && visNew <= 1 && ambNew <= 1) {
        out = {id, duration, ampNew, visNew != 0, ambNew != 0};
        return true;
    }

    const int ampOld = *reinterpret_cast<const int*>(p + 0x8);
    const std::uint8_t visOld = *reinterpret_cast<const std::uint8_t*>(p + 0xC);
    const std::uint8_t ambOld = *reinterpret_cast<const std::uint8_t*>(p + 0xD);
    if (ampOld >= 0 && ampOld <= 255 && visOld <= 1 && ambOld <= 1) {
        out = {id, duration, ampOld, visOld != 0, ambOld != 0};
        return true;
    }
    return false;
}

// Score a vector of pointers to effect instances. Returns the element count
// only when EVERY element validates, otherwise 0 — a partially valid vector
// is almost certainly a false positive.
EFFECTDISPLAY_RAW_READ
std::size_t scorePointerVector(const VectorInfo& vi) {
    const std::size_t size = (vi.end - vi.begin) / sizeof(std::uintptr_t);
    if (size == 0) return 0;
    RawEffect tmp;
    for (std::size_t i = 0; i < size; ++i) {
        const std::uintptr_t p = *reinterpret_cast<const std::uintptr_t*>(vi.begin + i * 8);
        if (!validateEffectInstance(p, tmp)) return 0;
    }
    return size;
}

// ---------------------------------------------------------------------------
// Effect list resolver
// ---------------------------------------------------------------------------

struct ResolvedVector {
    bool valid = false;
    int mode = 0;                 // 1 = member vector, 2 = vector behind a getter
    std::uintptr_t memberOff = 0; // mode 1: vector at player + memberOff
    Expr expr{};                  // mode 2: vector at expr(player)
    std::size_t score = 0;
};

class EffectListResolver {
public:
    bool refresh(void* player, std::vector<RawEffect>& out) {
        const std::uintptr_t self = reinterpret_cast<std::uintptr_t>(player);
        if (self != m_self) {
            m_self = self;
            m_state = {};
            m_nextScan = std::chrono::steady_clock::now();
        }

        if (m_state.valid && read(self, out)) return true;

        m_state = {};
        const auto now = std::chrono::steady_clock::now();
        if (now < m_nextScan) return false;
        m_nextScan = now + std::chrono::seconds(5);
        scan(self);
        if (m_state.valid) return read(self, out);
        return false;
    }

private:
    EFFECTDISPLAY_RAW_READ
    void scan(std::uintptr_t self) {
        // 1) Direct scan of the object memory for a member vector whose
        //    elements all validate as effect instances.
        ResolvedVector bestMember;
        for (std::uintptr_t off = 0x40; off <= 0x6000; off += 8) {
            VectorInfo vi;
            if (!readVector(self + off, vi)) continue;
            const std::size_t score = scorePointerVector(vi);
            if (score > bestMember.score) bestMember = {true, 1, off, {}, score};
        }

        // 2) Scan the vtable for a trivial getter returning the effect
        //    vector. This is the strongest signal (Mob::getAllEffects is a
        //    known virtual in modern builds), so it wins over the object
        //    scan when both produce a candidate.
        ResolvedVector bestGetter;
        if (s_maps.contains(self, 8)) {
            const std::uintptr_t vtbl = *reinterpret_cast<const std::uintptr_t*>(self);
            for (std::size_t slot = 0; slot < 900; ++slot) {
                const std::uintptr_t codeAddr = vtbl + slot * 8;
                if (!s_maps.contains(codeAddr, 8)) break;
                const std::uintptr_t entry = *reinterpret_cast<const std::uintptr_t*>(codeAddr);
                if (!s_maps.contains(entry, 20)) continue;
                Expr expr;
                if (!evalGetter(reinterpret_cast<const std::uint32_t*>(entry), 5, expr)) continue;
                std::uintptr_t vecAddr = 0;
                if (!exprAddr(self, expr, vecAddr)) continue;
                VectorInfo vi;
                if (!readVector(vecAddr, vi)) continue;
                const std::size_t score = scorePointerVector(vi);
                if (score > bestGetter.score) bestGetter = {true, 2, 0, expr, score};
            }
        }

        if (bestGetter.valid) m_state = bestGetter;
        else m_state = bestMember;
    }

    EFFECTDISPLAY_RAW_READ
    bool read(std::uintptr_t self, std::vector<RawEffect>& out) {
        out.clear();
        std::uintptr_t vecAddr = 0;
        if (m_state.mode == 1) {
            vecAddr = self + m_state.memberOff;
        } else if (m_state.mode == 2) {
            if (!exprAddr(self, m_state.expr, vecAddr)) return false;
        } else {
            return false;
        }

        VectorInfo vi;
        if (!readVector(vecAddr, vi)) return false;

        const std::size_t size = (vi.end - vi.begin) / sizeof(std::uintptr_t);
        if (size == 0 || size > 40) return false;

        out.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            const std::uintptr_t p = *reinterpret_cast<const std::uintptr_t*>(vi.begin + i * 8);
            RawEffect fx;
            if (!validateEffectInstance(p, fx)) return false;
            out.push_back(fx);
        }
        return true;
    }

    std::uintptr_t m_self = 0;
    ResolvedVector m_state{};
    std::chrono::steady_clock::time_point m_nextScan{};
};

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::string formatDuration(int ticks) {
    if (ticks < 0) return "inf";
    const int totalSec = ticks / 20;
    const int h = totalSec / 3600;
    const int m = (totalSec / 60) % 60;
    const int s = totalSec % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

const char* romanNumeral(int n) {
    static const char* const kRoman[] = {
        "", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X",
        "XI", "XII", "XIII", "XIV", "XV"
    };
    if (n >= 0 && n < static_cast<int>(sizeof(kRoman) / sizeof(kRoman[0]))) return kRoman[n];
    return nullptr;
}

float calcTextWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (const char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ') width += size * 0.3f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') width += size * 0.8f;
        else width += size * 0.58f;
    }
    return width;
}

std::uint32_t glyphColor(std::uint32_t rgb) {
    const float r = ((rgb >> 16) & 0xFFu) / 255.0f;
    const float g = ((rgb >> 8) & 0xFFu) / 255.0f;
    const float b = (rgb & 0xFFu) / 255.0f;
    return (r * 0.299f + g * 0.587f + b * 0.114f) > 0.62f ? 0x60000000u : 0xFFFFFFFFu;
}

} // namespace

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

EffectDisplayModule* EffectDisplayModule::s_instance = nullptr;

static void effectDisplayTick(void* localPlayer) {
    auto* mod = EffectDisplayModule::getInstance();
    if (mod && mod->enabled) mod->onLocalPlayerTick(localPlayer);
}

EffectDisplayModule::EffectDisplayModule()
    : Module("Effect Display", "Displays active status effects with name, amplifier and duration timer.") {
    s_instance = this;
}

EffectDisplayModule::~EffectDisplayModule() {
    if (s_instance == this) s_instance = nullptr;
}

EffectDisplayModule* EffectDisplayModule::getInstance() {
    return s_instance;
}

void EffectDisplayModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { effectDisplayTick(event.player); });
}

void EffectDisplayModule::onEnable() {
    m_refreshCooldown = 0;
}

void EffectDisplayModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_effects.clear();
}

void EffectDisplayModule::onLocalPlayerTick(void* localPlayer) {
    if (!localPlayer) return;
    ++m_tickCounter;

    if (localPlayer != m_lastPlayer) {
        // The player object changed (respawn / dimension change): start over.
        m_lastPlayer = localPlayer;
        m_durationsTick = true;
        m_probeId = -1;
        m_staticBase.clear();
    }

    if (m_refreshCooldown > 0) {
        --m_refreshCooldown;
        return;
    }
    m_refreshCooldown = 5;
    refresh(localPlayer);
}

void EffectDisplayModule::refresh(void* localPlayer) {
    static EffectListResolver resolver;

    std::vector<RawEffect> raw;
    if (!resolver.refresh(localPlayer, raw)) return; // not resolved (yet) — keep the current display

    // --- duration sync detection ---
    // Find a probe effect: the one with the longest finite duration.
    int probeId = -1;
    int probeDur = -1;
    for (const auto& fx : raw) {
        if (fx.duration >= 0 && fx.duration < 2000000000 && fx.duration > probeDur) {
            probeDur = fx.duration;
            probeId = fx.id;
        }
    }

    bool probeStillActive = false;
    int probeCurrent = -2;
    for (const auto& fx : raw) {
        if (fx.id == m_probeId) {
            probeStillActive = true;
            probeCurrent = fx.duration;
            break;
        }
    }

    if (m_probeId < 0 || !probeStillActive) {
        m_probeId = probeId;
        m_probeRaw = probeDur;
        m_probeTick = m_tickCounter;
    } else if (m_durationsTick && m_tickCounter - m_probeTick >= 20) {
        // After ~1s: if the stored duration did not decrease, the client is
        // not ticking durations locally and we must track them ourselves.
        if (probeCurrent >= m_probeRaw && probeCurrent >= 0) m_durationsTick = false;
        m_probeTick = m_tickCounter;
        m_probeRaw = probeCurrent;
    }

    // --- build the display list ---
    std::vector<EffectInfo> next;
    next.reserve(raw.size());
    for (const auto& fx : raw) {
        if (m_hideInvisible && !fx.visible) continue;
        if (m_hideAmbient && fx.ambient) continue;

        EffectInfo info{fx.id, fx.duration, fx.amplifier, fx.visible, fx.ambient};

        if (info.duration >= 0) {
            if (m_hideExpiring && info.duration < m_expireSeconds * 20) continue;
            // Keep an observation base per effect so the remaining time can
            // be tracked locally if the client does not tick durations.
            auto it = m_staticBase.find(fx.id);
            if (it == m_staticBase.end() || it->second.raw != fx.duration) {
                m_staticBase[fx.id] = {fx.duration, m_tickCounter};
            }
            if (!m_durationsTick) {
                const StaticBase& base = m_staticBase[fx.id];
                info.duration = base.raw - (m_tickCounter - base.tick);
                if (info.duration < 0) info.duration = 0;
            }
        }

        next.push_back(info);
    }

    if (m_sortByDuration) {
        std::sort(next.begin(), next.end(), [](const EffectInfo& a, const EffectInfo& b) {
            const bool aInf = a.duration < 0;
            const bool bInf = b.duration < 0;
            if (aInf != bInf) return bInf;
            return a.duration < b.duration;
        });
    }

    if (m_maxEffects > 0 && static_cast<int>(next.size()) > m_maxEffects) {
        next.resize(static_cast<std::size_t>(m_maxEffects));
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_effects.swap(next);
    }
}

void EffectDisplayModule::onFrame() {
    if (!enabled) return;

    std::vector<EffectInfo> effects;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        effects = m_effects;
    }
    if (effects.empty()) return;

    std::vector<PLModMenu_DrawCommand> cmds;
    cmds.reserve(effects.size() * 5 + 1);

    const float panelW = std::max(m_panelWidth, 100.0f);
    const float iconW = m_showIcons ? (m_iconSize + 10.0f) : 0.0f;
    const float rowH = std::max(m_iconSize, m_size * 1.2f) + m_spacing;
    const bool horizontal = (m_direction == "Horizontal");
    const float pad = 8.0f;
    const float totalH = static_cast<float>(effects.size()) * rowH - m_spacing;

    if (m_background) {
        PLModMenu_DrawCommand bg = {};
        bg.type = PL_DRAW_RECT_FILLED;
        bg.x = hudPosX - pad;
        bg.y = hudPosY - pad;
        bg.w = panelW + pad * 2.0f;
        bg.h = totalH + pad * 2.0f;
        const std::uint32_t alpha = static_cast<std::uint32_t>(m_backgroundOpacity * 255.0f) & 0xFFu;
        bg.color = (m_backgroundColorHex & 0x00FFFFFFu) | (alpha << 24);
        cmds.push_back(bg);
    }

    for (std::size_t i = 0; i < effects.size(); ++i) {
        const EffectInfo& fx = effects[i];
        const EffectDef& def = effectDef(fx.id);

        const float rx = horizontal ? hudPosX + static_cast<float>(i) * (panelW + m_spacing) : hudPosX;
        const float ry = horizontal ? hudPosY : hudPosY + static_cast<float>(i) * rowH;
        const float cy = ry + rowH * 0.5f;

        if (m_showIcons) {
            const float cx = rx + m_iconSize * 0.5f;

            PLModMenu_DrawCommand rim = {};
            rim.type = PL_DRAW_CIRCLE_FILLED;
            rim.x = cx;
            rim.y = cy;
            rim.size = m_iconSize + 4.0f;
            rim.color = 0x40000000u;
            cmds.push_back(rim);

            PLModMenu_DrawCommand icon = {};
            icon.type = PL_DRAW_CIRCLE_FILLED;
            icon.x = cx;
            icon.y = cy;
            icon.size = m_iconSize;
            icon.color = 0xFF000000u | def.color;
            cmds.push_back(icon);

            const float gs = m_iconSize * 0.42f;
            const float gw = calcTextWidth(def.glyph, gs);
            PLModMenu_DrawCommand glyph = {};
            glyph.type = PL_DRAW_TEXT;
            glyph.x = cx - gw * 0.5f;
            glyph.y = cy + gs * 0.35f;
            glyph.w = 0.0f;
            glyph.h = gs;
            glyph.size = gs;
            glyph.color = glyphColor(def.color);
            glyph.text = def.glyph;
            cmds.push_back(glyph);
        }

        const float textX = rx + iconW;
        const float baseY = cy + m_size * 0.4f;

        std::string line = m_showNames ? std::string(def.name) : std::string();
        if (m_showAmplifier && fx.amplifier > 0) {
            if (!line.empty()) line += ' ';
            if (m_romanNumerals) {
                const char* r = romanNumeral(fx.amplifier);
                if (r && *r) line += r;
                else line += "+" + std::to_string(fx.amplifier);
            } else {
                line += "+" + std::to_string(fx.amplifier);
            }
        }
        if (!line.empty()) {
            PLModMenu_DrawCommand nameCmd = {};
            nameCmd.type = PL_DRAW_TEXT;
            nameCmd.x = textX;
            nameCmd.y = baseY;
            nameCmd.w = 0.0f;
            nameCmd.h = m_size;
            nameCmd.size = m_size;
            nameCmd.color = m_textColorHex;
            nameCmd.text = line;
            cmds.push_back(nameCmd);
        }

        if (m_showTimers) {
            const std::string timer = formatDuration(fx.duration);
            const float tw = calcTextWidth(timer, m_size);
            PLModMenu_DrawCommand timerCmd = {};
            timerCmd.type = PL_DRAW_TEXT;
            timerCmd.x = rx + panelW - tw - 4.0f;
            timerCmd.y = baseY;
            timerCmd.w = 0.0f;
            timerCmd.h = m_size;
            timerCmd.size = m_size;
            timerCmd.color = m_textColorHex;
            timerCmd.text = timer;
            cmds.push_back(timerCmd);
        }
    }

    ::submitDrawCommands(moduleId, cmds);
}

void EffectDisplayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();

    if (j.contains("m_size")) m_size = j["m_size"].get<float>();
    if (j.contains("m_iconSize")) m_iconSize = j["m_iconSize"].get<float>();
    if (j.contains("m_spacing")) m_spacing = j["m_spacing"].get<float>();
    if (j.contains("m_panelWidth")) m_panelWidth = j["m_panelWidth"].get<float>();

    if (j.contains("m_showIcons")) m_showIcons = j["m_showIcons"].get<bool>();
    if (j.contains("m_showNames")) m_showNames = j["m_showNames"].get<bool>();
    if (j.contains("m_showTimers")) m_showTimers = j["m_showTimers"].get<bool>();
    if (j.contains("m_showAmplifier")) m_showAmplifier = j["m_showAmplifier"].get<bool>();
    if (j.contains("m_romanNumerals")) m_romanNumerals = j["m_romanNumerals"].get<bool>();

    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_maxEffects")) m_maxEffects = j["m_maxEffects"].get<int>();
    if (j.contains("m_hideAmbient")) m_hideAmbient = j["m_hideAmbient"].get<bool>();
    if (j.contains("m_hideInvisible")) m_hideInvisible = j["m_hideInvisible"].get<bool>();
    if (j.contains("m_hideExpiring")) m_hideExpiring = j["m_hideExpiring"].get<bool>();
    if (j.contains("m_expireSeconds")) m_expireSeconds = j["m_expireSeconds"].get<int>();
    if (j.contains("m_sortByDuration")) m_sortByDuration = j["m_sortByDuration"].get<bool>();

    if (j.contains("m_direction")) {
        std::string d = j["m_direction"].get<std::string>();
        if (const auto pos = d.find(','); pos != std::string::npos) d.resize(pos);
        if (d == "Horizontal" || d == "Vertical") m_direction = std::move(d);
    }

    auto loadHex = [&](const char* key, std::uint32_t& target) {
        if (!j.contains(key)) return;
        std::string hex = j[key].get<std::string>();
        if (hex.length() > 1 && hex[0] == '#') {
            try { target = std::stoul(hex.substr(1), nullptr, 16); } catch (...) {}
        }
    };
    loadHex("m_backgroundColorHex", m_backgroundColorHex);
    loadHex("m_textColorHex", m_textColorHex);
}

void EffectDisplayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;

    j["m_size"] = m_size;
    j["m_iconSize"] = m_iconSize;
    j["m_spacing"] = m_spacing;
    j["m_panelWidth"] = m_panelWidth;

    j["m_showIcons"] = m_showIcons;
    j["m_showNames"] = m_showNames;
    j["m_showTimers"] = m_showTimers;
    j["m_showAmplifier"] = m_showAmplifier;
    j["m_romanNumerals"] = m_romanNumerals;

    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_maxEffects"] = m_maxEffects;
    j["m_hideAmbient"] = m_hideAmbient;
    j["m_hideInvisible"] = m_hideInvisible;
    j["m_hideExpiring"] = m_hideExpiring;
    j["m_expireSeconds"] = m_expireSeconds;
    j["m_sortByDuration"] = m_sortByDuration;

    // Keep both radio options so the menu always offers them; the current
    // selection is stored first so it stays selected after a reload.
    j["m_direction"] = m_direction + "," + (m_direction == "Vertical" ? "Horizontal" : "Vertical");

    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%08X", m_backgroundColorHex);
    j["m_backgroundColorHex"] = std::string(hex);
    std::snprintf(hex, sizeof(hex), "#%08X", m_textColorHex);
    j["m_textColorHex"] = std::string(hex);
}
