#include "thirdpersonnametag.hpp"
#include "selfnametag_patch.hpp"

ThirdPersonNametagModule::ThirdPersonNametagModule()
    : Module("Third Person Nametag", "Shows your own nametag in third person view.") {
    m_patched = false;
}

ThirdPersonNametagModule::~ThirdPersonNametagModule() {
    removePatch();
}

void ThirdPersonNametagModule::onInit() {
    bedrocktools::modules::visual::selfnametag_patch::init();
}

void ThirdPersonNametagModule::applyPatch() {
    if (m_patched) return;
    m_patched = bedrocktools::modules::visual::selfnametag_patch::acquire();
}

void ThirdPersonNametagModule::removePatch() {
    if (!m_patched) return;
    bedrocktools::modules::visual::selfnametag_patch::release();
    m_patched = false;
}

void ThirdPersonNametagModule::onEnable() {
    applyPatch();
}

void ThirdPersonNametagModule::onDisable() {
    removePatch();
}
