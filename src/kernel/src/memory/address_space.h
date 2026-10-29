#pragma once
#include "mapping_ranges.h"
#include <stdint.h>
#include "address.h"
#include "spinlock.h"
#include "sync/kmutex.h"
#include "paging_admission.h"
#include "user_access_ranges.h"

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
    PagingAdmission pagingAdmission;
    bool pagingApplication; // boot-only executable experiment; fork inherits, exec resets
    bool pagingReclaimEnabled; // explicit single-task experiment; mutation protects both
    uint64_t pagingReclaimCursor;
    uint64_t pagingResidentTarget; // soft target in pages; zero disables
    uint64_t pagingRecentFaults[8];
    unsigned pagingRecentNext;
    UserAccessRegistry userAccess;
    volatile uint64_t tlbCpuMask;
    // Shared by CLONE_VM; copied on fork, reset by a fresh exec address space.
    volatile uint32_t membarrierPrivateRegistered;
    uint64_t programBreak;
    uint64_t mmapNext;
    CompositorVfbAllocation* fbAllocations; // legacy graphics backing owned by this AS
    AddressSpaceState* cleanupNext;
    // Weak diagnostic registry: only completed construction is published.
    AddressSpaceState* registryNext;
    bool published;
    uint64_t programBreakLow, programBreakHigh; // bounds of this image, independent of leader exec
    MappingRangeTable ranges; // authoritative mapping extents, permissions and ownership
    uint64_t argumentStart, argumentEnd; // live, contiguous exec argument strings; mutation protects reads
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
// One cleanup-lock snapshot. Includes unpublished/retained user AS lifetimes,
// not just queued work; zero queues alone never proves cleanup is drained.
struct ProcessCleanupStatus {
    uint64_t liveUserAddressSpaces,queuedProcesses,queuedAddressSpaces,activeFinalizers;
    uint64_t completedProcesses,completedAddressSpaces;
};
ProcessCleanupStatus ProcessGetCleanupStatus();
void ProcessQueueDestroy(Process* process);
// Consuming wait only; caller must retain a kernel region across this wait.
void ProcessDestroyAndWait(Process* process);
}
