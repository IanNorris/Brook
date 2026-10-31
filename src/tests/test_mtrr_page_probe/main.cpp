#include "../../kernel/src/mtrr_page_probe.h"
#include <cassert>
using namespace brook;
static FramebufferMtrrs initial(){FramebufferMtrrs m{};m.capability=(1ull<<10)|2;m.count=2;m.physicalBits=36;m.defaults=(1ull<<11)|6;return m;}
static void range(FramebufferMtrrs& m,unsigned i,uint64_t base,uint64_t size,unsigned type){
    m.base[i]=base|type;m.mask[i]=((~(size-1))&((1ull<<m.physicalBits)-1)&~4095ull)|(1ull<<11);
}
int main(){
    constexpr uint64_t base=0x80000000;auto m=initial();MtrrPageObservation out{};
    assert(MtrrObservePage(m,base,out) && out.type==6 && out.usedDefault && !out.matchedTypes);
    range(m,0,base,1ull<<21,1);assert(MtrrObservePage(m,base,out) && out.type==1 && !out.usedDefault);
    assert(MtrrObservePage(m,base+(1ull<<21)-4096,out) && out.type==1);
    assert(MtrrObservePage(m,base+(1ull<<21),out) && out.usedDefault && out.type==6);
    range(m,0,base,1ull<<21,6);range(m,1,base,1ull<<21,4);assert(MtrrObservePage(m,base,out) && out.type==4);
    range(m,1,base,1ull<<21,1);assert(!MtrrObservePage(m,base,out));
    range(m,0,base,1ull<<21,0);assert(MtrrObservePage(m,base,out) && out.type==0);
    m=initial();range(m,0,base,1ull<<21,1);m.mask[0]&=~(1ull<<11);assert(MtrrObservePage(m,base,out) && out.usedDefault && out.type==6);
    m=initial();m.defaults&=~(1ull<<11);assert(!MtrrObservePage(m,base,out));
    m=initial();m.defaults=(1ull<<11)|2;assert(!MtrrObservePage(m,base,out));
    m=initial();m.defaults|=1ull<<12;assert(!MtrrObservePage(m,base,out));
    m=initial();m.capability&=~(1ull<<10);m.defaults=(1ull<<11)|1;assert(!MtrrObservePage(m,base,out));
    m=initial();m.capability&=~(1ull<<10);range(m,0,base,1ull<<21,1);assert(!MtrrObservePage(m,base,out));
    m=initial();range(m,0,base,1ull<<21,6);m.mask[0]^=1ull<<24;assert(!MtrrObservePage(m,base,out));
    m=initial();range(m,0,base+4096,1ull<<21,6);assert(!MtrrObservePage(m,base,out));
    m=initial();range(m,0,base,1ull<<21,6);m.base[0]|=1ull<<55;assert(!MtrrObservePage(m,base,out));
    m=initial();m.count=33;assert(!MtrrObservePage(m,base,out));
    m=initial();assert(!MtrrObservePage(m,0xff000, out) && !MtrrObservePage(m,base+1,out));
    assert(!MtrrObservePage(m,1ull<<36,out));
}
