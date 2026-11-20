#pragma once
#include "native_tsc_observe.h"
#include "native_tsc_policy.h"

namespace brook {
// Experimental, serialized clock for the opt-in Dell audio backend only. This
// is not a general kernel clock. Boot observations are necessary but cannot
// prove future cross-core stability, calibration accuracy, S3 or CPU hotplug.
// The owner must serialize every read, reject suspend/hotplug, and fault audio
// rather than hide a backwards sample. The underlying ordered TSC must be read
// on the CPU index supplied by the caller without migration during the read.
class NativeTscAudioClock {
    uint64_t origin_=0,last_=0,failTsc_=0,failLast_=0;uint32_t khz_=0,cpus_=0,lastCpu_=0,failCpu_=0;bool ready_=false,failed_=false;
    uint8_t failReason_=0;
    bool fault(uint8_t reason,uint32_t cpu,uint64_t tsc){if(!failed_){failReason_=reason;failCpu_=cpu;failTsc_=tsc;failLast_=last_;}
        failed_=true;return false;}
public:
    // Diagnostics for the first failure: 1 not admitted, 2 CPU out of range,
    // 3 backwards TSC (failLast-failTsc ticks, e.g. cross-CPU skew), 4 overflow.
    enum : uint8_t {FailNone=0,FailNotReady=1,FailCpu=2,FailBackwards=3,FailOverflow=4};
    uint8_t failReason()const{return failReason_;}uint32_t failCpu()const{return failCpu_;}uint32_t lastCpu()const{return lastCpu_;}
    uint64_t failTsc()const{return failTsc_;}uint64_t failLast()const{return failLast_;}uint32_t khz()const{return khz_;}
    NativeTscAudioClock()=default;
    NativeTscAudioClock(const NativeTscAudioClock&)=delete;
    NativeTscAudioClock& operator=(const NativeTscAudioClock&)=delete;
    bool init(const NativeTscObservation& o,uint32_t cpus,uint64_t origin){
        if(ready_ || failed_ || !origin || !o.capable || !o.measured || !o.consistent ||
           o.khz<1000 || o.khz>20000000 || !cpus || cpus>64 ||
           o.coreExpected!=cpus-1 || o.coreCount!=cpus-1)return false;
        const int64_t maxSkew=int64_t(o.khz)/100; // 10 us, in TSC ticks
        const uint64_t maxRoundTrip=uint64_t(o.khz)/10; // 100 us
        for(uint32_t i=0;i<o.coreCount;++i){const auto& c=o.cores[i];
            if(c.cpu!=i+1 || c.result!=NativeTscExchangeResult::Complete || c.exchanges!=5 ||
               c.interval.lower>c.interval.upper || c.interval.lower < -maxSkew ||
               c.interval.upper>maxSkew || !c.interval.roundTrip || c.interval.roundTrip>maxRoundTrip)return false;}
        origin_=last_=origin;khz_=o.khz;cpus_=cpus;ready_=true;return true;
    }
    bool read(uint32_t cpu,uint64_t tsc,uint64_t& ns){
        if(!ready_ || failed_)return fault(FailNotReady,cpu,tsc);
        if(cpu>=cpus_)return fault(FailCpu,cpu,tsc);
        if(tsc<last_)return fault(FailBackwards,cpu,tsc);
        uint64_t elapsed=0;if(!NativeTscElapsedNs(origin_,tsc,khz_,elapsed) || elapsed==~uint64_t(0))return fault(FailOverflow,cpu,tsc);
        last_=tsc;lastCpu_=cpu;ns=elapsed+1;return true;
    }
    bool failed()const{return failed_;}
};
}
