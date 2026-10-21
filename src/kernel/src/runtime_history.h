#pragma once
#include <stdint.h>

namespace brook {
enum class RuntimeHistoryKind : uint8_t {
    Baseline, Charged, Idle, Invalid, Backwards, Clamp, StaleOwner, StaleGroup, Owner
};
inline const char* RuntimeHistoryKindName(RuntimeHistoryKind kind) {
    switch(kind) {
    case RuntimeHistoryKind::Baseline:return "baseline";
    case RuntimeHistoryKind::Charged:return "charged";
    case RuntimeHistoryKind::Idle:return "idle";
    case RuntimeHistoryKind::Invalid:return "invalid";
    case RuntimeHistoryKind::Backwards:return "backwards";
    case RuntimeHistoryKind::Clamp:return "clamp";
    case RuntimeHistoryKind::StaleOwner:return "stale_owner";
    case RuntimeHistoryKind::StaleGroup:return "stale_group";
    case RuntimeHistoryKind::Owner:return "owner";
    }
    return "unknown";
}
// Plain fields allow the disabled ledger path to avoid constructing a record.
// Owner events reuse the last accepted raw pair; they do NOT read a new clock.
struct RuntimeHistoryEntry {
    uint64_t sequence, sample, wallNs, stealNs, floorBeforeNs, floorAfterNs;
    uint64_t incarnationBefore, incarnationAfter, chargedNs, idleNs;
    uint16_t pidBefore, pidAfter;
    RuntimeHistoryKind kind;
    uint8_t attempts;
    bool valid, baselineBefore, baselineAfter;
};
constexpr unsigned RuntimeHistoryCapacity = 64;
struct RuntimeHistoryStatus {
    uint64_t epoch=0, sequence=0, triggerSequence=0, triggerSample=0, triggerFloorNs=0;
    uint64_t prefixOverwritten=0, omittedAfterFreeze=0, catchupSequence=0;
    unsigned cpus=0, cpu=0, triggerCpu=0, count=0, postEvents=0, capacity=0;
    bool enabled=false, triggered=false, frozen=false, allFrozen=false;
    bool caughtUp=false, baselineLost=false, overflow=false;
};
struct RuntimeHistorySnapshot : RuntimeHistoryStatus {
    RuntimeHistoryEntry entries[RuntimeHistoryCapacity]{};
};

// All access is serialized by the ledger caller. Fixed storage, no Process
// pointers, allocation, clocks or policy calls. A first clamp globally triggers
// bounded per-CPU tails, preserving migration context with one event order.
template<unsigned Cpus, unsigned Capacity=RuntimeHistoryCapacity, unsigned Tail=16>
class RuntimeHistory {
    static_assert(Cpus>0 && Capacity>0 && Capacity<=RuntimeHistoryCapacity);
    static_assert(Tail>0 && Tail<Capacity);
    struct Cpu {
        RuntimeHistoryEntry entries[Capacity]{};
        uint64_t overwritten=0, omitted=0;
        unsigned next=0, count=0, post=0;
        bool frozen=false;
    } cpus_[Cpus]{};
    uint64_t epoch_=0, sequence_=0, triggerSequence_=0, triggerSample_=0, triggerFloor_=0, catchupSequence_=0;
    unsigned active_=0, triggerCpu_=0, frozen_=0;
    bool enabled_=false, triggered_=false, caughtUp_=false, baselineLost_=false, overflow_=false;
    static void Increment(uint64_t& n) { if(n!=~uint64_t(0))++n; }
public:
    bool Enabled() const { return enabled_; }
    bool Recording() const { return enabled_ && !overflow_ && frozen_<active_; }
    bool Reset(unsigned cpus) {
        if(!cpus || cpus>Cpus || epoch_==~uint64_t(0))return false;
        ++epoch_;active_=cpus;sequence_=triggerSequence_=triggerSample_=triggerFloor_=catchupSequence_=0;
        triggerCpu_=frozen_=0;triggered_=caughtUp_=baselineLost_=overflow_=false;
        // Old entries stay inaccessible: reset only indices/metadata, not a
        // large array under the scheduler lock.
        for(auto& c:cpus_){c.overwritten=c.omitted=0;c.next=c.count=c.post=0;c.frozen=false;}
        return true;
    }
    bool Start(unsigned cpus) {
        if(enabled_)return active_==cpus; // repeated on cannot erase evidence
        if(!Reset(cpus))return false;
        enabled_=true;return true;
    }
    void Stop() { enabled_=false; }
    void Record(unsigned cpu, RuntimeHistoryEntry entry) {
        if(!Recording() || cpu>=active_)return;
        auto& c=cpus_[cpu];
        if(c.frozen){Increment(c.omitted);return;}
        if(sequence_==~uint64_t(0)){overflow_=true;return;}
        entry.sequence=++sequence_;
        c.entries[c.next]=entry;c.next=(c.next+1)%Capacity;
        if(c.count<Capacity)++c.count;else Increment(c.overwritten);
        if(!triggered_) {
            if(entry.kind==RuntimeHistoryKind::Clamp){
                triggered_=true;triggerCpu_=cpu;triggerSequence_=entry.sequence;
                triggerSample_=entry.sample;triggerFloor_=entry.floorBeforeNs;
            }
            return;
        }
        ++c.post;
        if(cpu==triggerCpu_) {
            if(entry.kind==RuntimeHistoryKind::Invalid || entry.kind==RuntimeHistoryKind::Backwards)
                baselineLost_=true;
            if(!baselineLost_ && !caughtUp_ && entry.kind!=RuntimeHistoryKind::Owner &&
               entry.valid && entry.baselineAfter && entry.wallNs>=entry.stealNs &&
               entry.wallNs-entry.stealNs>=triggerFloor_){
                caughtUp_=true;catchupSequence_=entry.sequence;
            }
        }
        if(c.post==Tail){c.frozen=true;++frozen_;}
    }
    bool Read(unsigned cpu, RuntimeHistorySnapshot* out, bool entries=true) const {
        if(!out || cpu>=Cpus || (active_ && cpu>=active_))return false;
        static_cast<RuntimeHistoryStatus&>(*out)={};const auto& c=cpus_[cpu];
        out->epoch=epoch_;out->sequence=sequence_;out->triggerSequence=triggerSequence_;
        out->triggerSample=triggerSample_;out->triggerFloorNs=triggerFloor_;
        out->prefixOverwritten=c.overwritten;out->omittedAfterFreeze=c.omitted;
        out->catchupSequence=catchupSequence_;out->cpus=active_;out->cpu=cpu;out->triggerCpu=triggerCpu_;
        out->count=c.count;out->postEvents=c.post;out->capacity=Capacity;
        out->enabled=enabled_;out->triggered=triggered_;out->frozen=c.frozen;
        out->allFrozen=active_ && frozen_==active_;out->caughtUp=caughtUp_;
        out->baselineLost=baselineLost_;out->overflow=overflow_;
        const unsigned first=(c.next+Capacity-c.count)%Capacity;
        if(entries)for(unsigned i=0;i<c.count;++i)out->entries[i]=c.entries[(first+i)%Capacity];
        return true;
    }
};
} // namespace brook
