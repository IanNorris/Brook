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
    BdwClockState autoClock{};
    assert(!autoClock.automatic && BdwClockEffectiveBin(autoClock)==10);
    autoClock.lastCaps=0x060613;
    constexpr uint64_t TscKhz=1000;
    BdwClockLoadSample load{1'000'000,850'000,0};
    auto nextWindow=[&](uint64_t busyTicks){
        load.elapsedTicks+=200'000;
        load.renderTicks+=busyTicks;
        return BdwClockSelectRequest(autoClock,true,load,TscKhz);
    };
    assert(BdwClockSelectRequest(autoClock,true,load,TscKhz)==12);
    BdwClockSetAuto(autoClock);
    assert(BdwClockSelectRequest(autoClock,true,load,TscKhz)==0); // no thermal sample
    BdwClockNoteTemp(autoClock,70,1000);
    assert(BdwClockSelectRequest(autoClock,true,{},TscKhz)==0); // no worker timing
    autoClock.lastCaps=0;
    assert(BdwClockSelectRequest(autoClock,true,load,TscKhz)==0); // no caps
    autoClock.lastCaps=0x060613;
    assert(BdwClockSelectRequest(autoClock,true,load,TscKhz)==12);
    assert(nextWindow(180'000)==12 && autoClock.policy.loadPermille==900);
    assert(nextWindow(180'000)==14);  // sustained: 700
    assert(nextWindow(180'000)==16);  // 800
    assert(nextWindow(180'000)==18);  // 900
    assert(nextWindow(180'000)==19);  // fused 950 ceiling
    assert(nextWindow(190'000)==19);
    assert(nextWindow(140'000)==19);  // deadband holds the request
    assert(nextWindow(100'000)==12);  // idle restores the base
    assert(nextWindow(160'000)==12);
    assert(nextWindow(160'000)==14);
    autoClock.lastCaps=0x06060e;      // another SKU capped at 700
    assert(nextWindow(180'000)==14);
    autoClock.lastCaps=0x060613;
    assert(nextWindow(180'000)==16);

    assert(BdwClockNoteTemp(autoClock,80,3000)==0);
    assert(nextWindow(180'000)==12);  // thermal headroom, before a trip
    assert(BdwClockNoteTemp(autoClock,90,3100)==1);
    assert(nextWindow(180'000)==10);
    BdwClockSetAuto(autoClock);
    assert(autoClock.tripped);
    assert(BdwClockNoteTemp(autoClock,75,3200)==0);
    assert(BdwClockNoteTemp(autoClock,75,8199)==0 && nextWindow(180'000)==10);
    assert(BdwClockNoteTemp(autoClock,75,8200)==-1 && nextWindow(180'000)==12);
    assert(nextWindow(180'000)==12);
    assert(nextWindow(180'000)==14);

    assert(BdwClockSelectRequest(autoClock,false,load,TscKhz)==0); // AC or device admission lost
    assert(BdwClockSelectRequest(autoClock,true,load,TscKhz)==12);
    assert(nextWindow(180'000)==12);
    assert(nextWindow(180'000)==14);
    load.elapsedTicks+=1'000'001; // stale history after a stall
    assert(BdwClockSelectRequest(autoClock,true,load,TscKhz)==12);
    assert(nextWindow(180'000)==12);
    assert(nextWindow(180'000)==14);
    ++load.resets;
    assert(BdwClockSelectRequest(autoClock,true,load,TscKhz)==12);
    assert(BdwClockSelectRequest(autoClock,true,load,0)==0); // no reliable clock
    BdwClockSetMhz(autoClock,750);
    assert(!autoClock.automatic && BdwClockSelectRequest(autoClock,true,{},TscKhz)==15);

    // RAPL: ESU 14 -> 61.04 uJ per tick; wraparound handled.
    const uint64_t unit=uint64_t(14)<<8;
    assert(BdwClockEnergyDeltaUj(0,16384,unit)==1000000);
    assert(BdwClockEnergyDeltaUj(0xffffc000u,0,unit)==1000000);
    puts("test_intel_bdw_render_clock: ok");
    return 0;
}
