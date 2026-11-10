#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace bedrocktools::sdk { class ClientInstance; }

// Draws the selected cape directly into the world, the same way the Wings
// module draws geometry, instead of writing it into the player's skin.
//
// Why this exists: patching SerializedSkin works only when the module receives
// a player tick *and* the skin layout matches the build. Both assumptions have
// failed on real devices, and a skin-only cape then never appears at all. This
// path needs only the RenderLevel hook, which is the hook Wings proves works.
class CustomCapesModule;

class CapeRendererModule : public Module {
public:
    CapeRendererModule();
    ~CapeRendererModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

private:
    void applyPatch();
    void resolveGameFunctions();
    void* localPlayer() const;

    void* m_patchTarget = nullptr;
    bool m_patched = false;

    // Resolved game functions. Zero means the signature did not match this
    // build, which is reported in the menu instead of failing silently.
    void* m_vertexUv = nullptr;
    void* m_vertex = nullptr;
    void* m_tessBegin = nullptr;
    void* m_tessColor = nullptr;
    void* m_renderMesh = nullptr;

    std::uintptr_t m_material = 0;

    // Camera-relative anchoring, refreshed on every frame.
    mutable std::mutex m_stateMutex;
    bedrocktools::sdk::Vec3 m_feet{};
    bedrocktools::sdk::Vec2 m_bodyRot{};
    float m_speed = 0.0f;
    float m_verticalSpeed = 0.0f;
    bool m_onGround = true;
    bool m_hasAnchor = false;
    float m_swayPhase = 0.0f;
    float m_lastFrameSeconds = 0.0f;

    // Diagnostics for the menu status row.
    std::atomic<std::uint32_t> m_drawCount{0};
    std::atomic<std::uint32_t> m_frameCount{0};
    std::atomic<std::uint32_t> m_lastVertexCount{0};

public:
    // Diagnostics read by CustomCapesModule::saveConfig().
    bool ready() const { return m_patchTarget != nullptr && m_renderMesh && m_tessBegin; }
    bool hasVertexUv() const { return m_vertexUv != nullptr; }

    // Accessors used by the render detour, which lives in cape_renderer.cpp.
    std::mutex& stateMutex() const { return m_stateMutex; }
    bool hasAnchorLocked() const { return m_hasAnchor; }
    bedrocktools::sdk::Vec3 feetLocked() const { return m_feet; }
    bedrocktools::sdk::Vec2 bodyRotLocked() const { return m_bodyRot; }
    float speedLocked() const { return m_speed; }
    float verticalSpeedLocked() const { return m_verticalSpeed; }
    bool onGroundLocked() const { return m_onGround; }
    float swayPhaseLocked() const { return m_swayPhase; }

    void* levelRenderer() const;
    void* tessBegin() const { return m_tessBegin; }
    void* tessColor() const { return m_tessColor; }
    void* vertexUv() const { return m_vertexUv; }
    void* vertex() const { return m_vertex; }
    void* renderMesh() const { return m_renderMesh; }
    void* patchTarget() const { return m_patchTarget; }

    // The single registered instance, or nullptr before registration. Used by
    // Custom Capes to switch the renderer on together with the cape.
    static CapeRendererModule* instance();

    void noteDraw(std::uint32_t vertices) {
        m_drawCount.fetch_add(1, std::memory_order_release);
        m_lastVertexCount.store(vertices, std::memory_order_release);
    }
    std::uint32_t drawCount() const { return m_drawCount.load(std::memory_order_acquire); }
    std::uint32_t frameCount() const { return m_frameCount.load(std::memory_order_acquire); }
    std::uint32_t lastVertexCount() const { return m_lastVertexCount.load(std::memory_order_acquire); }
};
