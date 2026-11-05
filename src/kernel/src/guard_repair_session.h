#pragma once
#include <stdint.h>
namespace brook {
struct Device;struct Process;struct Vnode;struct ChildWaitSnapshot;
// A rejected completion must still permanently disable a drained adapter.
// -115 is reserved for the case where references or I/O have not drained.
inline int GuardRepairRejectedOutcome(bool quiescent) {
    return quiescent?-5:-115;
}
// A preen session followed by a distinct read-only verifier, on the
// boot-reserved data adapter.
bool GuardRepairBegin(Device*,const Process*,bool writable);
// Tracked mode is controller- and incarnation-bound. Poll consumes a scheduler
// child event; completion can be claimed once, only after all views/I/O drain.
bool GuardRepairBeginTracked(Device*,const Process*,bool writable);
int GuardRepairPollTracked(Device*,uint16_t pid,uint64_t incarnation,ChildWaitSnapshot*);
bool GuardRepairConsumeCompletion(Device*,bool writable);
Vnode* GuardRepairOpenLinux(uint64_t flags,int* error);
// Revokes access. A busy result permanently prevents the next phase.
bool GuardRepairEnd();
void GuardRepairAbort();
// After revocation, both open descriptions and admitted I/O must drain before
// changing the underlying adapter's phase.
bool GuardRepairQuiescent();
}
