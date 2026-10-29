#include <cassert>
#include <cstdio>
#include "../../kernel/src/memory/oom_policy.h"
#include "../../kernel/src/memory/oom_recovery.h"
using namespace brook;
int main() {
    constexpr auto reserve=OomUserReservePages;
    assert(!OomUserAllocationFits(reserve,1));
    assert(OomUserAllocationFits(reserve+1,1));
    assert(OomUserAllocationFits(2*reserve,reserve));
    assert(!OomUserAllocationFits(2*reserve,reserve+1));
    assert(!OomUserAllocationFits(0,1));
    using A=OomPolicy::Action;using P=OomPolicy::Phase;
    OomPolicy policy;
    assert(policy.Tick(0,OomPolicy::HighPages)==A::None);
    assert(policy.Tick(1,OomPolicy::LowPages)==A::Reclaim);
    assert(policy.Tick(100,0)==A::Reclaim);
    assert(!policy.CommitVictim(100,0)); // reclaim grace cannot be bypassed
    assert(policy.Tick(201,0)==A::SelectVictim);
    assert(!policy.CommitVictim(201,OomPolicy::HighPages)); // recheck pressure
    assert(policy.CommitVictim(201,0));
    for(uint64_t t=202;t<10201;t+=19) {
        assert(policy.Tick(t,OomPolicy::HighPages)==A::None);
        assert(!policy.CommitVictim(t,0)); // simultaneous pressure never adds another victim
    }
    assert(policy.Tick(10201,0)==A::CleanupStalled);
    for(uint64_t t=10202;t<100000;t+=73)assert(policy.Tick(t,0)==A::None);
    assert(policy.CurrentPhase()==P::Stalled);
    assert(policy.Tick(100000,0,true)==A::CleanupComplete);
    assert(policy.Tick(100001,0)==A::None);
    assert(policy.Tick(105000,0)==A::Reclaim);
    assert(policy.Tick(105200,0)==A::SelectVictim);
    policy.NoEligibleVictim(105200);
    assert(policy.Tick(135199,0)==A::None);
    assert(policy.Tick(135200,0)==A::Reclaim);
    // Clock reversal cannot prematurely finish grace or trigger an escalation.
    assert(policy.Tick(1,0)==A::Reclaim);
    assert(policy.Tick(135400,0)==A::SelectVictim);
    assert(policy.CommitVictim(135400,0));
    assert(policy.Tick(135401,1,true)==A::CleanupComplete);
    OomPolicy reclaimed;
    assert(reclaimed.Tick(5,0)==A::Reclaim);
    assert(reclaimed.Tick(6,OomPolicy::HighPages)==A::None);
    assert(reclaimed.CurrentPhase()==P::Cooldown);
    OomCandidate largest{},candidate{1,10,100,4};
    assert(OomPreferCandidate(candidate,largest));largest=candidate;
    candidate.exclusiveResidentPages=1000;candidate.service=true;
    assert(!OomPreferCandidate(candidate,largest));candidate.service=false;
    candidate.sharedOutsideGroup=true;assert(!OomPreferCandidate(candidate,largest));candidate.sharedOutsideGroup=false;
    candidate.kernel=true;assert(!OomPreferCandidate(candidate,largest));candidate.kernel=false;
    candidate.exiting=true;assert(!OomPreferCandidate(candidate,largest));candidate.exiting=false;
    candidate.pid=1;assert(!candidate.Eligible());candidate.pid=4;
    assert(OomPreferCandidate(candidate,largest));
    candidate.addressSpaceIdentity=0;assert(!candidate.Eligible());
    std::puts("OOM policy: reclaim, hysteresis, single victim, stalled cleanup and critical/shared exclusions PASS");
}
