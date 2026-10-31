#include "core/input/TouchDispatch.hpp"
#include <cstdio>
using namespace bedrocktools;
int main() {
    int failures=0, calls=0, raw=0;
    auto check=[&](bool ok,const char* label){std::printf("%s %s\n",ok?"ok":"FAIL",label); failures+=!ok;};
    events::EventBus bus;
    int lastAction=-1,lastId=-1; float lastX=0,lastY=0; bool lastDown=false;
    auto legacy=[&](float x,float y,bool down){++calls; lastX=x; lastY=y; lastDown=down; return true;};
    auto subscription=bus.subscribe<events::TouchInputEvent>([&](auto& e){++raw; lastAction=e.action; lastId=e.pointerId;});
    check(core::dispatchTouch(bus,0,3,10,20,legacy) && calls==1 && lastDown && lastX==10 && lastY==20 && lastId==3, "down reaches raw and legacy handlers");
    check(!core::dispatchTouch(bus,2,4,11,22,legacy) && calls==1 && raw==2 && lastAction==2 && lastId==4, "move reaches raw only");
    check(!core::dispatchTouch(bus,5,8,11,22,legacy) && calls==1 && lastAction==5 && lastId==8, "secondary down reaches raw only");
    check(core::dispatchTouch(bus,3,3,12,23,legacy) && calls==2 && !lastDown, "cancel retains legacy release behavior");
    auto blocker=bus.subscribe<events::TouchInputEvent>([](auto& e){if(e.action==1) e.cancel();});
    check(core::dispatchTouch(bus,1,3,12,23,legacy) && calls==2, "cancelled event is consumed before legacy handler");
    bus.unsubscribe(blocker); bus.unsubscribe(subscription);
    check(core::dispatchTouch(bus,1,3,12,23,legacy) && calls==3, "unsubscribed handlers restore original behavior");
    return failures?1:0;
}
