#include "modules/spatialgui/container_tracker.hpp"
#include <cstdio>
using bedrocktools::spatialgui::ContainerTracker;
int main() {
    int failures=0; auto check=[&](bool ok,const char* s){std::printf("%s %s\n",ok?"ok":"FAIL",s); failures+=!ok;};
    int a=0,b=0,c=0; ContainerTracker t;
    t.opened(nullptr); check(!t.isTop(nullptr) && !t.isTop(&a), "empty and null are not active");
    t.opened(&a); t.opened(&b);
    check(t.isTop(&b) && !t.isTop(&a), "latest opened controller is top");
    t.closed(&a); t.closed(&c);
    check(t.isTop(&b), "closing non-top and unknown controller is safe");
    t.opened(&a); t.opened(&b); t.opened(&a);
    check(t.isTop(&a), "duplicate open promotes controller");
    t.closed(&a);
    check(t.isTop(&b) && !t.isTop(&a), "one close removes promoted controller");
    t.clear(); check(!t.isTop(&b), "world transition or disable clears identities");
    return failures ? 1 : 0;
}
