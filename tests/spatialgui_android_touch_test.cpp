// Fake JNI: tests the bridge protocol, not actual Android event delivery.
#define __ANDROID__ 1
#include "modules/spatialgui/android_touch.cpp"
#include <cstdio>
#include <limits>
static JNIEnv env;
static JavaVM vm{&env,true};
namespace bedrocktools::launcher { void* javaVm() { return &vm; } }
using bedrocktools::spatialgui::AndroidTouch;
int main() {
    int errors=0;
    auto check=[&](bool ok,const char* s){std::printf("%s %s\n",ok?"ok":"FAIL",s); errors+=!ok;};
    AndroidTouch touch;
    check(!touch.move(1,2) && !touch.up(1,2), "no movement or release without down");
    env.onMain=false;
    check(!touch.down(1,2) && env.calls==0 && env.frames==0, "off-main thread rejected");
    env.onMain=true; env.resumed=false;
    check(!touch.down(1,2) && env.calls==0, "paused activity rejected");
    env.resumed=true;
    check(!touch.down(std::numeric_limits<float>::infinity(),2), "invalid coordinates rejected");
    check(touch.down(5,6) && env.action==0 && env.x==5 && env.y==6 && env.downTime==env.eventTime, "down dispatched with correct coordinates and time");
    auto start=env.downTime;
    check(!touch.down(5,6), "duplicate down rejected");
    check(touch.move(7,8) && env.action==2 && env.downTime==start && env.eventTime>start, "move keeps original down time");
    check(touch.cancel() && env.action==3 && env.x==7 && env.y==8 && !touch.cancel(), "cancel uses last position and ends gesture");
    check(touch.down(1,2) && touch.up(3,4) && env.action==1 && !touch.up(3,4), "up ends gesture");
    check(touch.down(1,2), "gesture starts before lifecycle interruption");
    env.resumed=false;
    check(!touch.cancel() && !touch.move(1,2), "failed cancellation clears gesture on pause");
    check(env.frames==0, "all JNI local frames released");
    return errors?1:0;
}
