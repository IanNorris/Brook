#pragma once
#include <stdint.h>

namespace brook {
// Classify one retained render run (gen 387: a compositor job panicked as
// "Broadwell worker uncertain" with every transaction stage 0). Several paths
// end a run before anything is written to the device: the forcewake claim is
// busy, an entry evidence check fails, forcewake did not reach an idle and
// quiescent state (the callback is then not attempted), the callback's own
// entry check refuses, or GT init finds unstable registers and returns
// NotStarted with zero writes. Those are refusals, not uncertainty: the job is
// still Prepared and armed and may simply be run again.
//
// Codes recorded by the forcewake adapter (BdwForcewakeAdapterResult):
enum BdwForcewakeRefusal : uint8_t {
    BdwFwRefusalNone=0,BdwFwRefusalClaimBusy,BdwFwRefusalClaimUncertain,BdwFwRefusalArguments,
    BdwFwRefusalForcewakePage,BdwFwRefusalEnginePage,BdwFwRefusalPatPage,BdwFwRefusalResetPage,
    BdwFwRefusalClock,BdwFwRefusalClockStalled
};
// Codes recorded by the render callback before any device write
// (BdwRenderRunResult::entryRefusal).
enum BdwRenderEntryRefusal : uint8_t {
    BdwRenderRefusalNone=0,BdwRenderRefusalAckOrPat,BdwRenderRefusalArguments,BdwRenderRefusalPageEvidence,
    BdwRenderRefusalGtNotStarted
};
struct BdwRenderAttemptView {
    uint8_t adapterRefusal=0;            // BdwForcewakeRefusal
    bool requested=false,released=false,clearDelivered=false; // forcewake request bit lifecycle
    bool workAttempted=false;            // the render callback ran
    uint8_t entryRefusal=0;              // BdwRenderEntryRefusal, set only before any device write
    uint32_t gtWrites=0;                 // GT init register writes attempted
    bool gtReady=false,patReady=false,runComplete=false,contextSaved=false,resetAttempted=false,patRestored=false,uncertain=false;
    bool executeAttempted=false,pollYieldFailed=false;
};
enum class BdwRenderAttemptClass : uint8_t {Ran,RefusedUntouched,Uncertain};
inline BdwRenderAttemptClass BdwClassifyRenderAttempt(const BdwRenderAttemptView& v){
    const bool transactionUntouched=!v.gtReady && !v.patReady && !v.runComplete && !v.contextSaved &&
        !v.resetAttempted && !v.patRestored && !v.uncertain && !v.gtWrites && !v.executeAttempted && !v.pollYieldFailed;
    // Never woken: refused before the request bit was written.
    if(!v.requested)return (!v.workAttempted && transactionUntouched)?BdwRenderAttemptClass::RefusedUntouched:BdwRenderAttemptClass::Uncertain;
    // Woken: the request bit must have been cleared and drained.
    if(!v.released || !v.clearDelivered)return BdwRenderAttemptClass::Uncertain;
    if(!v.workAttempted)return transactionUntouched?BdwRenderAttemptClass::RefusedUntouched:BdwRenderAttemptClass::Uncertain;
    if(v.entryRefusal!=BdwRenderRefusalNone)
        return transactionUntouched?BdwRenderAttemptClass::RefusedUntouched:BdwRenderAttemptClass::Uncertain;
    return BdwRenderAttemptClass::Ran;
}
inline const char* BdwRenderAttemptReason(const BdwRenderAttemptView& v){
    switch(v.adapterRefusal){
        case BdwFwRefusalClaimBusy:return "forcewake claim busy";
        case BdwFwRefusalClaimUncertain:return "forcewake claim left uncertain by an earlier user";
        case BdwFwRefusalArguments:return "forcewake arguments or ownership";
        case BdwFwRefusalForcewakePage:return "forcewake page evidence";
        case BdwFwRefusalEnginePage:return "engine page evidence";
        case BdwFwRefusalPatPage:return "PAT page evidence";
        case BdwFwRefusalResetPage:return "reset page evidence";
        case BdwFwRefusalClock:return "TSC read";
        case BdwFwRefusalClockStalled:return "TSC not advancing";
        default:break;
    }
    if(v.requested && !v.workAttempted)return "forcewake not acquired, or engines not idle and quiescent";
    switch(v.entryRefusal){
        case BdwRenderRefusalAckOrPat:return "forcewake ACK or PAT changed before the callback";
        case BdwRenderRefusalArguments:return "render callback arguments";
        case BdwRenderRefusalPageEvidence:return "render page evidence";
        case BdwRenderRefusalGtNotStarted:return "GT init not started (registers unstable or engines busy)";
        default:break;
    }
    return "none";
}
// Bounded retry of an untouched refusal: the job stays armed; the worker
// waits a tick (outside every critical section) and runs it again.
constexpr unsigned BdwRenderRefusalRetries=16;
}
