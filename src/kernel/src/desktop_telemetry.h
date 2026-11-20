#pragma once
#include <stdint.h>
namespace brook {
// Fixed-time graph history. Each slot keeps the longest completed interval
// whose end falls in that 100 ms slot. Zero is a gap with no completion.
struct DesktopFrameBins {
    static constexpr unsigned SlotMs = 100;
    static constexpr unsigned SlotCount = 100;
    static constexpr unsigned WindowMs = SlotMs * SlotCount;

    uint64_t newest = 0;
    uint32_t peaks[SlotCount]{};

    void Record(uint64_t tick, uint32_t interval) {
        const uint64_t slot = tick / SlotMs;
        if (slot < newest) return;
        const uint64_t skipped = slot - newest;
        if (skipped >= SlotCount) {
            for (auto& peak : peaks) peak = 0;
        } else {
            for (uint64_t i = 1; i <= skipped; ++i) peaks[(newest + i) % SlotCount] = 0;
        }
        newest = slot;
        auto& peak = peaks[slot % SlotCount];
        if (interval > peak) peak = interval;
    }

    uint32_t At(uint64_t slot) const {
        return slot <= newest && newest - slot < SlotCount ? peaks[slot % SlotCount] : 0;
    }

    // Columns are ordered oldest to newest. A narrower view combines
    // adjacent slots and keeps their peak so an isolated slow frame remains visible.
    uint32_t ColumnPeak(uint64_t tick, unsigned column, unsigned columns) const {
        const uint64_t slot = tick / SlotMs;
        const unsigned lo = (columns - 1 - column) * SlotCount / columns;
        const unsigned hi = (columns - column) * SlotCount / columns;
        uint32_t peak = 0;
        for (unsigned age = lo; age < hi && age <= slot; ++age) {
            const auto value = At(slot - age);
            if (value > peak) peak = value;
        }
        return peak;
    }

    // Snapshot presentation data on a fixed render clock. Only the slot ages
    // matter, so equivalent pictures have identical snapshot bytes.
    void Capture(const DesktopFrameBins& source, uint64_t observed, uint64_t displayTick) {
        const uint64_t slot = observed / SlotMs;
        newest = displayTick / SlotMs;
        for (unsigned age = 0; age < SlotCount; ++age) {
            const auto index = (newest % SlotCount + SlotCount - age) % SlotCount;
            peaks[index] = age <= newest && age <= slot ? source.At(slot - age) : 0;
        }
    }
};

// Bounded cadence history. Times are monotonic milliseconds, not render CPU time.
// Expiry keeps inactive applications from displaying stale 'live' frame rates.
template<unsigned Capacity=128> struct DesktopIntervals {
    struct Sample { uint64_t tick; uint32_t ms; } samples[Capacity]{};
    unsigned next=0,count=0; uint64_t last=0;
    DesktopFrameBins bins;
    void Reset() { next=count=0;last=0;bins={}; }
    void Record(uint64_t now) {
        if(last && now>last) {
            uint64_t elapsed=now-last;
            const auto interval = static_cast<uint32_t>(elapsed>(~uint32_t{0})?(~uint32_t{0}):elapsed);
            samples[next]={now,interval};
            bins.Record(now, interval);
            next=(next+1)%Capacity;if(count<Capacity)++count;
        }
        last=now;
    }
    struct Summary { unsigned count; uint32_t p80,p95,max; };
    Summary Read(uint64_t now,uint32_t windowMs=DesktopFrameBins::WindowMs) const {
        uint32_t values[Capacity];unsigned n=0;
        for(unsigned i=0;i<count;++i)if(now>=samples[i].tick && now-samples[i].tick<=windowMs)values[n++]=samples[i].ms;
        for(unsigned i=1;i<n;++i){uint32_t v=values[i];unsigned j=i;while(j && values[j-1]>v){values[j]=values[j-1];--j;}values[j]=v;}
        return n?Summary{n,values[(n*80+99)/100-1],values[(n*95+99)/100-1],values[n-1]}:Summary{};
    }
};
// GPU-source client frames reach the compositor as offers the renderer
// reports (window identity + commit). Count each new commit once for the
// focused window: a retained offer sampled again, or a stale report from a
// previous window, is ignored.
struct DesktopClientFrameFilter {
    uint64_t identity=0,commit=0;
    void Reset(uint64_t focused){identity=focused;commit=0;}
    bool Accept(uint64_t focused,uint64_t reportIdentity,uint64_t reportCommit){
        if(!focused || reportIdentity!=focused || !reportCommit)return false;
        if(identity!=focused){identity=focused;commit=0;}
        if(reportCommit<=commit)return false;
        commit=reportCommit;return true;
    }
};
struct DesktopTelemetry {
    uint64_t tick,readBytesPerSecond,writeBytesPerSecond,downBytesPerSecond,upBytesPerSecond;
    uint32_t cpuPercent,memoryPercent;
};
}
