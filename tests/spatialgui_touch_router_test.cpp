#include "modules/spatialgui/touch_router.hpp"
#include <cmath>
#include <cstdio>
using namespace bedrocktools::spatialgui;
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* msg){std::printf("%s %s\n",ok?"ok":"FAIL",msg); failures+=!ok;};
    Camera camera{{1,0,0,0, 0,1,0,0, 0,0,0,-.45f, 0,0,-1,.55f}, 600,600};
    auto panel=makePanel({0,0,-3}, {0,0,0}, 2,2);
    if (!panel) return 1;
    TouchRouter router;
    auto send=[&](int action,int id,Vec2 p){return router.route(action,id,p,camera,*panel,{200,100});};
    auto out=send(0,1,{450,300});
    check(!out.consume && !out.event, "outside press untouched");
    out=send(0,1,{300,300});
    check(out.consume && out.event && out.event->phase==TouchPhase::Down && std::fabs(out.event->ui.x-100)<.001f, "inside press captures mapped coordinates");
    out=send(5,2,{300,300});
    check(!out.consume && !out.event, "other pointer untouched");
    out=send(2,1,{350,300});
    check(out.consume && out.event && out.event->phase==TouchPhase::Move && out.event->ui.x>100, "captured move mapped");
    const float last=out.event ? out.event->ui.x : 0;
    out=send(2,1,{500,300});
    check(out.consume && out.event && out.event->phase==TouchPhase::Cancel && out.event->ui.x==last, "leaving panel cancels at last valid position");
    out=send(2,1,{300,300});
    check(out.consume && !out.event, "cancelled pointer remains consumed");
    out=send(1,1,{300,300});
    check(out.consume && !out.event, "release after cancellation is consumed without duplicate event");
    out=send(0,1,{300,300}); out=send(1,1,{300,300});
    check(out.consume && out.event && out.event->phase==TouchPhase::Up, "new complete gesture works");
    out=send(0,9,{300,300}); auto reset=router.reset();
    check(reset && reset->phase==TouchPhase::Cancel && !router.reset(), "closing produces one cancellation");
    out=send(0,1,{300,300}); out=send(3,1,{300,300});
    check(out.consume && out.event && out.event->phase==TouchPhase::Cancel, "Android cancel propagated");
    out=send(0,1,{300,300}); out=send(1,1,{500,300});
    check(out.consume && out.event && out.event->phase==TouchPhase::Cancel, "release outside panel cancels");
    return failures?1:0;
}
