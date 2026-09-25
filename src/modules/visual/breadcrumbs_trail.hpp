#pragma once

#include <bedrocktools/sdk/Types.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// Pure trail state for the Breadcrumbs module. The renderer only walks the
// samples this container hands out, so the recording rules (sampling rate,
// duplicate suppression, trail length) are host-testable.
namespace breadcrumbs {

using bedrocktools::sdk::Vec3;

// The outlines are lifted slightly off the ground so they do not z-fight with
// the block they sit on.
inline constexpr float kFootprintLift = 0.05f;
inline constexpr float kArrowLength = 0.45f;
inline constexpr float kArrowBackOff = 0.05f;

// Bounded list of positions the player has walked through.
class Trail {
public:
    void setMaxPoints(int maxPoints) {
        m_maxPoints = maxPoints > 0 ? static_cast<std::size_t>(maxPoints) : 1;
        trim();
    }

    void setInterval(int interval) { m_interval = interval > 0 ? interval : 1; }

    // Called once per client tick. A sample is only stored every `interval`
    // ticks, and not at all while the player is still inside the block the
    // previous sample was taken in. Returns true when a sample was stored.
    bool onTick(const Vec3& position) {
        if (++m_tickCounter < m_interval) return false;
        m_tickCounter = 0;
        if (isDuplicate(position)) return false;

        m_points.push_back(position);
        trim();
        return true;
    }

    void clear() {
        m_points.clear();
        m_tickCounter = 0;
    }

    bool empty() const { return m_points.empty(); }
    std::size_t size() const { return m_points.size(); }
    std::size_t maxPoints() const { return m_maxPoints; }
    const std::vector<Vec3>& points() const { return m_points; }

    // Oldest samples fade out towards the head of the trail so the direction of
    // travel reads at a glance.
    static constexpr float fade(std::size_t index, std::size_t count) {
        if (count == 0) return 1.0f;
        return 0.1f + 0.9f * (static_cast<float>(index + 1) / static_cast<float>(count));
    }

private:
    // Same block column and no meaningful height change: the trail would only
    // redraw the square it already drew.
    bool isDuplicate(const Vec3& position) const {
        if (m_points.empty()) return false;
        const Vec3& last = m_points.back();
        const auto column = [](float value) { return static_cast<int>(std::floor(value)); };
        if (column(position.x) != column(last.x) || column(position.z) != column(last.z)) {
            return false;
        }
        return std::fabs(position.y - last.y) < 1.0f;
    }

    void trim() {
        if (m_points.size() <= m_maxPoints) return;
        m_points.erase(m_points.begin(),
                       m_points.begin() + static_cast<std::ptrdiff_t>(m_points.size() - m_maxPoints));
    }

    std::vector<Vec3> m_points;
    std::size_t m_maxPoints = 1000;
    int m_interval = 5;
    int m_tickCounter = 0;
};

// The four corners of the block footprint a sample sits on, in draw order.
inline std::array<Vec3, 4> footprint(const Vec3& sample) {
    const float x = std::floor(sample.x);
    const float z = std::floor(sample.z);
    const float y = sample.y + kFootprintLift;
    return {{{x, y, z}, {x + 1.0f, y, z}, {x + 1.0f, y, z + 1.0f}, {x, y, z + 1.0f}}};
}

// Centre of the same footprint; the connector lines run between these.
inline Vec3 centre(const Vec3& sample) {
    return {std::floor(sample.x) + 0.5f, sample.y + kFootprintLift, std::floor(sample.z) + 0.5f};
}

struct Arrowhead {
    Vec3 tip{0.0f, 0.0f, 0.0f};
    Vec3 left{0.0f, 0.0f, 0.0f};
    Vec3 right{0.0f, 0.0f, 0.0f};
    bool valid = false;
};

// Arrow pointing from `from` towards `to`, drawn just short of `to`. Invalid
// when the two samples are close enough that a direction cannot be told.
inline Arrowhead arrowhead(const Vec3& from, const Vec3& to) {
    Vec3 dir{to.x - from.x, to.y - from.y, to.z - from.z};
    const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (length <= 0.001f) return {};

    dir.x /= length;
    dir.y /= length;
    dir.z /= length;
    // Perpendicular of the direction, flattened to the horizontal plane so the
    // wings of the arrow never tilt with the slope.
    const Vec3 side{dir.z, 0.0f, -dir.x};

    Arrowhead head;
    head.valid = true;
    head.tip = {to.x - dir.x * kArrowBackOff, to.y - dir.y * kArrowBackOff,
                to.z - dir.z * kArrowBackOff};
    head.left = {head.tip.x - dir.x * kArrowLength + side.x * (kArrowLength * 0.8f),
                 head.tip.y - dir.y * kArrowLength + side.y * (kArrowLength * 0.8f),
                 head.tip.z - dir.z * kArrowLength + side.z * (kArrowLength * 0.8f)};
    head.right = {head.tip.x - dir.x * kArrowLength - side.x * (kArrowLength * 0.8f),
                  head.tip.y - dir.y * kArrowLength - side.y * (kArrowLength * 0.8f),
                  head.tip.z - dir.z * kArrowLength - side.z * (kArrowLength * 0.8f)};
    return head;
}

}  // namespace breadcrumbs
