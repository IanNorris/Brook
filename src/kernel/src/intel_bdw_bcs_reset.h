#pragma once
#include <stdint.h>

namespace brook {
// A domain acknowledgement does not establish an initialized engine, a
// synchronized CSB, or safe DMA reclamation. The owner must retain all
// referenced resources until it independently proves those properties.
enum class BdwBcsResetResult { NotStarted, DomainAcknowledged, Uncertain };
enum class BdwBcsResetIntent { IdleTakeover, StopOwnedWork };

// Based on Gen8 i915 engine-reset preparation and the Gen6 BLT domain reset.
// Io must exclusively own BCS and its forcewake, serialize GT reset with all
// other users, provide ordered UC accesses and a monotonic clock, and keep all
// engine mappings valid. StopOwnedWork is allowed only after this owner has
// taken the engine and may have submitted work; it may need to reset a busy
// or enabled ring. This deliberately refuses catastrophic-error state instead
// of attempting Linux's special recovery path.
template<uint32_t EngineBase,uint32_t DomainBit,class Io>
BdwBcsResetResult BdwResetEngineDomain(Io& io,BdwBcsResetIntent intent) {
    static_assert((EngineBase==0x22000 && DomainBit==8) ||
                  (EngineBase==0x2000 && DomainBit==2),"unsupported engine");
    constexpr uint32_t resetCtl=EngineBase+0xd0, domain=0x941c,miMode=EngineBase+0x9c;
    constexpr uint32_t ready=2, catastrophic=4;
    if(!io.owned() || (intent==BdwBcsResetIntent::IdleTakeover && !io.quiescent()))return BdwBcsResetResult::NotStarted;
    uint64_t origin=0;if(!io.monotonicMs(origin))return BdwBcsResetResult::NotStarted;
    uint32_t value=0;
    if(!io.readReg(resetCtl,value) || value==0xffffffffu || (value&catastrophic))
        return BdwBcsResetResult::NotStarted;
    uint32_t domains=0;
    if(!io.readReg(domain,domains) || domains==0xffffffffu || (domains&0x9fu))
        return BdwBcsResetResult::NotStarted;
    enum class WaitResult { Ready, Timeout, Error };
    auto wait=[&](uint32_t reg,uint32_t mask,uint32_t expected,uint64_t timeout) {
        uint64_t start=0,previous=0;if(!io.monotonicMs(start))return WaitResult::Error;previous=start;
        for(unsigned i=0;i<1000000;++i) {
            uint32_t v=0;uint64_t now=0;
            if(!io.owned() || !io.readReg(reg,v) || v==0xffffffffu ||
               !io.monotonicMs(now) || now<previous)return WaitResult::Error;
            if(reg==resetCtl && (v&catastrophic))return WaitResult::Error;
            previous=now;if((v&mask)==expected)return WaitResult::Ready;
            if(now-start>=timeout)return WaitResult::Timeout;
            io.relax();
        }
        // An exhausted poll budget is not evidence that the clock reached
        // the timeout (it may have stalled). Do not continue on that basis.
        return WaitResult::Error;
    };
    if(intent==BdwBcsResetIntent::StopOwnedWork) {
        // Stop the owned command streamer before reset. As in i915, an idle
        // timeout does not by itself prevent the separate reset-ready
        // handshake; a failed MMIO write remains uncertain.
        if(!io.writeReg(miMode,0x01000100u))return BdwBcsResetResult::Uncertain;
        if(wait(miMode,0x200u,0x200u,1000)==WaitResult::Error)return BdwBcsResetResult::Uncertain;
    }
    if(!(value&ready)) {
        if(!io.writeReg(resetCtl,0x00010001u) || wait(resetCtl,ready,ready,700)!=WaitResult::Ready) {
            // A failed write may have reached the device; attempt cancellation
            // but never infer clean state from cancellation alone.
            (void)io.writeReg(resetCtl,0x00010000u);
            return BdwBcsResetResult::Uncertain;
        }
    }
    (void)origin;
    // Always treat the domain write as potentially delivered on failure.
    const bool reset=io.owned() && io.writeReg(domain,DomainBit) && wait(domain,DomainBit,0,500)==WaitResult::Ready;
    // Drop REQUEST_RESET even on a failed domain reset. Posting-read and
    // verify the software request bit, without requiring READY to clear.
    uint32_t after=0;
    const bool cancel=io.owned() && io.writeReg(resetCtl,0x00010000u) &&
        io.readReg(resetCtl,after) && after!=0xffffffffu && !(after&1u);
    return reset && cancel?BdwBcsResetResult::DomainAcknowledged:BdwBcsResetResult::Uncertain;
}
template<class Io> BdwBcsResetResult BdwResetBcsDomain(Io& io,BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover) {
    return BdwResetEngineDomain<0x22000,8>(io,intent);
}
// RCS requires its own forcewake, exclusive engine ownership and GT-reset
// serialization. A domain acknowledgement alone does not initialize render
// state, retire workloads, or prove that any backing pages may be reclaimed.
template<class Io> BdwBcsResetResult BdwResetRcsDomain(Io& io,BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover) {
    return BdwResetEngineDomain<0x2000,2>(io,intent);
}
}
