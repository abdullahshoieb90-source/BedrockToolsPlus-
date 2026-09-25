#pragma once

#include <bedrocktools/sdk/Types.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

// Pure geometry for the Chunk Border module. Keeping line generation free of
// Minecraft pointers makes the grid rules host-testable.
namespace chunkborder {

using bedrocktools::sdk::Vec3;

struct Line {
    Vec3 from;
    Vec3 to;
};

// Build height of the supported Bedrock versions.
inline constexpr float kWorldBottom = -64.0f;
inline constexpr float kWorldTop = 320.0f;
inline constexpr int kChunkSize = 16;

// Corner of the 16x16 column the position falls in, snapped to the world
// bottom so the whole column can be drawn from a single anchor.
inline Vec3 chunkAnchor(const Vec3& position) {
    return {std::floor(position.x / static_cast<float>(kChunkSize)) *
                static_cast<float>(kChunkSize),
            0.0f,
            std::floor(position.z / static_cast<float>(kChunkSize)) *
                static_cast<float>(kChunkSize)};
}

// The wireframe of the chunk the player stands in plus the posts of the chunks
// around it. Corners and grid lines are kept apart because they are drawn in
// different colours.
struct Border {
    std::vector<Line> corners;   // the four posts of the player's chunk
    std::vector<Line> grid;      // inner posts and the horizontal rings
    std::vector<Line> adjacent;  // posts of the eight surrounding chunks

    std::size_t lineCount() const { return corners.size() + grid.size() + adjacent.size(); }
};

inline void addPost(std::vector<Line>& out, float x, float z) {
    out.push_back({{x, kWorldBottom, z}, {x, kWorldTop, z}});
}

// `horizontalSpacing` is the distance between two posts measured along X/Z,
// `verticalSpacing` the distance between two rings measured along Y. A spacing
// of zero (or less) disables that family of lines.
inline Border buildBorder(const Vec3& playerPosition, float verticalSpacing,
                          int horizontalSpacing) {
    Border border;
    const Vec3 anchor = chunkAnchor(playerPosition);
    const float size = static_cast<float>(kChunkSize);

    if (horizontalSpacing > 0) {
        for (int offset = 0; offset <= kChunkSize; offset += horizontalSpacing) {
            const float o = static_cast<float>(offset);
            // Only 0 and 16 are actual chunk corners; a spacing that does not
            // divide 16 simply stops at the last offset inside the chunk.
            const bool corner = offset == 0 || offset == kChunkSize;
            auto& target = corner ? border.corners : border.grid;

            addPost(target, anchor.x + o, anchor.z);
            addPost(target, anchor.x + o, anchor.z + size);
            // At 0 and 16 the two side walls already drew these posts.
            if (!corner) {
                addPost(target, anchor.x, anchor.z + o);
                addPost(target, anchor.x + size, anchor.z + o);
            }
        }
    }

    if (verticalSpacing > 0.0f) {
        for (float y = kWorldBottom; y <= kWorldTop; y += verticalSpacing) {
            border.grid.push_back({{anchor.x, y, anchor.z}, {anchor.x + size, y, anchor.z}});
            border.grid.push_back(
                {{anchor.x, y, anchor.z + size}, {anchor.x + size, y, anchor.z + size}});
            border.grid.push_back({{anchor.x, y, anchor.z}, {anchor.x, y, anchor.z + size}});
            border.grid.push_back(
                {{anchor.x + size, y, anchor.z}, {anchor.x + size, y, anchor.z + size}});
        }
    }

    // The twelve remaining corner posts of the 3x3 chunk neighbourhood, so the
    // borders of the chunks next to the player read as well.
    for (int x = -1; x <= 2; ++x) {
        for (int z = -1; z <= 2; ++z) {
            if (x >= 0 && x <= 1 && z >= 0 && z <= 1) continue;
            addPost(border.adjacent, anchor.x + static_cast<float>(x * kChunkSize),
                    anchor.z + static_cast<float>(z * kChunkSize));
        }
    }

    return border;
}

}  // namespace chunkborder
