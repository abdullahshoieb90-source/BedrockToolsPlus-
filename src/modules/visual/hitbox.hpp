#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <string>

class HitboxModule : public Module {
public:
    HitboxModule();
    ~HitboxModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    
    bool showEntities = true;
    bool showPlayers = true;
    bool showItems = true;
    uint32_t showItemsColor = 0xFFFFFFFFu;
    // Draw the local player's own box when the camera is in third person.
    // First-person (including jumping, where the camera interpolates above
    // the tick AABB) is skipped so the box does not fill the screen.
    bool show3rdPerson = false;
    bool showEyeLine = false;
    bool showLookLine = false;
    float lookLineLength = 2.0f;

    // Cull hitboxes that are fully hidden behind solid blocks.
    //
    // Off by default: the cull runs a voxel raycast per actor per frame
    // through BlockSource::isSolidBlockingBlock, and if that resolution is
    // wrong for the running build it suppresses *every* box, which reads as
    // "the module does nothing" with no way to recover. It was hardcoded on
    // before; making it a setting means the overlay always draws out of the
    // box and the cull is opt-in.
    bool hideBehindWalls = false;

    // Screen-space fallback: the same boxes, projected onto the launcher's HUD
    // layer, used while the world render pass has not drawn for a while (an
    // unresolved level-renderer signature, a player-renderer layout that does
    // not match this header). On by default - it is the difference between an
    // empty screen and a working overlay on such a build.
    bool hudFallback = true;

    // Draw the boxes on the HUD layer even while the world pass says it is
    // drawing. Off by default. It exists for the case HUD Diagnostics shows
    // "world pass live" and the screen is still empty: the pass is running and
    // its output is not visible, and this takes the overlay off it entirely.
    bool hudFallbackAlways = false;

    // Vertical field of view (degrees) the fallback projects with. 70 is the
    // game's default; it only has to line the boxes up with the entities.
    float hudFov = 70.0f;

    // Print the overlay's own status on the HUD surface: which pass is drawing,
    // how many actors each source handed back and which game functions
    // resolved. Off by default (it is a debug readout) - but it is the only
    // end-user-visible diagnostic the mod has: it needs no adb, no logcat and
    // no second device, and it says on screen whether the world pass, the HUD
    // fallback, or neither of them is running.
    bool hudDiagnostics = false;

    // Skip actors the game reports as invisible.
    //
    // Off by default: this is a hitbox overlay, and on a build where the
    // invisibility query resolves to the wrong function it answers "true" for
    // every actor, which hides every box in the game.
    bool hideInvisible = false;

    // Line thickness (menu slider units). 1.0 keeps the classic hairline
    // look; anything above that is drawn as real geometry (beams around
    // every edge) whose world-space width is lineThickness * 0.01 blocks,
    // because GL line width is ignored by most mobile GL ES drivers.
    float lineThickness = 1.0f;

    
    // Stored as AARRGGBB. Alpha is always forced opaque when loading,
    // saving, and drawing so changing line thickness never washes the color out.
    uint32_t hitboxColor = 0xFFFFFFFF;
    uint32_t eyeLineColor = 0xFFFF0000;
    uint32_t lookLineColor = 0xFF0000FF; 

    bool hitboxIndicator = false;             
    uint32_t indicatorDefaultColor = 0xFFFFFFFF; 
    uint32_t indicatorActiveColor = 0xFFFF0000;  


private:
    bool m_patched;
    void* m_patchTarget;

    void* m_tessBeginAddr;
    void* m_tessColorAddr;
    void* m_tessVertexAddr;
    void* m_renderMaterialGroupAddr;

    void applyPatch();
};
