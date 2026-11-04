#include <cassert>
#include "intel_bdw_scanout.h"
using namespace brook;
int main(){
    constexpr uint64_t m=1024*1024,bar0=0xf6000000,bar2=0xe0000000;
    BdwGopPtePlan p{};
    assert(BdwPlanGopPtes(bar0,16*m,bar2,256*m,1,0x01c1,bar2+123,16,4,128,p));
    assert(p.firstIndex==0 && p.lastIndex==0 && p.firstAddress==bar0+8*m && p.lastAddress==p.firstAddress);
    assert(BdwPlanGopPtes(bar0,16*m,bar2,256*m,1,0x01c1,bar2+4090,16,4,128,p));
    assert(p.firstIndex==0 && p.lastIndex==1 && p.lastAddress==bar0+8*m+8);
    assert(BdwPlanGopPtes(bar0,16*m,bar2,256*m,1,0x01c1,bar2+256*m-4096,1024,1,4096,p));
    assert(p.lastIndex==65535 && p.lastAddress==bar0+8*m+65535*8);
    auto old=p;
    assert(!BdwPlanGopPtes(bar0,16*m,bar2,256*m,1,0x01c1,bar2+256*m-4095,1024,1,4096,p));
    assert(p.lastAddress==old.lastAddress);
    assert(!BdwPlanGopPtes(bar0,16*m,bar2,256*m,0xff,0x01c1,bar2,16,4,128,p));
    assert(!BdwPlanGopPtes(bar0,16*m,bar2,256*m,1,0x01c1,bar2-1,16,4,128,p));
    assert(!BdwPlanGopPtes(bar0,16*m,bar2,256*m,1,0x01c1,bar2,32,4,64,p));
    assert(!BdwPlanGopPtes(~uint64_t{0}-(16*m-1),16*m,bar2,256*m,1,0x01c1,bar2,16,4,128,p));
    assert(!BdwPlanGopPtes(bar2,16*m,bar2,256*m,1,0x01c1,bar2,16,4,128,p));
}
