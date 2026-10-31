#include "../../kernel/src/ahci_read_plan.h"
#include <cassert>
using namespace brook;
int main() {
    AhciReadPlan p{};
    assert(AhciPlanIdentify(p,0x200000,false));
    assert(p.fis[0]==0x27 && p.fis[1]==0x80 && p.fis[2]==0xec);
    assert(p.commandFlags==5 && p.prdtEntries==1 && p.byteCountMinusOne==511);
    assert(AhciPlanRead(p,0x123456789abCull,128,0x123456790000ull,0x200000,true));
    assert(p.fis[2]==0x25 && p.fis[4]==0xbc && p.fis[5]==0x9a && p.fis[6]==0x78);
    assert(p.fis[7]==0x40 && p.fis[8]==0x56 && p.fis[9]==0x34 && p.fis[10]==0x12);
    assert(p.fis[12]==128 && p.fis[13]==0 && p.byteCountMinusOne==65535);
    assert(!(p.commandFlags & (1u<<6)));
    assert(!AhciPlanRead(p,0,0,1000,0x1000,false));
    assert(!AhciPlanRead(p,999,2,1000,0x1000,false));
    assert(!AhciPlanRead(p,(1ull<<48)-1,2,~uint64_t{0},0x1000,true));
    assert(!AhciPlanRead(p,0,129,1000,0x1000,false));
    assert(!AhciPlanRead(p,0,1,1000,0xfffffff0ull,false));
    assert(!AhciPlanRead(p,0,1,1000,~uint64_t{0}-1,true));
    assert(!AhciPlanIdentify(p,0x1001,true));
    assert(!AhciReadPlanDma(p,0x1000,511,true));
    assert(AhciPlanRead(p,0,1,1000,0xfffffe00ull,false));
    assert(!AhciPlanRead(p,0,1,1000,0xfffffffeull,false));
    assert(AhciPlanRead(p,0,1,1000,0x100000000ull,true) && p.dmaHigh==1);
    assert(AhciPlanRead(p,(1ull<<48)-1,1,1ull<<48,0x1000,true));
    assert(AhciPlanIdentify(p,0x1000,true) && p.fis[4]==0 && p.fis[7]==0 && p.fis[12]==0);
}
