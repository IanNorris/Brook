#pragma once

namespace brook {
// Sequencing for an already-owned render engine. The owner retains every
// page and mapping regardless of outcome; this does not establish ownership.
// GT/PAT setup and run may report uncertain hardware effects via uncertain().
// After PAT is active, always reset the possibly submitted engine before
// restoring global PAT, including on a failed run or snapshot.
struct BdwRenderTransactionResult {
    bool gtReady=false,patReady=false,runComplete=false,contextSaved=false;
    bool resetAttempted=false,resetComplete=false,patRestored=false,uncertain=false;
};
template<class Owner> BdwRenderTransactionResult BdwRunOwnedRenderTransaction(Owner& owner) {
    BdwRenderTransactionResult r{};
    r.gtReady=owner.initializeGt();
    if(!r.gtReady){r.uncertain=owner.uncertain();return r;}
    r.patReady=owner.establishPat();
    if(!r.patReady){r.uncertain=owner.uncertain();return r;}
    r.runComplete=owner.run();
    if(r.runComplete)r.contextSaved=owner.saveContext();
    r.resetAttempted=true;r.resetComplete=owner.resetAfter();
    // A prior uncertain GT/TLB operation is not made safe merely by an
    // engine reset. Leave PAT and the pinned mappings in place for recovery.
    if(r.resetComplete && !owner.uncertain())r.patRestored=owner.restorePat();
    r.uncertain=owner.uncertain() || !r.resetComplete || !r.patRestored;
    return r;
}
}
