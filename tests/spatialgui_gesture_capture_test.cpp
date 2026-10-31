#include "modules/spatialgui/gesture_capture.hpp"
#include <cstdio>
using namespace bedrocktools::spatialgui;
int main() {
    int errors=0;
    auto check=[&](bool ok,const char* s){std::printf("%s %s\n",ok?"ok":"FAIL",s); errors+=!ok;};
    Camera camera{{1,0,0,0, 0,1,0,0, 0,0,0,-.45f, 0,0,-1,.55f},600,600};
    auto panel=makePanel({0,0,-3},{0,0,0},2,2);
    if (!panel) return 1;
    GestureCapture cap;
    auto send=[&](int action,int id,Vec2 p){return cap.route(action,id,p,camera,*panel,{200,100});};
    check(!send(0,1,{500,300}).consume && !send(5,2,{300,300}).consume && !cap.captured(), "cannot capture midway through vanilla gesture");
    auto event=send(0,1,{300,300});
    check(event.consume && event.event && event.event->phase==TouchPhase::Down, "initial down captures gesture");
    check(send(5,2,{500,300}).consume && send(2,2,{500,300}).consume, "second finger is swallowed");
    check(send(2,1,{320,300}).event->phase==TouchPhase::Move, "primary movement maps to UI");
    event=send(6,1,{320,300});
    check(event.consume && event.event && event.event->phase==TouchPhase::Up && cap.captured(), "primary release forwards up but gesture remains owned");
    check(send(2,2,{400,300}).consume && send(1,2,{400,300}).consume && !cap.captured(), "remaining finger never leaks");
    send(0,3,{300,300}); send(5,4,{300,300});
    event=send(3,3,{300,300});
    check(event.consume && event.event && event.event->phase==TouchPhase::Cancel && cap.captured(), "cancel emitted once and remains owned for other fingers");
    event=send(3,4,{300,300});
    check(event.consume && !event.event && !cap.captured(), "last cancel clears capture without duplicate");
    send(0,5,{300,300});
    check(cap.cancelUi()->phase==TouchPhase::Cancel && cap.captured(), "closing UI keeps ownership until Android gesture ends");
    event=send(1,5,{300,300});
    check(event.consume && !event.event && !cap.captured(), "release after UI close consumed");
    send(0,6,{300,300});
    check(cap.abandon()->phase==TouchPhase::Cancel && !cap.captured(), "lost stream can be explicitly abandoned");
    return errors?1:0;
}
