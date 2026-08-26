#include "playerhealth.hpp"
#include "playerhealth_format.hpp"
#include "selfnametag_patch.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ph = bedrocktools::visual::playerhealth;

namespace {

using GetRuntimeActorListFn = std::vector<void*> (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorGetNameTagFn = std::string (*)(void*);
using ActorSetNameTagFn = void (*)(void*, const std::string&);
using SynchedActorDataEnsureIndexFn = void (*)(void*, std::uint16_t);
using ActorSynchedDataUpdateAlwaysShowNameTagFn = void (*)(void*, const void*);
using MobGetHealthFn = float (*)(void*);

GetRuntimeActorListFn s_getRuntimeActorList = nullptr;
ActorIsPlayerFn s_actorIsPlayer = nullptr;
ActorGetNameTagFn s_getNameTag = nullptr;
ActorSetNameTagFn s_setNameTag = nullptr;
SynchedActorDataEnsureIndexFn s_ensureIndex = nullptr;
ActorSynchedDataUpdateAlwaysShowNameTagFn s_updateAlwaysShowNameTag = nullptr;
MobGetHealthFn s_mobGetHealth = nullptr;

// Health can change every tick (damage, regeneration, and respawn). Updating
// each tick also makes the module independent from timing-sensitive nametag
// patches such as Third Person Nametag.
constexpr int kRefreshTicks = 1;

// ---------------------------------------------------------------------------
// SynchedActorData access, laid out exactly as the TNT Timer module walks it:
//   actor + Actor::mEntityData  -> EntityDataWrapper (a pointer member)
//   *wrapper                    -> SynchedActorData component
//   component + 0x0 / 0x8       -> begin / end of a DataItem* vector that is
//                                  indexed by the actor data id
//   DataItem + 0x8 / 0xA / 0x10 -> type / id / value (+0xC is header)
// ---------------------------------------------------------------------------

void* getEntityDataWrapper(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityData
    );
}

void* getEntityContext(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityContext
    );
}

void* getDataComponent(void* actor) {
    if (!actor) return nullptr;
    return *reinterpret_cast<void**>(getEntityDataWrapper(actor));
}

void** getItemsBegin(void* component) {
    if (!component) return nullptr;
    return *reinterpret_cast<void***>(component);
}

std::size_t getItemsSize(void* component) {
    if (!component) return 0;
    auto** begin = *reinterpret_cast<void***>(component);
    auto** end = *reinterpret_cast<void***>(reinterpret_cast<std::uintptr_t>(component) + sizeof(void*));
    if (!begin || !end || end < begin) return 0;
    return static_cast<std::size_t>(end - begin);
}

void markDataItemPresentAndDirty(void* component, std::size_t id) {
    if (!component || id >= 192) return;

    auto* dirty = reinterpret_cast<std::uint64_t*>(
        reinterpret_cast<std::uintptr_t>(component) + 0x18
    );
    auto* present = reinterpret_cast<std::uint64_t*>(
        reinterpret_cast<std::uintptr_t>(component) + 0x30
    );

    const std::size_t word = id / 64;
    const std::uint64_t bit = std::uint64_t{1} << (id % 64);
    dirty[word] |= bit;
    present[word] |= bit;
}

// Steals the vtable of any byte (type 0) DataItem already owned by the
// component so a missing always-show item can be created the same way the
// TNT Timer module creates one.
void* findByteDataItemVtable(void* component) {
    auto** begin = getItemsBegin(component);
    const std::size_t size = getItemsSize(component);
    if (!begin) return nullptr;

    for (std::size_t i = 0; i < size; ++i) {
        void* item = begin[i];
        if (!item) continue;

        const auto address = reinterpret_cast<std::uintptr_t>(item);
        const auto type = *reinterpret_cast<const std::uint8_t*>(
            address + bedrocktools::sdk::offsets::DataItem::mType
        );
        if (type == bedrocktools::sdk::offsets::DataItem::ByteType) return *reinterpret_cast<void**>(item);
    }

    return nullptr;
}

bool readAlwaysShowItem(void* actor, std::int8_t& value) {
    void* component = getDataComponent(actor);
    if (!component) return false;

    constexpr std::size_t id = bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow;
    if (getItemsSize(component) <= id) return false;

    auto** begin = getItemsBegin(component);
    if (!begin) return false;

    void* item = begin[id];
    if (!item) return false;

    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mType
    );
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mId
    );
    if (type != bedrocktools::sdk::offsets::DataItem::ByteType || itemId != id) return false;

    value = *reinterpret_cast<const std::int8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mValue
    );
    return true;
}

bool writeAlwaysShowItem(void* actor, std::int8_t value) {
    if (!actor || !s_ensureIndex || !s_updateAlwaysShowNameTag) return false;

    void* wrapper = getEntityDataWrapper(actor);
    void* context = getEntityContext(actor);
    void* component = getDataComponent(actor);
    if (!wrapper || !context || !component) return false;

    constexpr std::uint16_t id = static_cast<std::uint16_t>(
        bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow
    );

    s_ensureIndex(component, id);

    auto** begin = getItemsBegin(component);
    if (!begin || getItemsSize(component) <= id) return false;

    void* item = begin[id];
    if (!item) {
        void* vtable = findByteDataItemVtable(component);
        if (!vtable) return false;

        constexpr std::size_t itemSize = bedrocktools::sdk::offsets::DataItem::mMinimumSize;
        item = ::operator new(itemSize);
        std::memset(item, 0, itemSize);
        *reinterpret_cast<void**>(item) = vtable;
        *reinterpret_cast<std::uint8_t*>(
            reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mType
        ) = bedrocktools::sdk::offsets::DataItem::ByteType;
        *reinterpret_cast<std::uint16_t*>(
            reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mId
        ) = id;
        begin[id] = item;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mType
    );
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mId
    );
    if (type != bedrocktools::sdk::offsets::DataItem::ByteType || itemId != id) return false;

    *reinterpret_cast<std::int8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mValue
    ) = value;

    markDataItemPresentAndDirty(component, id);
    s_updateAlwaysShowNameTag(context, wrapper);
    return true;
}

// Health is read primarily from the Mob/Health Attribute, with a fallback
// to actor data id 1 (ActorDataIds::Health). Most metadata builds expose it
// as an int, some servers / protocol bridges send a float, and newer builds
// may keep it as a short.
void* getHealthAttribute(void* actor) {
    if (!actor) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Mob::mHealthAttribute
    );
}

bool readAttributeInstanceHealth(void* attributeInstance, float& out) {
    if (!attributeInstance) return false;
    out = *reinterpret_cast<const float*>(
        reinterpret_cast<std::uintptr_t>(attributeInstance) + bedrocktools::sdk::offsets::AttributeInstance::mCurrentValue
    );
    return true;
}

bool readHealthFromAttribute(void* actor, float& out) {
    if (!actor) return false;

    if (s_mobGetHealth) {
        const float val = s_mobGetHealth(actor);
        if (val >= 0.0f) {
            out = val;
            return true;
        }
    }

    void* attr = getHealthAttribute(actor);
    if (attr) {
        float val = 0.0f;
        if (readAttributeInstanceHealth(attr, val) && val >= 0.0f) {
            out = val;
            return true;
        }
    }

    return false;
}

bool readHealthFromMetadata(void* actor, float& out) {
    void* component = getDataComponent(actor);
    if (!component) return false;

    constexpr std::size_t id = bedrocktools::sdk::offsets::ActorDataIds::Health;
    if (getItemsSize(component) <= id) return false;

    auto** begin = getItemsBegin(component);
    if (!begin) return false;

    void* item = begin[id];
    const auto isHealthItem = [id](void* candidate) {
        if (!candidate) return false;
        const auto candidateAddress = reinterpret_cast<std::uintptr_t>(candidate);
        return *reinterpret_cast<const std::uint16_t*>(
            candidateAddress + bedrocktools::sdk::offsets::DataItem::mId
        ) == id;
    };
    // A few Bedrock builds leave holes in the indexed vector after respawn.
    // In that case slot 1 may contain another integer (often value 2), which
    // made a player at 20 HP appear to have one heart. Find HEALTH by its id.
    if (!isHealthItem(item)) {
        item = nullptr;
        const std::size_t size = getItemsSize(component);
        for (std::size_t i = 0; i < size; ++i) {
            if (isHealthItem(begin[i])) {
                item = begin[i];
                break;
            }
        }
    }
    if (!item) return false;

    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mType
    );
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mId
    );
    if (itemId != id) return false;

    if (type == bedrocktools::sdk::offsets::DataItem::ShortType) {
        out = static_cast<float>(*reinterpret_cast<const std::int16_t*>(
            address + bedrocktools::sdk::offsets::DataItem::mValue
        ));
        return true;
    }
    if (type == bedrocktools::sdk::offsets::DataItem::IntType) {
        out = static_cast<float>(*reinterpret_cast<const int*>(
            address + bedrocktools::sdk::offsets::DataItem::mValue
        ));
        return true;
    }
    if (type == bedrocktools::sdk::offsets::DataItem::FloatType) {
        out = *reinterpret_cast<const float*>(
            address + bedrocktools::sdk::offsets::DataItem::mValue
        );
        return true;
    }
    return false;
}

bool readHealth(void* actor, float& out) {
    if (readHealthFromAttribute(actor, out)) {
        return true;
    }
    return readHealthFromMetadata(actor, out);
}

bool readPosition(void* actor, bedrocktools::sdk::Vec3& out) {
    if (!actor) return false;
    const auto svc = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mStateVectorComponent
    );
    if (!svc) return false;
    out = *reinterpret_cast<const bedrocktools::sdk::Vec3*>(svc);
    return true;
}

float distanceSquared(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

PlayerHealthModule* PlayerHealthModule::s_instance = nullptr;

PlayerHealthModule::PlayerHealthModule()
    : Module("Player Health", "Shows every player's live health as a second nametag line, right below the name and above the head. Pick the look in the Style selector: Hearts, Bar or Numbers.") {
    s_instance = this;
}

PlayerHealthModule::~PlayerHealthModule() {
    releaseSelfNametagPatch();
    if (s_instance == this) s_instance = nullptr;
}

void PlayerHealthModule::onInit() {
    s_getRuntimeActorList = reinterpret_cast<GetRuntimeActorListFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList)
    );
    s_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer)
    );
    s_getNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag)
    );
    s_setNameTag = reinterpret_cast<ActorSetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSetNameTag)
    );
    s_ensureIndex = reinterpret_cast<SynchedActorDataEnsureIndexFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SynchedActorDataEnsureIndex)
    );
    s_updateAlwaysShowNameTag = reinterpret_cast<ActorSynchedDataUpdateAlwaysShowNameTagFn>(
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::ActorSynchedDataUpdateAlwaysShowNameTag
        )
    );
    s_mobGetHealth = reinterpret_cast<MobGetHealthFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MobGetHealth)
    );
    bedrocktools::modules::visual::selfnametag_patch::init();

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (PlayerHealthModule::s_instance) PlayerHealthModule::s_instance->onLocalPlayerTick(event.player);
        }
    );
}

void PlayerHealthModule::onDisable() {
    releaseSelfNametagPatch();
}

void PlayerHealthModule::restoreOne(void* actor, TrackedPlayer& state) {
    if (s_setNameTag) s_setNameTag(actor, state.baseName);
    if (state.forcedAlwaysShow) {
        writeAlwaysShowItem(actor, state.hadAlwaysShowItem ? state.alwaysShowValue : 0);
        state.forcedAlwaysShow = false;
    }
}

void PlayerHealthModule::restoreTracked(const std::vector<void*>& liveActors) {
    if (m_tracked.empty()) return;

    std::unordered_set<void*> live(liveActors.begin(), liveActors.end());
    for (auto& [actor, state] : m_tracked) {
        // Only touch actors that still exist this tick; despawned actors took
        // their nametag with them and their memory must not be touched.
        if (live.count(actor)) restoreOne(actor, state);
    }
    m_tracked.clear();
}

void PlayerHealthModule::updateSelfNametagPatch() {
    const bool needed = enabled && m_showSelf;
    if (needed == m_selfNametagPatchActive) return;

    if (needed) {
        m_selfNametagPatchActive = bedrocktools::modules::visual::selfnametag_patch::acquire();
    } else {
        releaseSelfNametagPatch();
    }
}

void PlayerHealthModule::releaseSelfNametagPatch() {
    if (!m_selfNametagPatchActive) return;
    bedrocktools::modules::visual::selfnametag_patch::release();
    m_selfNametagPatchActive = false;
}

void PlayerHealthModule::onLocalPlayerTick(void* localPlayer) {
    updateSelfNametagPatch();
    if (!localPlayer) return;
    if (!s_getRuntimeActorList || !s_actorIsPlayer || !s_getNameTag || !s_setNameTag) return;

    std::vector<void*> actors;
    auto* level = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(localPlayer) + bedrocktools::sdk::offsets::Actor::mLevel
    );
    if (level) {
        auto* actorManager = *reinterpret_cast<void**>(
            reinterpret_cast<std::uintptr_t>(level) + bedrocktools::sdk::offsets::Level::mActorManager
        );
        if (actorManager) actors = s_getRuntimeActorList(actorManager);
    }

    if (!enabled) {
        // Restore runs on the game thread only, and only for actors that are
        // still alive (see restoreTracked).
        if (!m_tracked.empty()) restoreTracked(actors);
        return;
    }

    if (++m_refreshTicks < kRefreshTicks) return;
    m_refreshTicks = 0;

    bedrocktools::sdk::Vec3 localPos{};
    const bool haveLocalPos = readPosition(localPlayer, localPos);
    const float rangeSq = m_range > 0 ? static_cast<float>(m_range) * static_cast<float>(m_range) : -1.0f;

    std::unordered_set<void*> seen;
    seen.reserve((actors.size() + 1) * 2);

    for (void* actor : actors) {
        if (!actor) continue;

        if (actor == localPlayer && !m_showSelf) {
            const auto it = m_tracked.find(actor);
            if (it != m_tracked.end()) {
                restoreOne(actor, it->second);
                m_tracked.erase(it);
            }
            continue;
        }

        if (!s_actorIsPlayer(actor)) continue;
        if (!seen.insert(actor).second) continue;

        bedrocktools::sdk::Vec3 pos{};
        if (rangeSq >= 0.0f && haveLocalPos && readPosition(actor, pos) && distanceSquared(pos, localPos) > rangeSq) {
            const auto it = m_tracked.find(actor);
            if (it != m_tracked.end()) {
                restoreOne(actor, it->second);
                m_tracked.erase(it);
            }
            continue;
        }

        float health = 0.0f;
        if (!readHealth(actor, health)) {
            // No synced health item for this actor: leave its nametag alone.
            const auto it = m_tracked.find(actor);
            if (it != m_tracked.end()) {
                restoreOne(actor, it->second);
                m_tracked.erase(it);
            }
            continue;
        }
        if (health < 0.0f) health = 0.0f;

        auto it = m_tracked.find(actor);
        if (it == m_tracked.end()) {
            TrackedPlayer state;
            std::int8_t alwaysShow = 0;
            state.hadAlwaysShowItem = readAlwaysShowItem(actor, alwaysShow);
            state.alwaysShowValue = alwaysShow;
            it = m_tracked.emplace(actor, std::move(state)).first;
        }
        TrackedPlayer& state = it->second;

        std::string current = s_getNameTag(actor);
        if (current != state.lastWritten) {
            // The nametag changed under us (server rename, Nick module, a
            // previous session of this module...): adopt the new text as the
            // base and rebuild our line on top of it.
            state.baseName = current;
            state.lastWritten.clear();
        }

        state.maxObserved = std::max(state.maxObserved, health);

        const std::string line = ph::composeHealthLine(m_styleIndex, health, state.maxObserved);
        std::string desired;
        if (state.baseName.empty()) {
            desired = line;
        } else {
            desired = state.baseName + "\n" + line;
        }

        if (desired != current) {
            s_setNameTag(actor, desired);
        }
        state.lastWritten = std::move(desired);

        if (m_alwaysShow) {
            if (!state.hadAlwaysShowItem || state.alwaysShowValue == 0) {
                if (!state.forcedAlwaysShow) {
                    state.forcedAlwaysShow = writeAlwaysShowItem(actor, 1);
                }
            } else {
                // The server itself asked for an always-visible nametag.
                state.forcedAlwaysShow = false;
            }
        } else if (state.forcedAlwaysShow) {
            writeAlwaysShowItem(actor, state.hadAlwaysShowItem ? state.alwaysShowValue : 0);
            state.forcedAlwaysShow = false;
        }
    }

    // Drop entries for actors that are gone from the runtime list. Their
    // nametags despawned with them, so there is nothing to restore; the
    // entries are erased without dereferencing the stale pointers.
    for (auto entry = m_tracked.begin(); entry != m_tracked.end();) {
        if (!seen.count(entry->first)) {
            entry = m_tracked.erase(entry);
        } else {
            ++entry;
        }
    }
}

void PlayerHealthModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_style")) m_style = j["m_style"].get<std::string>();
    m_styleIndex = ph::resolveStyleIndex(m_style);
    if (j.contains("m_alwaysShow")) m_alwaysShow = j["m_alwaysShow"].get<bool>();
    if (j.contains("m_showSelf")) m_showSelf = j["m_showSelf"].get<bool>();
    if (j.contains("m_range")) {
        const int range = j["m_range"].get<int>();
        m_range = std::clamp(range, 0, 180);
    }
    updateSelfNametagPatch();
}

void PlayerHealthModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_style"] = ph::styleRadioValue(m_styleIndex);
    j["m_alwaysShow"] = m_alwaysShow;
    j["m_showSelf"] = m_showSelf;
    j["m_range"] = m_range;
}
