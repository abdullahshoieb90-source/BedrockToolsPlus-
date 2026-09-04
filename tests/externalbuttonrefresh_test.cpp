// Unit tests for the external overlay button refresh (Android JNI path).
//
// Regression tests for the bug where editing a Command Hotkey command/label
// or a Comment Key text (or a button size/color) never showed up on the
// already visible overlay button until the module was removed and re-added:
//
//   * the ModMenu config callback can run on a native thread that is NOT
//     attached to the JVM. The refresh must AttachCurrentThread for the
//     duration of the update and DetachCurrentThread afterwards (leaving the
//     caller's thread state as it found it) instead of silently doing
//     nothing, which is what made the stale button survive every edit.
//   * the already-created Java overlay button is updated in place: the fresh
//     button definition is swapped onto the overlay, applyConfigurationChanges
//     re-applies size/colors, and configureOverlayView re-writes the label
//     text on the existing view.
//   * buttons owned by other modules are left untouched.
//   * a thread that is already attached is used as-is (no attach/detach pair).
//   * the attach is not leaked when the launcher classes cannot be found.
//
// The launcher classes are faked through tests/fakejni/jni.h hooks, so the
// production ExternalButtonRefresh.cpp translation unit is compiled and
// exercised directly on the host.
//
// A second section covers queryButtonGeometry (used by the Hotbar Slots
// "Draw Icons On Buttons" mode): the fake android.view.View reports a scripted
// screen position/size/visibility, and the tests assert that only visible,
// measured buttons owned by the module produce geometry entries.
//
// Build and run standalone:
//     g++ -std=c++20 -Wall -Wextra -I src -I tests/fakejni \
//         tests/externalbuttonrefresh_test.cpp -o /tmp/externalbuttonrefresh_test
//     /tmp/externalbuttonrefresh_test

#include "fakejni/jni.h"

// The implementation under test is compiled for the Android JNI path.
#define __ANDROID__ 1
#include "launcher/ExternalButtonRefresh.cpp"

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

// ---------------------------------------------------------------------------
// Fake launcher object model
// ---------------------------------------------------------------------------

struct FakeClass;

struct FakeObject : _jobject {
    std::string kind; // "manager" | "map" | "button" | "overlay" | "view" | "string"
    std::string text; // payload for strings / button ids

    // Per-overlay records (what the real ExternalButtonOverlay would render).
    jobject currentButton = nullptr; // value of its "button" field
    int applyChangesCalls = 0;       // applyConfigurationChanges() invocations
    int configureCalls = 0;          // configureOverlayView(view) invocations
    jobject configureViewArg = nullptr;

    // Per-view records (what the real android.view.View would report).
    jint viewFlags = 0;  // mViewFlags bitmask (0 == VISIBLE)
    jint viewWidth = 0;  // getWidth()
    jint viewHeight = 0; // getHeight()
    jint screenX = 0;    // getLocationOnScreen()[0]
    jint screenY = 0;    // getLocationOnScreen()[1]

    // Payload of fake jintArray objects.
    std::vector<jint> ints;

    FakeObject() = default;
    FakeObject(std::string k, std::string t = {}) : kind(std::move(k)), text(std::move(t)) {}
};

struct FakeClass : _jclass {
    std::string name;
    FakeClass* super = nullptr;
    std::vector<std::string> fields; // fields declared on this exact class

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

// Launcher classes. Note that "overlayView" only exists on the *base* class so
// the tests exercise the superclass walk in the field lookup.
FakeClass gManagerClass{"InbuiltOverlayManager", nullptr, {"externalButtonOverlayMap"}};
FakeClass gBridgeClass{"ExternalModBridge", nullptr, {}};
FakeClass gButtonClass{"ExternalButton", nullptr, {"buttonId", "moduleId"}};
FakeClass gOverlayBase{"ExternalOverlayBase", nullptr, {"overlayView"}};
FakeClass gOverlayClass{"ExternalButtonOverlay", &gOverlayBase, {"button"}};
FakeClass gMapClass{"java/util/Map", nullptr, {}};
FakeClass gViewClass{"android/view/View", nullptr, {"mViewFlags"}};

FakeMethod mGetInstance{"getInstance"};
FakeMethod mGetCount{"getExternalButtonCount"};
FakeMethod mGetButton{"getExternalButton"};
FakeMethod mMapGet{"get"};
FakeMethod mApplyChanges{"applyConfigurationChanges"};
FakeMethod mConfigure{"configureOverlayView"};
FakeMethod mGetLocation{"getLocationOnScreen"};
FakeMethod mGetWidth{"getWidth"};
FakeMethod mGetHeight{"getHeight"};

FakeField fOverlays{"externalButtonOverlayMap"};
FakeField fButtonId{"buttonId"};
FakeField fModuleId{"moduleId"};
FakeField fOverlayButton{"button"};
FakeField fOverlayView{"overlayView"};
FakeField fViewFlags{"mViewFlags"};

// Instances: two buttons registered in the bridge, one for Command Hotkey and
// one owned by an unrelated module (Zoom), each with its own overlay.
FakeObject gManager{"manager"};
FakeObject gMap{"map"};
FakeObject gButtonStale{"button"};   // old definition still held by the overlay
FakeObject gButtonFresh{"button"};   // freshly registered Command Hotkey button
FakeObject gButtonZoom{"button"};    // belongs to another module
FakeObject gOverlay{"overlay"};
FakeObject gOverlayZoom{"overlay"};
FakeObject gView{"view"};
FakeObject gViewZoom{"view"};
FakeObject gModuleStr{"string", "bedrocktoolsplus.Command Hotkey"};
FakeObject gZoomModuleStr{"string", "bedrocktoolsplus.Zoom"};
FakeObject gButtonIdStr{"string", "bedrocktoolsplus.CommandHotkey.Button1"};
FakeObject gZoomButtonIdStr{"string", "bedrocktoolsplus.Zoom.Button1"};

// VM / call bookkeeping.
int g_attachCalls = 0;
int g_detachCalls = 0;
int g_attachResult = JNI_OK;
int g_getSuperclassCalls = 0;
bool g_classesReachable = true;
std::vector<std::string> g_log; // ordered events: "attach", "detach", "findclass:..."

FakeObject* asFake(jobject o) { return static_cast<FakeObject*>(o); }

// ---------------------------------------------------------------------------
// JNIEnv hooks: a miniature scripted InbuiltOverlayManager / ExternalModBridge
// ---------------------------------------------------------------------------

jclass hk_FindClass(JNIEnv*, const char* name) {
    g_log.push_back(std::string("findclass:") + name);
    if (!g_classesReachable) return nullptr;
    if (std::strstr(name, "InbuiltOverlayManager")) return &gManagerClass;
    if (std::strstr(name, "ExternalModBridge$ExternalButton")) return &gButtonClass;
    if (std::strstr(name, "ExternalButtonOverlay")) return &gOverlayClass;
    if (std::strstr(name, "ExternalModBridge")) return &gBridgeClass;
    if (std::strcmp(name, "java/util/Map") == 0) return &gMapClass;
    if (std::strcmp(name, "android/view/View") == 0) return &gViewClass;
    return nullptr;
}

jmethodID hk_GetStaticMethodID(JNIEnv*, jclass c, const char* name, const char*) {
    if (c == &gManagerClass && std::strcmp(name, "getInstance") == 0) return &mGetInstance;
    if (c == &gBridgeClass && std::strcmp(name, "getExternalButtonCount") == 0) return &mGetCount;
    if (c == &gBridgeClass && std::strcmp(name, "getExternalButton") == 0) return &mGetButton;
    return nullptr;
}

jmethodID hk_GetMethodID(JNIEnv*, jclass c, const char* name, const char*) {
    if (c == &gMapClass && std::strcmp(name, "get") == 0) return &mMapGet;
    if (c == &gOverlayClass && std::strcmp(name, "applyConfigurationChanges") == 0) return &mApplyChanges;
    if (c == &gOverlayClass && std::strcmp(name, "configureOverlayView") == 0) return &mConfigure;
    if (c == &gViewClass && std::strcmp(name, "getLocationOnScreen") == 0) return &mGetLocation;
    if (c == &gViewClass && std::strcmp(name, "getWidth") == 0) return &mGetWidth;
    if (c == &gViewClass && std::strcmp(name, "getHeight") == 0) return &mGetHeight;
    return nullptr;
}

jfieldID hk_GetFieldID(JNIEnv*, jclass c, const char* name, const char*) {
    const FakeClass* fc = static_cast<FakeClass*>(c);
    for (const auto& f : fc->fields)
        if (f == name) {
            if (fc == &gManagerClass) return &fOverlays;
            if (fc == &gButtonClass)
                return std::strcmp(name, "buttonId") == 0 ? &fButtonId : &fModuleId;
            if (fc == &gOverlayClass) return &fOverlayButton;
            if (fc == &gOverlayBase) return &fOverlayView;
            if (fc == &gViewClass) return &fViewFlags;
        }
    return nullptr; // field not declared on this class -> walk continues
}

jclass hk_GetSuperclass(JNIEnv*, jclass c) {
    ++g_getSuperclassCalls;
    return static_cast<FakeClass*>(c)->super;
}

jobject hk_NewLocalRef(JNIEnv*, jobject o) { return o; }

jobject hk_CallStaticObjectMethod(JNIEnv*, jclass c, jmethodID m, va_list ap) {
    if (m == &mGetInstance) return &gManager;
    if (m == &mGetButton) {
        const jint index = va_arg(ap, jint);
        return index == 0 ? &gButtonFresh : &gButtonZoom;
    }
    (void)c;
    return nullptr;
}

jint hk_CallStaticIntMethod(JNIEnv*, jclass, jmethodID m, va_list) {
    return m == &mGetCount ? 2 : 0;
}

jobject hk_CallObjectMethod(JNIEnv*, jobject o, jmethodID m, va_list ap) {
    if (m == &mMapGet && o == &gMap) {
        const jobject key = va_arg(ap, jobject);
        if (key == &gButtonIdStr) return &gOverlay;
        if (key == &gZoomButtonIdStr) return &gOverlayZoom;
    }
    return nullptr;
}

jint hk_CallIntMethod(JNIEnv*, jobject o, jmethodID m, va_list) {
    const FakeObject* view = asFake(o);
    if (m == &mGetWidth) return view->viewWidth;
    if (m == &mGetHeight) return view->viewHeight;
    return 0;
}

jint hk_GetIntField(JNIEnv*, jobject o, jfieldID f) {
    if (f == &fViewFlags) return asFake(o)->viewFlags;
    return 0;
}

jintArray hk_NewIntArray(JNIEnv*, jsize len) {
    static std::vector<FakeObject*> pool;
    pool.push_back(new FakeObject{});
    pool.back()->kind = "intarray";
    pool.back()->ints.assign(static_cast<std::size_t>(len), 0);
    return pool.back();
}

void hk_GetIntArrayRegion(JNIEnv*, jintArray a, jsize start, jsize len, jint* buf) {
    const auto& ints = asFake(a)->ints;
    for (jsize i = 0; i < len; ++i) buf[i] = ints[static_cast<std::size_t>(start + i)];
}

void hk_CallVoidMethod(JNIEnv*, jobject o, jmethodID m, va_list ap) {
    FakeObject* target = asFake(o);
    if (m == &mApplyChanges) {
        ++target->applyChangesCalls;
    } else if (m == &mConfigure) {
        ++target->configureCalls;
        target->configureViewArg = va_arg(ap, jobject);
    } else if (m == &mGetLocation) {
        FakeObject* array = asFake(va_arg(ap, jobject));
        if (array->ints.size() >= 2) {
            array->ints[0] = target->screenX;
            array->ints[1] = target->screenY;
        }
    }
}

jobject hk_GetObjectField(JNIEnv*, jobject o, jfieldID f) {
    if (f == &fOverlays) return &gMap;
    if (f == &fModuleId)
        return o == &gButtonZoom ? &gZoomModuleStr : &gModuleStr;
    if (f == &fButtonId)
        return o == &gButtonZoom ? &gZoomButtonIdStr : &gButtonIdStr;
    if (f == &fOverlayButton) return asFake(o)->currentButton;
    if (f == &fOverlayView) return o == &gOverlayZoom ? &gViewZoom : &gView;
    return nullptr;
}

void hk_SetObjectField(JNIEnv*, jobject o, jfieldID f, jobject v) {
    if (f == &fOverlayButton) asFake(o)->currentButton = v;
}

jstring hk_NewStringUTF(JNIEnv*, const char* s) {
    static std::vector<FakeObject*> pool;
    pool.push_back(new FakeObject{"string", s});
    return pool.back();
}

jsize hk_GetStringUTFLength(JNIEnv*, jstring s) {
    return static_cast<jsize>(asFake(s)->text.size());
}

const char* hk_GetStringUTFChars(JNIEnv*, jstring s, jboolean* isCopy) {
    if (isCopy) *isCopy = JNI_FALSE;
    return asFake(s)->text.c_str();
}

void hk_ReleaseStringUTFChars(JNIEnv*, jstring, const char*) {}
jboolean hk_ExceptionCheck(JNIEnv*) { return JNI_FALSE; }
void hk_ExceptionClear(JNIEnv*) {}
void hk_DeleteLocalRef(JNIEnv*, jobject) {}
jboolean hk_IsSameObject(JNIEnv*, jobject a, jobject b) { return a == b; }

// ---------------------------------------------------------------------------
// Fake JavaVM
// ---------------------------------------------------------------------------

enum class EnvMode { Detached, Attached, VersionError };

EnvMode g_envMode = EnvMode::Detached;
JNIEnv g_env;
JavaVM g_vm;

jint hk_GetEnv(JavaVM*, void** out, jint) {
    if (g_envMode == EnvMode::Detached) return JNI_EDETACHED;
    if (g_envMode == EnvMode::VersionError) return JNI_EVERSION;
    *reinterpret_cast<JNIEnv**>(out) = &g_env;
    return JNI_OK;
}

jint hk_AttachCurrentThread(JavaVM*, JNIEnv** out, void*) {
    ++g_attachCalls;
    g_log.push_back("attach");
    if (g_attachResult != JNI_OK) return g_attachResult;
    *out = &g_env;
    return JNI_OK;
}

jint hk_DetachCurrentThread(JavaVM*) {
    ++g_detachCalls;
    g_log.push_back("detach");
    return JNI_OK;
}

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
    e.CallIntMethodVFn = hk_CallIntMethod;
    e.CallVoidMethodVFn = hk_CallVoidMethod;
    e.GetObjectFieldFn = hk_GetObjectField;
    e.GetIntFieldFn = hk_GetIntField;
    e.SetObjectFieldFn = hk_SetObjectField;
    e.NewStringUTFFn = hk_NewStringUTF;
    e.GetStringUTFLengthFn = hk_GetStringUTFLength;
    e.GetStringUTFCharsFn = hk_GetStringUTFChars;
    e.ReleaseStringUTFCharsFn = hk_ReleaseStringUTFChars;
    e.ExceptionCheckFn = hk_ExceptionCheck;
    e.ExceptionClearFn = hk_ExceptionClear;
    e.DeleteLocalRefFn = hk_DeleteLocalRef;
    e.IsSameObjectFn = hk_IsSameObject;
    e.NewIntArrayFn = hk_NewIntArray;
    e.GetIntArrayRegionFn = hk_GetIntArrayRegion;

    g_vm.GetEnvFn = hk_GetEnv;
    g_vm.AttachCurrentThreadFn = hk_AttachCurrentThread;
    g_vm.DetachCurrentThreadFn = hk_DetachCurrentThread;
}

void resetWorld() {
    g_attachCalls = g_detachCalls = g_getSuperclassCalls = 0;
    g_attachResult = JNI_OK;
    g_classesReachable = true;
    g_log.clear();

    gOverlay.currentButton = &gButtonStale;
    gOverlay.applyChangesCalls = 0;
    gOverlay.configureCalls = 0;
    gOverlay.configureViewArg = nullptr;
    gOverlayZoom.currentButton = &gButtonZoom;
    gOverlayZoom.applyChangesCalls = 0;
    gOverlayZoom.configureCalls = 0;
    gOverlayZoom.configureViewArg = nullptr;

    gView.viewFlags = 0; // VISIBLE
    gView.viewWidth = 96;
    gView.viewHeight = 96;
    gView.screenX = 120;
    gView.screenY = 240;
    gViewZoom.viewFlags = 0;
    gViewZoom.viewWidth = 64;
    gViewZoom.viewHeight = 64;
    gViewZoom.screenX = 10;
    gViewZoom.screenY = 20;
}

bool attachHappenedBeforeFirstJniCall() {
    for (const auto& event : g_log) {
        if (event == "attach") return true;
        if (event.rfind("findclass:", 0) == 0) return false;
    }
    return false;
}

} // namespace

int main() {
    installHooks();
    bedrocktools::launcher::setJavaVm(&g_vm);

    std::printf("external button refresh (detached native thread - the reported bug)\n");
    resetWorld();
    g_envMode = EnvMode::Detached;
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktoolsplus.Command Hotkey");

    check(g_attachCalls == 1, "detached thread is attached to the JVM exactly once");
    check(g_detachCalls == 1, "thread is detached again after the refresh");
    check(attachHappenedBeforeFirstJniCall(), "no JNI call happens before AttachCurrentThread");
    check(gOverlay.currentButton == &gButtonFresh,
          "overlay receives the freshly registered button definition");
    check(gOverlay.applyChangesCalls >= 1, "applyConfigurationChanges re-applies size/colors in place");
    check(gOverlay.configureCalls >= 1, "configureOverlayView re-applies the label text in place");
    check(gOverlay.configureViewArg == &gView, "configureOverlayView runs on the overlay's own view");
    check(g_getSuperclassCalls >= 1, "overlayView field found through the superclass walk");
    check(gOverlayZoom.applyChangesCalls == 0 && gOverlayZoom.configureCalls == 0,
          "other modules' overlays are left untouched");
    check(gOverlayZoom.currentButton == &gButtonZoom, "other modules' buttons are not swapped");

    std::printf("external button refresh (already-attached thread)\n");
    resetWorld();
    g_envMode = EnvMode::Attached;
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktoolsplus.Command Hotkey");

    check(g_attachCalls == 0 && g_detachCalls == 0,
          "already-attached thread is reused without attach/detach");
    check(gOverlay.currentButton == &gButtonFresh, "button definition still swapped in");
    check(gOverlay.applyChangesCalls >= 1 && gOverlay.configureCalls >= 1,
          "size/colors and label text still re-applied");

    std::printf("external button refresh (failure modes)\n");
    resetWorld();
    g_envMode = EnvMode::VersionError;
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktoolsplus.Command Hotkey");
    check(g_attachCalls == 0 && gOverlay.currentButton == &gButtonStale,
          "GetEnv failure leaves everything untouched");

    resetWorld();
    g_envMode = EnvMode::Detached;
    g_attachResult = JNI_ERR;
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktoolsplus.Command Hotkey");
    check(g_attachCalls == 1 && g_detachCalls == 0,
          "failed AttachCurrentThread is not followed by DetachCurrentThread");
    check(g_log.empty() || g_log.back() != "detach", "no detach event after a failed attach");

    resetWorld();
    g_envMode = EnvMode::Detached;
    g_classesReachable = false; // launcher classes not found on this build
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktoolsplus.Command Hotkey");
    check(g_attachCalls == 1 && g_detachCalls == 1,
          "early return while attached still detaches (no leaked attachment)");

    resetWorld();
    bedrocktools::launcher::setJavaVm(nullptr);
    bedrocktools::launcher::refreshExternalButtonsForModule("bedrocktoolsplus.Command Hotkey");
    check(g_attachCalls == 0 && g_detachCalls == 0,
          "no VM registered -> no-op without crashing");
    bedrocktools::launcher::setJavaVm(&g_vm);

    std::printf("button geometry query\n");
    resetWorld();
    g_envMode = EnvMode::Attached;
    {
        const auto geometries =
            bedrocktools::launcher::queryButtonGeometry("bedrocktoolsplus.Command Hotkey");
        check(geometries.size() == 1, "one geometry entry for the module's visible button");
        if (!geometries.empty()) {
            check(geometries[0].buttonId == "bedrocktoolsplus.CommandHotkey.Button1",
                  "geometry carries the overlay button id");
            check(geometries[0].x == 120.0f && geometries[0].y == 240.0f,
                  "geometry position comes from getLocationOnScreen");
            check(geometries[0].width == 96.0f && geometries[0].height == 96.0f,
                  "geometry size comes from getWidth/getHeight");
        }
    }
    check(g_attachCalls == 0 && g_detachCalls == 0,
          "query on an attached thread needs no attach/detach");

    // A button that is not visible (or has no size yet) reports no entry, so
    // the caller falls back to its strip position.
    resetWorld();
    g_envMode = EnvMode::Attached;
    gView.viewFlags = 4; // android.view.View.INVISIBLE
    check(bedrocktools::launcher::queryButtonGeometry("bedrocktoolsplus.Command Hotkey").empty(),
          "invisible button view is skipped");

    resetWorld();
    g_envMode = EnvMode::Attached;
    gView.viewWidth = 0; // laid out but not measured yet
    check(bedrocktools::launcher::queryButtonGeometry("bedrocktoolsplus.Command Hotkey").empty(),
          "zero-size button view is skipped");

    std::printf("button geometry query (threading and failure modes)\n");
    resetWorld();
    g_envMode = EnvMode::Detached;
    {
        const auto geometries =
            bedrocktools::launcher::queryButtonGeometry("bedrocktoolsplus.Command Hotkey");
        check(geometries.size() == 1, "detached thread still resolves the geometry");
    }
    check(g_attachCalls == 1 && g_detachCalls == 1,
          "query attaches and detaches a detached thread exactly once");

    resetWorld();
    g_envMode = EnvMode::Detached;
    g_classesReachable = false; // launcher classes not found on this build
    check(bedrocktools::launcher::queryButtonGeometry("bedrocktoolsplus.Command Hotkey").empty(),
          "missing launcher classes yield no geometry");
    check(g_attachCalls == 1 && g_detachCalls == 1,
          "early query return still detaches (no leaked attachment)");

    resetWorld();
    bedrocktools::launcher::setJavaVm(nullptr);
    check(bedrocktools::launcher::queryButtonGeometry("bedrocktoolsplus.Command Hotkey").empty(),
          "no VM registered -> empty geometry without crashing");
    bedrocktools::launcher::setJavaVm(&g_vm);

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
