#include "Runtime.hpp"
#include "GameHooks.hpp"
#include "config/ConfigManager.hpp"
#include "launcher/ExternalButtonRefresh.hpp"
#include "launcher/ModuleMenu.hpp"
#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <pl/Input.hpp>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace bedrocktools::core {
namespace {
std::atomic_bool enabled = false;
std::atomic_bool resolved = false;
std::atomic_bool installed = false;
// The launcher menu is registered independently of the game hooks (see
// install()); this keeps a retry from registering every module twice.
std::atomic_bool menuRegistered = false;
std::mutex resolveMutex;
std::mutex installMutex;
thread_local bool resolvingFromDlopen = false;
void* (*dlopenOriginal)(const char*, int) = nullptr;
bedrocktools::hooks::Handle dlopenHook = nullptr;
bool eventsWired = false;
int containerDepth = 0;
int chatDepth = 0;

class ResolveGuard {
public:
    ResolveGuard() : mPrevious(resolvingFromDlopen) { resolvingFromDlopen = true; }
    ~ResolveGuard() { resolvingFromDlopen = mPrevious; }
private:
    bool mPrevious;
};

void* dlopenDetour(const char* filename, int flags) {
    void* handle = dlopenOriginal ? dlopenOriginal(filename, flags) : nullptr;
    if (handle && filename && std::strstr(filename, "libminecraftpe.so") && !resolvingFromDlopen) {
        Runtime::get().minecraftLoaded();
    }
    return handle;
}
}

Runtime& Runtime::get() {
    static Runtime runtime;
    return runtime;
}

const std::filesystem::path& Runtime::resourceDirectory() const noexcept {
    return mResourceDirectory;
}

bool Runtime::launcherContext() const {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    char command[256]{};
    const auto size = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (size <= 0) return false;
    return std::strcmp(command, "org.levimc.launcher") == 0
        || std::strcmp(command, "org.levimc.launcher:minecraft") == 0
        || std::strcmp(command, "com.mojang.minecraftpe") == 0;
}

bool Runtime::resolveSignatures() {
    std::lock_guard lock(resolveMutex);
    if (resolved.load(std::memory_order_acquire)) return true;
    ResolveGuard guard;
    const bool ok = bedrocktools::memory::resolveAll("libminecraftpe.so");
    resolved.store(ok, std::memory_order_release);
    return ok;
}

void Runtime::wireEvents() {
    if (eventsWired) return;
    eventsWired = true;
    using namespace bedrocktools::events;
    bus().subscribe<FrameEvent>([](auto&) { ModuleRegistry::get().onFrame(); });

    // Tick watchdog.
    //
    // Every module's per-tick work hangs off LocalPlayerTickEvent, which is
    // published from the Actor::normalTick detour. That hook can install and
    // still never fire: the signature then matched a function the running build
    // of Minecraft does not call, and then no module updates at all.
    //
    // So the actor hook is not trusted because it installed - it is trusted
    // because it has counted a tick. Until it has, LocalPlayerTickEvent is
    // driven from the client-instance update hook (a separate signature). Once
    // the actor hook proves itself, the fallback switches itself off, so a
    // working build never double-ticks.
    bus().subscribe<ClientInstanceUpdateEvent>([](auto& event) {
        const std::uint64_t actorTicks = bedrocktools::core::gamehooks::actorTickCount();
        const bool fallbackNeeded = actorTicks == 0;

        // Number of client updates seen while waiting, used to give the actor
        // hook a fair chance before taking over: the two hooks fire at
        // different rates, and the fallback must not win a race at startup.
        static std::uint32_t updatesWhileWaiting = 0;
        static bool evaluated = false;
        if (!evaluated) {
            if (actorTicks == 0 && ++updatesWhileWaiting < 30) return;
            evaluated = true;
            bedrocktools::core::gamehooks::setFallbackTickSourceActive(fallbackNeeded);
        }
        if (!bedrocktools::core::gamehooks::fallbackTickSourceActive()) return;

        if (!event.clientInstance) return;
        auto* player = event.clientInstance->localPlayer();
        if (!player) return;
        LocalPlayerTickEvent tick{player};
        bus().publish(tick);
    });
    bus().subscribe<MouseInputEvent>([](auto& event) {
        if (ModuleRegistry::get().onMouseEvent(event.button, event.down)) event.cancel();
    });
    bus().subscribe<ScreenStateEvent>([](auto& event) {
        int& depth = event.screen == ScreenKind::Container ? containerDepth : chatDepth;
        if (event.phase == ScreenPhase::Opened) ++depth;
        else if (depth > 0) --depth;
        ModuleRegistry::get().setKeybindBlocked(containerDepth > 0 || chatDepth > 0);
    });
    pl::input::registerMouseCallback([](const pl::input::MouseEvent& input) {
        MouseInputEvent event{input.button, input.isDown};
        bus().publish(event);
        return event.cancelled();
    });
    pl::input::registerKeyCallback([](const pl::input::KeyEvent& input) {
        return ModuleRegistry::get().onKeyEvent(input.keyCode, input.isKeyDown);
    });
    pl::input::registerTouchCallback([](const pl::input::TouchEvent& input) {
        // Android MotionEvent actions: 0 = DOWN, 1 = UP, 3 = CANCEL.
        // Move/pointer-shift events are ignored; modules only track press state.
        if (input.action != 0 && input.action != 1 && input.action != 3) return false;
        const bool isDown = input.action == 0;
        return ModuleRegistry::get().onTouchEvent(input.x, input.y, isDown);
    });
}

bool Runtime::install() {
    std::lock_guard lock(installMutex);
    if (installed.load(std::memory_order_acquire)) return true;

    // The menu is registered even when a game hook cannot be installed.
    // Signature resolution depends on the exact Minecraft build, but the
    // launcher menu does not: bailing out early left the launcher with *zero*
    // modules after a game update, which looked exactly like every module had
    // disappeared. Hooks are retried on the next install() attempt, the menu is
    // registered only once (a duplicate module id is rejected by the launcher).
    const bool resolvedOk = resolved.load(std::memory_order_acquire) || resolveSignatures();
    const bool hooksOk = resolvedOk && gamehooks::install();

    if (!menuRegistered.load(std::memory_order_acquire)) {
        try {
            registerAllModules();
            wireEvents();
            ModuleRegistry::get().initialize();
            bedrocktools::config::ConfigManager::get().load();
            registerModulesWithLauncher();
            // Only latched once the pass completed, so a failure here is
            // retried on the next install() instead of leaving the menu empty
            // for the rest of the session.
            menuRegistered.store(true, std::memory_order_release);
        } catch (...) {
            bedrocktools::launcher::reportModuleRegistrationFailure("menu registration");
        }
    }

    if (!hooksOk) return false; // retried once the game library is available
    installed.store(true, std::memory_order_release);
    return true;
}

void Runtime::minecraftLoaded() {
    if (!resolveSignatures()) return;
    if (enabled.load(std::memory_order_acquire)) install();
}

bool Runtime::load(pl::mod::ModContext& context) {
    bedrocktools::launcher::setJavaVm(context.javaVm());
    mResourceDirectory = context.resourceDir();
    bedrocktools::config::ConfigManager::get().setConfigPath((context.configDir() / "config.json").string());
    if (!launcherContext()) return true;
    void* minecraft = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (minecraft) {
        resolveSignatures();
        dlclose(minecraft);
        return true;
    }
    bedrocktools::hooks::LibraryHandle libdl = bedrocktools::hooks::openLibrary("libdl.so");
    if (!libdl) return true;
    void* symbol = reinterpret_cast<void*>(bedrocktools::hooks::symbol(libdl, "dlopen"));
    if (symbol) dlopenHook = bedrocktools::hooks::install(symbol, reinterpret_cast<void*>(dlopenDetour), reinterpret_cast<void**>(&dlopenOriginal));
    bedrocktools::hooks::closeLibrary(libdl);
    return true;
}

bool Runtime::enable(pl::mod::ModContext&) {
    enabled.store(true, std::memory_order_release);
    if (!launcherContext()) return true;
    if (!resolved.load(std::memory_order_acquire)) {
        void* minecraft = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
        if (!minecraft) return true;
        resolveSignatures();
        dlclose(minecraft);
    }
    install();
    return true;
}

bool Runtime::disable(pl::mod::ModContext&) {
    enabled.store(false, std::memory_order_release);
    bedrocktools::config::ConfigManager::get().flush();
    return true;
}

bool Runtime::unload(pl::mod::ModContext&) {
    enabled.store(false, std::memory_order_release);
    bedrocktools::config::ConfigManager::get().flush();
    return true;
}

}
