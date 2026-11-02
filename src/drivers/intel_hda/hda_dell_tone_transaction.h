#pragma once
#include "hda_dell_tone_plan.h"

namespace brook {
// Bounded orchestration only. The adapter must exclusively own the controller,
// verify every operation/readback, publish DMA before RUN, and retain all DMA
// allocations and ownership whenever quarantine() is called, including PCI
// decode needed for cleanup. All operations have bounded deadlines; prepare
// and configure must never assert RUN. stop() returns true only after RUN is
// observed clear. releaseStoppedDma() must prevent any competing restart and
// retire descriptor references. restore() runs only muted and stopped: it must
// restore route/gain before restoring the original pin mute state, and capture
// and restore converter format as well as the stream assignment from the fresh
// snapshot. A previous boot's snapshot is never sufficient for execution.
enum class HdaToneResult : uint8_t {
    Complete, InvalidSnapshot, MuteFailed, ConfigureFailed, PrepareFailed,
    StartFailed, NoProgress, UnmuteFailed, PlaybackFailed, CleanupFailed
};
template<class Io> HdaToneResult HdaDellToneTransaction(Io& io,const HdaSessionReport& snapshot,bool audible) {
    HdaDellTonePlan plan{};
    if(!HdaDellMakeTonePlan(snapshot,plan))return HdaToneResult::InvalidSnapshot;
    // A failed SET can still have reached the codec: after any such failure,
    // keep output muted where possible and never assume state is restored.
    if(!io.muteAndVerify(plan)) {io.quarantine();return HdaToneResult::MuteFailed;}
    if(!io.configureMuted(plan)) {io.muteAndVerify(plan);io.quarantine();return HdaToneResult::ConfigureFailed;}
    if(!io.prepareDma(plan)) {io.muteAndVerify(plan);io.quarantine();return HdaToneResult::PrepareFailed;}
    HdaToneResult result=HdaToneResult::Complete;
    // startDma may have asserted RUN even if it reports failure.
    if(!io.startDma())result=HdaToneResult::StartFailed;
    else if(!io.checkProgress())result=HdaToneResult::NoProgress;
    else if(audible && !io.unmuteAndVerify(plan))result=HdaToneResult::UnmuteFailed;
    else if(audible && !io.playBounded())result=HdaToneResult::PlaybackFailed;
    const bool muted=io.muteAndVerify(plan);
    const bool stopped=io.stopAndVerify();
    if(!muted || !stopped) {io.quarantine();return HdaToneResult::CleanupFailed;}
    // On a failed transaction, preserve the muted, stopped state for diagnosis.
    // Quarantine prevents a caller from reusing resources after an uncertain
    // converter or codec operation, even if RUN was observed clear.
    if(result!=HdaToneResult::Complete) {io.quarantine();return result;}
    if(!io.disconnectAndVerify(plan) || !io.restoreAndVerify(plan,snapshot)) {
        io.muteAndVerify(plan);io.quarantine();return HdaToneResult::CleanupFailed;
    }
    if(!io.releaseStoppedDma()){io.muteAndVerify(plan);io.quarantine();return HdaToneResult::CleanupFailed;}
    return HdaToneResult::Complete;
}
}
