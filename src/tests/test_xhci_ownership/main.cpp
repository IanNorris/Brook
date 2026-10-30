#include "../../drivers/xhci/xhci_ownership.h"
#include <cassert>
#include "../../drivers/xhci/xhci_msi.h"
#include <map>
struct Registers {
    std::map<unsigned,unsigned> values;unsigned byteWrites=0,fullWrites=0,polls=0,releaseAt=1;
    unsigned read(unsigned offset){return values[offset];}
    void writeByte(unsigned offset,uint8_t v){assert(offset==0x43);values[0x40]=(values[0x40]&0x00ffffffu)|(unsigned(v)<<24);++byteWrites;}
    void write(unsigned offset,unsigned value){assert(offset==0x44);values[offset]=value&~(7u<<29);++fullWrites;}
    bool wait(){if(++polls==releaseAt)values[0x40]&=~(1u<<16);return polls<8;}
};
int main(){
    unsigned legacy;
    brook::XhciMsiLayout layout;
    assert(brook::XhciMsiRegisters(0x80,0,layout) && layout.data==0x88 && !layout.upper);
    assert(brook::XhciMsiRegisters(0x80,0x1f0,layout) && layout.data==0x8c && layout.upper==0x88 && layout.mask==0x90 && !(layout.enabled&0x70));
    assert(!brook::XhciMsiRegisters(0xfc,0x180,layout));
    assert(brook::XhciScratchpadCount(3u<<27)==3);
    assert(brook::XhciScratchpadCount(2u<<21)==64);
    assert(brook::XhciScratchpadCount((31u<<21)|(31u<<27))==1023);
    Registers r;r.values[0x20]=2|(8<<8);r.values[0x40]=1|(1u<<16);r.values[0x44]=0xe011;
    assert(brook::XhciAcquireFirmwareOwnership(r,0x20,65536,legacy));assert(legacy==0x40 && r.byteWrites==1 && r.fullWrites==1);
    assert((r.values[0x40]&(1u<<24)) && !(r.values[0x40]&(1u<<16)));
    r={};r.values[0x40]=1|(1u<<16);r.releaseAt=100;
    assert(!brook::XhciAcquireFirmwareOwnership(r,0x40,65536,legacy));assert(!r.fullWrites);
    r={};r.values[0x40]=1|(1u<<16)|(255<<8);
    assert(!brook::XhciAcquireFirmwareOwnership(r,0x40,128,legacy));assert(!r.byteWrites);
    r={};assert(brook::XhciAcquireFirmwareOwnership(r,0,65536,legacy));assert(!legacy && !r.byteWrites);
    assert(!brook::XhciAcquireFirmwareOwnership(r,3,65536,legacy));
    r.values[0x40]=0xffffffff;assert(!brook::XhciAcquireFirmwareOwnership(r,0x40,65536,legacy));
}
