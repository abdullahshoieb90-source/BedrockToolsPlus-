#include "misc/resourcepack.hpp"
#include "core/Runtime.hpp"
#include "resources/ResourcePackManager.hpp"
#include "launcher/ModuleMenu.hpp"
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <filesystem>
#include <pl/ModMenu.hpp>

namespace bedrocktools::modules {

ResourcePackModule* ResourcePackModule::g_resourcePack = nullptr;

ResourcePackModule::ResourcePackModule()
    : Module("Resource Pack Loader", "يسمح بوضع ملفات Resource Pack في مجلد BedrockToolsPlus داخل Launcher وتطبيقها على اللعبة.") {
    g_resourcePack = this;
}

ResourcePackModule::~ResourcePackModule() {
    if (g_resourcePack == this) g_resourcePack = nullptr;
}

void ResourcePackModule::onInit() {
    registerResources();
}

void ResourcePackModule::onEnable() {
    registerResources();
}

void ResourcePackModule::onDisable() {
    // ملاحظة: الـ Resource Packs يتم تطبيقها تلقائياً من قبل الـ Launcher عند تشغيل اللعبة
    // لذا لا نحتاج لإزالتها هنا إلا إذا أردنا إلغاء تحميلها (اختياري)
}

void ResourcePackModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
}

void ResourcePackModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
}

void ResourcePackModule::registerResources() {
    if (m_resourcesRegistered) return;

    const auto& runtime = bedrocktools::core::Runtime::get();
    const auto& packDir = runtime.resourcePackDirectory();
    
    // مسح ملفات الـ Resource Pack
    auto packs = bedrocktools::resources::scanResourcePacks(packDir.string());
    
    for (const auto& pack : packs) {
        // تسجيل الـ Resource Pack في الـ Launcher Mod Menu
        // ملاحظة: نستخدم pl::modmenu::registerResourcePack إذا كان متاحاً في الإصدار
        // أو نقوم بتسجيله كـ Resource Pack داخلي في الـ Mod
        // الـ Launcher (LeviLauncher) يدعم تحميل الـ Resource Packs من المجلد الخاص بالـ Mod
        // لذا نقوم بتسجيله هنا ليتم تطبيقه على اللعبة الأصلية.
        pl::modmenu::registerResourcePack(packDir / pack, pack);
    }

    m_resourcesRegistered = true;
}

} // namespace bedrocktools::modules
