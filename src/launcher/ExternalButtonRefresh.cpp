#include "ExternalButtonRefresh.hpp"

#include <atomic>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace bedrocktools::launcher {
namespace {

#if defined(__ANDROID__)

std::atomic<JavaVM*> gJavaVm{nullptr};

void clearJavaException(JNIEnv* env) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
}

constexpr const char* kExternalButtonOverlayName =
    "org/levimc/launcher/core/mods/inbuilt/overlay/ExternalButtonOverlay";

// Scans the overlays the launcher currently keeps for `moduleId` and reports
// whether any of them no longer has a matching native button (its slot was
// switched off in the module settings). Such an overlay cannot be reached by
// the per-button refresh loop - its native button is gone - so the launcher
// has to be asked to rebuild the module's whole button group instead.
//
// Best-effort: returns false (leaving the current behavior unchanged) when
// the launcher build does not expose the java.util.Map / Collection surface
// or the ExternalButtonOverlay accessors used here.
bool hasOrphanedOverlays(JNIEnv* env, jobject overlays, const std::string& moduleId,
                         const std::vector<std::string>& nativeButtonIds) {
    jclass mapClass = nullptr;
    jclass collectionClass = nullptr;
    jclass overlayClass = nullptr;
    jobject values = nullptr;
    jobject array = nullptr;
    bool orphaned = false;

    const auto cleanup = [&]() {
        if (values) env->DeleteLocalRef(values);
        if (array) env->DeleteLocalRef(array);
        if (mapClass) env->DeleteLocalRef(mapClass);
        if (collectionClass) env->DeleteLocalRef(collectionClass);
        if (overlayClass) env->DeleteLocalRef(overlayClass);
    };

    mapClass = env->FindClass("java/util/Map");
    collectionClass = env->FindClass("java/util/Collection");
    overlayClass = env->FindClass(kExternalButtonOverlayName);
    if (!mapClass || !collectionClass || !overlayClass || env->ExceptionCheck()) {
        clearJavaException(env);
        cleanup();
        return false;
    }

    const jmethodID mapValues =
        env->GetMethodID(mapClass, "values", "()Ljava/util/Collection;");
    const jmethodID collectionToArray =
        env->GetMethodID(collectionClass, "toArray", "()[Ljava/lang/Object;");
    const jmethodID overlayModuleId =
        env->GetMethodID(overlayClass, "getModuleId", "()Ljava/lang/String;");
    const jmethodID overlayButtonId =
        env->GetMethodID(overlayClass, "getButtonId", "()Ljava/lang/String;");
    clearJavaException(env); // any missing accessor -> bail out below
    if (!mapValues || !collectionToArray || !overlayModuleId || !overlayButtonId) {
        cleanup();
        return false;
    }

    values = env->CallObjectMethod(overlays, mapValues);
    array = values ? env->CallObjectMethod(values, collectionToArray) : nullptr;
    if (!values || !array || env->ExceptionCheck()) {
        clearJavaException(env);
        cleanup();
        return false;
    }

    const jsize length = env->GetArrayLength(static_cast<jarray>(array));
    for (jsize i = 0; i < length && !env->ExceptionCheck(); ++i) {
        jobject overlay = env->GetObjectArrayElement(static_cast<jobjectArray>(array), i);
        if (!overlay) continue;

        bool moduleMatches = false;
        jstring mod = static_cast<jstring>(env->CallObjectMethod(overlay, overlayModuleId));
        if (mod) {
            const char* actual = env->GetStringUTFChars(mod, nullptr);
            moduleMatches = actual && moduleId == actual;
            if (actual) env->ReleaseStringUTFChars(mod, actual);
            env->DeleteLocalRef(mod);
        }

        if (moduleMatches) {
            bool stillRegistered = false;
            jstring id = static_cast<jstring>(env->CallObjectMethod(overlay, overlayButtonId));
            if (id) {
                const char* actual = env->GetStringUTFChars(id, nullptr);
                if (actual) {
                    for (const auto& nativeId : nativeButtonIds) {
                        if (nativeId == actual) {
                            stillRegistered = true;
                            break;
                        }
                    }
                }
                if (actual) env->ReleaseStringUTFChars(id, actual);
                env->DeleteLocalRef(id);
            }
            if (!stillRegistered) {
                orphaned = true;
                env->DeleteLocalRef(overlay);
                break;
            }
        }
        env->DeleteLocalRef(overlay);
    }
    clearJavaException(env);
    cleanup();
    return orphaned;
}

// Runs the actual overlay refresh. The caller guarantees that env belongs to
// the current thread (attached if necessary), so every early return below is
// safe without extra thread bookkeeping.
void refreshExternalButtonsAttached(JNIEnv* env, std::string_view moduleId) {
    // The overlay keeps a Java ExternalButton object, so changing the native
    // definition alone is not enough to update an already visible button.
    // Replace that object in-place and ask the overlay to re-apply its view
    // configuration. This avoids the old hide/show workaround (which made the
    // buttons disappear while a text field was being edited).
    constexpr const char* managerName =
        "org/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager";
    constexpr const char* bridgeName =
        "org/levimc/launcher/core/mods/inbuilt/ExternalModBridge";
    constexpr const char* buttonName =
        "org/levimc/launcher/core/mods/inbuilt/ExternalModBridge$ExternalButton";
    constexpr const char* overlayName =
        "org/levimc/launcher/core/mods/inbuilt/overlay/ExternalButtonOverlay";

    jclass managerClass = env->FindClass(managerName);
    jclass bridgeClass = env->FindClass(bridgeName);
    jclass overlayClass = env->FindClass(overlayName);
    jclass buttonClass = env->FindClass(buttonName);
    if (!managerClass || !bridgeClass || !overlayClass || !buttonClass || env->ExceptionCheck()) {
        clearJavaException(env);
        return;
    }

    jclass mapClass = env->FindClass("java/util/Map");
    jmethodID getInstance = env->GetStaticMethodID(
        managerClass, "getInstance",
        "()Lorg/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager;");
    jmethodID getCount = env->GetStaticMethodID(bridgeClass, "getExternalButtonCount", "()I");
    jmethodID getButton = env->GetStaticMethodID(
        bridgeClass, "getExternalButton",
        "(I)Lorg/levimc/launcher/core/mods/inbuilt/ExternalModBridge$ExternalButton;");
    jfieldID overlaysField = env->GetFieldID(managerClass, "externalButtonOverlayMap", "Ljava/util/Map;");
    jmethodID mapGet = mapClass ? env->GetMethodID(
                                     mapClass, "get", "(Ljava/lang/Object;)Ljava/lang/Object;")
                                : nullptr;
    jfieldID buttonIdField = env->GetFieldID(buttonClass, "buttonId", "Ljava/lang/String;");
    jfieldID moduleIdField = env->GetFieldID(buttonClass, "moduleId", "Ljava/lang/String;");
    jfieldID overlayButtonField = env->GetFieldID(overlayClass, "button",
                                                    "Lorg/levimc/launcher/core/mods/inbuilt/ExternalModBridge$ExternalButton;");
    jmethodID applyChanges = env->GetMethodID(overlayClass, "applyConfigurationChanges", "()V");
    if (!getInstance || !getCount || !getButton || !overlaysField || !mapGet ||
        !buttonIdField || !moduleIdField || !overlayButtonField || !applyChanges ||
        env->ExceptionCheck()) {
        clearJavaException(env);
        if (mapClass) env->DeleteLocalRef(mapClass);
        return;
    }

    // Best-effort: applyConfigurationChanges resizes/recolors the existing
    // view but does not re-apply the label text on every launcher build (the
    // TextView text is only set in configureOverlayView at creation). Look up
    // the view field and the configure method so the new command/comment label
    // can be re-applied in place. These are optional: if absent on an older
    // launcher build, the resize/recolor path above still runs.
    jfieldID overlayViewField = nullptr;
    {
        // The field may live on ExternalButtonOverlay itself or on any
        // superclass depending on the launcher build, so walk the chain
        // instead of only probing the direct superclass.
        jclass walker = static_cast<jclass>(env->NewLocalRef(overlayClass));
        while (walker && !overlayViewField) {
            overlayViewField = env->GetFieldID(walker, "overlayView", "Landroid/view/View;");
            clearJavaException(env);
            if (overlayViewField) break;
            jclass next = env->GetSuperclass(walker);
            env->DeleteLocalRef(walker);
            walker = next;
        }
        if (walker) env->DeleteLocalRef(walker);
    }
    jmethodID configureOverlayView = env->GetMethodID(
        overlayClass, "configureOverlayView", "(Landroid/view/View;)V");
    clearJavaException(env);

    // Optional on older launcher builds: handleExternalModuleToggle makes the
    // overlay manager create/remove the ExternalButtonOverlay instances for a
    // module. Needed because slots switched on/off inside the module settings
    // change the native button set, and the loop below can only update
    // overlays that already exist. When it is absent the new/stale buttons
    // degrade to the old behavior (they appear/disappear on a module toggle).
    const jmethodID handleExternalToggle = env->GetMethodID(
        managerClass, "handleExternalModuleToggle", "(Ljava/lang/String;Z)V");
    clearJavaException(env);

    jobject manager = env->CallStaticObjectMethod(managerClass, getInstance);
    jobject overlays = manager ? env->GetObjectField(manager, overlaysField) : nullptr;
    if (!manager || !overlays || env->ExceptionCheck()) {
        clearJavaException(env);
        if (mapClass) env->DeleteLocalRef(mapClass);
        return;
    }

    const std::string moduleIdString(moduleId);
    jstring wantedModule = env->NewStringUTF(moduleIdString.c_str());
    const jint count = env->CallStaticIntMethod(bridgeClass, getCount);

    // Button ids of this module currently registered in the native bridge.
    // Used to tell newly registered buttons (no overlay yet) apart from
    // buttons whose overlay is no longer backed by a native registration.
    std::vector<std::string> nativeButtonIds;
    nativeButtonIds.reserve(count > 0 ? static_cast<std::size_t>(count) : 0);
    bool addedButton = false;

    for (jint i = 0; i < count && !env->ExceptionCheck(); ++i) {
        jobject button = env->CallStaticObjectMethod(bridgeClass, getButton, i);
        if (!button) continue;
        jstring buttonModule = static_cast<jstring>(env->GetObjectField(button, moduleIdField));
        if (!buttonModule) {
            env->DeleteLocalRef(button);
            continue;
        }

        // IsSameObject only compares references, and the wanted module id was
        // created fresh above, so in practice the two jstrings are distinct
        // objects and the *contents* must be compared. Skipping the content
        // comparison here silently filtered out every button and left the
        // visible overlay stale until the module was re-added.
        bool moduleMatches = env->IsSameObject(buttonModule, wantedModule);
        if (!moduleMatches) {
            const char* actual = env->GetStringUTFChars(buttonModule, nullptr);
            moduleMatches = actual && moduleIdString == actual;
            if (actual) env->ReleaseStringUTFChars(buttonModule, actual);
        }
        if (!moduleMatches) {
            env->DeleteLocalRef(button);
            env->DeleteLocalRef(buttonModule);
            continue;
        }

        jstring buttonId = static_cast<jstring>(env->GetObjectField(button, buttonIdField));
        if (buttonId) {
            const char* idChars = env->GetStringUTFChars(buttonId, nullptr);
            if (idChars) {
                nativeButtonIds.emplace_back(idChars);
                env->ReleaseStringUTFChars(buttonId, idChars);
            }
        }
        jobject overlay = buttonId ? env->CallObjectMethod(overlays, mapGet, buttonId) : nullptr;
        if (overlay) {
            // Swap in the freshly registered button definition, then re-apply
            // size/colors and the label text to the already visible view.
            env->SetObjectField(overlay, overlayButtonField, button);
            env->CallVoidMethod(overlay, applyChanges);

            // Re-run the view configuration so a changed command/comment label
            // is actually written to the label TextView. Safe to call on an
            // already-shown view; it re-finds the same child views and re-sets
            // their text/icon/colors. applyConfigurationChanges alone does not
            // re-apply the label text on every launcher build.
            if (overlayViewField && configureOverlayView) {
                jobject overlayView = env->GetObjectField(overlay, overlayViewField);
                if (overlayView) {
                    env->CallVoidMethod(overlay, configureOverlayView, overlayView);
                    env->DeleteLocalRef(overlayView);
                }
            }
        } else {
            // Registered natively but the launcher has no overlay for it yet
            // (the slot was just switched on). See the reconciliation below.
            addedButton = true;
        }
        if (buttonId) env->DeleteLocalRef(buttonId);
        if (buttonModule) env->DeleteLocalRef(buttonModule);
        env->DeleteLocalRef(button);
        if (overlay) env->DeleteLocalRef(overlay);
    }
    clearJavaException(env);

    // A slot switched on/off in the module settings changes the native button
    // set. Newly registered buttons have no ExternalButtonOverlay yet and
    // stale overlays are no longer reachable through the loop above, so make
    // the overlay manager reconcile the module's visible button group:
    //   * button added  -> show (re-scans the native registry and creates the
    //     missing overlays without touching the ones already on screen);
    //   * button removed -> hide + show, rebuilding the module's group from
    //     the current registry so the stale overlay is dropped.
    // Pure value edits (label text, size/color) never change the button set,
    // so they keep the no-flicker in-place path above.
    if (handleExternalToggle) {
        if (addedButton) {
            env->CallVoidMethod(manager, handleExternalToggle, wantedModule, JNI_TRUE);
        } else if (hasOrphanedOverlays(env, overlays, moduleIdString, nativeButtonIds)) {
            env->CallVoidMethod(manager, handleExternalToggle, wantedModule, JNI_FALSE);
            env->CallVoidMethod(manager, handleExternalToggle, wantedModule, JNI_TRUE);
        }
        clearJavaException(env);
    }

    if (wantedModule) env->DeleteLocalRef(wantedModule);
    env->DeleteLocalRef(overlays);
    env->DeleteLocalRef(manager);
    if (mapClass) env->DeleteLocalRef(mapClass);
    env->DeleteLocalRef(buttonClass);
    env->DeleteLocalRef(overlayClass);
    env->DeleteLocalRef(bridgeClass);
    env->DeleteLocalRef(managerClass);
}

#endif

} // namespace

void setJavaVm(void* javaVm) {
#if defined(__ANDROID__)
    gJavaVm.store(static_cast<JavaVM*>(javaVm), std::memory_order_release);
#else
    (void)javaVm;
#endif
}

void* javaVm() {
#if defined(__ANDROID__)
    return gJavaVm.load(std::memory_order_acquire);
#else
    return nullptr;
#endif
}

void refreshExternalButtonsForModule(std::string_view moduleId) {
#if !defined(__ANDROID__)
    (void)moduleId;
#else
    JavaVM* vm = gJavaVm.load(std::memory_order_acquire);
    if (!vm) return;

    // The ModMenu config-change callback is not guaranteed to arrive on a
    // thread that is already attached to the JVM (it can be dispatched from a
    // native game/hook thread). GetEnv would report JNI_EDETACHED there and a
    // plain "return" would leave the visible overlay stale - the user would
    // have to remove and re-add the module to see the new text or size. Attach
    // such a thread for the duration of the refresh and detach it afterwards,
    // leaving the caller's thread state exactly as it was.
    JNIEnv* env = nullptr;
    const jint envState = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envState == JNI_OK && env) {
        refreshExternalButtonsAttached(env, moduleId);
        return;
    }
    if (envState != JNI_EDETACHED) return;

    JavaVMAttachArgs attachArgs{
        JNI_VERSION_1_6, const_cast<char*>("BedrockToolsPlus/ButtonRefresh"), nullptr};
    if (vm->AttachCurrentThread(&env, &attachArgs) != JNI_OK || !env) return;

    // Detaching also frees every local reference created above, so the
    // borrowed thread does not leak references into the next attach.
    refreshExternalButtonsAttached(env, moduleId);
    vm->DetachCurrentThread();
#endif
}

} // namespace bedrocktools::launcher
