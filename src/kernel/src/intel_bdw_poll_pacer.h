#pragma once
#include <stdint.h>

namespace brook {
// Chooses, before each yielding GPU poll, between the existing one-tick
// sleep and a short spin slice. A spin slice still goes through
// BdwYieldGpuWait: the page-table lock is released and interrupts are
// enabled for the whole slice, then the lock is retaken and re-audited.
// Nothing here spins with interrupts disabled. All values are TSC ticks.
//
// The scheduler only offers 1 ms sleeps, so a completion is otherwise
// observed up to a full tick late. With a predicted completion time the
// pacer keeps sleeping while a whole tick (plus margin) still ends before
// the prediction, then spins in slices until completion or until the
// per-wait spin budget is spent, after which it reverts to sleeping. With
// no prediction it spins at most the (smaller) budget first. Each slice is
// at least three times the last measured relock+audit cost, bounding the
// interrupts-disabled share of a spinning wait to about a quarter.
struct BdwPollPacerConfig {uint64_t sleepQuantum,margin,minSlice,maxSlice;};
class BdwPollPacer {
    BdwPollPacerConfig config_{};
    uint64_t start_=0,expected_=0,budget_=0,spun_=0,audit_=0;
public:
    void Begin(const BdwPollPacerConfig& config,uint64_t now,uint64_t expected,uint64_t budget){
        config_=config;start_=now;expected_=expected;budget_=budget;spun_=0;audit_=0;
    }
    // Zero: sleep one scheduler tick. Otherwise the spin slice length.
    uint64_t Next(uint64_t now)const{
        if(spun_>=budget_)return 0;
        const uint64_t elapsed=now>start_?now-start_:0;
        if(expected_ && elapsed<expected_ && expected_-elapsed>config_.sleepQuantum+config_.margin)return 0;
        uint64_t slice=audit_<=~uint64_t{0}/3?audit_*3:~uint64_t{0};
        if(slice<config_.minSlice)slice=config_.minSlice;
        if(config_.maxSlice && slice>config_.maxSlice)slice=config_.maxSlice;
        const uint64_t left=budget_-spun_;
        return slice<left?slice:left;
    }
    // spent: the whole slice including yielding to other work and relocking.
    void Spun(uint64_t spent,uint64_t auditTicks){
        spun_=spent<budget_-spun_?spun_+spent:budget_;audit_=auditTicks;
    }
    uint64_t SpunTicks()const{return spun_;}
};

// Per-submitter completion-latency estimate (first completion poll to the
// poll that saw the breadcrumb). Worker-thread only. Falls fast and rises
// slowly, so the spin window opens early rather than late; an early window
// costs at most the spin budget, a late one only the old tick latency.
template<unsigned Capacity> class BdwCompletionPredictor {
    struct Entry {uint64_t key,ticks,used;};
    Entry entries_[Capacity]{};
    uint64_t clock_=0;
public:
    uint64_t Predict(uint64_t key){
        if(!key)return 0;
        for(auto& e:entries_)if(e.used && e.key==key){e.used=++clock_;return e.ticks;}
        return 0;
    }
    void Record(uint64_t key,uint64_t ticks){
        if(!key || !ticks)return;
        Entry* victim=&entries_[0];
        for(auto& e:entries_){
            if(e.used && e.key==key){
                e.ticks=ticks<e.ticks?ticks:e.ticks+(ticks-e.ticks)/4;e.used=++clock_;return;
            }
            if(!e.used || (victim->used && e.used<victim->used))victim=&e;
        }
        *victim={key,ticks,++clock_};
    }
};
}
