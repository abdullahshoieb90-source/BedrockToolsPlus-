#include "javaanimations.hpp"

#include "core/memory/Hooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/Events.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace {

using bedrocktools::memory::SignatureId;

// The vanilla renderer flags the first-person item pass in bit 0 of the item
// flags handed to ItemInHandRenderer::renderObject.
constexpr int kFirstPersonPass = 1 << 0;

struct MatrixBase {
    bool valid = false;
    glm::mat4 matrix = glm::mat4(1.0f);
    glm::mat4 inverse = glm::mat4(1.0f);
};

// An unusable base: applyPose falls back to transforming the matrix in place.
const MatrixBase kEmptyBase{};

struct FirstPersonFrame {
    void* renderContext = nullptr;
    MatrixBase base{};
};

struct HandFrame {
    void* renderContext = nullptr;
    bool mainHand = true;
    MatrixBase base{};
};

thread_local FirstPersonFrame* g_firstPersonFrame = nullptr;
thread_local HandFrame* g_handFrame = nullptr;

bool finiteMatrix(const glm::mat4& matrix) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) return false;
        }
    }
    return true;
}

// Reads the top of the game's matrix stack for this render context: that is
// the transform the item or the arm is about to be drawn with.
glm::mat4* currentMatrix(void* renderContext) {
    if (!renderContext) return nullptr;

    const auto base = reinterpret_cast<std::uintptr_t>(renderContext);
    const auto wrapper = *reinterpret_cast<std::uintptr_t*>(
        base + bedrocktools::sdk::offsets::RenderContext::mMatrixStackWrapper);
    if (!wrapper) return nullptr;

    const auto stack = *reinterpret_cast<std::uintptr_t*>(
        wrapper + bedrocktools::sdk::offsets::MatrixStackWrapper::mMatrixStack);
    if (!stack) return nullptr;

    const auto blocks = *reinterpret_cast<std::uintptr_t**>(
        stack + bedrocktools::sdk::offsets::MatrixStack::mBlocks);
    const auto start = *reinterpret_cast<std::size_t*>(
        stack + bedrocktools::sdk::offsets::MatrixStack::mStart);
    const auto size = *reinterpret_cast<std::size_t*>(
        stack + bedrocktools::sdk::offsets::MatrixStack::mSize);
    if (!blocks || size == 0) return nullptr;

    constexpr std::size_t elementsPerBlock = sizeof(glm::mat4) < 256 ? 4096 / sizeof(glm::mat4) : 16;
    const auto last = start + size - 1;
    const auto block = blocks[last / elementsPerBlock];
    if (!block) return nullptr;

    return reinterpret_cast<glm::mat4*>(block) + (last % elementsPerBlock);
}

void captureBase(MatrixBase& captured, void* renderContext) {
    captured.valid = false;
    captured.matrix = glm::mat4(1.0f);
    captured.inverse = glm::mat4(1.0f);

    auto* matrix = currentMatrix(renderContext);
    if (!matrix || !finiteMatrix(*matrix)) return;

    const float determinant = glm::determinant(*matrix);
    if (!std::isfinite(determinant) || std::fabs(determinant) < 0.000001f) return;

    const glm::mat4 inverse = glm::inverse(*matrix);
    if (!finiteMatrix(inverse)) return;

    captured.matrix = *matrix;
    captured.inverse = inverse;
    captured.valid = true;
}

// Rebuilds the drawn matrix as base * pose * local, i.e. the pose is applied in
// the hand's own space so the item swings around the hand instead of around the
// world origin.
void applyPose(glm::mat4& matrix, const MatrixBase& base, const javaanim::HandPose& pose) {
    if (javaanim::isNeutral(pose)) return;

    glm::mat4 local = base.valid ? base.inverse * matrix : matrix;
    if (!finiteMatrix(local)) return;

    if (pose.posX != 0.0f || pose.posY != 0.0f || pose.posZ != 0.0f) {
        local = glm::translate(glm::mat4(1.0f), glm::vec3(pose.posX, pose.posY, pose.posZ)) * local;
    }
    if (pose.rotX != 0.0f) local = glm::rotate(local, glm::radians(pose.rotX), glm::vec3(1.0f, 0.0f, 0.0f));
    if (pose.rotY != 0.0f) local = glm::rotate(local, glm::radians(pose.rotY), glm::vec3(0.0f, 1.0f, 0.0f));
    if (pose.rotZ != 0.0f) local = glm::rotate(local, glm::radians(pose.rotZ), glm::vec3(0.0f, 0.0f, 1.0f));
    if (pose.scale != 1.0f) local = glm::scale(local, glm::vec3(pose.scale));

    const glm::mat4 rebuilt = base.valid ? base.matrix * local : local;
    if (finiteMatrix(rebuilt)) matrix = rebuilt;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

void (*_renderFirstPerson_orig)(void*, void*, void*, int) = nullptr;

void _renderFirstPerson_hook(void* self, void* renderContext, void* previousProjection, int itemFlags) {
    FirstPersonFrame frame;
    frame.renderContext = renderContext;
    captureBase(frame.base, renderContext);

    auto* previous = g_firstPersonFrame;
    g_firstPersonFrame = &frame;
    if (_renderFirstPerson_orig) {
        _renderFirstPerson_orig(self, renderContext, previousProjection, itemFlags);
    }
    g_firstPersonFrame = previous;
}

void (*_renderHand_orig)(void*, void*, void*, int, float) = nullptr;

void _renderHand_hook(void* self, void* renderContext, void* player, int renderingMainHand, float frameAlpha) {
    HandFrame frame;
    frame.renderContext = renderContext;
    frame.mainHand = renderingMainHand != 0;
    captureBase(frame.base, renderContext);

    auto* previous = g_handFrame;
    g_handFrame = &frame;
    if (_renderHand_orig) {
        _renderHand_orig(self, renderContext, player, renderingMainHand, frameAlpha);
    }
    g_handFrame = previous;
}

void (*_renderObject_orig)(void*, void*, void*, void*, int) = nullptr;

void _renderObject_hook(void* self, void* renderContext, void* renderObject, void* renderMetadata, int itemFlags) {
    auto* module = JavaAnimationsModule::instance();

    const bool handScoped = g_handFrame && g_handFrame->renderContext == renderContext;
    const bool firstPersonScoped = g_firstPersonFrame && g_firstPersonFrame->renderContext == renderContext;
    const bool firstPerson = handScoped || firstPersonScoped || (itemFlags & kFirstPersonPass) != 0;

    glm::mat4* matrix = nullptr;
    glm::mat4 original(1.0f);
    bool restore = false;

    if (module && module->enabled && firstPerson) {
        matrix = currentMatrix(renderContext);
        if (matrix && finiteMatrix(*matrix)) {
            original = *matrix;
            restore = true;
        } else {
            matrix = nullptr;
        }
    }

    if (restore) {
        const bool mainHand = handScoped ? g_handFrame->mainHand : true;
        const MatrixBase& base = handScoped ? g_handFrame->base
                                            : (firstPersonScoped ? g_firstPersonFrame->base : kEmptyBase);
        applyPose(*matrix, base, module->poseNow(mainHand));
    }

    if (_renderObject_orig) {
        _renderObject_orig(self, renderContext, renderObject, renderMetadata, itemFlags);
    }

    if (restore) *matrix = original;
}

void (*_dispatcherRender_orig)(void*, void*, void*, void*, void*, int) = nullptr;

void _dispatcherRender_hook(void* self, void* renderContext, void* actor, void* position, void* rotation,
                            int ignoreLighting) {
    auto* module = JavaAnimationsModule::instance();

    const bool handScoped = g_handFrame && g_handFrame->renderContext == renderContext;
    const bool firstPersonScoped = !handScoped && g_firstPersonFrame &&
                                   g_firstPersonFrame->renderContext == renderContext;

    glm::mat4* matrix = nullptr;
    glm::mat4 original(1.0f);
    bool restore = false;

    if (module && module->enabled && (handScoped || firstPersonScoped)) {
        matrix = currentMatrix(renderContext);
        if (matrix && finiteMatrix(*matrix)) {
            original = *matrix;
            restore = true;
        } else {
            matrix = nullptr;
        }
    }

    if (restore) {
        const bool mainHand = handScoped ? g_handFrame->mainHand : true;
        const MatrixBase& base = handScoped ? g_handFrame->base : g_firstPersonFrame->base;
        // The arm follows the item, so the two never separate mid-swing.
        applyPose(*matrix, base, module->poseNow(mainHand));
    }

    if (_dispatcherRender_orig) {
        _dispatcherRender_orig(self, renderContext, actor, position, rotation, ignoreLighting);
    }

    if (restore) *matrix = original;
}

bool (*_localPlayerSwing_orig)(void*, std::uint8_t) = nullptr;

bool _localPlayerSwing_hook(void* player, std::uint8_t source) {
    const bool result = _localPlayerSwing_orig ? _localPlayerSwing_orig(player, source) : false;
    if (result) {
        auto* module = JavaAnimationsModule::instance();
        if (module) module->onSwing(player, source);
    }
    return result;
}

int (*_getModifiedSwingDuration_orig)(void*) = nullptr;

int _getModifiedSwingDuration_hook(void* mob) {
    const int original = _getModifiedSwingDuration_orig
                             ? _getModifiedSwingDuration_orig(mob)
                             : javaanim::kDefaultSwingTicks;
    auto* module = JavaAnimationsModule::instance();
    if (!module || !module->enabled) return original;
    return module->overrideSwingTicks(mob, original);
}

}  // namespace

JavaAnimationsModule* JavaAnimationsModule::s_instance = nullptr;

JavaAnimationsModule::JavaAnimationsModule()
    : Module("Java Animations",
             "Java 1.7-style first-person animation: held-item pose, swing arc, swing speed and eat/drink raise.") {
    hideInHudEditor = true;
    s_instance = this;
}

JavaAnimationsModule::~JavaAnimationsModule() {
    if (s_instance == this) s_instance = nullptr;
}

void JavaAnimationsModule::onInit() {
    using bedrocktools::memory::resolve;

    struct Target {
        std::uintptr_t address;
        void* detour;
        void** original;
    };

    const Target targets[] = {
        {resolve(SignatureId::ItemInHandRendererRenderFirstPerson),
         reinterpret_cast<void*>(&_renderFirstPerson_hook),
         reinterpret_cast<void**>(&_renderFirstPerson_orig)},
        {resolve(SignatureId::ItemInHandRendererRenderHand),
         reinterpret_cast<void*>(&_renderHand_hook),
         reinterpret_cast<void**>(&_renderHand_orig)},
        {resolve(SignatureId::ItemInHandRenderObject),
         reinterpret_cast<void*>(&_renderObject_hook),
         reinterpret_cast<void**>(&_renderObject_orig)},
        {resolve(SignatureId::ActorRenderDispatcherRender),
         reinterpret_cast<void*>(&_dispatcherRender_hook),
         reinterpret_cast<void**>(&_dispatcherRender_orig)},
        {resolve(SignatureId::LocalPlayerSwing),
         reinterpret_cast<void*>(&_localPlayerSwing_hook),
         reinterpret_cast<void**>(&_localPlayerSwing_orig)},
        {resolve(SignatureId::MobGetModifiedSwingDuration),
         reinterpret_cast<void*>(&_getModifiedSwingDuration_hook),
         reinterpret_cast<void**>(&_getModifiedSwingDuration_orig)},
    };

    for (const auto& target : targets) {
        if (!target.address) continue;
        bedrocktools::hooks::install(reinterpret_cast<void*>(target.address), target.detour, target.original);
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onLocalPlayerTick(event.player); });

    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>([this](auto& event) {
        if (event.action == bedrocktools::events::GameModeAction::UseItem) onUseItem();
    });
}

void JavaAnimationsModule::onEnable() {
    resetState();
}

void JavaAnimationsModule::onDisable() {
    resetState();
}

void JavaAnimationsModule::resetState() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_swing.reset();
    m_use.reset();
    m_haveSample = false;
}

void JavaAnimationsModule::onSwing(void* player, std::uint8_t source) {
    (void)source;
    if (!enabled) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    // Only the local player's own swings move the hand; when the local player
    // has not been resolved yet the swing is accepted, because the swing hook
    // only exists on LocalPlayer in the first place.
    if (m_localPlayer && player != m_localPlayer) return;
    if (m_settings.style == javaanim::SwingStyle::Vanilla) return;
    m_swing.start(m_swingTicks, m_settings.style);
}

void JavaAnimationsModule::onUseItem() {
    if (!enabled) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_settings.style == javaanim::SwingStyle::Vanilla) return;
    m_use.noteUse();
}

void JavaAnimationsModule::onLocalPlayerTick(void* player) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_localPlayer = player;
}

int JavaAnimationsModule::overrideSwingTicks(void* mob, int originalTicks) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // The override is animation-only, and it is scoped to the local player so
    // mobs keep their own swing length.
    if (m_settings.style == javaanim::SwingStyle::Vanilla) return originalTicks;
    if (m_localPlayer && mob != m_localPlayer) return originalTicks;
    if (originalTicks <= 0) return originalTicks;
    return javaanim::clampSwingTicks(m_swingTicks);
}

javaanim::HandPose JavaAnimationsModule::poseNow(bool mainHand) {
    const auto now = Clock::now();

    std::lock_guard<std::mutex> lock(m_mutex);

    float dt = 0.0f;
    if (m_haveSample) {
        dt = std::chrono::duration<float>(now - m_lastSample).count();
    }
    m_lastSample = now;
    m_haveSample = true;

    if (!(dt > 0.0f)) dt = 0.0f;
    // A stall (a paused game, a hitch) must never fast-forward the animation.
    if (dt > 0.25f) dt = 0.25f;

    m_swing.advance(dt);
    m_use.advance(dt, m_settings.useRaise && enabled);

    if (!enabled) return javaanim::HandPose{};

    return javaanim::currentPose(m_settings, m_swing, m_use, mainHand);
}

void JavaAnimationsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("m_swingStyle")) {
        const int index = javaanim::resolveStyleIndex(j["m_swingStyle"].get<std::string>());
        m_settings.style = javaanim::styleForIndex(index);
    } else if (j.contains("swingStyle")) {
        const int index = javaanim::resolveStyleIndex(j["swingStyle"].get<std::string>());
        m_settings.style = javaanim::styleForIndex(index);
    }

    if (j.contains("m_swingTicks")) {
        try {
            m_swingTicks = javaanim::clampSwingTicks(j["m_swingTicks"].get<int>());
        } catch (...) {}
    } else if (j.contains("swingTicks")) {
        try {
            m_swingTicks = javaanim::clampSwingTicks(j["swingTicks"].get<int>());
        } catch (...) {}
    }

    if (j.contains("m_swingScale")) {
        try {
            m_settings.swingScale = javaanim::clampRange(j["m_swingScale"].get<float>(), 0.0f, 5.0f);
        } catch (...) {}
    }

    if (j.contains("m_itemPose")) {
        try { m_settings.itemPose = j["m_itemPose"].get<bool>(); } catch (...) {}
    }
    if (j.contains("m_useRaise")) {
        try { m_settings.useRaise = j["m_useRaise"].get<bool>(); } catch (...) {}
    }
}

void JavaAnimationsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    const int index = m_settings.style == javaanim::SwingStyle::Java18
                          ? 1
                          : (m_settings.style == javaanim::SwingStyle::Vanilla ? 2 : 0);
    j["m_swingStyle"] = javaanim::styleRadioValue(index);
    j["m_swingTicks"] = javaanim::clampSwingTicks(m_swingTicks);
    j["m_swingScale"] = javaanim::clampRange(m_settings.swingScale, 0.0f, 5.0f);
    j["m_itemPose"] = m_settings.itemPose;
    j["m_useRaise"] = m_settings.useRaise;
}
