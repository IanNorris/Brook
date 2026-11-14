#pragma once
#include <stdint.h>

namespace brook {
// Runtime-selectable Broadwell render-clock request (BROOK_BDW_RPS_TRIAL).
//
// The render job still writes GEN6_RPNSWREQ (0xa008) only for the duration of
// the job and restores the saved request afterwards; this header only chooses
// WHICH ratio is requested. The ratio is clamped to the fused [RPn, RP0]
// range from the frequency-caps register (0x145998: RP0 bits 7:0, RP1 15:8,
// RPn 23:16, in 50 MHz units). Only the standard RPS software request is
// touched: no voltage, no overclocking or turbo-limit registers.
//
// Thermal guard: the worker samples the package digital thermal sensor (at
// most every 50 ms). At or above the limit (default 90 C) the request falls
// back to the 500 MHz default and stays there (latched) until the next
// `gpu clock` command; each trip is counted and logged once.
constexpr uint32_t BdwClockBinMhz=50;
constexpr uint32_t BdwClockDefaultMhz=500;
constexpr int32_t BdwClockDefaultLimitC=90;

struct BdwClockCaps { uint32_t rpn=0,rp1=0,rp0=0; bool valid=false; };
inline BdwClockCaps BdwClockDecodeCaps(uint32_t caps){
    BdwClockCaps c{};
    if(caps==0 || caps==0xffffffffu)return c;
    c.rp0=caps&0xff;c.rp1=(caps>>8)&0xff;c.rpn=(caps>>16)&0xff;
    c.valid=c.rpn && c.rp0 && c.rpn<=c.rp0 && c.rp1>=c.rpn && c.rp1<=c.rp0;
    return c;
}
// MHz to a request bin, rounded down to 50 MHz and clamped to [RPn, RP0].
// Without valid caps only the default bin is allowed.
inline uint32_t BdwClockClampBin(uint32_t mhz,const BdwClockCaps& caps){
    uint32_t bin=mhz/BdwClockBinMhz;
    if(!caps.valid)return BdwClockDefaultMhz/BdwClockBinMhz;
    if(bin<caps.rpn)bin=caps.rpn;
    if(bin>caps.rp0)bin=caps.rp0;
    return bin;
}
// IA32_PACKAGE_THERM_STATUS (0x1b1) bits 22:16 = degrees below TjMax;
// MSR_TEMPERATURE_TARGET (0x1a2) bits 23:16 = TjMax. Returns -1 when unusable.
inline int32_t BdwClockDecodeTempC(uint64_t thermStatus,uint64_t tempTarget){
    const int32_t tjmax=int32_t((tempTarget>>16)&0xff);
    const int32_t below=int32_t((thermStatus>>16)&0x7f);
    if(tjmax<60 || tjmax>125)return -1;
    return tjmax-below;
}
// MSR_RAPL_POWER_UNIT (0x606) bits 12:8: energy unit = 1/2^ESU joules.
// Energy counters are 32-bit and wrap; returns microjoules for a delta.
inline uint64_t BdwClockEnergyDeltaUj(uint32_t before,uint32_t after,uint64_t powerUnit){
    const uint32_t esu=uint32_t((powerUnit>>8)&0x1f);
    const uint64_t ticks=uint32_t(after-before);
    return (ticks*1000000ull)>>esu;
}

struct BdwClockState {
    uint32_t requestedMhz=BdwClockDefaultMhz;
    int32_t limitC=BdwClockDefaultLimitC;
    uint32_t tripped=0;          // latched thermal fallback
    uint64_t trips=0,samples=0;
    int32_t lastTempC=-1,maxTempC=-1;
    uint32_t lastCaps=0;         // last caps register read inside a render job
    uint32_t lastRequestedBin=0; // bin the worker asked for on its last job
    uint64_t lastSampleTsc=0;
};
inline BdwClockState& BdwClock(){static BdwClockState s{};return s;}

// Pure decision used by the worker: which bin to request for this job.
inline uint32_t BdwClockEffectiveBin(const BdwClockState& s){
    const auto caps=BdwClockDecodeCaps(s.lastCaps);
    const uint32_t mhz=s.tripped?BdwClockDefaultMhz:s.requestedMhz;
    if(!caps.valid)return BdwClockDefaultMhz/BdwClockBinMhz;
    return BdwClockClampBin(mhz,caps);
}
// Apply a thermal sample; returns true if this sample newly tripped the guard.
inline bool BdwClockNoteTemp(BdwClockState& s,int32_t tempC){
    s.samples++;s.lastTempC=tempC;
    if(tempC>s.maxTempC)s.maxTempC=tempC;
    if(tempC>=0 && tempC>=s.limitC && !s.tripped){s.tripped=1;s.trips++;return true;}
    return false;
}
// `gpu clock <MHz>`: set the request (clears a latched thermal trip).
inline void BdwClockSetMhz(BdwClockState& s,uint32_t mhz){
    s.requestedMhz=mhz<BdwClockBinMhz?BdwClockDefaultMhz:mhz;s.tripped=0;
}
}
