#pragma once
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
    static constexpr uint32_t MAX_MEMFD_MAPS = 256;
    struct MemFdMap { uint64_t vaddr, length, offset, vmmFlags; void* mfd; uint32_t prot, maxProt; };
    MemFdMap memfdMaps[MAX_MEMFD_MAPS];
    static constexpr uint32_t MAX_FILE_MAPS = 4096;
    struct FileMap { uint64_t vaddr, length, offset, vmmFlags; Vnode* vnode; };
    FileMap fileMaps[MAX_FILE_MAPS];
    SpinLock fileMapLock;
    static constexpr uint32_t MAX_FB_MAPS = 4;
    struct FbMap { uint64_t vaddr, length; };
    FbMap fbMaps[MAX_FB_MAPS];
    CompositorVfbAllocation* fbAllocations; // legacy graphics backing owned by this AS
    AddressSpaceState* cleanupNext;
};

void AddressSpaceKernelInit();
AddressSpaceState* AddressSpaceKernel(); // Immortal; initialized before APs/tasks.
AddressSpaceState* AddressSpaceCreate(uint16_t allocationPid);
void AddressSpaceRef(AddressSpaceState* space);
// Final release queues destruction to process context; never performs FS I/O.
void AddressSpaceUnref(AddressSpaceState* space);
// Execute at most one queued finalizer, exclusively from process context.
bool ProcessCleanupStep();
void ProcessStartCleanupWorker();
void ProcessQueueDestroy(Process* process);
// Consuming wait only; caller must retain a kernel region across this wait.
void ProcessDestroyAndWait(Process* process);
}
