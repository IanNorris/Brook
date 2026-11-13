#pragma once
#include <stdint.h>

namespace brook {
// GEN8_MCR_SELECTOR and GEN8_FUSE2 use the Broadwell encoding. This checks
// only that the selected slice and subslice are not fused off; the caller
// must separately keep the relevant power/forcewake domain active, serialize
// selector changes and establish that the samples are current and stable.
// Gen8 MCR writes are multicast; this does not change the selector.
inline bool BdwMcrSelectionPresent(uint32_t selector,uint32_t fuse2) {
    if(selector==0xffffffffu || fuse2==0xffffffffu)return false;
    const uint32_t slice=(selector>>26)&3u,subslice=(selector>>24)&3u;
    if(slice>=3 || subslice>=3)return false;
    const uint32_t enabledSlices=(fuse2>>25)&7u,disabledSubslices=(fuse2>>21)&7u;
    return (enabledSlices&(1u<<slice)) && !(disabledSubslices&(1u<<subslice));
}
enum class BdwRenderGtInitStatus : uint8_t { NotStarted, Applied, Uncertain };
struct BdwRenderGtInitResult {
    BdwRenderGtInitStatus status=BdwRenderGtInitStatus::NotStarted;
    uint8_t stage=0;uint32_t writes=0;
    uint32_t before[6]{},l3Before=0,l3After=0;
    uint32_t selector=0,fuse2=0;bool topologySampled=false;
    bool dopRestoreAttempted=false,dopRestored=false;
};
// Broadwell GT-side portions of Linux i915's clock-gating initialization.
// These settings are global; the owner must serialize all GT users and resets,
// hold the appropriate forcewake, prove active display/firmware compatibility,
// establish valid MCR read steering and broadcast writes, and supply ordered
// MMIO and a delay of at least one microsecond. It must recover on Uncertain.
// This does not initialize display, context WAs or a render default context.
template<class Io> BdwRenderGtInitResult BdwInitializeRenderGt(Io& io) {
    BdwRenderGtInitResult result{};
    constexpr uint32_t regs[]={0x4090,0x20a0,0x2050,0x9430,0x9400,0x9424};
    constexpr uint32_t creditReg=0xb100,creditMask=(0x1fu<<19)|(0x1fu<<14);
    constexpr uint32_t creditValue=(15u<<19)|(1u<<14);
    if(!io.owned() || !io.allEnginesQuiescent() || !io.displayAndFirmwareSafe() || !io.mcrSteeringReady())return result;
    for(unsigned i=0;i<6;++i){uint32_t second=0;
        if(!io.readReg(regs[i],result.before[i]) || result.before[i]==0xffffffffu ||
           !io.readReg(regs[i],second) || second!=result.before[i])return result;
    }
    uint32_t second=0;
    if(!io.readMcr(creditReg,result.l3Before) || result.l3Before==0xffffffffu ||
       !io.readMcr(creditReg,second) || second!=result.l3Before ||
       !io.owned() || !io.allEnginesQuiescent() || !io.displayAndFirmwareSafe() || !io.mcrSteeringReady())return result;
    auto write=[&](uint32_t reg,uint32_t value){++result.writes;return io.owned() && io.writeReg(reg,value);};
    auto verify=[&](uint32_t reg,uint32_t mask,uint32_t value){uint32_t v=0;
        return io.owned() && io.readReg(reg,v) && v!=0xffffffffu && (v&mask)==(value&mask);};
    result.status=BdwRenderGtInitStatus::Uncertain;result.stage=1;
    // Preserve unrelated bits; the PSMI control uses masked-write semantics.
    if(!write(regs[0],result.before[0]|0x40u) || !verify(regs[0],0x40u,0x40u))return result;
    result.stage=2;
    if(!write(regs[1],result.before[1]&~0x88000u) || !verify(regs[1],0x88000u,0))return result;
    result.stage=3;
    if(!write(regs[2],0x10001000u) || !verify(regs[2],0x1000u,0x1000u))return result;
    result.stage=4;
    if(!write(regs[3],result.before[3]|0x4000u) || !verify(regs[3],0x4000u,0x4000u))return result;
    result.stage=5;
    if(!write(regs[4],result.before[4]|0x10000u) || !verify(regs[4],0x10000u,0x10000u))return result;
    result.stage=6;
    // Once the temporary clock-gating write is attempted, always try to
    // restore it after the credit write/delay, including on failure.
    const bool disabled=write(regs[5],result.before[5]&~1u) && verify(regs[5],1,0);
    bool credits=false,waited=false,creditAttempted=false;
    if(disabled && io.owned() && io.mcrSteeringReady()) {
        ++result.writes;creditAttempted=true;
        credits=io.writeMcrBroadcast(creditReg,(result.l3Before&~creditMask)|creditValue) &&
            io.readMcr(creditReg,result.l3After) && result.l3After!=0xffffffffu &&
            (result.l3After&creditMask)==creditValue;
        // Even a failed posted write can have reached hardware. Delay before
        // re-enabling DOP gating regardless of the apparent write result.
        waited=io.delayAtLeastOneMicrosecond();
    }
    // If timing could not be established after a possibly delivered L3
    // write, leave DOP gating disabled and let the owner recover. Re-enabling
    // it without the required delay would compound the uncertain state.
    if(creditAttempted && !waited)return result;
    result.dopRestoreAttempted=true;
    result.dopRestored=write(regs[5],result.before[5]) && verify(regs[5],1,result.before[5]);
    if(!disabled || !credits || !waited || !result.dopRestored || !io.owned())return result;
    result.stage=7;result.status=BdwRenderGtInitStatus::Applied;return result;
}
}
