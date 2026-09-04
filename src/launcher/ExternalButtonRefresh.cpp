#include "ExternalButtonRefresh.hpp"

#include <atomic>
#include <string>

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

// Runs fn with a JNIEnv for the current thread. Callers are not guaranteed to
// run on a thread that is already attached to the JVM (config callbacks and
// render hooks can be dispatched from native game/hook threads), so a thread
// the JVM does not know about is attached for the duration of the call and
// detached afterwards, leaving the caller's thread state exactly as it was.
// Detaching also frees every local reference created above, so a borrowed
// thread does not leak references into the next attach.
template <typename Fn>
void withAttachedEnv(Fn&& fn) {
    JavaVM* vm = gJavaVm.load(std::memory_order_acquire);
    if (!vm) return;

    JNIEnv* env = nullptr;
    const jint envState = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envState == JNI_OK && env) {
        fn(env);
        return;
    }
    if (envState != JNI_EDETACHED) return;

    JavaVMAttachArgs attachArgs{
        JNI_VERSION_1_6, const_cast<char*>("BedrockToolsPlus/ButtonRefresh"), nullptr};
    if (vm->AttachCurrentThread(&env, &attachArgs) != JNI_OK || !env) return;

    fn(env);
    vm->DetachCurrentThread();
}

// Looks up a field that may live on the class itself or on any superclass
// depending on the launcher build (ExternalButtonOverlay.overlayView moved
// between the class and its base across builds).
jfieldID findFieldInHierarchy(JNIEnv* env, jclass start, const char* name, const char* sig) {
    jfieldID found = nullptr;
    jclass walker = static_cast<jclass>(env->NewLocalRef(start));
    while (walker && !found) {
        found = env->GetFieldID(walker, name, sig);
        clearJavaException(env);
        if (found) break;
        jclass next = env->GetSuperclass(walker);
        env->DeleteLocalRef(walker);
        walker = next;
    }
    if (walker) env->DeleteLocalRef(walker);
    return found;
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
        }
        if (buttonId) env->DeleteLocalRef(buttonId);
        if (buttonModule) env->DeleteLocalRef(buttonModule);
        env->DeleteLocalRef(button);
        if (overlay) env->DeleteLocalRef(overlay);
    }
    clearJavaException(env);
    if (wantedModule) env->DeleteLocalRef(wantedModule);
    env->DeleteLocalRef(overlays);
    env->DeleteLocalRef(manager);
    if (mapClass) env->DeleteLocalRef(mapClass);
    env->DeleteLocalRef(buttonClass);
    env->DeleteLocalRef(overlayClass);
    env->DeleteLocalRef(bridgeClass);
    env->DeleteLocalRef(managerClass);
}

// android.view.View visibility bits (View.VISIBILITY_MASK). The field and the
// mask have been stable since API 1, but the lookup below is still optional:
// builds that hide the field simply report every laid-out button as visible.
constexpr jint ViewVisibilityMask = 0x0000000C;

// Reads one overlay's view geometry. Returns false when the view is missing,
// has no size yet, or is not visible, in which case the caller falls back to
// whatever it would draw without button geometry.
bool viewGeometry(JNIEnv* env, jobject view, jmethodID getLocation, jmethodID getWidth,
                  jmethodID getHeight, jfieldID visibilityField, ButtonGeometry& out) {
    if (!view || env->ExceptionCheck()) return false;

    if (visibilityField) {
        const jint flags = env->GetIntField(view, visibilityField);
        if (env->ExceptionCheck() || (flags & ViewVisibilityMask) != 0) {
            clearJavaException(env);
            return false;
        }
    }

    const jint width = env->CallIntMethod(view, getWidth);
    const jint height = env->CallIntMethod(view, getHeight);
    if (env->ExceptionCheck() || width <= 0 || height <= 0) {
        clearJavaException(env);
        return false;
    }

    jintArray location = env->NewIntArray(2);
    if (!location || env->ExceptionCheck()) {
        clearJavaException(env);
        return false;
    }
    env->CallVoidMethod(view, getLocation, location);
    jint xy[2] = {0, 0};
    if (!env->ExceptionCheck()) env->GetIntArrayRegion(location, 0, 2, xy);
    clearJavaException(env);
    env->DeleteLocalRef(location);

    out.x = static_cast<float>(xy[0]);
    out.y = static_cast<float>(xy[1]);
    out.width = static_cast<float>(width);
    out.height = static_cast<float>(height);
    return true;
}

std::vector<ButtonGeometry> queryButtonGeometryAttached(JNIEnv* env, std::string_view moduleId) {
    std::vector<ButtonGeometry> result;
    constexpr const char* managerName =
        "org/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager";
    constexpr const char* bridgeName =
        "org/levimc/launcher/core/mods/inbuilt/ExternalModBridge";
    constexpr const char* buttonName =
        "org/levimc/launcher/core/mods/inbuilt/ExternalModBridge$ExternalButton";
    constexpr const char* overlayName =
        "org/levimc/launcher/core/mods/inbuilt/overlay/ExternalButtonOverlay";
    constexpr const char* viewName = "android/view/View";

    jclass managerClass = env->FindClass(managerName);
    jclass bridgeClass = env->FindClass(bridgeName);
    jclass buttonClass = env->FindClass(buttonName);
    jclass overlayClass = env->FindClass(overlayName);
    jclass viewClass = env->FindClass(viewName);
    if (!managerClass || !bridgeClass || !buttonClass || !overlayClass || !viewClass ||
        env->ExceptionCheck()) {
        clearJavaException(env);
        // The query runs repeatedly, so every early return below releases the
        // class references it resolved instead of accumulating them on the
        // (possibly permanently attached) calling thread.
        if (managerClass) env->DeleteLocalRef(managerClass);
        if (bridgeClass) env->DeleteLocalRef(bridgeClass);
        if (buttonClass) env->DeleteLocalRef(buttonClass);
        if (overlayClass) env->DeleteLocalRef(overlayClass);
        if (viewClass) env->DeleteLocalRef(viewClass);
        return result;
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
    jmethodID getLocation = env->GetMethodID(viewClass, "getLocationOnScreen", "([I)V");
    jmethodID getWidth = env->GetMethodID(viewClass, "getWidth", "()I");
    jmethodID getHeight = env->GetMethodID(viewClass, "getHeight", "()I");
    if (!getInstance || !getCount || !getButton || !overlaysField || !mapGet ||
        !buttonIdField || !moduleIdField || !getLocation || !getWidth || !getHeight ||
        env->ExceptionCheck()) {
        clearJavaException(env);
        if (mapClass) env->DeleteLocalRef(mapClass);
        env->DeleteLocalRef(viewClass);
        env->DeleteLocalRef(buttonClass);
        env->DeleteLocalRef(overlayClass);
        env->DeleteLocalRef(bridgeClass);
        env->DeleteLocalRef(managerClass);
        return result;
    }

    jfieldID overlayViewField = findFieldInHierarchy(env, overlayClass, "overlayView", "Landroid/view/View;");
    jfieldID visibilityField = env->GetFieldID(viewClass, "mViewFlags", "I");
    clearJavaException(env); // optional: without it every laid-out button counts as visible
    if (!overlayViewField) {
        if (mapClass) env->DeleteLocalRef(mapClass);
        env->DeleteLocalRef(viewClass);
        env->DeleteLocalRef(buttonClass);
        env->DeleteLocalRef(overlayClass);
        env->DeleteLocalRef(bridgeClass);
        env->DeleteLocalRef(managerClass);
        return result;
    }

    jobject manager = env->CallStaticObjectMethod(managerClass, getInstance);
    jobject overlays = manager ? env->GetObjectField(manager, overlaysField) : nullptr;
    if (!manager || !overlays || env->ExceptionCheck()) {
        clearJavaException(env);
        if (mapClass) env->DeleteLocalRef(mapClass);
        env->DeleteLocalRef(viewClass);
        env->DeleteLocalRef(buttonClass);
        env->DeleteLocalRef(overlayClass);
        env->DeleteLocalRef(bridgeClass);
        env->DeleteLocalRef(managerClass);
        return result;
    }

    const std::string moduleIdString(moduleId);
    jstring wantedModule = env->NewStringUTF(moduleIdString.c_str());
    const jint count = env->CallStaticIntMethod(bridgeClass, getCount);
    for (jint i = 0; i < count && !env->ExceptionCheck(); ++i) {
        jobject button = env->CallStaticObjectMethod(bridgeClass, getButton, i);
        if (!button) continue;
        jstring buttonModule = static_cast<jstring>(env->GetObjectField(button, moduleIdField));
        if (!buttonModule) {
            env->DeleteLocalRef(button);
            continue;
        }

        // Same content comparison as the refresh path: the wanted module id
        // is a fresh jstring, so reference equality never matches.
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
        jobject overlay = buttonId ? env->CallObjectMethod(overlays, mapGet, buttonId) : nullptr;
        jobject view = overlay ? env->GetObjectField(overlay, overlayViewField) : nullptr;
        if (view) {
            ButtonGeometry entry;
            if (viewGeometry(env, view, getLocation, getWidth, getHeight, visibilityField, entry)) {
                const char* idChars = buttonId ? env->GetStringUTFChars(buttonId, nullptr) : nullptr;
                entry.buttonId = idChars ? idChars : "";
                if (idChars) env->ReleaseStringUTFChars(buttonId, idChars);
                result.push_back(std::move(entry));
            }
            env->DeleteLocalRef(view);
        }
        if (buttonId) env->DeleteLocalRef(buttonId);
        if (buttonModule) env->DeleteLocalRef(buttonModule);
        env->DeleteLocalRef(button);
        if (overlay) env->DeleteLocalRef(overlay);
    }
    clearJavaException(env);
    if (wantedModule) env->DeleteLocalRef(wantedModule);
    env->DeleteLocalRef(overlays);
    env->DeleteLocalRef(manager);
    if (mapClass) env->DeleteLocalRef(mapClass);
    env->DeleteLocalRef(viewClass);
    env->DeleteLocalRef(buttonClass);
    env->DeleteLocalRef(overlayClass);
    env->DeleteLocalRef(bridgeClass);
    env->DeleteLocalRef(managerClass);
    return result;
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
    withAttachedEnv([&](JNIEnv* env) { refreshExternalButtonsAttached(env, moduleId); });
#endif
}

std::vector<ButtonGeometry> queryButtonGeometry(std::string_view moduleId) {
#if !defined(__ANDROID__)
    (void)moduleId;
    return {};
#else
    std::vector<ButtonGeometry> result;
    withAttachedEnv([&](JNIEnv* env) { result = queryButtonGeometryAttached(env, moduleId); });
    return result;
#endif
}

} // namespace bedrocktools::launcher
