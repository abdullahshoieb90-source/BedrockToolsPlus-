// Host-side tests for reading the on-screen geometry of a launcher overlay
// button (Android JNI path).
//
// This is what lets the Hotbar Slots module paint the item of a hotbar slot
// *on the slot button itself* ("Use item icons from hotbar" in LeviLauncher)
// instead of on a separate HUD strip: the launcher owns the button views, so
// the module asks it where each button currently sits.
//
// Covered:
//   * the WindowManager.LayoutParams path (the launcher's normal case),
//   * the FrameLayout fallback path (leftMargin / topMargin),
//   * the surface size read from the activity's decor view,
//   * a hidden button reporting visible == false so no icon is painted where
//     the button used to be,
//   * a caller thread that is not attached to the JVM is attached and detached
//     again, and a missing launcher class fails cleanly (no leaked attach).
//
// Build and run standalone:
//     g++ -std=c++20 -Wall -Wextra -I src -I tests/fakejni
//         tests/externalbuttongeometry_test.cpp src/launcher/ExternalButtonRefresh.cpp
//         -o /tmp/g && /tmp/g
//
// (ExternalButtonRefresh.cpp is linked in for setJavaVm/javaVm; it is built
// with __ANDROID__ defined on the command line by scripts/run_tests.sh.)

#include "fakejni/jni.h"

#define __ANDROID__ 1
#include "launcher/ExternalButtonGeometry.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) std::printf("  ok   %s\n", what);
    else { std::printf("  FAIL %s\n", what); ++g_failures; }
}

void checkNear(float actual, float expected, const char* what) {
    const bool ok = (actual - expected) < 0.01f && (expected - actual) < 0.01f;
    if (ok) std::printf("  ok   %s\n", what);
    else { std::printf("  FAIL %s: expected %.2f, got %.2f\n", what, expected, actual); ++g_failures; }
}

// ---------------------------------------------------------------------------
// Fake launcher object model
// ---------------------------------------------------------------------------

struct FakeClass;

struct FakeObject : _jobject {
    std::string kind;
    std::string text;
    FakeClass* klass = nullptr;
    FakeObject() = default;
    FakeObject(std::string k, std::string t = {}) : kind(std::move(k)), text(std::move(t)) {}
};

struct FakeClass : _jclass {
    std::string name;
    FakeClass* super = nullptr;
    std::vector<std::string> fields;
    FakeClass(std::string n, FakeClass* s, std::vector<std::string> f)
        : name(std::move(n)), super(s), fields(std::move(f)) {}
};

struct FakeMethod : _jmethodID {
    const char* name;
    explicit FakeMethod(const char* n) : name(n) {}
};
struct FakeField : _jfieldID {
    const char* name;
    explicit FakeField(const char* n) : name(n) {}
};

// "overlayView" / "wmParams" / "activity" live on the base class, mirroring
// BaseOverlayButton, so the field lookup has to walk the class chain.
FakeClass gManagerClass{"InbuiltOverlayManager", nullptr, {"externalButtonOverlayMap"}};
FakeClass gMapClass{"java/util/Map", nullptr, {}};
FakeClass gOverlayBase{"BaseOverlayButton", nullptr,
                       {"overlayView", "wmParams", "activity", "isShowing"}};
FakeClass gOverlayClass{"ExternalButtonOverlay", &gOverlayBase, {"button"}};
FakeClass gParamsClass{"WindowManager$LayoutParams", nullptr, {"x", "y", "width", "height"}};
FakeClass gFrameParamsClass{"FrameLayout$LayoutParams", nullptr,
                            {"leftMargin", "topMargin", "width", "height"}};
FakeClass gFrameMarginClass{"android/widget/FrameLayout$MarginLayoutParams", nullptr, {}};
FakeClass gViewClass{"View", nullptr, {}};
FakeClass gActivityClass{"Activity", nullptr, {}};
FakeClass gWindowClass{"Window", nullptr, {}};

FakeMethod mGetInstance{"getInstance"};
FakeMethod mMapGet{"get"};
FakeMethod mGetLayoutParams{"getLayoutParams"};
FakeMethod mGetVisibility{"getVisibility"};
FakeMethod mGetWindow{"getWindow"};
FakeMethod mGetDecorView{"getDecorView"};
FakeMethod mGetWidth{"getWidth"};
FakeMethod mGetHeight{"getHeight"};

FakeField fOverlays{"externalButtonOverlayMap"};
FakeField fOverlayView{"overlayView"};
FakeField fWmParams{"wmParams"};
FakeField fActivity{"activity"};
FakeField fShowing{"isShowing"};
FakeField fX{"x"};
FakeField fY{"y"};
FakeField fWidth{"width"};
FakeField fHeight{"height"};
FakeField fLeft{"leftMargin"};
FakeField fTop{"topMargin"};

FakeObject gManager{"manager"};
FakeObject gMap{"map"};
FakeObject gOverlay{"overlay"};
FakeObject gView{"view"};
FakeObject gWmParams{"params"};
FakeObject gFrameParams{"frameparams"};
FakeObject gActivity{"activity"};
FakeObject gWindow{"window"};
FakeObject gDecor{"view"};

// Scripted state the hooks report.
struct World {
    bool classesReachable = true;
    bool showing = true;
    int visibility = 0; // View.VISIBLE
    bool useWindowManager = true;
    int x = 300, y = 900, width = 120, height = 120;
    int decorWidth = 2400, decorHeight = 1080;
} g_world;

int g_attachCalls = 0;
int g_detachCalls = 0;

FakeObject* asFake(jobject o) { return static_cast<FakeObject*>(o); }

jclass hk_FindClass(JNIEnv*, const char* name) {
    if (!g_world.classesReachable) return nullptr;
    if (std::strstr(name, "InbuiltOverlayManager")) return &gManagerClass;
    if (std::strcmp(name, "java/util/Map") == 0) return &gMapClass;
    if (std::strstr(name, "MarginLayoutParams")) return &gFrameMarginClass;
    return nullptr;
}

jmethodID hk_GetStaticMethodID(JNIEnv*, jclass c, const char* name, const char*) {
    if (c == &gManagerClass && std::strcmp(name, "getInstance") == 0) return &mGetInstance;
    return nullptr;
}

jmethodID hk_GetMethodID(JNIEnv*, jclass c, const char* name, const char*) {
    if (c == &gMapClass && std::strcmp(name, "get") == 0) return &mMapGet;
    if (c == &gViewClass && std::strcmp(name, "getLayoutParams") == 0) return &mGetLayoutParams;
    if (c == &gViewClass && std::strcmp(name, "getVisibility") == 0) return &mGetVisibility;
    if (c == &gViewClass && std::strcmp(name, "getWidth") == 0) return &mGetWidth;
    if (c == &gViewClass && std::strcmp(name, "getHeight") == 0) return &mGetHeight;
    if (c == &gActivityClass && std::strcmp(name, "getWindow") == 0) return &mGetWindow;
    if (c == &gWindowClass && std::strcmp(name, "getDecorView") == 0) return &mGetDecorView;
    return nullptr;
}

jfieldID hk_GetFieldID(JNIEnv*, jclass c, const char* name, const char*) {
    auto* fc = static_cast<FakeClass*>(c);
    for (const auto& f : fc->fields) {
        if (f != name) continue;
        if (fc == &gManagerClass) return &fOverlays;
        if (fc == &gOverlayBase) {
            if (std::strcmp(name, "overlayView") == 0) return &fOverlayView;
            if (std::strcmp(name, "wmParams") == 0) return &fWmParams;
            if (std::strcmp(name, "activity") == 0) return &fActivity;
            if (std::strcmp(name, "isShowing") == 0) return &fShowing;
        }
        if (fc == &gParamsClass) {
            if (std::strcmp(name, "x") == 0) return &fX;
            if (std::strcmp(name, "y") == 0) return &fY;
            if (std::strcmp(name, "width") == 0) return &fWidth;
            if (std::strcmp(name, "height") == 0) return &fHeight;
        }
        if (fc == &gFrameParamsClass) {
            if (std::strcmp(name, "leftMargin") == 0) return &fLeft;
            if (std::strcmp(name, "topMargin") == 0) return &fTop;
            if (std::strcmp(name, "width") == 0) return &fWidth;
            if (std::strcmp(name, "height") == 0) return &fHeight;
        }
    }
    return nullptr;
}

jclass hk_GetSuperclass(JNIEnv*, jclass c) { return static_cast<FakeClass*>(c)->super; }
jobject hk_NewLocalRef(JNIEnv*, jobject o) { return o; }

jclass hk_GetObjectClass(JNIEnv*, jobject o) {
    auto* f = asFake(o);
    return f->klass ? static_cast<jclass>(f->klass) : nullptr;
}

jobject hk_CallStaticObjectMethod(JNIEnv*, jclass, jmethodID m, va_list) {
    return m == &mGetInstance ? &gManager : nullptr;
}

jint hk_CallStaticIntMethod(JNIEnv*, jclass, jmethodID, va_list) { return 0; }

jobject hk_CallObjectMethod(JNIEnv*, jobject o, jmethodID m, va_list ap) {
    if (m == &mMapGet && o == &gMap) {
        jobject key = va_arg(ap, jobject);
        return asFake(key)->text == "bedrocktoolsplus.HotbarSlots.Button1" ? &gOverlay : nullptr;
    }
    if (m == &mGetLayoutParams) return &gFrameParams;
    if (m == &mGetWindow) return &gWindow;
    if (m == &mGetDecorView) return &gDecor;
    return nullptr;
}

jint hk_CallIntMethod(JNIEnv*, jobject o, jmethodID m, va_list) {
    if (m == &mGetVisibility) return g_world.visibility;
    if (m == &mGetWidth && o == &gDecor) return g_world.decorWidth;
    if (m == &mGetHeight && o == &gDecor) return g_world.decorHeight;
    return 0;
}

void hk_CallVoidMethod(JNIEnv*, jobject, jmethodID, va_list) {}

jobject hk_GetObjectField(JNIEnv*, jobject, jfieldID f) {
    if (f == &fOverlays) return &gMap;
    if (f == &fOverlayView) return &gView;
    if (f == &fWmParams) return g_world.useWindowManager ? &gWmParams : nullptr;
    if (f == &fActivity) return &gActivity;
    return nullptr;
}

jint hk_GetIntField(JNIEnv*, jobject o, jfieldID f) {
    if (o == &gWmParams || o == &gFrameParams) {
        if (f == &fX || f == &fLeft) return g_world.x;
        if (f == &fY || f == &fTop) return g_world.y;
        if (f == &fWidth) return g_world.width;
        if (f == &fHeight) return g_world.height;
    }
    return 0;
}

jboolean hk_GetBooleanField(JNIEnv*, jobject, jfieldID f) {
    if (f == &fShowing) return g_world.showing ? JNI_TRUE : JNI_FALSE;
    return JNI_FALSE;
}

void hk_SetObjectField(JNIEnv*, jobject, jfieldID, jobject) {}

jstring hk_NewStringUTF(JNIEnv*, const char* s) {
    static std::vector<FakeObject*> pool;
    pool.push_back(new FakeObject{"string", s});
    return pool.back();
}
jsize hk_GetStringUTFLength(JNIEnv*, jstring s) { return static_cast<jsize>(asFake(s)->text.size()); }
const char* hk_GetStringUTFChars(JNIEnv*, jstring s, jboolean* isCopy) {
    if (isCopy) *isCopy = JNI_FALSE;
    return asFake(s)->text.c_str();
}
void hk_ReleaseStringUTFChars(JNIEnv*, jstring, const char*) {}
jboolean hk_ExceptionCheck(JNIEnv*) { return JNI_FALSE; }
void hk_ExceptionClear(JNIEnv*) {}
void hk_DeleteLocalRef(JNIEnv*, jobject) {}
jboolean hk_IsSameObject(JNIEnv*, jobject a, jobject b) { return a == b; }
jboolean hk_IsInstanceOf(JNIEnv*, jobject o, jclass c) {
    return (o == &gFrameParams && c == &gFrameMarginClass) ? JNI_TRUE : JNI_FALSE;
}

enum class EnvMode { Detached, Attached };
EnvMode g_envMode = EnvMode::Detached;
JNIEnv g_env;
JavaVM g_vm;

jint hk_GetEnv(JavaVM*, void** out, jint) {
    if (g_envMode == EnvMode::Detached) return JNI_EDETACHED;
    *reinterpret_cast<JNIEnv**>(out) = &g_env;
    return JNI_OK;
}
jint hk_AttachCurrentThread(JavaVM*, JNIEnv** out, void*) {
    ++g_attachCalls;
    *out = &g_env;
    return JNI_OK;
}
jint hk_DetachCurrentThread(JavaVM*) { ++g_detachCalls; return JNI_OK; }

void installHooks() {
    JNIEnv& e = g_env;
    e.FindClassFn = hk_FindClass;
    e.GetStaticMethodIDFn = hk_GetStaticMethodID;
    e.GetMethodIDFn = hk_GetMethodID;
    e.GetFieldIDFn = hk_GetFieldID;
    e.GetSuperclassFn = hk_GetSuperclass;
    e.NewLocalRefFn = hk_NewLocalRef;
    e.CallStaticObjectMethodVFn = hk_CallStaticObjectMethod;
    e.CallStaticIntMethodVFn = hk_CallStaticIntMethod;
    e.CallObjectMethodVFn = hk_CallObjectMethod;
    e.CallVoidMethodVFn = hk_CallVoidMethod;
    e.CallIntMethodVFn = hk_CallIntMethod;
    e.GetObjectFieldFn = hk_GetObjectField;
    e.SetObjectFieldFn = hk_SetObjectField;
    e.GetIntFieldFn = hk_GetIntField;
    e.GetBooleanFieldFn = hk_GetBooleanField;
    e.NewStringUTFFn = hk_NewStringUTF;
    e.GetStringUTFLengthFn = hk_GetStringUTFLength;
    e.GetStringUTFCharsFn = hk_GetStringUTFChars;
    e.ReleaseStringUTFCharsFn = hk_ReleaseStringUTFChars;
    e.ExceptionCheckFn = hk_ExceptionCheck;
    e.ExceptionClearFn = hk_ExceptionClear;
    e.DeleteLocalRefFn = hk_DeleteLocalRef;
    e.IsSameObjectFn = hk_IsSameObject;
    e.IsInstanceOfFn = hk_IsInstanceOf;
    e.GetObjectClassFn = hk_GetObjectClass;

    g_vm.GetEnvFn = hk_GetEnv;
    g_vm.AttachCurrentThreadFn = hk_AttachCurrentThread;
    g_vm.DetachCurrentThreadFn = hk_DetachCurrentThread;

    gOverlay.klass = &gOverlayClass;
    gView.klass = &gViewClass;
    gDecor.klass = &gViewClass;
    gWmParams.klass = &gParamsClass;
    gFrameParams.klass = &gFrameParamsClass;
    gActivity.klass = &gActivityClass;
    gWindow.klass = &gWindowClass;
}

void resetWorld() {
    g_world = World{};
    g_attachCalls = g_detachCalls = 0;
}

using bedrocktools::launcher::ExternalButtonGeometry;
using bedrocktools::launcher::queryExternalButtonGeometry;

constexpr const char* kButton = "bedrocktoolsplus.HotbarSlots.Button1";

} // namespace

int main() {
    installHooks();
    bedrocktools::launcher::setJavaVm(&g_vm);

    std::printf("window manager geometry\n");
    resetWorld();
    g_envMode = EnvMode::Attached;
    {
        ExternalButtonGeometry geometry;
        check(queryExternalButtonGeometry(kButton, geometry), "query succeeds");
        checkNear(geometry.x, 300.0f, "x");
        checkNear(geometry.y, 900.0f, "y");
        checkNear(geometry.width, 120.0f, "width");
        checkNear(geometry.height, 120.0f, "height");
        checkNear(geometry.screenWidth, 2400.0f, "surface width from decor view");
        checkNear(geometry.screenHeight, 1080.0f, "surface height from decor view");
        check(geometry.visible, "visible button reports visible");
        check(geometry.valid(), "geometry is usable");
    }

    std::printf("frame layout fallback\n");
    resetWorld();
    g_world.useWindowManager = false;
    g_world.x = 42;
    g_world.y = 84;
    {
        ExternalButtonGeometry geometry;
        check(queryExternalButtonGeometry(kButton, geometry), "query succeeds");
        checkNear(geometry.x, 42.0f, "leftMargin becomes x");
        checkNear(geometry.y, 84.0f, "topMargin becomes y");
        checkNear(geometry.width, 120.0f, "width");
    }

    std::printf("hidden button\n");
    resetWorld();
    g_world.visibility = 8; // View.GONE
    {
        ExternalButtonGeometry geometry;
        queryExternalButtonGeometry(kButton, geometry);
        check(!geometry.visible, "hidden button is not painted on");
    }
    resetWorld();
    g_world.showing = false;
    {
        ExternalButtonGeometry geometry;
        queryExternalButtonGeometry(kButton, geometry);
        check(!geometry.visible, "not-shown overlay is not painted on");
    }

    std::printf("unknown button\n");
    resetWorld();
    {
        ExternalButtonGeometry geometry;
        check(!queryExternalButtonGeometry("bedrocktoolsplus.HotbarSlots.Button9", geometry),
              "button without an overlay fails");
    }

    std::printf("detached caller thread\n");
    resetWorld();
    g_envMode = EnvMode::Detached;
    {
        ExternalButtonGeometry geometry;
        check(queryExternalButtonGeometry(kButton, geometry), "query succeeds after attaching");
        check(g_attachCalls == 1, "attached exactly once");
        check(g_detachCalls == 1, "detached again");
    }

    std::printf("launcher classes missing\n");
    resetWorld();
    g_world.classesReachable = false;
    {
        ExternalButtonGeometry geometry;
        check(!queryExternalButtonGeometry(kButton, geometry), "fails cleanly");
        check(g_attachCalls == g_detachCalls, "no leaked attach");
    }

    g_envMode = EnvMode::Attached;

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("externalbuttongeometry_test: all checks passed\n");
        return 0;
    }
    std::printf("externalbuttongeometry_test: %d check(s) failed\n", g_failures);
    return 1;
}
