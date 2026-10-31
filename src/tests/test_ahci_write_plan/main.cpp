#include "../../kernel/src/ahci_write_plan.h"
#include <cassert>
using namespace brook;
int main() {
    AhciReadPlan p{};
    assert(AhciPlanWrite(p,0x123456789abcull,128,0x123456790000ull,0x200000,true));
    assert(p.fis[2]==0x35 && p.fis[4]==0xbc && p.fis[5]==0x9a && p.fis[6]==0x78);
    assert(p.fis[7]==0x40 && p.fis[8]==0x56 && p.fis[9]==0x34 && p.fis[10]==0x12);
    assert(p.fis[12]==128 && p.fis[13]==0 && p.commandFlags==(5|(1u<<6)));
    assert(p.prdtEntries==1 && p.byteCountMinusOne==65535);
    assert(!AhciPlanWrite(p,0,0,100,0x1000,false));
    assert(!AhciPlanWrite(p,99,2,100,0x1000,false));
    assert(!AhciPlanWrite(p,(1ull<<48)-1,2,~uint64_t{0},0x1000,true));
    assert(!AhciPlanWrite(p,0,129,1000,0x1000,false));
    assert(!AhciPlanWrite(p,0,1,1000,0xfffffff0ull,false));
    assert(AhciPlanWrite(p,99,1,100,0xfffffe00ull,false));
    uint8_t identify[512]{};
    assert(!AtaSupportsFlushExt(nullptr,512) && !AtaSupportsFlushExt(identify,511));
    identify[167]=0x20;assert(!AtaSupportsFlushExt(identify,512)); // invalid word 83
    identify[167]=0x60;assert(AtaSupportsFlushExt(identify,512));
    identify[167]=0xe0;assert(!AtaSupportsFlushExt(identify,512));
    assert(!AhciPlanFlush(p,false) && p.fis[2]==0);
    assert(AhciPlanFlush(p,true) && p.fis[0]==0x27 && p.fis[1]==0x80 && p.fis[2]==0xea);
    assert(p.commandFlags==5 && p.prdtEntries==0 && p.dmaLow==0 && p.dmaHigh==0);
    for(unsigned i=3;i<sizeof(p.fis);++i)assert(p.fis[i]==0);
}
