#include "ExternalButtonGeometry.hpp"

#include "ExternalButtonRefresh.hpp"

#include <string>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace bedrocktools::launcher {
namespace {

#if defined(__ANDROID__)

void clearJavaException(JNIEnv* env) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
}

// Walks the class chain so a field declared on a base class (BaseOverlayButton)
// is found even though the overlay object's class is ExternalButtonOverlay.
jfieldID findFieldInHierarchy(JNIEnv* env, jclass klass, const char* name, const char* sig) {
    jfieldID field = nullptr;
    jclass walker = static_cast<jclass>(env->NewLocalRef(klass));
    while (walker && !field) {
        field = env->GetFieldID(walker, name, sig);
        clearJavaException(env);
        if (field) break;
        jclass next = env->GetSuperclass(walker);
        env->DeleteLocalRef(walker);
        walker = next;
    }
    if (walker) env->DeleteLocalRef(walker);
    return field;
}

// Size of the surface the button coordinates live in: the game's decor view,
// falling back to the display metrics (same order the launcher's own
// HotbarSlotOverlay uses).
void readSurfaceSize(JNIEnv* env, jobject activity, ExternalButtonGeometry& out) {
    if (!activity) return;
    jclass activityClass = env->GetObjectClass(activity);
    if (!activityClass) return;

    jmethodID getWindow = env->GetMethodID(activityClass, "getWindow", "()Landroid/view/Window;");
    clearJavaException(env);
    if (getWindow) {
        jobject window = env->CallObjectMethod(activity, getWindow);
        clearJavaException(env);
        if (window) {
            jclass windowClass = env->GetObjectClass(window);
            jmethodID getDecorView = windowClass
                ? env->GetMethodID(windowClass, "getDecorView", "()Landroid/view/View;")
                : nullptr;
            clearJavaException(env);
            jobject decor = getDecorView ? env->CallObjectMethod(window, getDecorView) : nullptr;
            clearJavaException(env);
            if (decor) {
                jclass viewClass = env->GetObjectClass(decor);
                jmethodID getWidth = viewClass ? env->GetMethodID(viewClass, "getWidth", "()I") : nullptr;
                jmethodID getHeight = viewClass ? env->GetMethodID(viewClass, "getHeight", "()I") : nullptr;
                clearJavaException(env);
                if (getWidth && getHeight) {
                    const jint w = env->CallIntMethod(decor, getWidth);
                    const jint h = env->CallIntMethod(decor, getHeight);
                    clearJavaException(env);
                    if (w > 0 && h > 0) {
                        out.screenWidth = static_cast<float>(w);
                        out.screenHeight = static_cast<float>(h);
                    }
                }
                if (viewClass) env->DeleteLocalRef(viewClass);
                env->DeleteLocalRef(decor);
            }
            if (windowClass) env->DeleteLocalRef(windowClass);
            env->DeleteLocalRef(window);
        }
    }

    if (out.screenWidth <= 0.0f || out.screenHeight <= 0.0f) {
        jmethodID getResources =
            env->GetMethodID(activityClass, "getResources", "()Landroid/content/res/Resources;");
        clearJavaException(env);
        jobject resources = getResources ? env->CallObjectMethod(activity, getResources) : nullptr;
        clearJavaException(env);
        if (resources) {
            jclass resourcesClass = env->GetObjectClass(resources);
            jmethodID getMetrics = resourcesClass
                ? env->GetMethodID(resourcesClass, "getDisplayMetrics",
                                   "()Landroid/util/DisplayMetrics;")
                : nullptr;
            clearJavaException(env);
            jobject metrics = getMetrics ? env->CallObjectMethod(resources, getMetrics) : nullptr;
            clearJavaException(env);
            if (metrics) {
                jclass metricsClass = env->GetObjectClass(metrics);
                jfieldID widthPixels = metricsClass ? env->GetFieldID(metricsClass, "widthPixels", "I") : nullptr;
                jfieldID heightPixels = metricsClass ? env->GetFieldID(metricsClass, "heightPixels", "I") : nullptr;
                clearJavaException(env);
                if (widthPixels && heightPixels) {
                    out.screenWidth = static_cast<float>(env->GetIntField(metrics, widthPixels));
                    out.screenHeight = static_cast<float>(env->GetIntField(metrics, heightPixels));
                }
                if (metricsClass) env->DeleteLocalRef(metricsClass);
                env->DeleteLocalRef(metrics);
            }
            if (resourcesClass) env->DeleteLocalRef(resourcesClass);
            env->DeleteLocalRef(resources);
        }
    }

    env->DeleteLocalRef(activityClass);
}

bool queryAttached(JNIEnv* env, std::string_view buttonId, ExternalButtonGeometry& out) {
    constexpr const char* managerName =
        "org/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager";

    jclass managerClass = env->FindClass(managerName);
    if (!managerClass || env->ExceptionCheck()) {
        clearJavaException(env);
        return false;
    }

    jclass mapClass = env->FindClass("java/util/Map");
    jmethodID getInstance = env->GetStaticMethodID(
        managerClass, "getInstance",
        "()Lorg/levimc/launcher/core/mods/inbuilt/overlay/InbuiltOverlayManager;");
    jfieldID overlaysField =
        env->GetFieldID(managerClass, "externalButtonOverlayMap", "Ljava/util/Map;");
    jmethodID mapGet = mapClass
        ? env->GetMethodID(mapClass, "get", "(Ljava/lang/Object;)Ljava/lang/Object;")
        : nullptr;
    if (!getInstance || !overlaysField || !mapGet || env->ExceptionCheck()) {
        clearJavaException(env);
        if (mapClass) env->DeleteLocalRef(mapClass);
        env->DeleteLocalRef(managerClass);
        return false;
    }

    jobject manager = env->CallStaticObjectMethod(managerClass, getInstance);
    jobject overlays = manager ? env->GetObjectField(manager, overlaysField) : nullptr;
    if (!manager || !overlays || env->ExceptionCheck()) {
        clearJavaException(env);
        if (manager) env->DeleteLocalRef(manager);
        if (overlays) env->DeleteLocalRef(overlays);
        if (mapClass) env->DeleteLocalRef(mapClass);
        env->DeleteLocalRef(managerClass);
        return false;
    }

    const std::string id(buttonId);
    jstring key = env->NewStringUTF(id.c_str());
    jobject overlay = key ? env->CallObjectMethod(overlays, mapGet, key) : nullptr;
    clearJavaException(env);

    bool ok = false;
    if (overlay) {
        jclass overlayClass = env->GetObjectClass(overlay);

        jfieldID showingField = findFieldInHierarchy(env, overlayClass, "isShowing", "Z");
        const bool showing = showingField ? env->GetBooleanField(overlay, showingField) == JNI_TRUE : true;

        jfieldID viewField = findFieldInHierarchy(env, overlayClass, "overlayView", "Landroid/view/View;");
        jobject view = viewField ? env->GetObjectField(overlay, viewField) : nullptr;
        clearJavaException(env);

        // The window-manager path is what the launcher uses normally; the
        // FrameLayout path is its fallback when adding a window fails.
        jfieldID paramsField = findFieldInHierarchy(env, overlayClass, "wmParams",
                                                    "Landroid/view/WindowManager$LayoutParams;");
        jobject params = paramsField ? env->GetObjectField(overlay, paramsField) : nullptr;
        clearJavaException(env);

        if (params) {
            jclass paramsClass = env->GetObjectClass(params);
            jfieldID xField = env->GetFieldID(paramsClass, "x", "I");
            jfieldID yField = env->GetFieldID(paramsClass, "y", "I");
            jfieldID wField = findFieldInHierarchy(env, paramsClass, "width", "I");
            jfieldID hField = findFieldInHierarchy(env, paramsClass, "height", "I");
            clearJavaException(env);
            if (xField && yField && wField && hField) {
                out.x = static_cast<float>(env->GetIntField(params, xField));
                out.y = static_cast<float>(env->GetIntField(params, yField));
                out.width = static_cast<float>(env->GetIntField(params, wField));
                out.height = static_cast<float>(env->GetIntField(params, hField));
                ok = true;
            }
            if (paramsClass) env->DeleteLocalRef(paramsClass);
            env->DeleteLocalRef(params);
        } else if (view) {
            jclass viewClass = env->GetObjectClass(view);
            jmethodID getLayoutParams =
                env->GetMethodID(viewClass, "getLayoutParams", "()Landroid/view/ViewGroup$LayoutParams;");
            clearJavaException(env);
            jobject layout = getLayoutParams ? env->CallObjectMethod(view, getLayoutParams) : nullptr;
            clearJavaException(env);
            jclass frameClass = env->FindClass("android/widget/FrameLayout$MarginLayoutParams");
            clearJavaException(env);
            if (!frameClass) frameClass = env->FindClass("android/view/ViewGroup$MarginLayoutParams");
            clearJavaException(env);
            if (layout && frameClass && env->IsInstanceOf(layout, frameClass)) {
                jclass layoutClass = env->GetObjectClass(layout);
                jfieldID leftField = findFieldInHierarchy(env, layoutClass, "leftMargin", "I");
                jfieldID topField = findFieldInHierarchy(env, layoutClass, "topMargin", "I");
                jfieldID wField = findFieldInHierarchy(env, layoutClass, "width", "I");
                jfieldID hField = findFieldInHierarchy(env, layoutClass, "height", "I");
                if (leftField && topField && wField && hField) {
                    out.x = static_cast<float>(env->GetIntField(layout, leftField));
                    out.y = static_cast<float>(env->GetIntField(layout, topField));
                    out.width = static_cast<float>(env->GetIntField(layout, wField));
                    out.height = static_cast<float>(env->GetIntField(layout, hField));
                    ok = true;
                }
                if (layoutClass) env->DeleteLocalRef(layoutClass);
            }
            if (frameClass) env->DeleteLocalRef(frameClass);
            if (layout) env->DeleteLocalRef(layout);
            if (viewClass) env->DeleteLocalRef(viewClass);
        }

        // A hidden view must not get an icon painted where it used to be.
        bool viewVisible = showing;
        if (view) {
            jclass viewClass = env->GetObjectClass(view);
            jmethodID getVisibility = viewClass ? env->GetMethodID(viewClass, "getVisibility", "()I") : nullptr;
            clearJavaException(env);
            if (getVisibility) {
                const jint visibility = env->CallIntMethod(view, getVisibility);
                clearJavaException(env);
                viewVisible = showing && visibility == 0; // View.VISIBLE
            }
            if (viewClass) env->DeleteLocalRef(viewClass);
        }
        out.visible = viewVisible;

        jfieldID activityField =
            findFieldInHierarchy(env, overlayClass, "activity", "Landroid/app/Activity;");
        jobject activity = activityField ? env->GetObjectField(overlay, activityField) : nullptr;
        clearJavaException(env);
        if (activity) {
            readSurfaceSize(env, activity, out);
            env->DeleteLocalRef(activity);
        }

        if (view) env->DeleteLocalRef(view);
        if (overlayClass) env->DeleteLocalRef(overlayClass);
        env->DeleteLocalRef(overlay);
    }

    if (key) env->DeleteLocalRef(key);
    env->DeleteLocalRef(overlays);
    env->DeleteLocalRef(manager);
    if (mapClass) env->DeleteLocalRef(mapClass);
    env->DeleteLocalRef(managerClass);
    clearJavaException(env);

    return ok && out.valid();
}

#endif // __ANDROID__

} // namespace

bool queryExternalButtonGeometry(std::string_view buttonId, ExternalButtonGeometry& out) {
#if !defined(__ANDROID__)
    (void)buttonId;
    (void)out;
    return false;
#else
    auto* vm = static_cast<JavaVM*>(javaVm());
    if (!vm) return false;

    JNIEnv* env = nullptr;
    const jint envState = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envState == JNI_OK && env) return queryAttached(env, buttonId, out);
    if (envState != JNI_EDETACHED) return false;

    JavaVMAttachArgs attachArgs{
        JNI_VERSION_1_6, const_cast<char*>("BedrockToolsPlus/ButtonGeometry"), nullptr};
    if (vm->AttachCurrentThread(&env, &attachArgs) != JNI_OK || !env) return false;
    const bool ok = queryAttached(env, buttonId, out);
    vm->DetachCurrentThread();
    return ok;
#endif
}

} // namespace bedrocktools::launcher
