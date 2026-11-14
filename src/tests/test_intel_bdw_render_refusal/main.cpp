// gen 387: a retained render run that stopped before any device write was
// reported as "Broadwell worker uncertain" and panicked. The classifier must
// call such runs untouched refusals (rerun the armed job) and keep every
// device-visible failure uncertain; the forcewake claim must not be poisoned
// by an untouched callback refusal.
#include "../../kernel/src/intel_bdw_render_refusal.h"
#include "../../kernel/src/intel_bdw_forcewake_adapter.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CHECK failed %s:%d %s\n",__FILE__,__LINE__,#x); std::abort(); } } while(0)
using namespace brook;
using C=BdwRenderAttemptClass;
static BdwRenderAttemptView woken(){BdwRenderAttemptView v{};v.requested=v.released=v.clearDelivered=true;return v;}
int main(){
    // Never woken: adapter or claim refusals.
    for(uint8_t code=BdwFwRefusalClaimBusy;code<=BdwFwRefusalClockStalled;++code){
        BdwRenderAttemptView v{};v.adapterRefusal=code;
        CHECK(BdwClassifyRenderAttempt(v)==C::RefusedUntouched);
        CHECK(std::strcmp(BdwRenderAttemptReason(v),"none")!=0);
    }
    {BdwRenderAttemptView v{};CHECK(BdwClassifyRenderAttempt(v)==C::RefusedUntouched);}
    // Woken, engines not idle/quiescent: callback not attempted, bit cleared.
    {auto v=woken();CHECK(BdwClassifyRenderAttempt(v)==C::RefusedUntouched);
     CHECK(std::strstr(BdwRenderAttemptReason(v),"quiescent"));}
    // Woken but the request bit was not cleared/drained: uncertain.
    {auto v=woken();v.released=false;CHECK(BdwClassifyRenderAttempt(v)==C::Uncertain);}
    {auto v=woken();v.clearDelivered=false;CHECK(BdwClassifyRenderAttempt(v)==C::Uncertain);}
    // Callback entry refusals (the gen 387 shape: every stage 0).
    for(uint8_t code=BdwRenderRefusalAckOrPat;code<=BdwRenderRefusalGtNotStarted;++code){
        auto v=woken();v.workAttempted=true;v.entryRefusal=code;
        CHECK(BdwClassifyRenderAttempt(v)==C::RefusedUntouched);
        // Any device-visible progress keeps it uncertain.
        auto w=v;w.gtWrites=1;CHECK(BdwClassifyRenderAttempt(w)==C::Uncertain);
        w=v;w.patReady=true;CHECK(BdwClassifyRenderAttempt(w)==C::Uncertain);
        w=v;w.executeAttempted=true;CHECK(BdwClassifyRenderAttempt(w)==C::Uncertain);
        w=v;w.resetAttempted=true;CHECK(BdwClassifyRenderAttempt(w)==C::Uncertain);
        w=v;w.pollYieldFailed=true;CHECK(BdwClassifyRenderAttempt(w)==C::Uncertain);
        w=v;w.uncertain=true;CHECK(BdwClassifyRenderAttempt(w)==C::Uncertain);
        w=v;w.released=false;CHECK(BdwClassifyRenderAttempt(w)==C::Uncertain);
    }
    // A callback that ran and did not refuse is the caller's to judge (Ran).
    {auto v=woken();v.workAttempted=true;v.gtReady=v.patReady=v.runComplete=v.contextSaved=v.resetAttempted=v.patRestored=true;
     CHECK(BdwClassifyRenderAttempt(v)==C::Ran);}
    {auto v=woken();v.workAttempted=true;v.gtReady=true;v.gtWrites=7; // GT applied, later failure: not a refusal
     CHECK(BdwClassifyRenderAttempt(v)==C::Ran);}
    // Never woken yet a transaction stage claims progress: inconsistent -> uncertain.
    {BdwRenderAttemptView v{};v.workAttempted=true;CHECK(BdwClassifyRenderAttempt(v)==C::Uncertain);}
    // Claim: an untouched callback refusal leaves the claim available; a
    // failed callback without that proof poisons it, as before.
    {BdwForcewakeClaim claim;CHECK(claim.acquire());
     BdwForcewakeAdapterResult r{};r.sequence.requested=r.sequence.released=r.clearDelivered=true;r.sequence.workAttempted=true;
     r.workRefusedUntouched=true;claim.finish(r);CHECK(claim.state()==0);CHECK(claim.acquire());
     r.workRefusedUntouched=false;claim.finish(r);CHECK(claim.state()==2);CHECK(!claim.acquire());}
    {BdwForcewakeClaim claim;CHECK(claim.acquire());BdwForcewakeAdapterResult r{};claim.finish(r);CHECK(claim.state()==0);}
    CHECK(BdwRenderRefusalRetries>=2 && BdwRenderRefusalRetries<=64);
    std::puts("BDW_RENDER_REFUSAL_PASS untouched refusals rerun; device-visible failures stay uncertain; claim not poisoned by untouched refusals");
}
