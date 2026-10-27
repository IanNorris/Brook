#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include "../../kernel/src/tick_clock.h"
#include "../../kernel/src/futex_timeout.h"
#include "../../kernel/src/timerfd_state.h"
#include "../../kernel/src/clock_interval.h"
#include "../../kernel/src/kvmclock.cpp"
#include "../../kernel/src/rtc.cpp"

namespace {
BrookPvclockSlot clockPages[BROOK_PVCLOCK_CPUS]{};
auto& page=clockPages[0].clock;
uint32_t currentCpu=0,registeredCpu=0;
uint64_t registeredAddress=0;
bool rdtscpSupported=true,registeredAux=false;
unsigned registrations=0;
unsigned testVendor=1,hypercallCalls=0;uint8_t hypercallInstruction=0;uint64_t hypercallNumber=0,hypercallTarget=0;int64_t hypercallReturn=0;
bool testKvmPresent=true;
uint32_t testKvmFeatures=0, testKvmHints=0, testCpuidCalls=0;
uint64_t sampledTsc=1600;
unsigned samples=0;
void (*duringSample)()=nullptr;
int failures=0;
uint8_t cmosSeconds=17;
#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"PVCLOCK FAIL line %d: %s\n",__LINE__,#x);++failures;} } while(0)
void initialize() {
    for (unsigned cpu=0;cpu<BROOK_PVCLOCK_CPUS;++cpu) {clockPages[cpu]={};brook::g_clockReady[cpu]=false;}
    page.version=2;page.tsc_timestamp=1000;page.system_time=2000;
    page.tsc_to_system_mul=0x80000000;page.tsc_shift=1;page.flags=1;
    brook::g_pvti=clockPages;brook::g_enabled=brook::g_stableFeature=true;
    brook::g_clockReady[0]=true;brook::g_clockPage=brook::PhysicalAddress(0x100000);
    auto* metadata=brook::ClockMetadata();metadata->magic=BROOK_PVCLOCK_MAGIC;
    metadata->version=BROOK_PVCLOCK_VERSION;metadata->stride=BROOK_PVCLOCK_STRIDE;
    metadata->cpus=BROOK_PVCLOCK_CPUS;metadata->features=BROOK_PVCLOCK_RDTSCP;
    currentCpu=0;registrations=0;rdtscpSupported=true;
    sampledTsc=1600;samples=0;duringSample=nullptr;
}
}
namespace brook {
void KvmClockTestCpuid(uint32_t leaf,uint32_t sub,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d) {
    ++testCpuidCalls;a=b=c=d=0;
    CHECK(sub==0);
    if(leaf==0){
        if(testVendor==1){b=0x756e6547;d=0x49656e69;c=0x6c65746e;}
        else if(testVendor==2){b=0x68747541;d=0x69746e65;c=0x444d4163;}
    } else if (leaf==0x40000000 && testKvmPresent) {
        a=0x40000001;b=0x4b4d564b;c=0x564b4d56;d=0x4d;
    } else if (leaf==0x40000001) {a=testKvmFeatures;d=testKvmHints;}
}
int64_t KvmClockTestHypercall(uint8_t instruction,uint64_t number,uint64_t target){
    ++hypercallCalls;hypercallInstruction=instruction;hypercallNumber=number;hypercallTarget=target;return hypercallReturn;
}
uint32_t KvmClockTestCpu() {return currentCpu;}
bool KvmClockTestRdtscpSupported() {return rdtscpSupported;}
void KvmClockTestRegister(uint32_t cpu,uint64_t address,bool aux) {
    ++registrations;registeredCpu=cpu;registeredAddress=address;registeredAux=aux;
}
volatile uint64_t g_lapicTickCount = 0;
volatile uint64_t g_lapicRawTickCount = 0;
uint64_t ApicTickCount() { return g_lapicTickCount; }
uint8_t RtcTestCmosRead(uint8_t reg) {
    switch(reg) {
    case 0x00: return cmosSeconds;
    case 0x02: return 12;
    case 0x04: return 8;
    case 0x07: return 18;
    case 0x08: return 10;
    case 0x09: return 26;
    case 0x0b: return 6; // binary, 24-hour
    default: return 0;
    }
}
extern "C" void SerialPrintf(const char*, ...) {}
void ApicAdjustTimerRate(uint32_t) {}
uint64_t KvmClockTestTsc() {
    ++samples;if(duringSample)duringSample();return sampledTsc;
}
}
int main() {
    using namespace brook;
    // Real bounded steal-reader code: unsupported CPUs, odd versions and
    // reserved flags must not publish a partial/invalid value.
    KvmStealRecord stealPages[64]{};
    g_stealRecords=stealPages;g_stealEnabled[0]=true;g_stealEnabled[1]=true;
    // Actual cached wrapper: real APIC IDs differ deliberately from CPU indices.
    ConfigureSchedYield(0,1u<<13,51);ConfigureSchedYield(1,1u<<13,99);
    int64_t hypercallResult=777;const auto cachedCalls=testCpuidCalls;
    CHECK(KvmSchedYieldIssuerMask()==3);
    CHECK(KvmSchedYield(1,&hypercallResult) && hypercallResult==0 && hypercallCalls==1);
    CHECK(hypercallInstruction==1 && hypercallNumber==11 && hypercallTarget==99 && testCpuidCalls==cachedCalls);
    CHECK(!KvmSchedYield(0,&hypercallResult) && !KvmSchedYield(64,&hypercallResult) && !KvmSchedYield(1,nullptr) && hypercallCalls==1);
    currentCpu=1;CHECK(KvmSchedYield(0,&hypercallResult) && hypercallTarget==51);currentCpu=0;
    testVendor=2;ConfigureSchedYield(0,1u<<13,51);hypercallReturn=-1000;
    CHECK(KvmSchedYield(1,&hypercallResult) && hypercallInstruction==2 && hypercallResult==-1000);
    hypercallResult=777;const auto performed=hypercallCalls;
    ConfigureSchedYield(0,0,51);CHECK(!KvmSchedYield(1,&hypercallResult) && hypercallResult==777);
    testVendor=0;ConfigureSchedYield(0,1u<<13,51);CHECK(!KvmSchedYield(1,&hypercallResult));
    testVendor=1;ConfigureSchedYield(0,1u<<13,51);g_stealEnabled[1]=false;
    CHECK(!KvmSchedYield(1,&hypercallResult) && hypercallResult==777);g_stealEnabled[1]=true;
    g_yieldTargetReady[1]=false;CHECK(!KvmSchedYield(1,&hypercallResult));g_yieldTargetReady[1]=true;
    CHECK(hypercallCalls==performed);hypercallReturn=0;
    // Query advertised features separately from registered record hints. A
    // preemption hint is not versioned steal time and must not be cleared.
    testKvmFeatures=(1u<<5)|(1u<<7)|(1u<<13);testKvmHints=1;
    stealPages[0].preempted=1;stealPages[1].preempted=2;stealPages[1].version=3;
    KvmVirtualizationStatus virtualization{};
    KvmVirtualizationRead(2,&virtualization);
    CHECK(virtualization.present && virtualization.features==testKvmFeatures && virtualization.hints==1);
    CHECK(virtualization.cpus==2 && virtualization.registeredStealMask==3);
    CHECK(virtualization.preemptedHintMask==1 && virtualization.otherHintMask==2);
    CHECK(stealPages[0].preempted==1 && stealPages[1].preempted==2 && stealPages[1].version==3);
    uint8_t hintByte=0x77;const auto hintCpuidCalls=testCpuidCalls;
    CHECK(KvmStealReadPreempted(0,&hintByte) && hintByte==1);
    CHECK(KvmStealReadPreempted(1,&hintByte) && hintByte==2); // odd steal version doesn't govern hint byte
    CHECK(!KvmStealReadPreempted(64,&hintByte) && hintByte==2);
    CHECK(!KvmStealReadPreempted(2,&hintByte) && hintByte==2);
    CHECK(!KvmStealReadPreempted(0,nullptr));
    CHECK(testCpuidCalls==hintCpuidCalls && stealPages[1].preempted==2 && stealPages[1].version==3);
    stealPages[1].preempted=0xff;g_stealEnabled[63]=true;stealPages[63].preempted=1;
    KvmVirtualizationRead(UINT32_MAX,&virtualization);
    CHECK(virtualization.cpus==64 && virtualization.registeredStealMask==(uint64_t(1)<<63|3));
    CHECK(virtualization.preemptedHintMask==(uint64_t(1)<<63|3) && virtualization.otherHintMask==2);
    g_stealEnabled[63]=false;g_stealEnabled[1]=false;
    KvmVirtualizationRead(2,&virtualization);
    CHECK(virtualization.registeredStealMask==1 && virtualization.preemptedHintMask==1 && !virtualization.otherHintMask);
    testKvmFeatures=0x80000000;testKvmHints=0x80000001;
    KvmVirtualizationRead(2,&virtualization);
    CHECK(virtualization.features==0x80000000 && virtualization.hints==0x80000001);
    CHECK(virtualization.registeredStealMask==1); // Advertised bits do not invent/erase registration.
    g_stealRecords=nullptr;KvmVirtualizationRead(2,&virtualization);
    CHECK(!virtualization.registeredStealMask && !virtualization.preemptedHintMask);
    g_stealRecords=stealPages;testKvmFeatures=0;testKvmHints=0;
    KvmVirtualizationRead(0,&virtualization);
    CHECK(virtualization.present && !virtualization.features && !virtualization.cpus && !virtualization.registeredStealMask);
    testKvmPresent=false;KvmVirtualizationRead(2,&virtualization);
    CHECK(!virtualization.present && !virtualization.features && !virtualization.hints && !virtualization.registeredStealMask);
    const auto calls=testCpuidCalls;KvmVirtualizationRead(64,nullptr);CHECK(testCpuidCalls==calls);
    testKvmPresent=true;g_stealEnabled[1]=true;stealPages[1]={};
    uint64_t stolen=777;
    CHECK(KvmStealReadNs(0,&stolen) && stolen==0);
    stealPages[1].version=2;stealPages[1].steal=123456789;
    CHECK(KvmStealReadNs(1,&stolen) && stolen==123456789);
    stealPages[1].version=3;
    CHECK(!KvmStealReadNs(1,&stolen) && stolen==123456789);
    stealPages[1].version=4;stealPages[1].flags=1;
    CHECK(!KvmStealReadNs(1,&stolen) && stolen==123456789);
    CHECK(!KvmStealReadNs(64,&stolen) && !KvmStealReadNs(0,nullptr));
    g_stealEnabled[0]=false;
    CHECK(!KvmStealReadNs(0,&stolen));
    g_stealEnabled[1]=false;g_stealRecords=nullptr;
    // Futex deadlines retain ns precision and selected clock identity. A
    // rounded scheduler hint may not turn a not-yet-expired deadline into timeout.
    uint64_t nsValue=123;
    CHECK(FutexTimeoutNanoseconds(2,999999999,&nsValue) && nsValue==2999999999);
    CHECK(!FutexTimeoutNanoseconds(-1,0,&nsValue) && nsValue==2999999999);
    CHECK(!FutexTimeoutNanoseconds(0,-1,&nsValue));
    CHECK(!FutexTimeoutNanoseconds(0,1000000000,&nsValue));
    CHECK(FutexTimeoutNanoseconds(INT64_MAX,0,&nsValue) && nsValue==uint64_t(INT64_MAX));
    CHECK(!FutexTimeoutNanoseconds(0,0,nullptr));
    for(int64_t sec:{9223372035LL,9223372036LL,9223372037LL,9223372036854775807LL})
        for(int64_t fraction:{0LL,999999999LL}) {
            const auto expected=sec==9223372035LL?uint64_t(sec)*1000000000+fraction:uint64_t(INT64_MAX);
            CHECK(FutexTimeoutNanoseconds(sec,fraction,&nsValue) && nsValue==expected);
        }
    CHECK(!FutexTimeoutNanoseconds(INT64_MAX,-1,&nsValue));
    CHECK(!FutexTimeoutNanoseconds(INT64_MAX,1000000000,&nsValue));
    FutexWaitDeadline futex;
    CHECK(!futex.expired(~uint64_t(0)) && futex.wakeupTick(0,100)==~uint64_t(0));
    CHECK(FutexMakeDeadline(true,1000000,123456789,1,&futex));
    CHECK(futex.nanoseconds==124456789 && futex.clockId==1 && futex.finite);
    CHECK(!futex.expired(124456788) && futex.expired(124456789));
    CHECK(FutexMakeDeadline(true,2,uint64_t(INT64_MAX)-1,1,&futex) &&
          futex.nanoseconds==uint64_t(INT64_MAX) && futex.finite);
    CHECK(!futex.expired(uint64_t(INT64_MAX)-1) && futex.wakeupTick(0,10)!=~uint64_t(0));
    CHECK(FutexMakeDeadline(false,123456789,999999999,0,&futex));
    CHECK(futex.clockId==0 && futex.nanoseconds==123456789 && futex.expired(999999999));
    CHECK(!FutexMakeDeadline(false,1,0,2,&futex));
    CHECK(!FutexMakeDeadline(false,1,0,0,nullptr));
    CHECK(TimeoutAddSafe(0,0)==0);
    CHECK(TimeoutAddSafe(uint64_t(INT64_MAX)-2,1)==uint64_t(INT64_MAX)-1);
    CHECK(TimeoutAddSafe(uint64_t(INT64_MAX)-1,1)==uint64_t(INT64_MAX));
    CHECK(TimeoutAddSafe(uint64_t(INT64_MAX)-1,2)==uint64_t(INT64_MAX));
    CHECK(TimeoutAddSafe(100,uint64_t(INT64_MAX))==uint64_t(INT64_MAX));
    for (uint64_t remaining : {1ULL,99999ULL,500000ULL,999999ULL,1000000ULL,1000001ULL,10000000ULL}) {
        CHECK(FutexMakeDeadline(true,remaining,987654321,1,&futex));
        const uint64_t tick=futex.wakeupTick(987654321,4321);
        CHECK(tick==4321+(remaining+999999)/1000000);
        // An early coarse wake must leave the true timeout unexpired and
        // provide a future hint when the caller retries in the same queue.
        for (uint64_t elapsed=0;elapsed<remaining;elapsed+=100003) {
            CHECK(!futex.expired(987654321+elapsed));
            CHECK(futex.wakeupTick(987654321+elapsed,tick)>tick);
        }
    }
    CHECK(futex.wakeupTick(futex.nanoseconds,4321)==4321);
    CHECK(futex.wakeupTick(987654321,~uint64_t(0)-2)==~uint64_t(0)-1);

    uint64_t milliseconds=999;
    CHECK(TickClockElapsedMs(100,123,100,&milliseconds) && milliseconds==123);
    CHECK(TickClockElapsedMs(100,123,1000099,&milliseconds) && milliseconds==123);
    CHECK(TickClockElapsedMs(100,123,1000100,&milliseconds) && milliseconds==124);
    CHECK(!TickClockElapsedMs(100,123,99,&milliseconds) && milliseconds==124);
    CHECK(!TickClockElapsedMs(0,~uint64_t(0)-5,6000000,&milliseconds));
    CHECK(!TickClockElapsedMs(0,0,0,nullptr));
    volatile uint64_t floor=1000;
    CHECK(TickClockPublishFloor(&floor,900)==1000);
    CHECK(TickClockPublishFloor(&floor,1001)==1001);
    CHECK(TickClockPublishFloor(&floor,999)==1001);
    std::atomic<bool> regression{false};
    std::vector<std::thread> publishers;
    for (uint64_t cpu=0;cpu<8;++cpu) publishers.emplace_back([&,cpu] {
        uint64_t previous=0;
        for (uint64_t iteration=0;iteration<30000;++iteration) {
            const uint64_t now=TickClockPublishFloor(&floor,cpu*30000+iteration);
            if (now<previous) regression.store(true);
            previous=now;
        }
    });
    for (auto& publisher:publishers) publisher.join();
    CHECK(!regression.load() && __atomic_load_n(&floor,__ATOMIC_ACQUIRE)==239999);
    uint64_t result=99;
    initialize();CHECK(KvmClockReadStableNs(&result) && result==2600);
    page.tsc_shift=-1;CHECK(KvmClockReadStableNs(&result) && result==2150);
    page.flags=0;CHECK(!KvmClockReadStableNs(&result) && result==2150);
    initialize();g_stableFeature=false;CHECK(!KvmClockReadStableNs(&result) && !samples);
    initialize();g_enabled=false;CHECK(!KvmClockReadStableNs(&result));
    initialize();page.version=3;CHECK(!KvmClockReadStableNs(&result) && !samples);
    initialize();duringSample=[] {
        if(samples==1) {page.version=4;page.tsc_timestamp=1500;page.system_time=4000;}
    };
    CHECK(KvmClockReadStableNs(&result) && samples==2 && result==4100);
    initialize();duringSample=[] {page.version+=2;};CHECK(!KvmClockReadStableNs(&result) && samples==32);
    initialize();sampledTsc=999;CHECK(!KvmClockReadStableNs(&result));
    initialize();page.tsc_shift=64;CHECK(!KvmClockReadStableNs(&result));
    initialize();page.tsc_shift=-64;CHECK(!KvmClockReadStableNs(&result));
    initialize();page.tsc_to_system_mul=0;CHECK(!KvmClockReadStableNs(&result));
    initialize();sampledTsc=~uint64_t(0);page.tsc_timestamp=0;CHECK(!KvmClockReadStableNs(&result));
    initialize();page.system_time=~uint64_t(0)-100;CHECK(!KvmClockReadStableNs(&result));
    CHECK(!KvmClockReadStableNs(nullptr));
    initialize();currentCpu=1;
    CHECK(!KvmClockReadStableNs(&result)); // An unregistered AP cannot borrow CPU0.
    CHECK(KvmClockInitOnCpu(1));
    CHECK(registrations==1 && registeredCpu==1 && registeredAddress==0x100040 && registeredAux);
    CHECK(KvmClockRegisteredCpuMask()==3);
    CHECK(KvmClockInitOnCpu(1) && registrations==1); // Never reset a live record.
    CHECK(!KvmClockInitOnCpu(0) && !KvmClockInitOnCpu(64));
    clockPages[1].clock=page;clockPages[1].clock.system_time=9000;
    CHECK(KvmClockReadStableNs(&result) && result==9600);
    currentCpu=64;CHECK(!KvmClockReadStableNs(&result) && result==9600);

    initialize();clockPages[1].clock=page;g_clockReady[1]=true;
    clockPages[1].clock.system_time=4000;
    duringSample=[] {if(samples==1)currentCpu=1;};
    CHECK(KvmClockReadStableNs(&result) && samples==2 && result==4600);
    initialize();clockPages[1].clock=page;g_clockReady[1]=true;
    duringSample=[] {currentCpu^=1;};
    CHECK(!KvmClockReadStableNs(&result) && samples==32);

    // Resume AP1 before BSP0: the still-stale BSP record extrapolates across
    // the host pause. AP1's own record was updated before its VM entry.
    initialize();sampledTsc=3000000;currentCpu=1;
    clockPages[1].clock=page;clockPages[1].clock.tsc_timestamp=2999000;
    g_clockReady[1]=true;
    CHECK(KvmClockReadStableNs(&result) && result==3000);
    page.tsc_timestamp=2999000; // BSP0 finally resumes and catches up.
    CHECK(KvmClockReadStableNs(&result) && result==3000);
    CHECK(KvmClockReadNs()==3000); // Legacy consumers also use the current CPU.

    initialize();currentCpu=1;rdtscpSupported=false;
    CHECK(KvmClockInitOnCpu(1) && !registeredAux);
    CHECK(!(ClockMetadata()->features&BROOK_PVCLOCK_RDTSCP));
    // A recalibration step must not turn a fresh five-second I/O budget into
    // an immediate timeout. The previous unsigned-only check fails this case.
    CHECK(!ClockIntervalElapsed(9999,10000,5000));
    CHECK(!ClockIntervalElapsed(14999,10000,5000));
    CHECK(ClockIntervalElapsed(15000,10000,5000));
    CHECK(ClockIntervalElapsed(15001,10000,5000));
    const uint64_t nearWrap=~uint64_t(0)-999;
    CHECK(!ClockIntervalElapsed(3999,nearWrap,5000));
    CHECK(ClockIntervalElapsed(4000,nearWrap,5000));
    CHECK(!ClockIntervalElapsed((uint64_t(1)<<63)+10000,10000,5000));

    // An elapsed clock can skip a sampling boundary or be read twice within
    // one millisecond. Neither may lose a sample or charge it twice.
    uint64_t lastSample=0;
    CHECK(!ClockTakePeriodicSample(4999,5000,lastSample) && lastSample==0);
    CHECK(ClockTakePeriodicSample(5001,5000,lastSample) && lastSample==5000);
    CHECK(!ClockTakePeriodicSample(5001,5000,lastSample));
    CHECK(!ClockTakePeriodicSample(9999,5000,lastSample));
    CHECK(ClockTakePeriodicSample(10000,5000,lastSample) && lastSample==10000);
    CHECK(ClockTakePeriodicSample(29001,5000,lastSample) && lastSample==25000);
    CHECK(!ClockTakePeriodicSample(29001,5000,lastSample));
    CHECK(!ClockTakePeriodicSample(24000,5000,lastSample) && lastSample==25000);
    CHECK(!ClockTakePeriodicSample(30000,0,lastSample));
    lastSample=nearWrap;
    CHECK(ClockTakePeriodicSample(4001,5000,lastSample) && lastSample==4000);
    CHECK(!ClockTakePeriodicSample(4001,5000,lastSample));

    // Exercise the real RTC driver with CMOS and elapsed-source fixtures.
    // Its epoch and fractional second must advance together even when the
    // unrelated pvclock phase crosses a second boundary.
    initialize();sampledTsc=page.tsc_timestamp;page.system_time=9750000000ULL;
    g_lapicTickCount=300;g_lapicRawTickCount=713;
    RtcInit();const uint64_t epoch=g_wallEpochNs;
    CHECK(RtcNowNs()==epoch);
    page.system_time+=249999999;CHECK(RtcNowNs()==epoch+249999999);
    page.system_time+=1;CHECK(RtcNowNs()==epoch+250000000);
    g_lapicTickCount=1;g_bootEpochSec-=10; // apparent/RTC changes do not reanchor
    page.system_time+=750000000;CHECK(RtcNowNs()==epoch+1000000000);
    CHECK(RtcNow()==epoch/1000000000+1);
    // A CMOS change does not silently alter the immutable wall anchor. There
    // is no runtime clock-setting API; recalibration owns only legacy ticks.
    g_lapicTickCount=400;cmosSeconds=16;RtcRecalibrateLapic();
    CHECK(RtcNowNs()==epoch+1000000000);
    cmosSeconds=17;
    RtcSetTimezoneOffset(3600);CHECK(RtcNowLocal()==RtcNow()+3600);
    CHECK(RtcNowNs()==epoch+1000000000);

    // Non-KVM realtime uses the selected millisecond elapsed source.
    g_enabled=false;g_lapicRawTickCount=713;g_lapicTickCount=700;RtcInit();
    const uint64_t fallbackEpoch=g_wallEpochNs;
    CHECK(RtcNowNs()==fallbackEpoch);
    g_lapicRawTickCount+=999;g_lapicTickCount=1699;
    CHECK(RtcNowNs()==fallbackEpoch+999000000);
    g_lapicRawTickCount+=1;g_lapicTickCount=1700;
    CHECK(RtcNowNs()==fallbackEpoch+1000000000);
    CHECK(RtcNow()==fallbackEpoch/1000000000+1);
    // On VMware elapsed milliseconds come from virtual TSC, independently of
    // delivered timer IRQs. Raw IRQ changes cannot advance the RTC epoch.
    g_lapicRawTickCount+=1234567;
    CHECK(RtcNowNs()==fallbackEpoch+1000000000);

    // RTC frequency discipline must not rewrite the elapsed deadline domain.
    // A backwards step postpones already-armed wakeups; a forwards step can
    // consume an I/O timeout budget without any elapsed source time passing.
    // Exercise the actual RTC recalibration code, not a replacement model.
    for (unsigned available=0;available<2;++available) {
        initialize();g_enabled=available;cmosSeconds=17;
        g_lapicTickCount=2000;g_lapicRawTickCount=2000;RtcInit();
        g_lapicTickCount=6000;g_lapicRawTickCount=6000;cmosSeconds=18;
        RtcRecalibrateLapic();
        CHECK(g_lapicTickCount==6000);
        CHECK(g_lapicRawTickCount==6000);
        cmosSeconds=17;g_lapicTickCount=2000;g_lapicRawTickCount=2000;RtcInit();
        g_lapicTickCount=2001;g_lapicRawTickCount=2001;cmosSeconds=20;
        RtcRecalibrateLapic();
        CHECK(g_lapicTickCount==2001);
        CHECK(g_lapicRawTickCount==2001);
    }

    // The real timerfd accounting code, independent of scheduler tick phase.
    TimerFdState timer{};
    timer.armed=true;timer.deadline=16000001;
    timer.refresh(16000000);CHECK(!timer.pending && timer.armed);
    timer.refresh(16000001);CHECK(timer.pending==1 && !timer.armed);
    timer.refresh(999999999);CHECK(timer.pending==1);
    timer={};timer.armed=true;timer.deadline=100;timer.interval=7;
    timer.refresh(120);CHECK(timer.pending==3 && timer.deadline==121);
    timer.refresh(120);CHECK(timer.pending==3 && timer.deadline==121);
    timer.refresh(121);CHECK(timer.pending==4 && timer.deadline==128);
    timer.pending=0;timer.refresh(148);CHECK(timer.pending==3 && timer.deadline==149);
    // Retain submillisecond periodic precision, never round interval to ticks.
    timer={};timer.armed=true;timer.deadline=1000000000;timer.interval=100;
    timer.refresh(1001000050);CHECK(timer.pending==10001 && timer.deadline==1001000100);
    timer={};timer.armed=true;timer.deadline=0;timer.interval=1;
    timer.refresh(~uint64_t(0));CHECK(timer.pending==~uint64_t(0) && !timer.armed);
    timer={};timer.armed=true;timer.deadline=10;timer.pending=~uint64_t(0);
    timer.refresh(10);CHECK(timer.pending==~uint64_t(0));
    uint64_t ns=123;
    CHECK(TimerFdNanoseconds(1,999999999,&ns) && ns==1999999999);
    CHECK(!TimerFdNanoseconds(-1,0,&ns));
    CHECK(!TimerFdNanoseconds(0,-1,&ns));
    CHECK(!TimerFdNanoseconds(0,1000000000,&ns));
    CHECK(TimerFdNanoseconds(INT64_MAX,0,&ns) && ns==uint64_t(INT64_MAX));
    for(int64_t sec:{9223372035LL,9223372036LL,9223372036854775807LL})for(int64_t fraction:{0LL,999999999LL}){
        const auto expected=sec==9223372035LL?uint64_t(sec)*1000000000+fraction:uint64_t(INT64_MAX);
        CHECK(TimerFdNanoseconds(sec,fraction,&ns) && ns==expected);
    }
    CHECK(!TimerFdNanoseconds(INT64_MAX,-1,&ns) && ns==uint64_t(INT64_MAX));
    CHECK(!TimerFdNanoseconds(INT64_MAX,1000000000,&ns) && ns==uint64_t(INT64_MAX));
    timer={};timer.armed=true;timer.deadline=10;timer.interval=uint64_t(INT64_MAX);
    timer.refresh(100);CHECK(timer.pending==1 && timer.deadline==uint64_t(INT64_MAX) && timer.armed);
    timer.refresh(100);CHECK(timer.pending==1);
    timer.refresh(uint64_t(INT64_MAX));CHECK(timer.pending==2 && !timer.armed);
    timer.refresh(uint64_t(INT64_MAX));CHECK(timer.pending==2); // no manufactured terminal repeats

    std::printf("PVCLOCK_STABLE %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
