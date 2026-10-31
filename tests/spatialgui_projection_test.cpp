#include "modules/spatialgui/panel_projection.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
using namespace bedrocktools::spatialgui;
int main() {
    int errors=0; auto check=[&](bool b,const char* s){std::printf("%s %s\n",b?"ok":"FAIL",s); errors+=!b;};
    auto panel=makePanel({0,0,-3},{0,0,0},2,2); if(!panel) return 1;
    std::array<float,16> matrix{1,0,0,0, 0,1,0,0, 0,0,-11.f/9,-1, 0,0,-20.f/9,0};
    auto q=projectPanel(*panel,matrix);
    check(q && q->at(0).x==-1 && q->at(0).y==1 && q->at(0).w==3, "top-left retains homogeneous perspective coordinates");
    check(q && q->at(2).x==-1 && q->at(2).y==-1 && q->at(3).x==1, "strip order is TL TR BL BR");
    auto behind=makePanel({0,0,3},{0,0,0},2,2);
    check(behind && !projectPanel(*behind,matrix), "panel behind camera rejected");
    auto near=makePanel({0,0,-.5f},{0,0,0},2,2);
    check(near && !projectPanel(*near,matrix), "panel before near plane rejected");
    matrix[0]=std::numeric_limits<float>::quiet_NaN();
    check(!projectPanel(*panel,matrix), "invalid matrix rejected");
    return errors?1:0;
}
