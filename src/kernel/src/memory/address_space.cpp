#include "address_space.h"
#include "virtual_memory.h"
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
static AddressSpaceState* g_spaceHead;
static AddressSpaceState* g_spaceTail;
static Process* g_processHead;
static Process* g_processTail;
static Process* g_cleanupWorker;

void AddressSpaceKernelInit() {
    memset(&g_kernelSpace, 0, sizeof(g_kernelSpace));
    g_kernelSpace.kernel = true;
    g_kernelSpace.references = 1;
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
    space->allocationPid = pid;
    KMutexInit(&space->mutation);
    space->pageTable = VmmCreateUserPageTable();
    if (!space->pageTable) { kfree(space); return nullptr; }
    return space;
}
void AddressSpaceRef(AddressSpaceState* space) {
    if (!space || space->kernel) return;
    if (!__atomic_fetch_add(&space->references, 1, __ATOMIC_ACQ_REL))
        KernelPanic("AS: reference to retired space");
}
static void WakeCleanupWorker() {
    if (g_cleanupWorker) SchedulerUnblock(g_cleanupWorker);
}
void AddressSpaceUnref(AddressSpaceState* space) {
    if (!space || space->kernel) return;
    uint32_t before = __atomic_fetch_sub(&space->references, 1, __ATOMIC_ACQ_REL);
    if (!before) KernelPanic("AS: reference underflow");
    if (before != 1) return;
    uint64_t flags = IrqSpinLockAcquire(&g_cleanupLock);
    space->cleanupNext = nullptr;
    if (g_spaceTail) g_spaceTail->cleanupNext = space;
    else g_spaceHead = space;
    g_spaceTail = space;
    IrqSpinLockRelease(&g_cleanupLock, flags);
    WakeCleanupWorker();
}
void ProcessQueueDestroy(Process* process) {
    uint64_t flags = IrqSpinLockAcquire(&g_cleanupLock);
    process->destroyNext = nullptr;
    if (g_processTail) g_processTail->destroyNext = process;
    else g_processHead = process;
    g_processTail = process;
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
static void DestroyAddressSpace(AddressSpaceState* space) {
    // Refcount zero: no task/fault/job can enter this page table again. No
    // sleeping mutex is required, and all filesystem finalizers run here.
    TlbShootdownFull(space->pageTable.pml4.raw(), space->tlbCpuMask);
    // Borrowed leaves must be detached before the managed-leaf teardown walk.
    for (const auto& map : space->memfdMaps)
        for (uint64_t offset = 0; offset < map.length; offset += 4096)
            VmmUnmapPage(space->pageTable, VirtualAddress(map.vaddr + offset));
    for (const auto& map : space->fbMaps)
        for (uint64_t offset = 0; offset < map.length; offset += 4096)
            VmmUnmapPage(space->pageTable, VirtualAddress(map.vaddr + offset));
    VmmDestroyUserPageTable(space->pageTable, true);
    CompositorReleaseAddressSpaceBuffers(space);
    for (const auto& map : space->memfdMaps)
        if (map.length && map.mfd) MemFdHandleUnref(map.mfd);
    for (const auto& map : space->fileMaps) {
        if (!map.length || !map.vnode) continue;
        VfsFileMapUnpin(map.vnode);
        if (__atomic_fetch_sub(&map.vnode->refCount, 1, __ATOMIC_ACQ_REL) == 1)
            VfsClose(map.vnode);
    }
    // Never sweep by allocationPid: exec and PID reuse can own newer resources.
    kfree(space);
}
bool ProcessCleanupStep() {
    uint64_t flags = IrqSpinLockAcquire(&g_cleanupLock);
    AddressSpaceState* space = g_spaceHead;
    Process* process = nullptr;
    if (space) {
        g_spaceHead = space->cleanupNext;
        if (!g_spaceHead) g_spaceTail = nullptr;
    } else if ((process = g_processHead)) {
        g_processHead = process->destroyNext;
        if (!g_processHead) g_processTail = nullptr;
    }
    IrqSpinLockRelease(&g_cleanupLock, flags);
    if (space) DestroyAddressSpace(space);
    else if (process) {
        auto* completion = process->destroyCompletion;
        ProcessDestroyNow(process);
        if (completion) {
            // Final task/window drops enqueue AS jobs. Capture one finite batch
            // before waking waitpid: a new unrelated exec cannot extend it.
            flags = IrqSpinLockAcquire(&g_cleanupLock);
            auto* batch = g_spaceHead;
            g_spaceHead = g_spaceTail = nullptr;
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
