#pragma once

#include <bedrocktools/Export.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bedrocktools::memory {

enum class SignatureId : std::uint16_t {
    VersionString,
    Nametag,
    Fullbright,
    SetupFogPlayer,
    RaknetUpdate,
    NormalTick,
    Time,
    SetTime,
    EduMultiplayer,
    HudCursor,
    LevelInit,
    LevelDtor,
    ActorManagerList,
    DimensionTick,
    WeatherTick,
    WeatherGetRainLevel,
    WeatherGetLightningLevel,
    WeatherIsRaining,
    WeatherIsLightning,
    ActorShaderManagerSetEntityConstants,
    ActorShaderManagerSetupShaderParametersActorGlint,
    ActorShaderManagerSetupFoilShaderParameters,
    ActorShaderManagerSetupShaderParametersGlint,
    RenderItem,
    GetFov,
    GetPerspective,
    ClientInstanceUpdate,
    ClientInstanceGetLocalPlayer,
    ContainerScreenControllerDtor,
    ContainerScreenControllerOpen,
    ChatScreenDtor,
    ChatScreenOpen,
    BiomeGetTemperature,
    GetDestroyProgress,
    RenderLevel,
    TessellatorBegin,
    TessellatorColor,
    TessellatorVertex,
    MeshHelpersRenderMeshImmediately,
    MeshHelpersRenderMeshImmediately2,
    RenderMaterialGroupCommon,
    SurvivalModeStartDestroyBlock,
    GameModeStartDestroyBlock,
    GameModeStopDestroyBlock,
    SurvivalModeStartBuildBlock,
    GameModeStartBuildBlock,
    SurvivalModeUseItem,
    GameModeUseItem,
    SurvivalModeUseItemAsAttack,
    GameModeUseItemAsAttack,
    SurvivalModeUseItemOn,
    GameModeUseItemOn,
    SurvivalModeInteract,
    GameModeInteract,
    SurvivalModeAttack,
    GameModeAttack,
    GameModeAttackInternal,
    LevelGetHitResult,
    BlockSourceGetBiome,
    BlockSourceGetBlock,
    BlockSourceGetBrightness,
    BlockSourceIsSolidBlockingBlock,
    LocalPlayerApplyTurnDelta,
    LocalPlayerSwing,
    BaseOptionRegistryGetHideItemInHand,
    HitResultGetEntity,
    ActorIsPlayer,
    ActorIsInvisible,
    ActorFetchNearbyActorsSorted,
    ActorGetNameTag,
    ActorSetNameTag,
    SynchedActorDataEnsureIndex,
    ActorSynchedDataUpdateAlwaysShowNameTag,
    PrimedTntNormalTick,
    MinecraftUIRenderContextDrawText,
    ScreenViewRender,
    ContainerScreenControllerOnContainerSlotSelected,
    ContainerScreenControllerGetItemStack,
    ClientNetworkHandlerHandleSetTitle,
    ClientNetworkHandlerHandleText,
    LoopbackPacketSenderSendToServer,
    ClientInstanceGetPacketSender,
    MinecraftPacketsCreatePacket,
    LocalPlayerChangeDimension,
    NbtTreeFind,
    ItemStackBaseLoadItem,
    RenderPotionEffects,
    ItemStackBaseGetDamageValue,
    ItemStackBaseGetRawNameId,
    BaseActorRenderContextCtor,
    ItemRendererRenderGuiItemNew,
    ControlOptionEditorTick,
    ControlOptionEditorRender,
    BlockTessellatorTessellateFaceDown,
    BlockTessellatorTessellateFaceUp,
    BlockTessellatorTessellateFaceNorth,
    BlockTessellatorTessellateFaceSouth,
    BlockTessellatorTessellateFaceWest,
    BlockTessellatorTessellateFaceEast,
    BlockTessellatorTessellatePane,
    BlockSourceGetBlockForTessellation,
    TextureUVCoordinateSetCopyCtor,
    TextureUVCoordinateSetDtor,
    RenderChunkCoordinatorSetAllDirty,
    GuiDataDisplayAnnouncementMessage,
    GuiDataDisplayChatMessage,
    GuiDataDisplayClientMessage,
    GuiDataDisplayDevConsoleMessage,
    GuiDataDisplayLocalizableMessage,
    GuiDataDisplayLocalizedMessage,
    GuiDataDisplaySystemMessage,
    GuiDataDisplayTextObjectMessage,
    GuiDataDisplayTextObjectWhisperMessageText,
    GuiDataDisplayTextObjectWhisperMessageObject,
    GuiDataDisplayWhisperMessage,
    GuiDataAddMessage,
    ResourcePacksInfoPacketHandle,
    ResourcePackStackPacketHandle,    
    BlockTessellatorTessellateDoubleThinFenceInWorld,
    BlockGraphicsGetTexture,
    BlockOccluderUpdateRenderFace,
    ItemInHandRendererRenderFirstPerson,
    MobGetModifiedSwingDuration,
    ContainerScreenControllerHandleAutoPlace,
    ActorGetOffhandSlot,
    // Appended at the end so the numeric values of the ids above stay stable
    // for mods that were built against an older header.
    ItemStackBaseGetMaxDamage,
    Count
};

inline constexpr std::size_t SignatureCount = static_cast<std::size_t>(SignatureId::Count);

struct SignatureDefinition {
    SignatureId id;
    std::string_view pattern;
    // Optional dynamic symbol of the same function. Bedrock exports the
    // ItemStackBase accessors, and a symbol is an exact match where a short
    // byte pattern can land on a neighbouring function that happens to share
    // its prologue — which is how a durability bar silently reads 0 damage.
    // Tried first; the pattern above stays the fallback.
    std::string_view symbol{};
};

BEDROCKTOOLS_API bool resolveAll(std::string_view libraryName = "libminecraftpe.so");
BEDROCKTOOLS_API std::uintptr_t resolve(SignatureId id);
BEDROCKTOOLS_API void clear();

}
