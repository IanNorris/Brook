#pragma once
#include <stdint.h>

namespace brook {
// Runtime completion wait for the single issued AHCI command (slot 0). Pure
// logic shared with hosted tests. The panic-time journal writer never uses it:
// panic I/O keeps the bounded PortIo::waitClear polling loop.
//
// Phase 1 spins for at most spinNs: a short command often completes within a
// few microseconds and sleeping would only add wake latency. Phase 2 waits in
// the mode the caller may use right now:
//   Sleep - block until the completion interrupt, with a one-tick safety
//           timeout, so a lost interrupt degrades to millisecond polling;
//   Yield - give the CPU to other ready threads between polls;
//   Spin  - the previous pause loop (IRQs off, no thread, or panic pending).
// The tick deadline and the all-ones (device absent) check match the
// previous polling loop. maxPolls bounds the loop if the tick clock stalls.
enum class AhciWaitResult : uint8_t { Complete, Timeout, Absent };
enum class AhciWaitMode : uint8_t { Sleep, Yield, Spin };
struct AhciWaitTrace { uint32_t spins=0, sleeps=0, yields=0; bool spunOut=false; };

// Io: uint32_t ci(); uint64_t nowNs(); uint64_t ticks(); void pause();
//     AhciWaitMode mode(); void sleep(); void yield();
template<class Io> AhciWaitResult AhciWaitCommand(Io& io, uint64_t timeoutTicks, uint64_t spinNs,
                                                 uint64_t maxPolls, AhciWaitTrace& trace) {
    trace={};
    const uint64_t startTick=io.ticks();
    const uint64_t startNs=io.nowNs();
    auto expired=[&]{ const uint64_t now=io.ticks(); return now>=startTick && now-startTick>=timeoutTicks; };
    for(uint64_t polls=0;polls<maxPolls;++polls) {
        const uint32_t value=io.ci();
        if(value==0xffffffffu)return AhciWaitResult::Absent;
        if(!(value&1u))return AhciWaitResult::Complete;
        if(expired())return AhciWaitResult::Timeout;
        if(!trace.spunOut) {
            // Without a nanosecond clock (nowNs()==0) the budget is a pause
            // count instead; a backwards clock ends the spin phase.
            const uint64_t now=io.nowNs();
            const bool spinning=startNs ? now>=startNs && now-startNs<spinNs : trace.spins<spinNs/16;
            if(spinning) { ++trace.spins;io.pause();continue; }
            trace.spunOut=true;
        }
        switch(io.mode()) {
        case AhciWaitMode::Sleep: ++trace.sleeps;io.sleep();break;
        case AhciWaitMode::Yield: ++trace.yields;io.yield();break;
        case AhciWaitMode::Spin: io.pause();break;
        }
    }
    return AhciWaitResult::Timeout;
}

// Interrupt mode stays enabled only while sleeps observe their interrupt. A
// command that completed while its waiter slept a whole safety tick without
// any interrupt counts as a miss; MissLimit consecutive misses permanently
// revert to polling. A still-busy timeout (a long flush) is not a miss.
struct AhciIrqHealth {
    static constexpr uint32_t MissLimit=8;
    uint32_t consecutiveMisses=0;
    bool disabled=false;
    // Returns true while interrupt mode remains usable.
    bool Record(bool interrupted, bool timedOut, bool completed) {
        if(disabled)return false;
        if(interrupted)consecutiveMisses=0;
        else if(timedOut && completed && ++consecutiveMisses>=MissLimit)disabled=true;
        return !disabled;
    }
};

// Generic single-message PCI MSI capability layout (capability ID 5).
struct PciMsiLayout { uint8_t upper=0, data=0, mask=0; uint16_t enabled=0; };
inline bool PciMsiRegisters(uint32_t offset, uint16_t control, PciMsiLayout& out) {
    if(offset<0x40 || (offset&3) || offset>0xe8)return false;
    const bool wide=control&(1u<<7), masked=control&(1u<<8);
    out.upper=wide?uint8_t(offset+8):0; out.data=uint8_t(offset+(wide?12:8));
    out.mask=masked?uint8_t(offset+(wide?16:12)):0;
    out.enabled=uint16_t((control&~0x70u)|1u); // one message (MME=0), MSI enable
    return true;
}
// Port interrupt sources enabled only while a runtime waiter sleeps. Only
// completion (device-to-host register and set-device-bits FISes) and error
// sources: PIO/DMA setup FISes can be latched in PxIS before a command
// completes and would re-interrupt every rearm. An error leaves PxIS set and
// may leave CI set; the waiter then stops sleeping (AhciPortIrqErrors).
static constexpr uint32_t AhciPortIrqErrors=(1u<<24)|(1u<<26)|(1u<<27)|(1u<<28)|(1u<<29)|(1u<<30);
static constexpr uint32_t AhciPortIrqMask=(1u<<0)|(1u<<3)|AhciPortIrqErrors;
} // namespace brook
