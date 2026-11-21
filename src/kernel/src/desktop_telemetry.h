#pragma once
#include <stdint.h>
namespace brook {
// Exact frame intervals in a ten-second ring. A slot stores completion
// offsets as bits; only the first interval can predate the slot. Later
// intervals are the differences between consecutive completion offsets.
struct DesktopFrameBins {
    static constexpr unsigned SlotMs = 100;
    static constexpr unsigned SlotCount = 100;
    static constexpr unsigned WindowMs = SlotMs * SlotCount;
    static constexpr uint32_t MinimumScaleMs = 34; // two 16.7 ms frame budgets

    struct Slot {
        uint64_t completions[2]{};
        uint32_t firstInterval = 0;
    };
    struct Summary {
        unsigned count = 0;
        uint32_t p80 = 0, p95 = 0, max = 0;
        uint32_t Scale() const { return p95 > MinimumScaleMs ? p95 : MinimumScaleMs; }
    };
    uint64_t newest = 0;
    Slot slots[SlotCount]{};

    void Record(uint64_t tick, uint32_t interval) {
        const uint64_t slot = tick / SlotMs;
        if (!interval || slot < newest) return;
        const uint64_t skipped = slot - newest;
        if (skipped >= SlotCount) {
            for (auto& entry : slots) entry = {};
        } else {
            for (uint64_t i = 1; i <= skipped; ++i) slots[(newest + i) % SlotCount] = {};
        }
        newest = slot;
        auto& entry = slots[slot % SlotCount];
        if (!entry.firstInterval) entry.firstInterval = interval;
        const unsigned offset = tick % SlotMs;
        entry.completions[offset / 64] |= uint64_t{1} << (offset % 64);
    }

    Summary Read(uint64_t tick) const { return ReadAges(tick / SlotMs, 0, SlotCount); }

    // Columns are ordered oldest to newest. Coarser views pool the actual
    // intervals of the covered slots before computing the percentile.
    Summary Column(uint64_t tick, unsigned column, unsigned columns) const {
        if (column >= columns || columns > SlotCount) return {};
        const unsigned lo = (columns - 1 - column) * SlotCount / columns;
        const unsigned hi = (columns - column) * SlotCount / columns;
        return ReadAges(tick / SlotMs, lo, hi);
    }

    // Rebase whole slots, retaining both gaps and sample offsets.
    void Capture(const DesktopFrameBins& source, uint64_t observed, uint64_t displayTick) {
        const uint64_t slot = observed / SlotMs;
        newest = displayTick / SlotMs;
        for (unsigned age = 0; age < SlotCount; ++age) {
            const auto index = (newest % SlotCount + SlotCount - age) % SlotCount;
            const auto* entry = age <= newest && age <= slot ? source.At(slot - age) : nullptr;
            slots[index] = entry ? *entry : Slot{};
        }
    }

private:
    const Slot* At(uint64_t slot) const {
        return slot <= newest && newest - slot < SlotCount ? &slots[slot % SlotCount] : nullptr;
    }

    Summary ReadAges(uint64_t slot, unsigned lo, unsigned hi) const {
        uint32_t histogram[SlotMs]{}, longIntervals[SlotCount];
        unsigned longCount = 0;
        Summary summary{};
        for (unsigned age = lo; age < hi && age <= slot; ++age) {
            const auto* entry = At(slot - age);
            if (!entry || !entry->firstInterval) continue;
            int previous = -1;
            for (unsigned word = 0; word < 2; ++word) {
                uint64_t bits = entry->completions[word];
                while (bits) {
                    const int offset = int(word * 64 + __builtin_ctzll(bits));
                    const uint32_t interval = previous < 0 ? entry->firstInterval : uint32_t(offset - previous);
                    if (interval >= SlotMs) longIntervals[longCount++] = interval;
                    else ++histogram[interval];
                    ++summary.count;
                    if (interval > summary.max) summary.max = interval;
                    previous = offset;
                    bits &= bits - 1;
                }
            }
        }
        if (!summary.count) return summary;
        unsigned rank = 0;
        const unsigned rank80 = (summary.count * 80 + 99) / 100;
        const unsigned rank95 = (summary.count * 95 + 99) / 100;
        for (unsigned interval = 1; interval < SlotMs; ++interval) {
            rank += histogram[interval];
            if (!summary.p80 && rank >= rank80) summary.p80 = interval;
            if (rank >= rank95) {
                summary.p95 = interval;
                return summary;
            }
        }
        for (unsigned i = 1; i < longCount; ++i) {
            const auto interval = longIntervals[i];
            unsigned j = i;
            while (j && longIntervals[j - 1] > interval) {
                longIntervals[j] = longIntervals[j - 1];
                --j;
            }
            longIntervals[j] = interval;
        }
        if (!summary.p80) summary.p80 = longIntervals[rank80 - rank - 1];
        summary.p95 = longIntervals[rank95 - rank - 1];
        return summary;
    }
};
static_assert(sizeof(DesktopFrameBins) <= 2416, "Bound cadence snapshot storage");

// Bounded cadence history. Times are monotonic milliseconds, not render CPU time.
// Expiry keeps inactive applications from displaying stale 'live' frame rates.
template<unsigned Capacity=128> struct DesktopIntervals {
    struct Sample { uint64_t tick; uint32_t ms; } samples[Capacity]{};
    unsigned next=0,count=0; uint64_t last=0;
    DesktopFrameBins bins;
    void Reset() { next=count=0;last=0;bins={}; }
    void Record(uint64_t now) {
        if (last && now <= last) return;
        if(last) {
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
