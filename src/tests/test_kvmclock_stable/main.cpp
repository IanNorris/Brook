#include <cstdio>
#include "../../kernel/src/clock_interval.h"
#include "../../kernel/src/kvmclock.cpp"
#include "../../kernel/src/rtc.cpp"

namespace {
brook::PvClockVcpuTimeInfo page{};
uint64_t sampledTsc=1600;
unsigned samples=0;
void (*duringSample)()=nullptr;
int failures=0;
uint8_t cmosSeconds=17;
#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"PVCLOCK FAIL line %d: %s\n",__LINE__,#x);++failures;} } while(0)
void initialize() {
    page={};page.version=2;page.tsc_timestamp=1000;page.system_time=2000;
    page.tsc_to_system_mul=0x80000000;page.tsc_shift=1;page.flags=1;
    brook::g_pvti=&page;brook::g_enabled=brook::g_stableFeature=true;
    sampledTsc=1600;samples=0;duringSample=nullptr;
}
}
namespace brook {
volatile uint64_t g_lapicTickCount = 0;
volatile uint64_t g_lapicRawTickCount = 0;
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

    // Exercise the real RTC driver with CMOS and elapsed-source fixtures.
    // Its epoch and fractional second must advance together even when the
    // unrelated pvclock phase crosses a second boundary.
    initialize();page.tsc_to_system_mul=0;page.system_time=9750000000ULL;
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

    // Non-KVM realtime uses raw delivered ticks, never adjustable LAPIC ticks.
    g_enabled=false;g_lapicRawTickCount=713;RtcInit();
    const uint64_t fallbackEpoch=g_wallEpochNs;
    CHECK(RtcNowNs()==fallbackEpoch);
    g_lapicRawTickCount+=999;g_lapicTickCount=90000;
    CHECK(RtcNowNs()==fallbackEpoch+999000000);
    g_lapicRawTickCount+=1;g_lapicTickCount=0;
    CHECK(RtcNowNs()==fallbackEpoch+1000000000);
    CHECK(RtcNow()==fallbackEpoch/1000000000+1);

    std::printf("PVCLOCK_STABLE %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
