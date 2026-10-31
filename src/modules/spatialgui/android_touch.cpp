#include "android_touch.hpp"
#include "launcher/ExternalButtonRefresh.hpp"
#include <cmath>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace bedrocktools::spatialgui {
bool AndroidTouch::down(float x, float y) {
    if (m_active || !send(0, x, y)) return false;
    m_active = true; m_x = x; m_y = y; return true;
}
bool AndroidTouch::move(float x, float y) {
    if (!m_active || !send(2, x, y)) return false;
    m_x = x; m_y = y; return true;
}
bool AndroidTouch::up(float x, float y) {
    if (!m_active) return false;
    if (!send(1, x, y)) return false;
    m_active = false; m_downTime = 0; return true;
}
bool AndroidTouch::cancel() {
    if (!m_active) return false;
    const bool ok = send(3, m_x, m_y);
    // Activity/context may have disappeared. Never keep a stale gesture.
    m_active = false; m_downTime = 0; return ok;
}

bool AndroidTouch::send(int action, float x, float y) {
#if !defined(__ANDROID__)
    (void)action; (void)x; (void)y;
    return false;
#else
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    auto* vm = static_cast<JavaVM*>(bedrocktools::launcher::javaVm());
    JNIEnv* env = nullptr;
    // The preloader touch callback runs on the Android UI thread. Do not
    // attach arbitrary render threads or call Activity methods from them.
    if (!vm || vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) return false;
    if (env->PushLocalFrame(16) != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    auto finish = [&](bool ok) {
        if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
        env->PopLocalFrame(nullptr);
        return ok;
    };
    jclass looper = env->FindClass("android/os/Looper");
    if (!looper || env->ExceptionCheck()) return finish(false);
    auto myLooper = env->GetStaticMethodID(looper, "myLooper", "()Landroid/os/Looper;");
    auto mainLooper = env->GetStaticMethodID(looper, "getMainLooper", "()Landroid/os/Looper;");
    if (!myLooper || !mainLooper || env->ExceptionCheck()) return finish(false);
    jobject current = env->CallStaticObjectMethod(looper, myLooper);
    jobject main = env->CallStaticObjectMethod(looper, mainLooper);
    if (env->ExceptionCheck() || !current || !main || !env->IsSameObject(current, main)) return finish(false);

    jclass state = env->FindClass("org/levimc/launcher/core/minecraft/MinecraftActivityState");
    if (!state || env->ExceptionCheck()) return finish(false);
    auto getActivity = env->GetStaticMethodID(state, "getCurrentActivity", "()Landroid/app/Activity;");
    auto resumed = env->GetStaticMethodID(state, "isResumed", "()Z");
    if (!getActivity || !resumed || env->ExceptionCheck()) return finish(false);
    if (!env->CallStaticBooleanMethod(state, resumed) || env->ExceptionCheck()) return finish(false);
    jobject activity = env->CallStaticObjectMethod(state, getActivity);
    if (!activity || env->ExceptionCheck()) return finish(false);
    jclass activityClass = env->GetObjectClass(activity);
    if (!activityClass || env->ExceptionCheck()) return finish(false);
    auto dispatch = env->GetMethodID(activityClass, "dispatchTouchEventToGame", "(Landroid/view/MotionEvent;)Z");
    if (!dispatch || env->ExceptionCheck()) return finish(false);
    jclass clock = env->FindClass("android/os/SystemClock");
    jclass motion = env->FindClass("android/view/MotionEvent");
    if (!clock || !motion || env->ExceptionCheck()) return finish(false);
    auto uptime = env->GetStaticMethodID(clock, "uptimeMillis", "()J");
    auto obtain = env->GetStaticMethodID(motion, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
    auto setSource = env->GetMethodID(motion, "setSource", "(I)V");
    auto recycle = env->GetMethodID(motion, "recycle", "()V");
    if (!uptime || !obtain || !setSource || !recycle || env->ExceptionCheck()) return finish(false);
    const jlong now = env->CallStaticLongMethod(clock, uptime);
    if (env->ExceptionCheck()) return finish(false);
    const jlong start = action == 0 ? now : static_cast<jlong>(m_downTime);
    jobject event = env->CallStaticObjectMethod(motion, obtain, start, now,
                                                static_cast<jint>(action), x, y, 0);
    if (!event || env->ExceptionCheck()) return finish(false);
    // InputDevice.SOURCE_TOUCHSCREEN. Dispatch directly to the game's
    // superclass path, avoiding re-entry into Preloader's touch callback.
    env->CallVoidMethod(event, setSource, 0x1002);
    if (env->ExceptionCheck()) return finish(false);
    env->CallBooleanMethod(activity, dispatch, event);
    if (env->ExceptionCheck()) return finish(false);
    env->CallVoidMethod(event, recycle);
    if (env->ExceptionCheck()) return finish(false);
    if (action == 0) m_downTime = now;
    return finish(true);
#endif
}
}
