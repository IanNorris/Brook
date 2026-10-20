#include "address_space.h"
#include "exec_page_cache.h"
#include "virtual_memory.h"
#include "physical_memory.h"
#include "heap.h"
#include "process.h"
#include "scheduler.h"
#include "syscall.h"
#include "vfs.h"
#include "apic.h"
#include "compositor.h"
#include "panic.h"
#include "string.h"

namespace brook {
void ProcessDestroyNow(Process* process);
static AddressSpaceState g_kernelSpace;
static uint64_t g_identity;
static IrqSpinLock g_cleanupLock;
static IrqSpinLock g_registryLock;
static AddressSpaceState* g_publishedSpaces;
static AddressSpaceState* g_spaceHead;
static AddressSpaceState* g_spaceTail;
static Process* g_processHead;
static Process* g_processTail;
static Process* g_cleanupWorker;
static ProcessCleanupStatus g_cleanupStatus{};
ProcessCleanupStatus ProcessGetCleanupStatus() {
    const auto flags=IrqSpinLockAcquire(&g_cleanupLock);
    const auto result=g_cleanupStatus;
    IrqSpinLockRelease(&g_cleanupLock,flags);return result;
}

void AddressSpaceKernelInit() {
    memset(&g_kernelSpace, 0, sizeof(g_kernelSpace));
    g_kernelSpace.kernel = true;
    g_kernelSpace.references = 1;
    g_kernelSpace.ranges.owner = &g_kernelSpace;
    g_kernelSpace.pageTable = VmmKernelCR3();
    KMutexInit(&g_kernelSpace.mutation);
}
AddressSpaceState* AddressSpaceKernel() { return &g_kernelSpace; }
AddressSpaceState* AddressSpaceCreate(uint16_t pid) {
    auto* space = static_cast<AddressSpaceState*>(kmalloc(sizeof(AddressSpaceState)));
    if (!space) return nullptr;
    memset(space, 0, sizeof(*space));
    space->references = 1;
    space->identity = __atomic_add_fetch(&g_identity, 1, __ATOMIC_RELAXED);
    space->generation = 1;
    space->ranges.owner = space;
    if(!space->identity)KernelPanic("AS: identity exhausted");
    space->allocationPid = pid;
    KMutexInit(&space->mutation);
    space->pageTable = VmmCreateUserPageTable();
    if (!space->pageTable) { kfree(space); return nullptr; }
    const auto flags=IrqSpinLockAcquire(&g_cleanupLock);
    ++g_cleanupStatus.liveUserAddressSpaces;
    IrqSpinLockRelease(&g_cleanupLock,flags);
    return space;
}
bool AddressSpaceTryRef(AddressSpaceState* space) {
    if(!space)return false;
    if(space->kernel)return true;
    auto count=__atomic_load_n(&space->references,__ATOMIC_ACQUIRE);
    while(count) {
        if(count==~uint32_t{0})KernelPanic("AS: reference overflow");
        if(__atomic_compare_exchange_n(&space->references,&count,count+1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return true;
    }
    return false;
}
void AddressSpaceRef(AddressSpaceState* space) {
    if(space && !AddressSpaceTryRef(space))KernelPanic("AS: reference to retired space");
}
static void WakeCleanupWorker() {
    if (g_cleanupWorker) SchedulerUnblock(g_cleanupWorker);
}
void AddressSpacePublish(AddressSpaceState* space) {
    if (!space || space->kernel) return;
    const uint64_t flags=IrqSpinLockAcquire(&g_registryLock);
    if (!space->published) {
        if (!__atomic_load_n(&space->references,__ATOMIC_ACQUIRE))
            KernelPanic("AS: publishing retired space");
        space->registryNext=g_publishedSpaces;g_publishedSpaces=space;
        space->published=true;
    }
    IrqSpinLockRelease(&g_registryLock,flags);
}
uint64_t AddressSpaceIdentityCeiling() {
    return __atomic_load_n(&g_identity,__ATOMIC_ACQUIRE);
}
AddressSpaceState* AddressSpaceNextReference(uint64_t after,uint64_t ceiling) {
    const uint64_t flags=IrqSpinLockAcquire(&g_registryLock);
    for (;;) {
        AddressSpaceState* best=nullptr;
        for(auto* space=g_publishedSpaces;space;space=space->registryNext)
            if(space->identity>after && space->identity<=ceiling &&
               (!best || space->identity<best->identity)) best=space;
        if(!best) { IrqSpinLockRelease(&g_registryLock,flags);return nullptr; }
        if(AddressSpaceTryRef(best)) {
            IrqSpinLockRelease(&g_registryLock,flags);return best;
        }
        // Last-unref may await this lock before unlinking. Never resurrect it
        // or repeatedly select the same zero-reference entry.
        after=best->identity;
    }
}
void AddressSpaceUnref(AddressSpaceState* space) {
    if (!space || space->kernel) return;
    uint32_t before = __atomic_fetch_sub(&space->references, 1, __ATOMIC_ACQ_REL);
    if (!before) KernelPanic("AS: reference underflow");
    if (before != 1) return;
    uint64_t flags=IrqSpinLockAcquire(&g_registryLock);
    if(space->published) {
        auto** link=&g_publishedSpaces;
        while(*link && *link!=space)link=&(*link)->registryNext;
        if(!*link)KernelPanic("AS: published space missing from registry");
        *link=space->registryNext;space->registryNext=nullptr;space->published=false;
    }
    IrqSpinLockRelease(&g_registryLock,flags);
    flags = IrqSpinLockAcquire(&g_cleanupLock);
    space->cleanupNext = nullptr;
    if (g_spaceTail) g_spaceTail->cleanupNext = space;
    else g_spaceHead = space;
    g_spaceTail = space;
    ++g_cleanupStatus.queuedAddressSpaces;
    IrqSpinLockRelease(&g_cleanupLock, flags);
    WakeCleanupWorker();
}
void ProcessQueueDestroy(Process* process) {
    uint64_t flags = IrqSpinLockAcquire(&g_cleanupLock);
    process->destroyNext = nullptr;
    if (g_processTail) g_processTail->destroyNext = process;
    else g_processHead = process;
    g_processTail = process;
    ++g_cleanupStatus.queuedProcesses;
    IrqSpinLockRelease(&g_cleanupLock, flags);
    WakeCleanupWorker();
}
void ProcessDestroyAndWait(Process* process) {
    Process* waiter = ProcessCurrent();
    if (!waiter || !waiter->kernelRegionDepth)
        KernelPanic("AS: cleanup wait requires protected process context");
    ProcessCleanupCompletion completion{waiter, false};
    process->destroyCompletion = &completion;
    ProcessQueueDestroy(process);
    while (!__atomic_load_n(&completion.done, __ATOMIC_ACQUIRE))
        SchedulerBlock(waiter);
}
struct TeardownRanges { const MappingRangeNode* cursor; };
static bool BorrowedTeardownLeaf(VirtualAddress address, uint64_t, void* opaque) {
    auto& ranges = *static_cast<TeardownRanges*>(opaque);
    while (ranges.cursor && ranges.cursor->range.end <= address.raw()) ranges.cursor = ranges.cursor->next;
    if (ranges.cursor && ranges.cursor->range.begin <= address.raw())
        return ranges.cursor->range.leafOwnership == MappingLeafOwnership::Borrowed;
    // Unpublished constructor failure may leave ordinary managed leaves before
    // its range record exists; borrowed leaves always publish metadata first.
    return false;
}
static void DestroyAddressSpace(AddressSpaceState* space) {
    // Refcount zero: no task/fault/job can enter this page table again. No
    // sleeping mutex is required, and all filesystem finalizers run here.
    TlbShootdownFull(space->pageTable.pml4.raw(), space->tlbCpuMask);
    // Walk populated page tables, not the potentially multi-TiB virtual extents
    // of sparse reservations. Backing descriptors stay live through this walk.
    TeardownRanges ranges{space->ranges.head};
    VmmDestroyUserPageTable(space->pageTable,true,BorrowedTeardownLeaf,&ranges);
    CompositorReleaseAddressSpaceBuffers(space);
    MappingRangeDestroy(space->ranges);
    // Never sweep by allocationPid: exec and PID reuse can own newer resources.
    kfree(space);
    // No AS, page-table, range or filesystem locks remain held here.
    ExecPageCacheTrimIdle();
    const auto flags=IrqSpinLockAcquire(&g_cleanupLock);
    --g_cleanupStatus.liveUserAddressSpaces;
    ++g_cleanupStatus.completedAddressSpaces;
    IrqSpinLockRelease(&g_cleanupLock,flags);
}
bool ProcessCleanupStep() {
    uint64_t flags = IrqSpinLockAcquire(&g_cleanupLock);
    AddressSpaceState* space = g_spaceHead;
    Process* process = nullptr;
    if (space) {
        g_spaceHead = space->cleanupNext;
        if (!g_spaceHead) g_spaceTail = nullptr;
        --g_cleanupStatus.queuedAddressSpaces;
    } else if ((process = g_processHead)) {
        g_processHead = process->destroyNext;
        if (!g_processHead) g_processTail = nullptr;
        --g_cleanupStatus.queuedProcesses;
    }
    if(space || process)++g_cleanupStatus.activeFinalizers;
    IrqSpinLockRelease(&g_cleanupLock, flags);
    if (space) DestroyAddressSpace(space);
    else if (process) {
        auto* completion = process->destroyCompletion;
        ProcessDestroyNow(process);
        flags=IrqSpinLockAcquire(&g_cleanupLock);
        ++g_cleanupStatus.completedProcesses;
        IrqSpinLockRelease(&g_cleanupLock,flags);
        if (completion) {
            // Final task/window drops enqueue AS jobs. Capture one finite batch
            // before waking waitpid: a new unrelated exec cannot extend it.
            flags = IrqSpinLockAcquire(&g_cleanupLock);
            auto* batch = g_spaceHead;
            g_spaceHead = g_spaceTail = nullptr;
            g_cleanupStatus.queuedAddressSpaces = 0;
            // The outer process finalizer remains active through this batch.
            IrqSpinLockRelease(&g_cleanupLock, flags);
            while (batch) {
                auto* next = batch->cleanupNext;
                DestroyAddressSpace(batch);
                batch = next;
            }
            // The caller can observe done before the wake. Pin its Process and
            // never touch the stack-owned ticket after publishing completion.
            Process* waiter = completion->waiter;
            ProcessRef(waiter);
            __atomic_store_n(&completion->done, true, __ATOMIC_RELEASE);
            SchedulerUnblock(waiter);
            ProcessUnref(waiter);
        }
    }
    if(space || process) {
        flags=IrqSpinLockAcquire(&g_cleanupLock);
        --g_cleanupStatus.activeFinalizers;
        IrqSpinLockRelease(&g_cleanupLock,flags);
    }
    return space || process;
}
static void CleanupMain(void*) {
    Process* self = ProcessCurrent();
    for (;;) if (!ProcessCleanupStep()) SchedulerBlock(self);
}
void ProcessStartCleanupWorker() {
    if (g_cleanupWorker) return;
    g_cleanupWorker = KernelThreadCreate("task_cleanup", CleanupMain, nullptr, 2);
    if (!g_cleanupWorker) KernelPanic("AS: cannot create cleanup worker");
    SchedulerAddProcess(g_cleanupWorker);
}
}
