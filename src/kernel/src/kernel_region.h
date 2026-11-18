#pragma once
#include <stdint.h>

namespace brook {
// Lock-free kernel-region enter/leave for the current process.
//
// A kernel region (depth > 0) or the operation latch (kernelOperationProtected)
// makes SchedulerMarkTerminated defer termination (terminalPending) instead of
// publishing Terminated. Enter used to take the global ready lock to make its
// "not Terminated" check atomic with that decision; it ran twice per user copy
// and was ~93% of all ready-lock traffic on the Dell.
//
// Depth changes are atomic read-modify-writes. Entry runs on the owner with
// IF clear, so its own timer tick (the BSP reaper enters a region) cannot
// interleave with the publication below. The terminator holds the
// ready lock. The two sides form a store-buffering (Dekker) pair:
//
//   owner:      depth += 1; latch = 1; fence;  read claim, state
//   terminator: claim = 1;             fence;  read depth, latch
//
// With both fences at least one side sees the other's store:
//   - owner sees claim == 0 and state != Terminated: the terminator's later
//     read sees depth > 0 and defers. Entry succeeded.
//   - owner sees claim == 1 (a terminator is deciding, or decided): it
//     resolves under the ready lock, where the decision is settled. The
//     terminator either deferred (it saw our depth) and cleared the claim, or
//     published Terminated (it read depth before our store), and we undo.
// Leave needs no lock: after depth drops to zero the latch still defers
// termination until the locked operation boundary clears it.
enum class KernelRegionEnter : uint8_t { Entered, Resolve };

struct KernelRegionUndo { uint32_t depth=0; bool latch=false; };

// Owner, IF clear. On Resolve the region is provisionally published; the
// caller must call KernelRegionResolveLocked under the ready lock.
template<class P,class Terminated>
inline KernelRegionEnter KernelRegionOwnerEnter(P* p,Terminated terminated,KernelRegionUndo* undo){
    undo->latch=__atomic_load_n(&p->kernelOperationProtected,__ATOMIC_RELAXED);
    undo->depth=__atomic_fetch_add(&p->kernelRegionDepth,1u,__ATOMIC_RELAXED);
    __atomic_store_n(&p->kernelOperationProtected,true,__ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if(!__atomic_load_n(&p->terminalClaim,__ATOMIC_RELAXED) && !terminated(p))
        return KernelRegionEnter::Entered;
    return KernelRegionEnter::Resolve;
}
// Owner, holding the ready lock after Resolve. Returns false (and withdraws
// the provisional region) when the process has been terminated.
template<class P,class Terminated>
inline bool KernelRegionResolveLocked(P* p,Terminated terminated,const KernelRegionUndo& undo){
    if(!terminated(p))return true;
    __atomic_fetch_sub(&p->kernelRegionDepth,1u,__ATOMIC_RELAXED);
    if(!undo.latch)__atomic_store_n(&p->kernelOperationProtected,false,__ATOMIC_RELAXED);
    return false;
}
// Owner. Returns false on an unbalanced leave.
template<class P>
inline bool KernelRegionOwnerLeave(P* p){
    uint32_t depth=__atomic_load_n(&p->kernelRegionDepth,__ATOMIC_RELAXED);
    do { if(!depth)return false; }
    while(!__atomic_compare_exchange_n(&p->kernelRegionDepth,&depth,depth-1,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
    return true;
}
// Terminator, holding the ready lock, for a process not yet Terminated.
// Returns true when a region or latch is live (defer: terminalPending). When
// it returns false the claim stays set and the caller publishes Terminated
// before releasing the lock.
template<class P>
inline bool KernelRegionBusyForTerminationLocked(P* p){
    __atomic_store_n(&p->terminalClaim,uint8_t(1),__ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const bool busy=__atomic_load_n(&p->kernelRegionDepth,__ATOMIC_RELAXED) ||
                    __atomic_load_n(&p->kernelOperationProtected,__ATOMIC_RELAXED);
    if(busy)__atomic_store_n(&p->terminalClaim,uint8_t(0),__ATOMIC_RELAXED);
    return busy;
}
}
