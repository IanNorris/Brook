#pragma once
#include <stdint.h>
namespace brook {
// Bounded cadence history. Times are monotonic milliseconds, not render CPU time.
// Expiry keeps inactive applications from displaying stale 'live' frame rates.
template<unsigned Capacity=128> struct DesktopIntervals {
    struct Sample { uint64_t tick; uint32_t ms; } samples[Capacity]{};
    unsigned next=0,count=0; uint64_t last=0;
    void Reset() { next=count=0;last=0; }
    void Record(uint64_t now) {
        if(last && now>last) {
            uint64_t elapsed=now-last;
            samples[next]={now,static_cast<uint32_t>(elapsed>(~uint32_t{0})?(~uint32_t{0}):elapsed)};
            next=(next+1)%Capacity;if(count<Capacity)++count;
        }
        last=now;
    }
    struct Summary { unsigned count; uint32_t p80,p95,max; };
    Summary Read(uint64_t now,uint32_t windowMs=10000) const {
        uint32_t values[Capacity];unsigned n=0;
        for(unsigned i=0;i<count;++i)if(now>=samples[i].tick && now-samples[i].tick<=windowMs)values[n++]=samples[i].ms;
        for(unsigned i=1;i<n;++i){uint32_t v=values[i];unsigned j=i;while(j && values[j-1]>v){values[j]=values[j-1];--j;}values[j]=v;}
        return n?Summary{n,values[(n*80+99)/100-1],values[(n*95+99)/100-1],values[n-1]}:Summary{};
    }
};
struct DesktopTelemetry {
    uint64_t tick,readBytesPerSecond,writeBytesPerSecond,downBytesPerSecond,upBytesPerSecond;
    uint32_t cpuPercent,memoryPercent;
};
}
