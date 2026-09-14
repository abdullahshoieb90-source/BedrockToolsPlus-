// Host-side tests for the Storage ESP scan and highlight rules.
//
// Build: g++ -std=c++20 -I include -I src tests/storageesp_geometry_test.cpp
//        -o /tmp/storageesp_geometry_test
// Run:   /tmp/storageesp_geometry_test

#include "modules/visual/storageesp_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::printf("  ok   %s\n", message);
    } else {
        std::printf("  FAIL %s\n", message);
        ++failures;
    }
}

bool near(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

using storageesp::ScanRegion;
using storageesp::StorageKind;

ScanRegion makeRegion(int x, int y, int z, int radius, int height) {
    ScanRegion region;
    region.anchor = {x, y, z};
    region.blockRadius = radius;
    region.verticalRadius = height;
    return region;
}

} // namespace

int main() {
    using namespace bedrocktools::sdk;

    std::printf("storage esp block classification\n");
    check(storageesp::classify("minecraft:chest") == StorageKind::Chest, "chest is a chest");
    check(storageesp::classify("chest") == StorageKind::Chest, "unnamespaced names still match");
    check(storageesp::classify("minecraft:trapped_chest") == StorageKind::TrappedChest,
          "trapped chests are their own group");
    check(storageesp::classify("minecraft:ender_chest") == StorageKind::EnderChest,
          "ender chests are not confused with normal chests");
    check(storageesp::classify("minecraft:undyed_shulker_box") == StorageKind::ShulkerBox,
          "undyed shulker boxes are shulker boxes");
    check(storageesp::classify("minecraft:light_blue_shulker_box") == StorageKind::ShulkerBox,
          "dyed shulker boxes are shulker boxes");
    check(storageesp::classify("minecraft:barrel") == StorageKind::Barrel, "barrel");
    check(storageesp::classify("minecraft:hopper") == StorageKind::Hopper, "hopper");
    check(storageesp::classify("minecraft:blast_furnace") == StorageKind::Furnace,
          "blast furnaces are furnaces");
    check(storageesp::classify("minecraft:smoker") == StorageKind::Furnace, "smoker");
    check(storageesp::classify("minecraft:dropper") == StorageKind::Dispenser,
          "droppers share the dispenser group");
    check(storageesp::classify("minecraft:stone") == StorageKind::None, "stone is not storage");
    check(storageesp::classify("minecraft:air") == StorageKind::None, "air is not storage");
    check(storageesp::classify("minecraft:furnace_minecart") == StorageKind::None,
          "entities are not matched by a block-name suffix");
    check(storageesp::classify("") == StorageKind::None, "an empty name is not storage");

    storageesp::CategoryFilter filter;
    filter.dispensers = false;
    check(storageesp::enabled(filter, StorageKind::Chest), "enabled groups are highlighted");
    check(!storageesp::enabled(filter, StorageKind::Dispenser),
          "a group turned off in the menu is skipped");
    check(!storageesp::enabled(filter, StorageKind::None), "None is never drawn");

    std::printf("storage esp box geometry\n");
    const BlockPos position{-4, 63, 9};
    const blockoutline::Box model = storageesp::makeStorageBox(position, StorageKind::Chest, 0.0f, true);
    check(near(model.min.x, -3.9375f) && near(model.max.x, -3.0625f),
          "a chest box keeps the 1/16 model margin on X and Z");
    check(near(model.min.y, 63.0625f) && near(model.max.y, 63.9375f),
          "a chest box is 0.875 blocks tall like the model");

    const blockoutline::Box voxel = storageesp::makeStorageBox(position, StorageKind::Chest, 0.0f, false);
    check(near(voxel.min.x, -4.0f) && near(voxel.max.x, -3.0f) && near(voxel.min.y, 63.0f) &&
              near(voxel.max.y, 64.0f),
          "without model sizing every group fills its voxel");

    const blockoutline::Box expanded = storageesp::makeStorageBox(position, StorageKind::Barrel, 0.002f, true);
    check(near(expanded.min.x, -4.002f) && near(expanded.max.z, 10.002f),
          "expansion grows the box outwards on every axis");

    const blockoutline::Box hopper = storageesp::makeStorageBox(position, StorageKind::Hopper, 0.0f, true);
    check(near(hopper.min.y, 63.15625f) && near(hopper.max.y, 64.0f),
          "a model-sized hopper box starts at the funnel");

    const auto edges = blockoutline::boxEdges(voxel);
    check(edges.size() == 12, "every highlighted block has twelve edges");
    const auto faces = blockoutline::boxFaces(voxel);
    check(faces.size() == 6, "every highlighted block has six fill faces");

    std::printf("storage esp scan region\n");
    check(storageesp::floorDiv(-1, 16) == -1 && storageesp::floorDiv(15, 16) == 0 &&
              storageesp::floorDiv(16, 16) == 1,
          "chunk division rounds towards negative infinity");

    check(storageesp::chunkRadiusFor(makeRegion(0, 0, 0, 8, 4)) == 1,
          "a small radius still scans the player's own chunk ring");
    check(storageesp::chunkRadiusFor(makeRegion(0, 0, 0, 64, 4)) == 4,
          "the chunk radius follows the block radius");

    const ScanRegion region = makeRegion(8, 64, -7, 24, 3);
    check(storageesp::blocksPerCell(region) == 16 * 16 * 7, "a cell covers 16x16 columns and the layer window");

    std::vector<storageesp::ScanCell> cells(storageesp::maxCellCount(region));
    const std::size_t cellCount =
        storageesp::collectScanCells(region, cells.data(), cells.size());
    cells.resize(cellCount);
    check(cellCount == 9, "a 24 block radius covers the full 3x3 chunk grid");
    check(cells[0].cx == 0 && cells[0].cz == 0, "the sweep starts in the player's own chunk");

    std::set<std::pair<int, int>> uniqueCells;
    for (const auto& cell : cells) uniqueCells.emplace(cell.cx, cell.cz);
    check(uniqueCells.size() == cells.size(), "no chunk column is scanned twice per sweep");

    const ScanRegion tight = makeRegion(8, 0, 8, 10, 2);
    std::vector<storageesp::ScanCell> tightCells(storageesp::maxCellCount(tight));
    const std::size_t tightCount =
        storageesp::collectScanCells(tight, tightCells.data(), tightCells.size());
    tightCells.resize(tightCount);
    check(tightCount == 5, "a scan area that does not reach the corners drops them");
    check(std::none_of(tightCells.begin(), tightCells.end(),
                       [](const storageesp::ScanCell& cell) {
                           return cell.cx != 0 && cell.cz != 0;
                       }),
          "only the corner columns are cut from the grid");

    std::printf("storage esp scan order\n");
    const BlockPos first = storageesp::blockInCell(region, cells[0], 0);
    check(first.x == region.anchor.x && first.y == region.anchor.y && first.z == region.anchor.z,
          "the first block of a sweep is the block the player stands in");

    bool layersAlternated = true;
    for (std::size_t layer = 0; layer < 7; ++layer) {
        const int expected = (layer % 2 == 0) ? int(layer / 2) : -int((layer + 1) / 2);
        const BlockPos probe = storageesp::blockInCell(region, cells[0], layer * 256);
        if (probe.y != region.anchor.y + expected) layersAlternated = false;
    }
    check(layersAlternated, "layers are walked outwards from the player's own layer");

    std::set<std::tuple<int, int, int>> visited;
    bool insideCell = true;
    const std::size_t perCell = storageesp::blocksPerCell(region);
    for (std::size_t cell = 0; cell < cellCount; ++cell) {
        const int baseX = 0 + cells[cell].cx * 16;
        const int baseZ = -16 + cells[cell].cz * 16;
        for (std::size_t index = 0; index < perCell; ++index) {
            const BlockPos block = storageesp::blockInCell(region, cells[cell], index);
            visited.emplace(block.x, block.y, block.z);
            if (block.x < baseX || block.x >= baseX + 16 || block.z < baseZ ||
                block.z >= baseZ + 16 || block.y < region.anchor.y - region.verticalRadius ||
                block.y > region.anchor.y + region.verticalRadius) {
                insideCell = false;
            }
        }
    }
    check(insideCell, "every scanned position stays inside its chunk column and layer window");
    check(visited.size() == cellCount * perCell, "a sweep visits each block of the region exactly once");
    std::set<int> columns;
    for (const std::uint8_t packed : storageesp::cellColumnOrder()) {
        columns.insert(packed % 16 * 100 + packed / 16);
    }
    check(storageesp::cellColumnOrder().size() == 256 && columns.size() == 256,
          "the column order covers every column of a chunk exactly once");

    std::printf("storage esp found-block cache\n");
    storageesp::StorageCache cache;
    check(cache.empty(), "a fresh cache is empty");
    cache.put({1, 2, 3}, StorageKind::Chest);
    cache.put({4, 5, 6}, StorageKind::EnderChest);
    cache.put({1, 2, 3}, StorageKind::Barrel);
    check(cache.size() == 2, "a refreshed position does not duplicate the entry");
    const auto* found = cache.find({1, 2, 3});
    check(found && found->kind == StorageKind::Barrel, "a sweep that re-finds a block updates its group");
    check(cache.erase({4, 5, 6}), "a block that stopped being storage is dropped immediately");
    check(!cache.erase({4, 5, 6}), "erasing twice reports nothing was removed");
    cache.put({20, 0, 0}, StorageKind::Hopper);
    cache.dropUnreachable({8, 0, 8}, 16, 8);
    check(cache.size() == 2 && cache.contains({20, 0, 0}),
          "positions the scan area can still reach stay remembered");
    cache.dropUnreachable({8, 0, 60}, 16, 8);
    check(cache.empty(), "blocks left outside the scan area are forgotten instead of never validated");
    cache.clear();
    check(cache.empty(), "clearing the cache drops everything");

    cache.put({0, 0, 0}, StorageKind::Chest);
    cache.put({0, 90, 0}, StorageKind::Chest);
    cache.put({40, 0, 0}, StorageKind::Barrel);
    cache.dropUnreachable({0, 2, 0}, 16, 8);
    check(cache.size() == 1 && cache.contains({0, 0, 0}),
          "the vertical margin drops tall offsets and the horizontal margin far ones");

    storageesp::StorageCache full;
    for (int i = 0; i < static_cast<int>(storageesp::StorageCache::kCapacity) + 64; ++i) {
        full.put({i, 0, 0}, StorageKind::Chest);
    }
    check(full.size() == storageesp::StorageCache::kCapacity,
          "the cache stops growing instead of eating memory in a giant base");

    std::printf("storage esp visible target selection\n");
    const std::vector<storageesp::FoundBlock> found1 = {
        {{0, 0, 0}, StorageKind::Chest},
        {{0, 0, 40}, StorageKind::Chest},
        {{0, 0, 5}, StorageKind::Dispenser},
        {{0, 0, 6}, StorageKind::Barrel},
    };
    std::vector<storageesp::OverlayTarget> targets;
    const Vec3 camera{0.5f, 0.5f, 0.5f};
    storageesp::CategoryFilter visibleFilter;
    visibleFilter.dispensers = false;
    storageesp::collectVisible(found1, camera, 24.0f, 64, visibleFilter, targets);
    check(targets.size() == 2, "targets outside the radius or in a disabled group are dropped");
    check(std::all_of(targets.begin(), targets.end(), [](const storageesp::OverlayTarget& target) {
              return target.kind == StorageKind::Chest || target.kind == StorageKind::Barrel;
          }),
          "the surviving targets keep their group");
    check(targets.front().position.z == 0 && targets.back().position.z == 6,
          "targets are ordered from the closest container outwards");

    storageesp::collectVisible(found1, camera, 24.0f, 1, visibleFilter, targets);
    check(targets.size() == 1, "the box cap keeps only the nearest highlights");

    storageesp::collectVisible(found1, camera, 24.0f, 64, storageesp::CategoryFilter{}, targets);
    check(targets.size() == 3, "enabling every group reveals the third container");

    const auto chestBoxes = storageesp::boxesForKind(targets, StorageKind::Chest, 0.002f, true);
    const auto barrelBoxes = storageesp::boxesForKind(targets, StorageKind::Barrel, 0.002f, true);
    check(chestBoxes.size() == 1 && barrelBoxes.size() == 1,
          "boxes are grouped per highlight color");
    check(near(chestBoxes.front().min.x, 0.0605f) && near(chestBoxes.front().max.x, 0.9395f),
          "a highlighted chest keeps its model margin plus the expansion");

    std::printf("storage esp block-type memoization\n");
    storageesp::TypeKindCache types;
    int resolved = 0;
    const int chestIdentity = 1;
    const int stoneIdentity = 2;
    auto resolveKind = [&](int marker) {
        ++resolved;
        return marker == 1 ? static_cast<int>(StorageKind::Chest) : static_cast<int>(StorageKind::None);
    };
    check(types.kindFor(&chestIdentity, [&] { return resolveKind(1); }) == StorageKind::Chest,
          "a storage block type resolves to its group");
    check(types.kindFor(&chestIdentity, [&] { return resolveKind(1); }) == StorageKind::Chest,
          "a repeated block type is answered from the cache");
    types.kindFor(&stoneIdentity, [&] { return resolveKind(0); });
    check(resolved == 2, "each block type is classified once per session");
    check(types.hits() == 1 && types.misses() == 2, "cache hits and misses are counted");
    types.clear();
    types.kindFor(&chestIdentity, [&] { return resolveKind(1); });
    check(resolved == 3, "clearing the cache (world or dimension change) forces a re-read");

    std::printf("storage esp scan speed setting\n");
    check(storageesp::kScanSpeedBudgets[0] < storageesp::kScanSpeedBudgets[3],
          "faster scan options check more blocks per tick");
    check(storageesp::scanSpeedBudget(0) == storageesp::kScanSpeedBudgets[0], "index maps to budget");
    check(storageesp::scanSpeedBudget(99) == storageesp::kScanSpeedBudgets[storageesp::kDefaultScanSpeed],
          "an out-of-range index falls back to the default budget");
    check(storageesp::resolveScanSpeed("1,Relaxed,Balanced,Fast,Instant") == 1,
          "the full launcher radio value resolves to its index");
    check(storageesp::resolveScanSpeed("2") == 2, "a bare numeric radio value resolves");
    check(storageesp::resolveScanSpeed("Instant") == 3, "a bare option name resolves");
    check(storageesp::resolveScanSpeed("nonsense") == storageesp::kDefaultScanSpeed,
          "an unknown option falls back to Balanced");
    check(storageesp::resolveScanSpeed(storageesp::scanSpeedRadioValue(3)) == 3,
          "the saved radio value round-trips");
    {
        const std::string saved = storageesp::scanSpeedRadioValue(1);
        std::size_t commas = 0;
        for (char ch : saved) {
            if (ch == ',') ++commas;
        }
        check(commas == storageesp::kScanSpeedCount,
              "the radio value lists every option after the selected index");
    }

    std::printf("\n");
    if (failures == 0) {
        std::printf("all storage esp geometry checks passed\n");
        return 0;
    }
    std::printf("%d storage esp geometry check(s) failed\n", failures);
    return 1;
}
