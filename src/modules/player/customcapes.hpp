#pragma once

#include "../Module.hpp"
#include <string>
#include <vector>

// Custom Capes - client-side cape swapper
//
// Lets the user replace the local player's cape with their own PNG files.
// Drop any number of PNG capes into <config dir>/capes (next to config.json;
// the folder is created automatically on first init and a small default cape
// plus a README are written when it is empty). The module then exposes a
// "Cape" radio selector in the launcher menu; the selected cape is written
// into SerializedSkinImpl::mCapeImage (an mce::Image) of the local player's
// SerializedSkin so the engine renders it like a normal cape.
//
// WHY mCapeId IS ALSO WRITTEN (the actual "cape never shows" gate):
//
//  Modern game versions (MC 1.26) only render the classic cape when the
//  skin's SerializedSkinImpl::mCapeId is non-empty. Writing pixels into
//  mCapeImage alone is not enough — the renderer checks the id (and the
//  engine caches cape textures keyed by it), so a bare image blob with an
//  empty id is never drawn. This is the piece the working Custom Capes
//  build (the one users had before the module was rewritten) carried: it
//  wrote a synthetic short-string id ("bedrocktoolsplus-N") next to the
//  image. The id is regenerated every time a cape is (re)loaded so the
//  engine's texture cache is invalidated on selection changes and the new
//  pixels show up without leaving the world.
//
// Notes:
//  * Classic (non-persona) skins only - persona skins render their cape from
//    persona pieces, not from mCapeImage, and are left untouched.
//  * The cape is client-side only: other players still see your real cape.
//  * The original cape image AND the original 24-byte mCapeId std::string
//    slot are backed up on first application and restored when the module
//    is disabled (or on respawn the game rebuilds the skin anyway).
//
// WHY THE PATCH ALSO RUNS FROM ClientInstanceUpdateEvent (the "cape never
// shows" bug and its fix):
//
//  The engine builds the cape mesh - and decides whether a cape exists at
//  all - ONCE from the skin, when the player's renderer data is created at
//  world entry (uploading the cape texture to its cache then). A patch that
//  first lands in the per-tick path is one frame too late for that first
//  build: a player who owned no cape at world entry never gets a cape mesh,
//  so the pixels in mCapeImage are never drawn no matter how often we
//  rewrite them.
//
//  ClientInstance::update runs once per frame and the level render follows
//  it in the same frame, so ClientInstanceUpdateEvent fires BEFORE the
//  render pass. The hook resolves the local player through the ClientInstance
//  vtable slot (VTable::ClientInstanceGetLocalPlayer, the same dispatch the
//  Shulker Preview module uses on device) and runs the same guarded apply.
//  On the very first frame after the local player is created the cape is
//  therefore already in SerializedSkinImpl when the renderer data is built -
//  the engine creates a real cape mesh and uploads our texture through its
//  own pipeline (vanilla cape, with the game's waving animation).
//
//  Consequence: the module must be enabled BEFORE entering the world (or the
//  world must be re-entered after enabling it). Enabling it while already
//  standing in a world patches the skin, but the mesh was already built, so
//  the cape shows on the next world entry.
//
// The blob we install is owned by the engine once installed: the engine calls
// the deleter slot (freeCapePixels) when it destroys the mce::Image, so we
// never free installed pixels ourselves and never touch memory a second time.
class CustomCapesModule : public Module {
public:
    CustomCapesModule();
    ~CustomCapesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the LocalPlayerTickEvent subscription (game thread).
    void onLocalPlayerTick(void* player);

    // Called from the ClientInstanceUpdateEvent subscription (game thread,
    // once per frame BEFORE the level render pass). Resolves the local
    // player through the ClientInstance vtable slot and runs the same
    // guarded apply as the tick path. This is what makes the cape visible at
    // all: it lands the patch in SerializedSkinImpl on the first frame after
    // join, before the engine builds the cape mesh from the skin (see the
    // "cape never shows" note in the class comment). No-op while disabled -
    // the tick path owns restore/teardown.
    void onClientInstanceUpdate(void* clientInstance);

    // --- Host-testable helpers -------------------------------------------------

    // Re-scans <config dir>/capes for *.png files and rebuilds the selectable
    // cape list (sorted by file name). Creates the folder, writes the README
    // and, when the folder holds no PNGs, a bundled default cape so the
    // selector is never empty. Keeps the current selection when it still
    // exists, otherwise falls back to the first cape.
    void scanCapesDirectory();

    // Applies the currently selected cape to the player's SerializedSkinImpl
    // cape image. Returns true when the cape image was (re)installed, false
    // when there was nothing to do (module disabled, nothing selected, no
    // player/skin, already applied, or the PNG failed to decode).
    bool applyCapeToPlayer(void* player);

    // Restores the cape image that existed before the module first touched
    // it (backup taken from the first skin we modified).
    void restoreOriginalCape(void* player);

    // Selectable cape ids (file names without the .png extension, sorted).
    const std::vector<std::string>& capeNames() const { return m_capeNames; }
    // Directory capes are loaded from (<config dir>/capes).
    const std::string& capesDirectory() const { return m_capesDir; }
    int selectedIndex() const { return m_selectedIndex; }
    const std::string& selectedName() const { return m_selectedName; }

    // Deleter installed into the mce::Blob deleter slot for pixels we hand to
    // the engine (malloc'd buffers, released with free).
    static void freeCapePixels(unsigned char* pixels);

private:
    // Parses a config value for the cape selector: a full radio value
    // ("<index>,<id1>,<id2>,..."), a bare numeric index, or a cape id.
    void parseCapeValue(const std::string& value);

    // Decodes <id>.png from the capes folder into RGBA (malloc'd by stb,
    // caller must stbi_image_free it). Returns false when the file is missing
    // or not a valid image.
    bool loadCapePixels(const std::string& id, unsigned char** outPixels,
                        int* outWidth, int* outHeight);

    // Writes the cape image fields (format/width/height/depth/usage + blob)
    // into SerializedSkinImpl::mCapeImage, freeing the image the engine
    // currently owns. `pixels` becomes engine-owned (freed via freeCapePixels).
    void installCapeImage(void* skinImpl, unsigned char* pixels,
                          int width, int height);

    // Copies the current cape image (header fields + pixel bytes) and the
    // raw 24-byte mCapeId std::string slot so both can be restored on disable.
    // Only takes the backup once.
    void backupOriginalImage(void* skinImpl);

    std::vector<std::string> m_capeNames;   // sorted ids, no extension
    int m_selectedIndex = -1;               // -1 = none available
    std::string m_selectedName;             // resolved id, empty = none
    std::string m_capesDir;

    void* m_lastPlayer = nullptr;           // last ticked player (for restore)
    void* m_lastInstalledBlob = nullptr;    // blob currently in the game's cape image
    bool m_needApply = true;                // forces a re-apply even when the blob matches

    // Synthetic mCapeId (libc++ short string, <=22 chars) written next to the
    // cape image. Regenerated on every load so the engine's cape-texture cache
    // (keyed by mCapeId) is invalidated and a cape switch shows immediately.
    std::uint32_t m_capeIdSerial = 0;
    std::string m_activeCapeId = "bedrocktoolsplus";

    // Backup of the original cape image (taken from the first skin modified).
    unsigned char* m_originalPixels = nullptr;
    int m_originalWidth = 0;
    int m_originalHeight = 0;
    int m_originalSize = 0;
    std::uint32_t m_originalFormat = 0;
    std::uint32_t m_originalDepth = 0;
    unsigned char m_originalUsage = 0;
    bool m_hasOriginal = false;
    // Raw 24-byte libc++ std::string slot as it was before we wrote the
    // synthetic id (may be an empty SSO string or a heap pointer owned by the
    // engine; only the bytes are stored, never dereferenced).
    unsigned char m_originalCapeId[24] = {0};
};
