// Host test: render-clock caps decode, clamp, thermal decode/latch, RAPL units.
#include <cassert>
#include <cstdio>
#include "intel_bdw_render_clock.h"
using namespace brook;
int main(){
    // Dell XPS 13 9343 HD 5500 caps observed on gen 353: 0x060613 = RPn 300, RP1 300, RP0 950.
    auto c=BdwClockDecodeCaps(0x060613);
    assert(c.valid && c.rpn==6 && c.rp1==6 && c.rp0==19);
    assert(!BdwClockDecodeCaps(0).valid && !BdwClockDecodeCaps(0xffffffffu).valid);
    assert(!BdwClockDecodeCaps(0x130606).valid); // RPn above RP0
    // Clamp and 50 MHz rounding.
    assert(BdwClockClampBin(500,c)==10);
    assert(BdwClockClampBin(700,c)==14);
    assert(BdwClockClampBin(749,c)==14);
    assert(BdwClockClampBin(950,c)==19);
    assert(BdwClockClampBin(1500,c)==19);   // never above RP0
    assert(BdwClockClampBin(100,c)==6);     // never below RPn
    assert(BdwClockClampBin(950,BdwClockCaps{})==10); // unknown caps: default only
    // Thermal decode: TjMax 105 (0x690000), readout 35 below -> 70 C.
    assert(BdwClockDecodeTempC(uint64_t(35)<<16,uint64_t(105)<<16)==70);
    assert(BdwClockDecodeTempC(0,0)==-1);
    // State machine.
    BdwClockState s{};
    assert(BdwClockEffectiveBin(s)==10);            // no caps yet
    s.lastCaps=0x060613;
    assert(BdwClockEffectiveBin(s)==12);            // boot request 600 (fallbacks stay 500)
    BdwClockSetMhz(s,950);assert(BdwClockEffectiveBin(s)==19);
    assert(BdwClockNoteTemp(s,85,1000)==0);assert(BdwClockEffectiveBin(s)==19);
    assert(BdwClockNoteTemp(s,90,1050)==1);assert(s.tripped && s.trips==1);
    assert(BdwClockEffectiveBin(s)==10);            // fallback
    assert(BdwClockNoteTemp(s,95,1100)==0);assert(s.trips==1); // counted once
    assert(BdwClockNoteTemp(s,60,2000)==0 && BdwClockEffectiveBin(s)==10); // cool run starts
    assert(BdwClockNoteTemp(s,70,6999)==0 && s.tripped);  // 4.999 s: not yet
    assert(BdwClockNoteTemp(s,76,7000)==0 && s.tripped && !s.coolSinceMs); // above 75: run broken
    assert(BdwClockNoteTemp(s,75,8000)==0);assert(BdwClockNoteTemp(s,-1,12000)==0 && s.tripped); // invalid ignored
    assert(BdwClockNoteTemp(s,74,13000)==-1 && !s.tripped && s.recoveries==1 && BdwClockEffectiveBin(s)==19);
    assert(BdwClockNoteTemp(s,92,13050)==1 && s.trips==2);
    BdwClockSetMhz(s,700);assert(!s.tripped && !s.coolSinceMs && BdwClockEffectiveBin(s)==14);
    assert(s.maxTempC==95);
    assert(BdwClockNoteTemp(s,-1,14000)==0);       // invalid sample never trips
    // RAPL: ESU 14 -> 61.04 uJ per tick; wraparound handled.
    const uint64_t unit=uint64_t(14)<<8;
    assert(BdwClockEnergyDeltaUj(0,16384,unit)==1000000);
    assert(BdwClockEnergyDeltaUj(0xffffc000u,0,unit)==1000000);
    puts("test_intel_bdw_render_clock: ok");
    return 0;
}
