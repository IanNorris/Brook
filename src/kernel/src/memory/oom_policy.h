#pragma once
#include <stdint.h>

namespace brook {
// Allocation-free policy. All callbacks/locks/I/O/signals live in the worker,
// never in this state machine or in a failing allocation caller.
struct OomCandidate {
    uint64_t incarnation=0, addressSpaceIdentity=0, exclusiveResidentPages=0;
    uint16_t pid=0;
    bool kernel=false, service=false, sharedOutsideGroup=false, exiting=false;
    bool Eligible() const {
        return pid>1 && incarnation && addressSpaceIdentity && exclusiveResidentPages &&
            !kernel && !service && !sharedOutsideGroup && !exiting;
    }
};
inline bool OomPreferCandidate(const OomCandidate& candidate,const OomCandidate& prior) {
    if(!candidate.Eligible())return false;
    if(!prior.Eligible())return true;
    if(candidate.exclusiveResidentPages!=prior.exclusiveResidentPages)
        return candidate.exclusiveResidentPages>prior.exclusiveResidentPages;
    if(candidate.incarnation!=prior.incarnation)return candidate.incarnation>prior.incarnation;
    return candidate.pid<prior.pid;
}
class OomPolicy {
public:
    enum class Action { None, Reclaim, SelectVictim, CleanupComplete, CleanupStalled };
    enum class Phase { Idle, Reclaiming, Selecting, Waiting, Stalled, Cooldown };
    static constexpr uint64_t LowPages=8*1024*1024/4096;
    static constexpr uint64_t HighPages=32*1024*1024/4096;
    static constexpr uint64_t ReclaimMs=200, CleanupMs=10000, CooldownMs=5000, NoVictimMs=30000;
    Action Tick(uint64_t now,uint64_t freePages,bool victimGone=false) {
        if(now<lastNow_)now=lastNow_; // do not underflow elapsed time on clock reversal
        lastNow_=now;
        if(phase_==Phase::Waiting || phase_==Phase::Stalled) {
            // Headroom alone cannot prove victim cleanup. No second victim is
            // selected while the first's protected operations remain live.
            if(victimGone){Enter(Phase::Cooldown,now);return Action::CleanupComplete;}
            if(phase_==Phase::Waiting && Elapsed(now,CleanupMs)) {
                Enter(Phase::Stalled,now);return Action::CleanupStalled;
            }
            return Action::None;
        }
        if(phase_==Phase::Cooldown) {
            if(!Elapsed(now,cooldown_))return Action::None;
            Enter(Phase::Idle,now);
        }
        if(phase_==Phase::Idle) {
            if(freePages>LowPages)return Action::None;
            Enter(Phase::Reclaiming,now);return Action::Reclaim;
        }
        if(freePages>=HighPages || (Elapsed(now,ReclaimMs) && freePages>LowPages)) {
            cooldown_=CooldownMs;Enter(Phase::Cooldown,now);return Action::None;
        }
        if(phase_==Phase::Reclaiming) {
            if(!Elapsed(now,ReclaimMs))return Action::Reclaim;
            Enter(Phase::Selecting,now);
        }
        return phase_==Phase::Selecting?Action::SelectVictim:Action::None;
    }
    bool CommitVictim(uint64_t now,uint64_t freePages) {
        if(phase_!=Phase::Selecting || freePages>LowPages)return false;
        cooldown_=CooldownMs;Enter(Phase::Waiting,now<lastNow_?lastNow_:now);return true;
    }
    // Only for a commit rejected before any fatal request was published.
    void SelectionRejected(uint64_t now) {
        if(phase_==Phase::Waiting)Enter(Phase::Reclaiming,now<lastNow_?lastNow_:now);
    }
    void NoEligibleVictim(uint64_t now) {
        if(phase_!=Phase::Selecting)return;
        cooldown_=NoVictimMs;Enter(Phase::Cooldown,now<lastNow_?lastNow_:now);
    }
    Phase CurrentPhase() const{return phase_;}
private:
    Phase phase_=Phase::Idle;
    uint64_t began_=0,lastNow_=0,cooldown_=CooldownMs;
    bool Elapsed(uint64_t now,uint64_t duration)const{return now>=began_ && now-began_>=duration;}
    void Enter(Phase phase,uint64_t now){phase_=phase;began_=now;}
};
}
