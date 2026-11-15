#pragma once
#include <stdint.h>
namespace brook {
struct BdwExecSnapshot;
// Stack-owned by an exec ioctl from before decoding until publication or
// rejected-snapshot cleanup completes. Never copy while registered.
// A reserved entry (ReservePreparation) also holds one queue slot until its
// snapshot is admitted or the entry ends. The preparer publishes snapshot
// once built so admission can consume exactly this entry's slot. waiting
// marks an unregistered entry counted as a queue-space waiter.
struct BdwSubmissionPreparation {
    uint64_t owner=0;BdwSubmissionPreparation* next=nullptr;
    BdwExecSnapshot* snapshot=nullptr;bool reserved=false,waiting=false;
};
// Queue-space waiting counters (BdwSubmissionQueue::SpaceStats). waits:
// reservations that had to wait; granted/refused/abandoned: how those waits
// ended (slot, poisoned or frozen refusal, signal or stall); signals: Space
// wakes issued. occupied/reserved/waiters are instantaneous.
struct BdwQueueSpaceStats {
    uint64_t waits=0,granted=0,refused=0,abandoned=0,signals=0;
    uint32_t occupied=0,reserved=0,waiters=0;
};
// The application worker's counters and the present owner it protects.
BdwQueueSpaceStats BdwApplicationQueueSpaceStats(uint64_t& presentOwner);
}
