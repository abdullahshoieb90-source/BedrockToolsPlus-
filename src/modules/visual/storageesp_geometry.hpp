#pragma once

#include "blockoutline_geometry.hpp"

#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Pure logic for the Storage ESP module: which block names count as storage,
// how a found block is turned into a box, how tracer lines are anchored to it,
// how a budgeted world scan walks the chunks around the player, and how found
// blocks are remembered/expired.
//
// Nothing here touches Minecraft memory, so every rule the module relies on
// can be exercised by the host tests (tests/storageesp_geometry_test.cpp).
namespace storageesp {

using bedrocktools::sdk::BlockPos;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;

// Storage groups the module can highlight. Each group has its own menu toggle
// and color, so `None` is the only value that is never drawn.
enum class StorageKind : std::uint8_t {
    None = 0,
    Chest,
    CopperChest,
    TrappedChest,
    EnderChest,
    ShulkerBox,
    Barrel,
    Hopper,
    Furnace,
    Dispenser,
    Count,
};

inline constexpr std::size_t kindCount = static_cast<std::size_t>(StorageKind::Count);

inline constexpr std::string_view stripNamespace(std::string_view name) {
    constexpr std::string_view prefix = "minecraft:";
    if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
    return name;
}

// Maps a block's full name onto a highlight group. Names come from the game's
// own HashedString, so they are already lower-case and namespaced; anything the
// module does not consider storage maps to None.
inline StorageKind classify(std::string_view rawName) {
    const std::string_view name = stripNamespace(rawName);
    if (name.empty()) return StorageKind::None;

    if (name == "chest") return StorageKind::Chest;
    if (name == "trapped_chest") return StorageKind::TrappedChest;
    if (name == "ender_chest") return StorageKind::EnderChest;

    // Copper chests ship as one block per oxidation stage ("copper_chest",
    // "exposed_copper_chest", "weathered_copper_chest", "oxidized_copper_chest")
    // and again per waxed stage, so a suffix test covers every variant a world
    // can contain — including builds that keep oxidation in a block property
    // instead and only ever report the base name.
    if (name == "copper_chest" || name.ends_with("_copper_chest")) {
        return StorageKind::CopperChest;
    }

    // Colored boxes are "<color>_shulker_box"; Bedrock also still ships the
    // undyed variant under two different names across versions.
    if (name == "shulker_box" || name == "undyed_shulker_box" ||
        name.ends_with("_shulker_box")) {
        return StorageKind::ShulkerBox;
    }

    if (name == "barrel") return StorageKind::Barrel;
    if (name == "hopper") return StorageKind::Hopper;
    // Exact names rather than a "furnace" suffix: Bedrock keeps the burning
    // state in a block property, and a suffix test would also match the
    // furnace minecart entity.
    if (name == "furnace" || name == "blast_furnace" || name == "smoker") {
        return StorageKind::Furnace;
    }
    if (name == "dispenser" || name == "dropper") return StorageKind::Dispenser;
    return StorageKind::None;
}

// One bool per highlight group. The module keeps these as plain settings (so
// the launcher menu can list them) and hands this view to the pure helpers.
struct CategoryFilter {
    bool chests = true;
    bool copperChests = true;
    bool trappedChests = true;
    bool enderChests = true;
    bool shulkerBoxes = true;
    bool barrels = true;
    bool hoppers = true;
    bool furnaces = true;
    bool dispensers = true;
};

inline constexpr bool enabled(const CategoryFilter& filter, StorageKind kind) {
    switch (kind) {
        case StorageKind::Chest: return filter.chests;
        case StorageKind::CopperChest: return filter.copperChests;
        case StorageKind::TrappedChest: return filter.trappedChests;
        case StorageKind::EnderChest: return filter.enderChests;
        case StorageKind::ShulkerBox: return filter.shulkerBoxes;
        case StorageKind::Barrel: return filter.barrels;
        case StorageKind::Hopper: return filter.hoppers;
        case StorageKind::Furnace: return filter.furnaces;
        case StorageKind::Dispenser: return filter.dispensers;
        case StorageKind::None:
        case StorageKind::Count:
            break;
    }
    return false;
}

// Signed integer division that rounds towards negative infinity, so block
// coordinates divide into chunk indices correctly below zero.
inline constexpr int floorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

inline constexpr int kChunkSize = 16; // a chunk column is 16x16 blocks

// Distance from `anchor` to the closest block of the [first, last] range on one
// axis; zero when the anchor sits inside it.
inline constexpr int axisGap(int anchor, int first, int last) {
    if (anchor < first) return first - anchor;
    if (anchor > last) return anchor - last;
    return 0;
}

struct ScanCell {
    int cx = 0; // chunk offset from the anchor's chunk, on X
    int cz = 0; // chunk offset from the anchor's chunk, on Z
};

// The volume a single sweep covers. Scanning works in whole chunk columns
// because that is the granularity the world is stored in: inside a column the
// module checks every block of a vertical window around the player.
struct ScanRegion {
    BlockPos anchor{};       // block the sweep is centered on (the player's feet)
    int blockRadius = 24;    // horizontal reach in blocks
    int verticalRadius = 16; // blocks above and below the anchor layer
};

inline constexpr int chunkRadiusFor(const ScanRegion& region) {
    const int radius = region.blockRadius / kChunkSize;
    return radius < 1 ? 1 : radius;
}

inline constexpr std::size_t blocksPerCell(const ScanRegion& region) {
    return static_cast<std::size_t>(kChunkSize) * static_cast<std::size_t>(kChunkSize) *
           static_cast<std::size_t>(2 * region.verticalRadius + 1);
}

inline constexpr std::size_t maxCellCount(const ScanRegion& region) {
    const int chunkRadius = chunkRadiusFor(region);
    const std::size_t side = static_cast<std::size_t>(2 * chunkRadius + 1);
    return side * side;
}

// True when the chunk column could hold a block within blockRadius of the
// anchor; the corners of the chunk grid are skipped for a round scan area.
inline constexpr bool cellInRadius(const ScanRegion& region, int cellX, int cellZ) {
    const int anchorChunkX = floorDiv(region.anchor.x, kChunkSize) * kChunkSize;
    const int anchorChunkZ = floorDiv(region.anchor.z, kChunkSize) * kChunkSize;
    const int gapX = axisGap(region.anchor.x, anchorChunkX + cellX * kChunkSize,
                             anchorChunkX + cellX * kChunkSize + kChunkSize - 1);
    const int gapZ = axisGap(region.anchor.z, anchorChunkZ + cellZ * kChunkSize,
                             anchorChunkZ + cellZ * kChunkSize + kChunkSize - 1);
    const double distance = static_cast<double>(gapX) * gapX + static_cast<double>(gapZ) * gapZ;
    return distance <= static_cast<double>(region.blockRadius) * region.blockRadius;
}

// Orders the cells of one Chebyshev ring around the anchor chunk. `index` must
// be < 8 * ring; the four sides are walked clockwise without repeating corners.
inline constexpr ScanCell cellInShell(int ring, int index) {
    if (ring <= 0) return {0, 0};
    const int side = 2 * ring;
    switch (index / side) {
        case 0: return {-ring + (index % side), -ring};
        case 1: return {ring, -ring + (index % side)};
        case 2: return {ring - (index % side), ring};
        default: return {-ring, ring - (index % side)};
    }
}

inline constexpr int cellPriority(const ScanRegion& region, ScanCell cell) {
    const int centerX = floorDiv(region.anchor.x, kChunkSize) * kChunkSize + cell.cx * kChunkSize + kChunkSize / 2;
    const int centerZ = floorDiv(region.anchor.z, kChunkSize) * kChunkSize + cell.cz * kChunkSize + kChunkSize / 2;
    const int dx = centerX - region.anchor.x;
    const int dz = centerZ - region.anchor.z;
    return dx * dx + dz * dz;
}

// Fills `out` with the chunk columns a sweep has to visit, closest to the
// anchor first so a scan that runs out of budget near the player still covers
// the interesting blocks. Returns the number of written cells.
inline std::size_t collectScanCells(const ScanRegion& region, ScanCell* out, std::size_t capacity) {
    if (!out || capacity == 0) return 0;

    struct ScoredCell {
        ScanCell cell;
        int priority;
        int order;
    };

    // Only a handful of columns are ever in range, so sorting them here costs
    // nothing and keeps the ordering stable without allocating in the scan.
    std::vector<ScoredCell> cells;
    const int chunkRadius = chunkRadiusFor(region);
    for (int ring = 0; ring <= chunkRadius; ++ring) {
        const int shellSize = ring == 0 ? 1 : 8 * ring;
        for (int index = 0; index < shellSize; ++index) {
            const ScanCell cell = cellInShell(ring, index);
            if (!cellInRadius(region, cell.cx, cell.cz)) continue;
            cells.push_back({cell, cellPriority(region, cell), static_cast<int>(cells.size())});
        }
    }
    std::stable_sort(cells.begin(), cells.end(), [](const ScoredCell& a, const ScoredCell& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.order < b.order;
    });

    const std::size_t count = std::min(cells.size(), capacity);
    for (std::size_t i = 0; i < count; ++i) out[i] = cells[i].cell;
    return count;
}

// Positive modulo, so the column rotation below stays in range for a negative
// shift.
inline constexpr int floorMod(int value, int modulus) {
    return ((value % modulus) + modulus) % modulus;
}

// Columns inside a chunk cell, nearest to the cell center first. The table is
// static because the center of a 16x16 cell is always the same place;
// blockInCell() rotates it so that "the center" is the player's own column.
inline const std::array<std::uint8_t, kChunkSize * kChunkSize>& cellColumnOrder() {
    static const std::array<std::uint8_t, kChunkSize * kChunkSize> order = [] {
        std::array<std::uint8_t, kChunkSize * kChunkSize> result{};
        std::size_t written = 0;
        const int center = kChunkSize / 2;
        for (int ring = 0; ring <= kChunkSize; ++ring) {
            for (int z = 0; z < kChunkSize; ++z) {
                for (int x = 0; x < kChunkSize; ++x) {
                    const int distance =
                        std::max(std::abs(x - center), std::abs(z - center));
                    if (distance != ring) continue;
                    if (written >= result.size()) break;
                    result[written++] = static_cast<std::uint8_t>(z * kChunkSize + x);
                }
            }
        }
        return result;
    }();
    return order;
}

// The block a scan cursor points at: `blockIndex` walks the cell from the
// anchor's own layer outwards (0, -1, +1, -2, +2, ...) and each layer outwards
// from the anchor's own column, so the block the player is standing in is
// always the first thing a sweep checks and their surroundings are refreshed
// before the far side of the area.
inline BlockPos blockInCell(const ScanRegion& region, const ScanCell& cell, std::size_t blockIndex) {
    const std::size_t perLayer = kChunkSize * kChunkSize;
    const std::size_t layer = blockIndex / perLayer;
    const std::size_t column = blockIndex % perLayer;

    const int dy = (layer % 2 == 0) ? static_cast<int>(layer / 2)
                                   : -static_cast<int>((layer + 1) / 2);

    const int anchorChunkX = floorDiv(region.anchor.x, kChunkSize) * kChunkSize;
    const int anchorChunkZ = floorDiv(region.anchor.z, kChunkSize) * kChunkSize;
    const int shiftX = (region.anchor.x - anchorChunkX) - kChunkSize / 2;
    const int shiftZ = (region.anchor.z - anchorChunkZ) - kChunkSize / 2;

    const std::uint8_t packed = cellColumnOrder()[column % perLayer];
    const int localX = floorMod(packed % kChunkSize + shiftX, kChunkSize);
    const int localZ = floorMod(packed / kChunkSize + shiftZ, kChunkSize);

    return {anchorChunkX + cell.cx * kChunkSize + localX,
            region.anchor.y + dy,
            anchorChunkZ + cell.cz * kChunkSize + localZ};
}

// A block the scan found. Entries live as long as the scan area can still reach
// them: while a position stays inside the area the sweep confirms it again (or
// drops it the moment it stops being storage), so nothing needs a timer.
struct FoundBlock {
    BlockPos position{};
    StorageKind kind = StorageKind::None;
};

inline constexpr std::uint64_t packPos(const BlockPos& position) {
    // 26 bits of X, 26 of Z, 12 of Y: enough for every world coordinate
    // Bedrock can reach, and cheap enough to hash on every scanned block.
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.x)) & 0x3FFFFFFull) << 38 |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.z)) & 0x3FFFFFFull) << 12 |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.y)) & 0xFFFull);
}

inline constexpr BlockPos unpackPos(std::uint64_t packed) {
    auto signExtend = [](std::uint64_t value, unsigned bits) {
        const std::uint64_t sign = std::uint64_t{1} << (bits - 1);
        return static_cast<int>((value ^ sign) - sign);
    };
    return {signExtend(packed >> 38, 26), signExtend(packed & 0xFFF, 12),
            signExtend((packed >> 12) & 0x3FFFFFF, 26)};
}

// Everything the module remembers between sweeps. Blocks are refreshed every
// time a sweep reaches them, so a broken or moved container drops out on its
// own instead of needing a block-change hook.
class StorageCache {
public:
    // A world with a lot of storage is exactly what the module is for, but the
    // cache still has to stay small enough to copy on every frame.
    static constexpr std::size_t kCapacity = 4096;

    void put(const BlockPos& position, StorageKind kind) {
        const std::uint64_t key = packPos(position);
        if (m_entries.size() >= kCapacity && !m_entries.count(key)) return;
        auto& slot = m_entries[key];
        slot.position = position;
        slot.kind = kind;
    }

    bool erase(const BlockPos& position) { return m_entries.erase(packPos(position)) != 0; }

    bool contains(const BlockPos& position) const { return m_entries.count(packPos(position)) != 0; }

    // Drops positions the scan area can no longer reach, because nothing will
    // ever confirm or clear them again. The margins make the test a superset of
    // the area, so a highlight does not blink while the player crosses a chunk
    // border and the set of scanned columns changes underneath it.
    void dropUnreachable(const BlockPos& anchor, int horizontalMargin, int verticalMargin) {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            const auto& position = it->second.position;
            const bool reachable =
                std::abs(position.x - anchor.x) <= horizontalMargin &&
                std::abs(position.z - anchor.z) <= horizontalMargin &&
                std::abs(position.y - anchor.y) <= verticalMargin;
            if (reachable) ++it;
            else it = m_entries.erase(it);
        }
    }

    void clear() { m_entries.clear(); }

    std::size_t size() const { return m_entries.size(); }
    bool empty() const { return m_entries.empty(); }

    const FoundBlock* find(const BlockPos& position) const {
        const auto it = m_entries.find(packPos(position));
        return it == m_entries.end() ? nullptr : &it->second;
    }

    template <class Fn>
    void forEach(Fn&& fn) const {
        for (const auto& [key, entry] : m_entries) fn(entry);
    }

    std::vector<FoundBlock> snapshot() const {
        std::vector<FoundBlock> result;
        result.reserve(m_entries.size());
        for (const auto& [key, entry] : m_entries) result.push_back(entry);
        return result;
    }

private:
    std::unordered_map<std::uint64_t, FoundBlock> m_entries;
};

// What one frame draws: found blocks that are close enough, in the enabled
// groups, limited to the nearest `maxCount` of them so a huge base cannot make
// the renderer submit thousands of boxes.
struct OverlayTarget {
    BlockPos position{};
    StorageKind kind = StorageKind::None;
    float distance = 0.0f;
};

inline void collectVisible(const std::vector<FoundBlock>& found,
                           const Vec3& camera,
                           float radius,
                           std::size_t maxCount,
                           const CategoryFilter& filter,
                           std::vector<OverlayTarget>& out) {
    out.clear();
    const float radiusSq = radius * radius;
    for (const auto& block : found) {
        if (block.kind == StorageKind::None || !enabled(filter, block.kind)) continue;

        const float centerX = static_cast<float>(block.position.x) + 0.5f;
        const float centerY = static_cast<float>(block.position.y) + 0.5f;
        const float centerZ = static_cast<float>(block.position.z) + 0.5f;
        const float dx = centerX - camera.x;
        const float dy = centerY - camera.y;
        const float dz = centerZ - camera.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq > radiusSq) continue;

        out.push_back({block.position, block.kind, std::sqrt(distanceSq)});
    }

    std::sort(out.begin(), out.end(), [](const OverlayTarget& a, const OverlayTarget& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        const std::uint64_t keyA = packPos(a.position);
        const std::uint64_t keyB = packPos(b.position);
        return keyA < keyB;
    });
    if (maxCount < out.size()) out.resize(maxCount);
}

// Chest-like blocks are smaller than a voxel in the game (1/16 margin on all
// sides but the bottom, 0.875 blocks tall), hoppers are a thin funnel, and
// everything else fills its voxel. "Model Sized Boxes" uses those bounds so
// neighbouring containers stay readable; turning it off draws full voxels.
inline constexpr blockoutline::Box makeStorageBox(const BlockPos& position,
                                                  StorageKind kind,
                                                  float expansion,
                                                  bool modelSized) {
    const float x = static_cast<float>(position.x);
    const float y = static_cast<float>(position.y);
    const float z = static_cast<float>(position.z);

    float inset = 0.0f;
    float minY = 0.0f;
    float maxY = 1.0f;
    if (modelSized) {
        switch (kind) {
            case StorageKind::Chest:
            case StorageKind::CopperChest:
            case StorageKind::TrappedChest:
            case StorageKind::EnderChest:
                inset = 0.0625f;
                minY = 0.0625f;
                maxY = 0.9375f;
                break;
            case StorageKind::Hopper:
                minY = 0.15625f;
                break;
            default:
                break;
        }
    }

    return {
        {x - expansion + inset, y - expansion + minY, z - expansion + inset},
        {x + 1.0f + expansion - inset, y + maxY + expansion, z + 1.0f + expansion - inset},
    };
}

// Boxes for a highlight group, in world space.
inline std::vector<blockoutline::Box> boxesForKind(const std::vector<OverlayTarget>& targets,
                                                   StorageKind kind,
                                                   float expansion,
                                                   bool modelSized) {
    std::vector<blockoutline::Box> boxes;
    for (const auto& target : targets) {
        if (target.kind != kind) continue;
        boxes.push_back(makeStorageBox(target.position, kind, expansion, modelSized));
    }
    return boxes;
}

// The middle of the box a highlight draws for one container. Tracers aim here
// rather than at the voxel center so the line still meets the block when model
// sizing shrinks it (a chest is only 0.875 blocks tall).
inline Vec3 storageCenter(const BlockPos& position, StorageKind kind, bool modelSized) {
    const blockoutline::Box box = makeStorageBox(position, kind, 0.0f, modelSized);
    return {(box.min.x + box.max.x) * 0.5f,
            (box.min.y + box.max.y) * 0.5f,
            (box.min.z + box.max.z) * 0.5f};
}

// Depth a tracer start is pulled to, in blocks in front of the camera. A
// segment that begins exactly on the eye plane has zero view depth, and a zero
// depth is a divide by zero in the projection: mobile GLES drivers drop the
// primitive instead of clipping it, which is why a start that lands there (the
// feet of a player looking level, or the dropped anchor of a level view) is
// pulled a little way down the line towards its container. The value only has
// to be comfortably above zero (and above the game's own near plane); it does
// not move the line on screen, because the start stays on the line it already
// had, just closer to its container.
inline constexpr float kTracerNearPlane = 0.20f;

// Distance the width of a tracer's near end is measured at, so the strip cannot
// widen into a wedge where it leaves the camera.
inline constexpr float kTracerWidthFloor = kTracerNearPlane;

// Smallest angle, in radians, between the two eye rays that reach a tracer's
// ends for the tracer to be drawn at all. Below it the segment has no screen
// extent to speak of: every point of it lands on the same pixel, and any ribbon
// widened from it is degenerate, because both of its sides project onto that
// same point. With the shipped anchors this only happens for a container on the
// line through the camera and the anchor, which no camera angle could show as
// anything but a dot.
inline constexpr float kTracerMinimumSpan = 0.005f;

inline constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

// What the tracer pass knows about the view, resolved once per frame.
struct TracerView {
    Vec3 camera{};
    // Zero vector when the game did not expose a usable rotation; clipping then
    // falls back to padding the start away from the camera by distance.
    Vec3 forward{};
};

// The camera's look direction from the local player's rotation, using the
// convention the game itself uses: rot.x is pitch (negative = looking up),
// rot.y is yaw in degrees, yaw 0 looks towards +Z (south) and grows towards -X
// (west). The render pass only gets a camera *position* out of the level
// renderer, so this is how it learns which side of the eye plane a point is on.
// A rotation that cannot be one (not finite, or a "pitch" outside +-90) comes
// back as an empty vector, i.e. "unknown": the clipping then repairs a start
// near the eye instead of culling every line the player is looking at.
inline Vec3 viewForward(const Vec2& rotation) {
    if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) ||
        std::fabs(rotation.x) > 91.0f) {
        return {};
    }
    const float yaw = rotation.y * kDegreesToRadians;
    const float pitch = rotation.x * kDegreesToRadians;
    const float cosPitch = std::cos(pitch);
    return {-std::sin(yaw) * cosPitch, -std::sin(pitch), std::cos(yaw) * cosPitch};
}

// How far in front of the camera a world point sits; negative behind it.
inline constexpr float viewDepth(const Vec3& point, const Vec3& camera, const Vec3& forward) {
    return (point.x - camera.x) * forward.x + (point.y - camera.y) * forward.y +
           (point.z - camera.z) * forward.z;
}

// The angle between the two eye rays that reach a segment's ends, in radians.
// Zero means both ends sit on the same ray: the projection then puts every
// point of the segment on one pixel, so the line cannot be seen at all, however
// it is widened. This is the reason a tracer may not start at the eye, and what
// the origin table below is checked against.
inline float viewAngleSpan(const Vec3& from, const Vec3& to, const Vec3& camera) {
    const float ax = from.x - camera.x;
    const float ay = from.y - camera.y;
    const float az = from.z - camera.z;
    const float bx = to.x - camera.x;
    const float by = to.y - camera.y;
    const float bz = to.z - camera.z;
    const float lengthA = std::sqrt(ax * ax + ay * ay + az * az);
    const float lengthB = std::sqrt(bx * bx + by * by + bz * bz);
    if (lengthA < 0.00001f || lengthB < 0.00001f) return 0.0f;
    const float cosine = (ax * bx + ay * by + az * bz) / (lengthA * lengthB);
    return std::acos(std::clamp(cosine, -1.0f, 1.0f));
}

inline Vec3 lerp(const Vec3& from, const Vec3& to, float t) {
    return {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.z + (to.z - from.z) * t};
}

// Clips one tracer against the eye plane, in place. Returns false when the
// container itself is at or behind it — no part of that line could ever be seen,
// and submitting it would put a degenerate vertex in the batch.
inline bool clipTracerEdge(blockoutline::Edge& edge, const TracerView& view, float nearPlane) {
    const float forwardLength = std::sqrt(view.forward.x * view.forward.x +
                                          view.forward.y * view.forward.y +
                                          view.forward.z * view.forward.z);
    // Written negated so a non-finite direction also lands here: every
    // comparison against NaN is false, and a NaN forward would otherwise turn
    // the clip below into NaN vertices.
    if (!(forwardLength >= 0.5f)) {
        // No usable view direction: the only case that can be repaired is a
        // start sitting on the camera itself, so push it towards the container.
        const float dx = view.camera.x - edge.from.x;
        const float dy = view.camera.y - edge.from.y;
        const float dz = view.camera.z - edge.from.z;
        if (dx * dx + dy * dy + dz * dz >= nearPlane * nearPlane) return true;

        const float sx = edge.to.x - edge.from.x;
        const float sy = edge.to.y - edge.from.y;
        const float sz = edge.to.z - edge.from.z;
        const float length = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (length < 0.00001f) return false;
        const float pad = std::min(nearPlane, length * 0.5f);
        edge.from = {edge.from.x + sx / length * pad, edge.from.y + sy / length * pad,
                     edge.from.z + sz / length * pad};
        return true;
    }

    const float depthTo = viewDepth(edge.to, view.camera, view.forward);
    if (depthTo <= nearPlane) return false;

    const float depthFrom = viewDepth(edge.from, view.camera, view.forward);
    if (depthFrom >= nearPlane) return true;

    // depthTo > nearPlane >= depthFrom, so the span cannot be zero.
    edge.from = lerp(edge.from, edge.to, (nearPlane - depthFrom) / (depthTo - depthFrom));
    return true;
}

// Tracer lines for one highlight group: a segment from the chosen origin to
// every container of that group, clipped to what the camera can actually see,
// ready for the same line renderer the box outlines use. `out` is reused across
// frames, so a busy base costs no allocation once the vector has grown to size.
inline void collectTracers(const std::vector<OverlayTarget>& targets,
                           StorageKind kind,
                           const Vec3& origin,
                           bool modelSized,
                           const TracerView& view,
                           std::vector<blockoutline::Edge>& out) {
    out.clear();
    for (const auto& target : targets) {
        if (target.kind != kind) continue;
        blockoutline::Edge line{origin, storageCenter(target.position, kind, modelSized)};
        if (!clipTracerEdge(line, view, kTracerNearPlane)) continue;
        if (viewAngleSpan(line.from, line.to, view.camera) < kTracerMinimumSpan) continue;
        out.push_back(line);
    }
}

// Block types are stable for a whole session, and a sweep looks at the same
// handful of stone/dirt/planks pointers millions of times, so classification
// is memoized on the game's BlockLegacy pointer. Direct-mapped on purpose: a
// lookup is one mask and one compare, and a stale entry can only ever cost one
// wrong group until it is replaced.
class TypeKindCache {
public:
    template <class Resolver>
    StorageKind kindFor(const void* identity, Resolver&& resolve) {
        if (!identity) return StorageKind::None;
        const std::size_t slot = slotFor(identity);
        if (m_slots[slot].identity == identity) {
            ++m_hits;
            return static_cast<StorageKind>(m_slots[slot].kind);
        }
        ++m_misses;
        const StorageKind kind = static_cast<StorageKind>(resolve());
        m_slots[slot] = {identity, static_cast<std::uint8_t>(kind)};
        return kind;
    }

    void clear() {
        for (auto& slot : m_slots) slot = {};
        m_hits = 0;
        m_misses = 0;
    }

    std::size_t hits() const { return m_hits; }
    std::size_t misses() const { return m_misses; }

private:
    static constexpr std::size_t kSlots = 512;

    struct Slot {
        const void* identity = nullptr;
        std::uint8_t kind = 0;
    };

    static std::size_t slotFor(const void* identity) {
        const auto value = reinterpret_cast<std::uintptr_t>(identity);
        return (value >> 4) & (kSlots - 1);
    }

    std::array<Slot, kSlots> m_slots{};
    std::size_t m_hits = 0;
    std::size_t m_misses = 0;
};

// Menu radios persist as "<selectedIndex>,<Option1>,<Option2>,...". Writing the
// value out lists every option, which is what the launcher needs to build the
// picker, and reading it back accepts that whole string, a bare index (older
// configs stored one) or a bare option name.
inline std::string radioValue(std::span<const std::string_view> names, int index, int fallback) {
    if (index < 0 || static_cast<std::size_t>(index) >= names.size()) index = fallback;
    std::string value = std::to_string(index);
    for (const std::string_view name : names) {
        value += ',';
        value += name;
    }
    return value;
}

inline int resolveRadio(std::span<const std::string_view> names, int fallback, std::string_view value) {
    if (value.empty() || names.empty()) return fallback;
    const std::size_t comma = value.find(',');
    const std::string_view head = value.substr(0, comma);

    bool numeric = !head.empty();
    for (char ch : head) {
        if (ch < '0' || ch > '9') {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        int index = 0;
        for (char ch : head) index = index * 10 + (ch - '0');
        if (index >= 0 && static_cast<std::size_t>(index) < names.size()) return index;
        return fallback;
    }

    for (std::size_t i = 0; i < names.size(); ++i) {
        if (head == names[i]) return static_cast<int>(i);
    }
    return fallback;
}

// "Scan Speed" is a menu radio: each option is a number of block positions the
// sweep is allowed to check per game tick. Keeping it a radio instead of a
// slider matches how the launcher renders big ranges and keeps the cost
// explicit.
inline constexpr std::size_t kScanSpeedCount = 4;
inline constexpr std::array<std::string_view, kScanSpeedCount> kScanSpeedNames = {
    "Relaxed", "Balanced", "Fast", "Instant",
};
inline constexpr std::array<std::size_t, kScanSpeedCount> kScanSpeedBudgets = {
    3000, 10000, 25000, 60000,
};

inline constexpr int kDefaultScanSpeed = 1;

inline constexpr std::size_t scanSpeedBudget(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kScanSpeedCount) {
        return kScanSpeedBudgets[kDefaultScanSpeed];
    }
    return kScanSpeedBudgets[static_cast<std::size_t>(index)];
}

inline std::string scanSpeedRadioValue(int index) {
    return radioValue(kScanSpeedNames, index, kDefaultScanSpeed);
}

inline int resolveScanSpeed(std::string_view value) {
    return resolveRadio(kScanSpeedNames, kDefaultScanSpeed, value);
}

// Where a tracer line starts. Both options sit deliberately *off* the eye ray:
// a segment that starts at the eye lies on a single view ray, so the game's
// projection collapses every point of it onto one pixel — the line is reduced
// to a dot, and a camera-facing strip built on that ray is seen edge-on, i.e.
// it covers no pixels at all. That is why a tracer anchored to the camera never
// showed up. Starting a little away from the eye is also what puts the line
// across the screen instead of into the crosshair.
//
// "Bottom" is the classic snapline: the line starts just under the eye, so it
// crosses the lower part of the screen on its way to the container. "Feet"
// starts it at the block the player stands in, which reads better in third
// person and shows direction relative to the player instead of the view.
inline constexpr float kTracerAnchorDrop = 0.45f;

enum class TracerOrigin : std::uint8_t {
    Bottom = 0,
    Feet,
    Count,
};

inline constexpr std::size_t kTracerOriginCount = static_cast<std::size_t>(TracerOrigin::Count);
inline constexpr std::array<std::string_view, kTracerOriginCount> kTracerOriginNames = {
    "Bottom", "Feet",
};

inline constexpr int kDefaultTracerOrigin = static_cast<int>(TracerOrigin::Bottom);

// What older configs stored for the option that used to sit exactly on the eye.
// "Camera" is the common one; the rest are the names other clients use for the
// same anchor, so an imported config keeps pointing where its author meant.
inline constexpr std::array<std::string_view, 4> kLegacyTracerOriginNames = {
    "Camera", "Crosshair", "Eye", "Screen",
};

inline std::string tracerOriginRadioValue(int index) {
    return radioValue(kTracerOriginNames, index, kDefaultTracerOrigin);
}

inline int resolveTracerOrigin(std::string_view value) {
    const int resolved = resolveRadio(kTracerOriginNames, -1, value);
    if (resolved >= 0) return resolved;
    for (const std::string_view legacy : kLegacyTracerOriginNames) {
        if (value == legacy) return kDefaultTracerOrigin;
    }
    return kDefaultTracerOrigin;
}

// Resolves the configured origin to the world-space point tracers start from.
// `feet` is the local player position sampled on the game tick, since the
// render thread only knows the camera. The camera itself is never returned: see
// the note on TracerOrigin above.
inline Vec3 tracerOriginPoint(TracerOrigin origin, const Vec3& camera, const Vec3& feet) {
    switch (origin) {
        case TracerOrigin::Feet: return feet;
        case TracerOrigin::Bottom:
        case TracerOrigin::Count:
        default: return {camera.x, camera.y - kTracerAnchorDrop, camera.z};
    }
}

} // namespace storageesp
