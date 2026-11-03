#pragma once
#include "recovery_guard.h"

// The caller must retain exclusive ownership of both pre-existing guard files,
// the ESP and the intended data partition for the entire transaction. IO must
// address those already-validated files, reject short transfers, and provide a
// checked durable sync and independent media readback after each sync (a
// retained FatFs FIL cache alone is insufficient). It does not acquire those capabilities,
// start a checker or write the data partition. The caller must poison its
// session after Uncertain, retain ownership, and forbid automatic retries;
// this stateless function cannot enforce those requirements across calls.
enum class BrookGuardBeginResult { Rejected, Uncertain, PendingDurable };

template<typename IO>
BrookGuardBeginResult BrookGuardBeginRepair(IO& io,const char* identity) {
    uint8_t a[BROOK_RECOVERY_GUARD_SIZE],b[BROOK_RECOVERY_GUARD_SIZE];
    if(!io.Read(0,a,sizeof(a)) || !io.Read(1,b,sizeof(b)) ||
       !BrookRecoveryGuardAllowsNormal(a,sizeof(a),b,sizeof(b),identity))
        return BrookGuardBeginResult::Rejected;
    const uint64_t generation=BrookUpdateGet64(a+8);
    if(generation==~uint64_t{0})return BrookGuardBeginResult::Rejected;
    BrookUpdatePut64(a+8,generation+1);
    BrookUpdatePut32(a+16,BROOK_RECOVERY_GUARD_PENDING);
    BrookUpdatePut32(a+124,BrookUpdateCrc(a,124));
    for(unsigned slot=0;slot<2;++slot) {
        // Once a write is attempted, even a reported error may have changed
        // storage. Never authorize repair after an ambiguous result.
        if(!io.Write(slot,a,sizeof(a)) || !io.Sync(slot) ||
           !io.Read(slot,b,sizeof(b)) || !BrookUpdateEqual(a,b,sizeof(a)))
            return BrookGuardBeginResult::Uncertain;
    }
    if(!io.Flush() || !io.Read(0,b,sizeof(b)) || !BrookUpdateEqual(a,b,sizeof(a)) ||
       !io.Read(1,b,sizeof(b)) || !BrookUpdateEqual(a,b,sizeof(a)))
        return BrookGuardBeginResult::Uncertain;
    return BrookGuardBeginResult::PendingDurable;
}
