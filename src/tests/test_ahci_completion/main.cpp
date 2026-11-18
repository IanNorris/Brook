#include "../../kernel/src/ahci_completion.h"
#include "../../kernel/src/storage_io_stats.h"
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>
using namespace brook;

// Scripted device: CI clears after `completeAfter` reads (never when ~0u).
// Time advances by nsPerRead per CI read; ticks by one per sleep/yield, or
// every ticksEvery reads when the caller only spins.
struct FakeIo {
    uint64_t completeAfter=~uint64_t(0), nsPerRead=1000, ticksEvery=0;
    uint64_t reads=0, ns=1, tick=100, pauses=0, sleeps=0, yields=0;
    uint32_t absentAfter=~0u;
    bool noClock=false, stallTicks=false;
    AhciWaitMode waitMode=AhciWaitMode::Sleep;
    uint32_t ci() {
        ++reads;ns+=nsPerRead;
        if(!stallTicks && ticksEvery && reads%ticksEvery==0)++tick;
        if(reads>=absentAfter)return 0xffffffffu;
        return reads>=completeAfter?0u:1u;
    }
    uint64_t nowNs(){return noClock?0:ns;}
    uint64_t ticks(){return tick;}
    void pause(){++pauses;}
    AhciWaitMode mode(){return waitMode;}
    void sleep(){++sleeps;if(!stallTicks)++tick;}
    void yield(){++yields;if(!stallTicks)++tick;}
};

int main() {
    AhciWaitTrace trace;
    // Completes during the spin phase: no sleep, no mode query cost.
    {FakeIo io;io.completeAfter=5;
     assert(AhciWaitCommand(io,2000,20000,1000000,trace)==AhciWaitResult::Complete);
     assert(!trace.spunOut && trace.spins==4 && io.sleeps==0 && io.yields==0);}
    // Already complete: zero spins.
    {FakeIo io;io.completeAfter=1;
     assert(AhciWaitCommand(io,2000,20000,1000000,trace)==AhciWaitResult::Complete && trace.spins==0);}
    // Long command: spins ~20 us then sleeps until completion.
    {FakeIo io;io.completeAfter=40;
     assert(AhciWaitCommand(io,2000,20000,1000000,trace)==AhciWaitResult::Complete);
     assert(trace.spunOut && trace.spins==19 && trace.sleeps==io.sleeps && io.sleeps==20);}
    // Same command where sleeping is not allowed: yields, then spins.
    {FakeIo io;io.completeAfter=40;io.waitMode=AhciWaitMode::Yield;
     assert(AhciWaitCommand(io,2000,20000,1000000,trace)==AhciWaitResult::Complete && io.yields==20 && !io.sleeps);}
    {FakeIo io;io.completeAfter=40;io.waitMode=AhciWaitMode::Spin;
     assert(AhciWaitCommand(io,2000,20000,1000000,trace)==AhciWaitResult::Complete && !io.yields && !io.sleeps &&
            io.pauses==39);}
    // Device removed (all-ones) is reported distinctly.
    {FakeIo io;io.absentAfter=30;
     assert(AhciWaitCommand(io,2000,20000,1000000,trace)==AhciWaitResult::Absent);}
    // Never completes: tick deadline ends the wait after timeoutTicks sleeps.
    {FakeIo io;
     assert(AhciWaitCommand(io,2000,20000,100000000,trace)==AhciWaitResult::Timeout);
     assert(io.tick-100==2000 && io.sleeps==2000);}
    // Spin-only waits use the same deadline from the tick clock.
    {FakeIo io;io.waitMode=AhciWaitMode::Spin;io.ticksEvery=10;
     assert(AhciWaitCommand(io,30,20000,100000000,trace)==AhciWaitResult::Timeout && io.tick-100==30);}
    // A stalled tick clock is bounded by the poll cap.
    {FakeIo io;io.stallTicks=true;
     assert(AhciWaitCommand(io,2000,20000,5000,trace)==AhciWaitResult::Timeout && io.reads==5000);}
    // Without a nanosecond clock the spin budget is a pause count.
    {FakeIo io;io.noClock=true;io.completeAfter=3000;
     assert(AhciWaitCommand(io,2000,16000,1000000,trace)==AhciWaitResult::Complete);
     assert(trace.spins==1000 && trace.spunOut && io.sleeps>0);}
    // A clock running backwards ends the spin phase rather than spinning on.
    {struct Backwards:FakeIo{uint64_t nowNs(){return ns>2?1:5;}};
     Backwards io;io.completeAfter=10;
     assert(AhciWaitCommand(io,2000,20000,1000000,trace)==AhciWaitResult::Complete && trace.spunOut);}
    // Tick wrap at start (now<start) does not expire immediately.
    {FakeIo io;io.tick=~uint64_t(0)-1;io.completeAfter=50;
     assert(AhciWaitCommand(io,2000,20000,1000000,trace)==AhciWaitResult::Complete);}

    // Interrupt health: only completed-without-interrupt timeouts count.
    {AhciIrqHealth h;
     for(uint32_t i=0;i+1<AhciIrqHealth::MissLimit;++i)assert(h.Record(false,true,true));
     assert(h.Record(true,false,true) && h.consecutiveMisses==0);           // a delivered interrupt resets
     for(int i=0;i<100;++i)assert(h.Record(false,true,false));             // long flush still busy: not a miss
     for(int i=0;i<100;++i)assert(h.Record(false,false,true));             // early wake, not a timeout
     for(uint32_t i=0;i+1<AhciIrqHealth::MissLimit;++i)assert(h.Record(false,true,true));
     assert(!h.Record(false,true,true) && h.disabled);
     assert(!h.Record(true,false,true));}                                   // never re-enables

    // MSI capability layout: 32-bit, 64-bit and per-vector-mask variants.
    {PciMsiLayout l;
     assert(PciMsiRegisters(0x80,0x0000,l) && l.data==0x88 && !l.upper && !l.mask && l.enabled==1);
     assert(PciMsiRegisters(0x80,0x0080,l) && l.upper==0x88 && l.data==0x8c && !l.mask && l.enabled==0x81);
     assert(PciMsiRegisters(0x50,0x0180,l) && l.upper==0x58 && l.data==0x5c && l.mask==0x60);
     assert(PciMsiRegisters(0x50,0x0100,l) && !l.upper && l.data==0x58 && l.mask==0x5c);
     assert(PciMsiRegisters(0x80,0x00f6,l) && l.enabled==0x87);              // MME cleared, MMC kept
     assert(!PciMsiRegisters(0x3c,0,l) && !PciMsiRegisters(0x81,0,l) && !PciMsiRegisters(0xec,0,l));}
    // The sleep-time port mask never includes setup FIS sources.
    static_assert(!(AhciPortIrqMask&((1u<<1)|(1u<<2)|(1u<<5))),"setup FISes re-interrupt each rearm");
    static_assert((AhciPortIrqMask&1u) && (AhciPortIrqMask&AhciPortIrqErrors)==AhciPortIrqErrors,"completion+errors");

    // Storage counters: concurrent drivers, exact totals.
    {StorageIoCounters c;std::vector<std::thread> threads;
     for(int t=0;t<4;++t)threads.emplace_back([&c,t]{for(int i=0;i<10000;++i){
         if(t&1)c.Write(4096);else c.Read(512);if(!(i%100))c.Flush();}});
     for(auto& t:threads)t.join();
     const auto s=c.Snapshot();
     assert(s.readOps==20000 && s.readBytes==20000ull*512 && s.writeOps==20000 && s.writeBytes==20000ull*4096 &&
            s.flushOps==400);
     StorageIoTotals sum;StorageIoAdd(sum,s);StorageIoAdd(sum,s);
     assert(sum.readBytes==2*s.readBytes && sum.writeOps==2*s.writeOps && sum.flushOps==800);}
    return 0;
}
