#include <cassert>
#include <climits>
#include <cstdio>
#include <thread>
#include "../../kernel/src/mouse.cpp"
namespace brook { static uint32_t scale=100; uint32_t WmGetAppliedScale(){return scale;} }
int main() {
    using namespace brook;
    int32_t x,y;
    MouseSetBounds(3200,1800);MouseSetPosition(100,100);
    MouseMoveRelative(10,-5);MouseGetPosition(&x,&y);assert(x==110&&y==95);
    scale=200;MouseMoveRelative(10,-5);MouseGetPosition(&x,&y);assert(x==130&&y==85);
    scale=150;MouseSetPosition(100,100);
    MouseMoveRelative(1,-1);MouseMoveRelative(1,-1);MouseGetPosition(&x,&y);assert(x==103&&y==97);
    // Tiny movements have the same total displacement as a large packet.
    for(int i=0;i<20;i++)MouseMoveRelative(1,1);
    MouseGetPosition(&x,&y);assert(x==133&&y==127);
    MouseSetPosition(100,100);MouseMoveRelative(20,20);MouseGetPosition(&x,&y);assert(x==130&&y==130);
    // Fractional carry cannot leak across a warp or a DPI switch.
    MouseMoveRelative(1,1);MouseSetPosition(100,100);MouseMoveRelative(1,1);
    MouseGetPosition(&x,&y);assert(x==101&&y==101);
    scale=100;MouseMoveRelative(1,1);MouseGetPosition(&x,&y);assert(x==102&&y==102);
    // Wide signed input clamps without integer overflow or edge debt.
    scale=200;MouseMoveRelative(INT_MAX,INT_MIN);MouseGetPosition(&x,&y);assert(x==3199&&y==0);
    MouseMoveRelative(-1,1);MouseGetPosition(&x,&y);assert(x==3197&&y==2);
    MouseSetBounds(0,0);MouseGetPosition(&x,&y);assert(x==0&&y==0);
    MouseMoveRelative(1,-1);MouseGetPosition(&x,&y);assert(x==0&&y==0);
    scale=100;MouseSetBounds(100000,100000);MouseSetPosition(100,100);
    std::thread first([]{for(int i=0;i<1000;i++)MouseMoveRelative(1,1);});
    std::thread second([]{for(int i=0;i<1000;i++)MouseMoveRelative(1,1);});
    first.join();second.join();MouseGetPosition(&x,&y);assert(x==2100&&y==2100);
    puts("mouse motion: DPI gain fractional carry warps clamping overflow PASS");
}
