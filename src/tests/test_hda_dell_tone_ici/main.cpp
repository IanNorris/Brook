#include <cassert>
#include <cstdint>
#include "../../drivers/intel_hda/hda_dell_tone_ici.h"
using namespace brook;
struct Fake {
    uint32_t cmd=0;uint16_t status=0;uint64_t now=0;unsigned writes=0;bool timeout=false,stale=false;
    uint16_t read16(uint32_t o){assert(o==0x68);return status;}
    uint32_t read32(uint32_t o){assert(o==0x64);return 0x1234;}
    void write16(uint32_t o,uint16_t v){assert(o==0x68);++writes;if(v==2 && !stale)status&=~2;else if(v==1)status=timeout?1:2;}
    void write32(uint32_t o,uint32_t v){assert(o==0x60);++writes;cmd=v;}
    uint64_t ticks(){return now++;}void pause(){}
};
int main(){
    Fake f;HdaDellToneIci<Fake> c(f,0x4011);uint32_t r=0;
    assert(!c.set8(0x14,0x705,0) && !c.set8(3,0x706,0x20) && !f.writes);
    assert(c.set8(0x14,0x701,1) && f.cmd==0x01470101);
    assert(c.setAmp(0x14,true,true,0,0x80) && f.cmd==0x0143a080);
    assert(c.setAmp(0x14,true,false,0,0x80) && f.cmd==0x01439080);
    assert(c.setAmp(0x0d,false,false,0,0) && f.cmd==0x00d35000);
    assert(c.setAmp(3,true,true,0,31) && f.cmd==0x0033a01f);
    assert(c.getAmp(0x0d,false,false,0,r) && f.cmd==0x00db0000 && r==0x1234);
    assert(c.get8(0x14,0xf01,r) && f.cmd==0x014f0100);assert(c.get8(3,0xf06,r) && f.cmd==0x003f0600);
    const auto writes=f.writes;assert(!c.setAmp(3,true,true,0,0x80) && !c.setAmp(0x14,true,true,1,0) &&
        !c.setAmp(3,false,true,0,0) && !c.getAmp(0x0d,true,true,0,r) && f.writes==writes);
    assert(c.setFormat(0x11) && f.cmd==0x00320011);assert(c.setFormat(0x4011) && f.cmd==0x00324011);
    assert(!c.setFormat(0x22));assert(c.getFormat(r) && f.cmd==0x003a0000);
    {Fake bad;bad.timeout=true;HdaDellToneIci<Fake> x(bad,0);assert(!x.set8(0x14,0x701,1) && x.failed());
     const auto before=bad.writes;assert(!x.getAmp(0x14,true,true,0,r) && !x.setAmp(0x14,true,true,0,0x80) && bad.writes==before);}
    {Fake bad;bad.status=2;bad.stale=true;HdaDellToneIci<Fake> x(bad,0);assert(!x.getFormat(r) && x.failed() && bad.writes==1);}
    {Fake bad;bad.status=1;HdaDellToneIci<Fake> x(bad,0);assert(!x.getFormat(r) && x.failed() && !bad.writes);}
}
