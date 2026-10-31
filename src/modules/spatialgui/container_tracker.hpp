#pragma once

#include <algorithm>
#include <mutex>
#include <vector>

namespace bedrocktools::spatialgui {
// Tracks opaque controller identities reported by the existing open/close
// events. Never dereferences or owns them. Screen render integration must only
// compare a controller extracted through a version-verified ScreenView ABI;
// this tracker cannot establish that a container is a chest or has focus.
class ContainerTracker {
public:
    void opened(const void* controller) {
        if (!controller) return;
        std::lock_guard lock(m_mutex);
        // Duplicate opens promote an existing controller without making a
        // duplicate entry, so one matching close removes it completely.
        std::erase(m_open, controller);
        m_open.push_back(controller);
    }
    void closed(const void* controller) {
        if (!controller) return;
        std::lock_guard lock(m_mutex);
        std::erase(m_open, controller);
    }
    bool isTop(const void* controller) const {
        if (!controller) return false;
        std::lock_guard lock(m_mutex);
        return !m_open.empty() && m_open.back() == controller;
    }
    bool hasOpen() const {
        std::lock_guard lock(m_mutex);
        return !m_open.empty();
    }
    void clear() {
        std::lock_guard lock(m_mutex);
        m_open.clear();
    }
private:
    mutable std::mutex m_mutex;
    std::vector<const void*> m_open;
};
}
