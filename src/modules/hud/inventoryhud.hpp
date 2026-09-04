#pragma once

#include "../Module.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class InventoryHudModule : public Module {
public:
    InventoryHudModule();
    ~InventoryHudModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void renderNative(void* context, void* client);

    enum class BackgroundStyle : int {
        Textured = 0,
        Flat = 1,
        Clean = 2
    };

    static constexpr std::size_t TotalInventorySlots = 36;
    static constexpr std::size_t MainInventorySlots = 27;
    static constexpr std::size_t HotbarSlots = 9;
    static constexpr int Columns = 9;

    // Position & HUD
    float hudPosX = 20.0f;
    float hudPosY = 120.0f;
    bool isHudModule = true;

private:
    struct SlotRuntime {
        std::atomic_bool hasItem{false};
        std::atomic_uint8_t count{0};
        std::atomic_int damage{0};
        std::atomic_int maxDamage{0};
        std::atomic_bool enchanted{false};
        std::atomic_int bundleWeight{-1};
    };

    struct ConfigSnapshot {
        bool showHotbar;
        bool hideInContainer;
        bool hideInChat;
        bool showEmptySlots;
        bool showStackCount;
        bool showDurability;
        bool showBundleWeight;
        bool showGlint;
        bool showSlotBackgrounds;
        BackgroundStyle backgroundStyle;
        float hudPosX;
        float hudPosY;
        float slotSize;
        float slotGap;
        float hotbarGap;
        float padding;
        float cornerRadius;
        float countTextSize;
        std::uint32_t countTextColor;
        std::uint32_t backgroundColor;
        float backgroundOpacity;
        std::uint32_t slotColor;
        float slotOpacity;
        float gridSize;
        float gridGap;
        float snapThreshold;
        std::uint32_t snapFlags;
    };

    ConfigSnapshot snapshotConfig() const;
    void clearRuntime();
    void submitEditorElements(const ConfigSnapshot& config, float totalWidth, float totalHeight);

    mutable std::mutex m_configMutex;
    std::array<SlotRuntime, TotalInventorySlots> m_runtime;

    std::atomic_bool m_inContainer{false};
    std::atomic_bool m_inChat{false};

    // Display options
    bool m_showHotbar = true;
    bool m_hideInContainer = true;
    bool m_hideInChat = false;
    bool m_showEmptySlots = true;

    // Items & labels
    bool m_showStackCount = true;
    float m_countTextSize = 10.0f;
    std::string m_countTextColor = "#FFFFFF";
    bool m_showDurability = true;
    bool m_showBundleWeight = true;
    bool m_showGlint = true;

    // Appearance & styles
    int m_backgroundStyle = 0; // 0 = Textured, 1 = Flat, 2 = Clean
    bool m_showSlotBackgrounds = true;
    std::string m_backgroundColor = "#000000";
    float m_backgroundOpacity = 0.6f;
    std::string m_slotColor = "#FFFFFF";
    float m_slotOpacity = 0.15f;
    float m_slotSize = 22.0f;
    float m_slotGap = 2.0f;
    float m_hotbarGap = 4.0f;
    float m_padding = 4.0f;
    float m_cornerRadius = 4.0f;

    // Snapping
    float m_gridSize = 16.0f;
    float m_gridGap = 4.0f;
    float m_snapThreshold = 12.0f;
    bool m_snapToGrid = true;
    bool m_snapToElements = true;
    bool m_snapToScreenCenter = true;
};
