#pragma once

#include <stdint.h>

namespace brook {

enum class BdwClientRetirement : uint8_t {
    Held, Replacement, Restore, Rejected,
};

struct BdwClientTimelineEntry {
    uint64_t sequence=0, owner=0, offer=0, buffer=0;
    uint64_t admittedNs=0, flushStartNs=0, flushEndNs=0, armNs=0, latchNs=0, retiredNs=0;
    uint64_t retiredBy=0;
    bool publishOnAp=false,latchOffWorker=false;
    BdwClientRetirement retirement=BdwClientRetirement::Held;
};

// Callers serialize all updates and snapshots with the display lock. Updates
// from an expired mapping must never be attributed to a recycled ring slot.
class BdwClientTimeline {
public:
    static constexpr uint32_t Capacity=64;

    uint64_t Admit(uint64_t owner,uint64_t offer,uint64_t buffer,bool publishOnAp,uint64_t now){
        if(sequence_==~uint64_t{0})return 0;
        const uint64_t sequence=++sequence_;
        auto& entry=entries_[sequence%Capacity];
        entry={};
        entry.sequence=sequence;
        entry.owner=owner;
        entry.offer=offer;
        entry.buffer=buffer;
        entry.publishOnAp=publishOnAp;
        entry.admittedNs=now;
        return sequence;
    }
    void Flushed(uint64_t sequence,uint64_t start,uint64_t end){
        if(auto* entry=Find(sequence)){entry->flushStartNs=start;entry->flushEndNs=end;}
    }
    void Armed(uint64_t sequence,uint64_t now){
        if(auto* entry=Find(sequence))entry->armNs=now;
    }
    void Latched(uint64_t sequence,uint64_t now,bool offWorker=false){
        if(auto* entry=Find(sequence)){entry->latchNs=now;entry->latchOffWorker=offWorker;}
    }
    void Retired(uint64_t sequence,uint64_t now,BdwClientRetirement reason,uint64_t successor=0){
        if(auto* entry=Find(sequence)){
            entry->retiredNs=now;
            entry->retiredBy=successor;
            entry->retirement=reason;
        }
    }
    uint32_t Read(BdwClientTimelineEntry* out,uint32_t max) const {
        if(!out)return 0;
        uint32_t count=sequence_<Capacity?uint32_t(sequence_):Capacity;
        if(count>max)count=max;
        const uint64_t first=sequence_-count+1;
        for(uint32_t i=0;i<count;++i)out[i]=entries_[(first+i)%Capacity];
        return count;
    }

private:
    BdwClientTimelineEntry* Find(uint64_t sequence){
        if(!sequence)return nullptr;
        auto& entry=entries_[sequence%Capacity];
        return entry.sequence==sequence?&entry:nullptr;
    }

    BdwClientTimelineEntry entries_[Capacity]{};
    uint64_t sequence_=0;
};

} // namespace brook
