#pragma once
#include <stdint.h>

namespace brook {
// Runtime-selectable Broadwell render-clock request (BROOK_BDW_RPS_TRIAL).
// Auto mode uses recent render-worker occupancy and stays off by default.
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
// back to the 500 MHz default until the cooldown completes. A manual
// frequency command can also clear a trip.
constexpr uint32_t BdwClockBinMhz=50;
constexpr uint32_t BdwClockDefaultMhz=500;
// Boot request on AC (Ian, 18 Nov): 600 MHz held 69-75 C at ~10 W in Stardew
// fullscreen with no trips, 27-29 -> 39 displayed fps; 700 MHz reached 98 C.
// The trip and no-caps fallbacks stay at BdwClockDefaultMhz.
constexpr uint32_t BdwClockBootMhz=600;
constexpr int32_t BdwClockDefaultLimitC=90;
// Hysteresis: a trip falls back to BdwClockDefaultMhz and recovers once every
// sample has been at or below limit-BdwClockRecoverMarginC for
// BdwClockRecoverMs (gen 439: a brief 96 C spike latched Stardew at 500 MHz
// for the rest of the boot, 39 -> 29 fps). PROCHOT (TjMax 105) still backstops.
constexpr int32_t BdwClockRecoverMarginC=15;
constexpr uint64_t BdwClockRecoverMs=5000;

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

struct BdwClockLoadSample {
    uint64_t elapsedTicks=0;
    uint64_t renderTicks=0;
    uint64_t resets=0;
};

struct BdwClockAutoPolicy {
    BdwClockLoadSample baseline{};
    uint32_t requestedMhz=BdwClockBootMhz;
    uint32_t loadPermille=0;
    uint32_t busyWindows=0;

    void Sample(const BdwClockLoadSample& next,uint64_t tscKhz,uint32_t ceilingMhz){
        constexpr uint64_t WindowMs=200,StaleAfterMs=1000;
        constexpr uint32_t BusyPermille=800,IdlePermille=500,StepMhz=100;
        if(!next.elapsedTicks || next.renderTicks>next.elapsedTicks || !tscKhz){
            *this={};
            return;
        }

        if(!baseline.elapsedTicks || next.resets!=baseline.resets ||
           next.elapsedTicks<baseline.elapsedTicks || next.renderTicks<baseline.renderTicks ||
           next.elapsedTicks-baseline.elapsedTicks>tscKhz*StaleAfterMs){
            *this={};
            baseline=next;
            return;
        }

        const uint64_t elapsed=next.elapsedTicks-baseline.elapsedTicks;
        if(elapsed<tscKhz*WindowMs)return;
        const uint64_t render=next.renderTicks-baseline.renderTicks;
        baseline=next;
        if(render>elapsed){
            *this={};
            return;
        }

        loadPermille=uint32_t(render*1000/elapsed);
        if(loadPermille>=BusyPermille){
            if(busyWindows<2)++busyWindows;
            if(busyWindows==2)requestedMhz+=StepMhz;
        }else{
            busyWindows=0;
            if(loadPermille<=IdlePermille)requestedMhz=BdwClockBootMhz;
        }
        if(requestedMhz>ceilingMhz)requestedMhz=ceilingMhz;
    }
};

struct BdwClockState {
    uint32_t requestedMhz=BdwClockBootMhz;
    int32_t limitC=BdwClockDefaultLimitC;
    uint32_t tripped=0;          // thermal fallback active
    uint64_t trips=0,recoveries=0,samples=0;
    uint64_t coolSinceMs=0;      // first sample of the current cool run while tripped (0 = none)
    int32_t lastTempC=-1,maxTempC=-1;
    uint32_t lastCaps=0;         // last caps register read inside a render job
    uint32_t lastRequestedBin=0; // bin the worker asked for on its last job
    uint64_t lastSampleTsc=0;
    bool automatic=false;        // changed only by the runtime clock command
    bool autoAdmitted=false;     // last job had usable power, caps and timing
    BdwClockAutoPolicy policy{};
};

// Pure decision used by the worker: which bin to request for this job.
inline uint32_t BdwClockEffectiveBin(const BdwClockState& s){
    const auto caps=BdwClockDecodeCaps(s.lastCaps);
    const uint32_t target=s.automatic?s.policy.requestedMhz:s.requestedMhz;
    const uint32_t mhz=s.tripped?BdwClockDefaultMhz:target;
    if(!caps.valid)return BdwClockDefaultMhz/BdwClockBinMhz;
    return BdwClockClampBin(mhz,caps);
}
// A zero request lets the admitted register transaction discover the fused
// caps without writing a new ratio.
inline uint32_t BdwClockSelectRequest(BdwClockState& s,bool deviceAdmitted,
                                      const BdwClockLoadSample& load,uint64_t tscKhz){
    if(!s.automatic)return BdwClockEffectiveBin(s);

    const auto caps=BdwClockDecodeCaps(s.lastCaps);
    s.autoAdmitted=deviceAdmitted && caps.valid && tscKhz &&
        s.lastTempC>=0 && load.elapsedTicks && load.renderTicks<=load.elapsedTicks;
    constexpr int32_t BoostThermalHeadroomC=10;
    if(!s.autoAdmitted || s.tripped || s.lastTempC>=s.limitC-BoostThermalHeadroomC)
        s.policy={};
    else
        s.policy.Sample(load,tscKhz,caps.rp0*BdwClockBinMhz);
    return s.autoAdmitted?BdwClockEffectiveBin(s):0;
}
// Apply a thermal sample taken at nowMs. Returns +1 if it newly tripped the
// guard, -1 if it ended a trip (cool for BdwClockRecoverMs), 0 otherwise.
// Invalid samples (tempC<0) change nothing.
inline int BdwClockNoteTemp(BdwClockState& s,int32_t tempC,uint64_t nowMs){
    s.samples++;s.lastTempC=tempC;
    if(tempC>s.maxTempC)s.maxTempC=tempC;
    if(tempC<0)return 0;
    if(tempC>=s.limitC){s.coolSinceMs=0;if(!s.tripped){s.tripped=1;s.trips++;return 1;}return 0;}
    if(!s.tripped)return 0;
    if(tempC>s.limitC-BdwClockRecoverMarginC){s.coolSinceMs=0;return 0;}
    if(!s.coolSinceMs){s.coolSinceMs=nowMs?nowMs:1;return 0;}
    if(nowMs-s.coolSinceMs<BdwClockRecoverMs)return 0;
    s.tripped=0;s.coolSinceMs=0;s.recoveries++;return -1;
}
// `gpu clock <MHz>`: set the request (clears a latched thermal trip).
inline void BdwClockSetAuto(BdwClockState& s){
    if(s.automatic)return;
    s.automatic=true;
    s.autoAdmitted=false;
    s.policy={};
}
inline void BdwClockSetMhz(BdwClockState& s,uint32_t mhz){
    s.automatic=false;
    s.autoAdmitted=false;
    s.requestedMhz=mhz<BdwClockBinMhz?BdwClockDefaultMhz:mhz;s.tripped=0;s.coolSinceMs=0;
}
}
