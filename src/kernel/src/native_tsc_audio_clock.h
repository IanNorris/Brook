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
    uint64_t origin_=0,last_=0;uint32_t khz_=0,cpus_=0;bool ready_=false,failed_=false;
public:
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
        if(!ready_ || failed_ || cpu>=cpus_ || tsc<last_){failed_=true;return false;}
        uint64_t elapsed=0;if(!NativeTscElapsedNs(origin_,tsc,khz_,elapsed) || elapsed==~uint64_t(0)){failed_=true;return false;}
        last_=tsc;ns=elapsed+1;return true;
    }
    bool failed()const{return failed_;}
};
}
