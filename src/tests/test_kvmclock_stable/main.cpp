#include <cstdio>
#include "../../kernel/src/clock_interval.h"
#include "../../kernel/src/kvmclock.cpp"

namespace {
brook::PvClockVcpuTimeInfo page{};
uint64_t sampledTsc=1600;
unsigned samples=0;
void (*duringSample)()=nullptr;
int failures=0;
#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"PVCLOCK FAIL line %d: %s\n",__LINE__,#x);++failures;} } while(0)
void initialize() {
    page={};page.version=2;page.tsc_timestamp=1000;page.system_time=2000;
    page.tsc_to_system_mul=0x80000000;page.tsc_shift=1;page.flags=1;
    brook::g_pvti=&page;brook::g_enabled=brook::g_stableFeature=true;
    sampledTsc=1600;samples=0;duringSample=nullptr;
}
}
namespace brook {
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

    std::printf("PVCLOCK_STABLE %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
