#pragma once
#include "recovery_guard.h"

// The caller must retain exclusive ownership of both pre-existing guard files,
// the ESP and the intended data partition for the entire transaction. IO must
// address those already-validated files, reject short transfers, and provide a
// checked durable sync and independent media readback after each sync (a
// retained FatFs FIL cache alone is insufficient). It does not acquire those capabilities,
// start a checker or write the data partition. The caller must poison its
// session after Uncertain, retain ownership, and forbid retries in that boot;
// this stateless function cannot enforce those requirements across calls.
enum class BrookGuardBeginResult { Rejected, Uncertain, PendingDurable };
enum class BrookGuardCompleteResult { Rejected, Uncertain, CleanDurable };
enum class BrookGuardAttemptResult { Rejected, Uncertain, PendingDurable, Exhausted };

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

// Before every automatic repair attempt, durably consume one of a bounded
// number of attempts. A torn or uncertain pair never authorizes a retry.
// expected receives the exact newly durable pending record only on success.
template<typename IO>
BrookGuardAttemptResult BrookGuardBeginAttempt(IO& io,const char* identity,uint32_t maximum,uint8_t* expected) {
    if(!expected || !maximum || maximum>3)return BrookGuardAttemptResult::Rejected;
    for(unsigned i=0;i<BROOK_RECOVERY_GUARD_SIZE;++i)expected[i]=0;
    uint8_t a[BROOK_RECOVERY_GUARD_SIZE],b[BROOK_RECOVERY_GUARD_SIZE];
    if(!io.Read(0,a,sizeof(a)) || !io.Read(1,b,sizeof(b)) ||
       !BrookRecoveryGuardMatchesPending(a,sizeof(a),b,sizeof(b),identity))return BrookGuardAttemptResult::Rejected;
    const uint32_t attempts=BrookUpdateGet32(a+20);
    if(attempts>=maximum)return BrookGuardAttemptResult::Exhausted;
    BrookUpdatePut32(a+20,attempts+1);BrookUpdatePut32(a+124,BrookUpdateCrc(a,124));
    for(unsigned slot=0;slot<2;++slot) {
        if(!io.Write(slot,a,sizeof(a)) || !io.Sync(slot) ||
           !io.Read(slot,b,sizeof(b)) || !BrookUpdateEqual(a,b,sizeof(a)))return BrookGuardAttemptResult::Uncertain;
    }
    if(!io.Flush() || !io.Read(0,b,sizeof(b)) || !BrookUpdateEqual(a,b,sizeof(a)) ||
       !io.Read(1,b,sizeof(b)) || !BrookUpdateEqual(a,b,sizeof(a)))return BrookGuardAttemptResult::Uncertain;
    for(unsigned i=0;i<sizeof(a);++i)expected[i]=a[i];
    return BrookGuardAttemptResult::PendingDurable;
}

// The caller must first have durably completed and independently verified the
// repair, drained every checker and permanently disabled data writes. Expected
// is the exact pending record retained from this repair attempt, not a record
// freshly trusted from disk. An uncertain outcome must never be retried.
template<typename IO>
BrookGuardCompleteResult BrookGuardCompleteRepair(IO& io,const char* identity,
                                                   const uint8_t* expected) {
    if(!expected)return BrookGuardCompleteResult::Rejected;
    uint8_t clean[BROOK_RECOVERY_GUARD_SIZE],a[BROOK_RECOVERY_GUARD_SIZE],b[BROOK_RECOVERY_GUARD_SIZE];
    for(unsigned i=0;i<sizeof(clean);++i)clean[i]=expected[i];
    if(BrookUpdateGet32(clean+16)!=BROOK_RECOVERY_GUARD_PENDING ||
       BrookUpdateGet32(clean+124)!=BrookUpdateCrc(clean,124))return BrookGuardCompleteResult::Rejected;
    BrookUpdatePut32(clean+16,BROOK_RECOVERY_GUARD_CLEAN);
    BrookUpdatePut32(clean+20,0);
    BrookUpdatePut32(clean+124,BrookUpdateCrc(clean,124));
    if(!BrookRecoveryGuardAllowsNormal(clean,sizeof(clean),clean,sizeof(clean),identity) ||
       !io.Read(0,a,sizeof(a)) || !io.Read(1,b,sizeof(b)) ||
       !BrookUpdateEqual(a,expected,sizeof(a)) || !BrookUpdateEqual(b,expected,sizeof(b)))
        return BrookGuardCompleteResult::Rejected;
    for(unsigned slot=0;slot<2;++slot) {
        if(!io.Write(slot,clean,sizeof(clean)) || !io.Sync(slot) ||
           !io.Read(slot,b,sizeof(b)) || !BrookUpdateEqual(clean,b,sizeof(b)))
            return BrookGuardCompleteResult::Uncertain;
    }
    if(!io.Flush() || !io.Read(0,b,sizeof(b)) || !BrookUpdateEqual(clean,b,sizeof(b)) ||
       !io.Read(1,b,sizeof(b)) || !BrookUpdateEqual(clean,b,sizeof(b)))
        return BrookGuardCompleteResult::Uncertain;
    return BrookGuardCompleteResult::CleanDurable;
}
