#include <cassert>
#include <cstdint>
#include "../../drivers/intel_hda/hda_dell_tone_codec.h"
using namespace brook;
struct Fake {
    uint8_t pin[2]{0,0},dac[2]{0x7f,0x7f},mixer[2]{0,0};uint8_t sel=0;
    unsigned operations=0,failAt=0,badReadAt=0;bool initialRestoreExpected=false;
    bool ok(){return ++operations!=failAt;}
    uint8_t* amp(uint8_t n,bool output){if(n==0x14 && output)return pin;if(n==3 && output)return dac;if(n==0x0d && !output)return mixer;assert(false);return nullptr;}
    bool getAmp(uint8_t n,bool output,bool left,uint8_t index,uint32_t& v){assert(index==0);v=amp(n,output)[left?0:1];bool result=ok();if(operations==badReadAt)v^=1;return result;}
    bool setAmp(uint8_t n,bool output,bool left,uint8_t index,uint8_t v){assert(index==0);
        if(n!=0x14)assert(pin[0]==0x80 && pin[1]==0x80);
        if(n==0x14 && v==0 && initialRestoreExpected)assert(sel==0 && dac[0]==0x7f && dac[1]==0x7f);
        amp(n,output)[left?0:1]=v;return ok();}
    bool set8(uint8_t n,uint16_t verb,uint8_t v){assert(n==0x14 && verb==0x701 && pin[0]==0x80 && pin[1]==0x80);sel=v;return ok();}
    bool get8(uint8_t n,uint16_t verb,uint32_t& v){assert(n==0x14 && verb==0xf01);v=sel;return ok();}
};
int main(){
    HdaDellTonePlan p{};p.pin=0x14;p.mixer=0x0d;p.dac=3;p.pinSelection=1;p.dacGain=31;
    HdaSessionReport old{};old.values[5]=0;old.ampValues[0]=old.ampValues[1]=0x7f;
    {Fake f;HdaDellToneCodec<Fake> c(f);assert(c.muteAndVerify(p));assert(c.configureMuted(p));
     assert(f.pin[0]==0x80 && f.pin[1]==0x80 && f.sel==1 && f.dac[0]==31 && f.dac[1]==31);
     assert(c.unmuteAndVerify(p));assert(f.pin[0]==0 && f.pin[1]==0);assert(c.muteAndVerify(p));
     f.initialRestoreExpected=true;assert(c.restoreAndVerify(p,old));assert(f.sel==0 && f.dac[0]==0x7f && f.pin[0]==0 && f.pin[1]==0);}
    // Every failure in the mutation/readback sequence leaves the pin muted
    // after the caller's mandated cleanup attempt, even if SET took effect.
    for(unsigned failure=1;failure<=18;++failure){Fake f;HdaDellToneCodec<Fake> c(f);assert(c.muteAndVerify(p));
        f.failAt=f.operations+failure;if(!c.configureMuted(p)){f.failAt=0;assert(c.muteAndVerify(p));assert(f.pin[0]==0x80 && f.pin[1]==0x80);}}
    {Fake baseline;HdaDellToneCodec<Fake> c(baseline);assert(c.muteAndVerify(p) && c.configureMuted(p));
     const auto before=baseline.operations;assert(c.unmuteAndVerify(p));const auto count=baseline.operations-before;
     for(unsigned failure=1;failure<=count;++failure){Fake f;HdaDellToneCodec<Fake> x(f);assert(x.muteAndVerify(p) && x.configureMuted(p));
      f.failAt=f.operations+failure;assert(!x.unmuteAndVerify(p));f.failAt=0;assert(x.muteAndVerify(p));assert(f.pin[0]==0x80 && f.pin[1]==0x80);}}
    {Fake baseline;HdaDellToneCodec<Fake> c(baseline);assert(c.muteAndVerify(p) && c.configureMuted(p));
     const auto before=baseline.operations;assert(c.restoreAndVerify(p,old));const auto count=baseline.operations-before;
     for(unsigned failure=1;failure<=count;++failure){Fake f;HdaDellToneCodec<Fake> x(f);assert(x.muteAndVerify(p) && x.configureMuted(p));
      f.failAt=f.operations+failure;assert(!x.restoreAndVerify(p,old));f.failAt=0;assert(x.muteAndVerify(p));assert(f.pin[0]==0x80 && f.pin[1]==0x80);}}
    {Fake f;HdaDellToneCodec<Fake> c(f);assert(c.muteAndVerify(p));f.badReadAt=f.operations+6;
     assert(!c.configureMuted(p));f.badReadAt=0;assert(c.muteAndVerify(p));}
}
