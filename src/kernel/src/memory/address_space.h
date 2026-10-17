#pragma once
#include "mapping_ranges.h"
#include <stdint.h>
#include "address.h"
#include "spinlock.h"
#include "sync/kmutex.h"

namespace brook {
struct Process;
struct Vnode;
struct ProcessCleanupCompletion { Process* waiter; volatile bool done; };
struct CompositorVfbAllocation;

// Independent of task/PID lifetime. Every sharing task owns a reference;
// faults and mutation jobs may retain additional references across blocking I/O.
struct AddressSpaceState {
    volatile uint32_t references;
    uint64_t identity;
    uint64_t generation;
    KMutex mutation;
    PageTable pageTable;
    uint16_t allocationPid;
    bool kernel;
    volatile uint64_t tlbCpuMask;
    uint64_t programBreak;
    uint64_t mmapNext;
    CompositorVfbAllocation* fbAllocations; // legacy graphics backing owned by this AS
    AddressSpaceState* cleanupNext;
    // Weak diagnostic registry: only completed construction is published.
    AddressSpaceState* registryNext;
    bool published;
    uint64_t programBreakLow, programBreakHigh; // bounds of this image, independent of leader exec
    MappingRangeTable ranges; // authoritative mapping extents, permissions and ownership
};

void AddressSpaceKernelInit();
AddressSpaceState* AddressSpaceKernel(); // Immortal; initialized before APs/tasks.
AddressSpaceState* AddressSpaceCreate(uint16_t allocationPid);
void AddressSpacePublish(AddressSpaceState* space);
uint64_t AddressSpaceIdentityCeiling();
// Owned reference to the lowest published identity in (after,ceiling].
// Registry protects acquisition against last-unref/queued teardown.
AddressSpaceState* AddressSpaceNextReference(uint64_t after, uint64_t ceiling);
void AddressSpaceRef(AddressSpaceState* space);
// Raw pointer lifetime must be protected (e.g. file registry lock). Never resurrects zero.
bool AddressSpaceTryRef(AddressSpaceState* space);
// Final release queues destruction to process context; never performs FS I/O.
void AddressSpaceUnref(AddressSpaceState* space);
// Execute at most one queued finalizer, exclusively from process context.
bool ProcessCleanupStep();
void ProcessStartCleanupWorker();
void ProcessQueueDestroy(Process* process);
// Consuming wait only; caller must retain a kernel region across this wait.
void ProcessDestroyAndWait(Process* process);
}
