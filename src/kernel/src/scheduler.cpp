#include "scheduler.h"
#include "process.h"
#include "cpu.h"
#include "smp.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "memory/address.h"
#include "gdt.h"
#include "serial.h"

#include <stdint.h>

// LAPIC tick counter (defined in apic.cpp, volatile because ISR-modified).
namespace brook { extern volatile uint64_t g_lapicTickCount; }

// Context switch — implemented in context_switch.S
extern "C" void context_switch(brook::SavedContext* oldCtx, brook::SavedContext* newCtx,
                                brook::FxsaveArea* oldFx, brook::FxsaveArea* newFx);

// Enter user mode for the first time (existing function in syscall.cpp).
namespace brook { void SwitchToUserMode(uint64_t userRsp, uint64_t userRip); }

namespace brook {

// ---------------------------------------------------------------------------
// Spinlock — simple ticket lock for SMP safety
// ---------------------------------------------------------------------------

struct Spinlock {
    volatile uint32_t next   = 0;
    volatile uint32_t serving = 0;
};

static inline void SpinlockAcquire(Spinlock& lock)
{
    uint32_t ticket = __atomic_fetch_add(&lock.next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock.serving, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");
}

static inline void SpinlockRelease(Spinlock& lock)
{
    __atomic_fetch_add(&lock.serving, 1, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// Per-CPU state
// ---------------------------------------------------------------------------

static constexpr uint32_t SCHED_MAX_CPUS = 64;

struct PerCpuSchedState {
    Process*         currentProcess;
    Process*         idleProcess;
    uint64_t         sliceStartTick;
    KernelCpuEnv*    cpuEnv;
};

static PerCpuSchedState g_perCpu[SCHED_MAX_CPUS] = {};

// Helpers
static inline uint32_t ThisCpu() { return SmpCurrentCpuIndex(); }

// Update the per-CPU syscall stack pointer.
static inline void SetSyscallStack(uint32_t cpuIdx, uint64_t stackTop)
{
    if (g_perCpu[cpuIdx].cpuEnv)
        g_perCpu[cpuIdx].cpuEnv->syscallStack = stackTop;
}

// ---------------------------------------------------------------------------
// Scheduler state
// ---------------------------------------------------------------------------

// Circular doubly-linked ready queue (global, shared by all CPUs).
static Process* g_readyHead  = nullptr;
static Spinlock g_readyLock;

// All processes (for blocked-process scanning).
static Process* g_allProcesses[MAX_PROCESSES] = {};
static uint32_t g_processCount = 0;
static Spinlock g_allProcLock;

// Next PID to allocate.
static uint16_t g_nextPid = 1;

// Guard: timer ticks are ignored until SchedulerStart sets this.
static volatile bool g_schedulerRunning = false;

// ---------------------------------------------------------------------------
// Ready queue operations (circular doubly-linked list)
// Caller must hold g_readyLock.
// ---------------------------------------------------------------------------

static void ReadyQueueInsertLocked(Process* proc)
{
    if (!g_readyHead)
    {
        proc->schedNext = proc;
        proc->schedPrev = proc;
        g_readyHead = proc;
    }
    else
    {
        Process* tail = g_readyHead->schedPrev;
        proc->schedNext = g_readyHead;
        proc->schedPrev = tail;
        tail->schedNext = proc;
        g_readyHead->schedPrev = proc;
    }
}

static void ReadyQueueRemoveLocked(Process* proc)
{
    if (proc->schedNext == proc)
    {
        g_readyHead = nullptr;
    }
    else
    {
        proc->schedPrev->schedNext = proc->schedNext;
        proc->schedNext->schedPrev = proc->schedPrev;
        if (g_readyHead == proc)
            g_readyHead = proc->schedNext;
    }
    proc->schedNext = nullptr;
    proc->schedPrev = nullptr;
}

// ---------------------------------------------------------------------------
// Idle process — halts until next interrupt (one per CPU)
// ---------------------------------------------------------------------------

static uint8_t g_idleStacks[SCHED_MAX_CPUS][4096] __attribute__((aligned(16)));

static void IdleLoop()
{
    for (;;)
        __asm__ volatile("sti\n\thlt" ::: "memory");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SchedulerInit()
{
    // Create idle process for BSP (CPU 0).
    auto* idle = static_cast<Process*>(kmalloc(sizeof(Process)));
    __builtin_memset(idle, 0, sizeof(Process));

    idle->pid = 0;
    idle->state = ProcessState::Ready;
    __builtin_memcpy(idle->name, "idle0", 6);

    idle->kernelStackBase = reinterpret_cast<uint64_t>(g_idleStacks[0]);
    idle->kernelStackTop  = reinterpret_cast<uint64_t>(g_idleStacks[0]) + sizeof(g_idleStacks[0]);
    idle->savedCtx.rsp = idle->kernelStackTop - 8;
    idle->savedCtx.rip = reinterpret_cast<uint64_t>(&IdleLoop);
    idle->savedCtx.rflags = 0x202;
    idle->savedCtx.cr3 = VmmKernelCR3().pml4.raw();
    idle->pageTable = VmmKernelCR3();

    g_perCpu[0].idleProcess = idle;
    g_perCpu[0].currentProcess = nullptr;

    SerialPuts("SCHED: scheduler initialised\n");
}

// Trampoline for processes that haven't run yet.
static void ProcessTrampoline()
{
    uint32_t cpu = ThisCpu();
    Process* proc = g_perCpu[cpu].currentProcess;
    SerialPrintf("SCHED: CPU%u entering user mode for '%s' (pid %u)\n",
                 cpu, proc->name, proc->pid);

    __asm__ volatile("sti");

    SwitchToUserMode(proc->stackTop, proc->elf.entryPoint);
    __builtin_unreachable();
}

void SchedulerAddProcess(Process* proc)
{
    proc->state = ProcessState::Ready;

    proc->savedCtx.rsp = proc->kernelStackTop - 8;
    proc->savedCtx.rip = reinterpret_cast<uint64_t>(&ProcessTrampoline);
    proc->savedCtx.rflags = 0x202;
    proc->savedCtx.cr3 = proc->pageTable.pml4.raw();
    proc->savedCtx.fsBase = proc->fsBase;

    SpinlockAcquire(g_readyLock);
    ReadyQueueInsertLocked(proc);
    SpinlockRelease(g_readyLock);

    SpinlockAcquire(g_allProcLock);
    if (g_processCount < MAX_PROCESSES)
        g_allProcesses[g_processCount++] = proc;
    SpinlockRelease(g_allProcLock);

    SerialPrintf("SCHED: added '%s' (pid %u) to ready queue\n",
                 proc->name, proc->pid);
}

void SchedulerRemoveProcess(Process* proc)
{
    SpinlockAcquire(g_readyLock);
    if (proc->state == ProcessState::Ready && proc->schedNext)
        ReadyQueueRemoveLocked(proc);
    SpinlockRelease(g_readyLock);

    SpinlockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        if (g_allProcesses[i] == proc)
        {
            g_allProcesses[i] = g_allProcesses[--g_processCount];
            break;
        }
    }
    SpinlockRelease(g_allProcLock);
}

void SchedulerBlock(Process* proc)
{
    SpinlockAcquire(g_readyLock);
    proc->state = ProcessState::Blocked;
    if (proc->schedNext)
        ReadyQueueRemoveLocked(proc);
    SpinlockRelease(g_readyLock);

    uint32_t cpu = ThisCpu();
    if (proc == g_perCpu[cpu].currentProcess)
        SchedulerYield();
}

void SchedulerUnblock(Process* proc)
{
    SpinlockAcquire(g_readyLock);
    if (proc->state != ProcessState::Blocked)
    {
        SpinlockRelease(g_readyLock);
        return;
    }
    proc->state = ProcessState::Ready;
    proc->wakeupTick = 0;
    ReadyQueueInsertLocked(proc);
    SpinlockRelease(g_readyLock);
}

uint32_t SchedulerReadyCount()
{
    SpinlockAcquire(g_readyLock);
    if (!g_readyHead) { SpinlockRelease(g_readyLock); return 0; }
    uint32_t count = 1;
    for (Process* p = g_readyHead->schedNext; p != g_readyHead; p = p->schedNext)
        ++count;
    SpinlockRelease(g_readyLock);
    return count;
}

// ---------------------------------------------------------------------------
// Context switch logic
// ---------------------------------------------------------------------------

// Pick the next process to run. Caller must hold g_readyLock.
static Process* PickNextLocked()
{
    if (g_readyHead)
    {
        Process* next = g_readyHead;
        ReadyQueueRemoveLocked(next);
        return next;
    }
    return nullptr; // caller should fall back to idle
}

// Check blocked processes for timed wakeups (called with NO locks held).
static void CheckBlockedWakeups()
{
    uint64_t now = g_lapicTickCount;
    // Snapshot processes to unblock without holding lock during unblock.
    Process* toUnblock[MAX_PROCESSES];
    uint32_t unblockCount = 0;

    SpinlockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (p->state == ProcessState::Blocked && p->wakeupTick != 0 && now >= p->wakeupTick)
        {
            if (unblockCount < MAX_PROCESSES)
                toUnblock[unblockCount++] = p;
        }
    }
    SpinlockRelease(g_allProcLock);

    for (uint32_t i = 0; i < unblockCount; ++i)
        SchedulerUnblock(toUnblock[i]);
}

// Reap terminated processes.
static void ReapTerminated()
{
    uint32_t cpu = ThisCpu();
    SpinlockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; )
    {
        Process* p = g_allProcesses[i];
        if (p->state == ProcessState::Terminated && p != g_perCpu[cpu].currentProcess)
        {
            SpinlockRelease(g_allProcLock);
            SerialPrintf("SCHED: reaping '%s' (pid %u)\n", p->name, p->pid);
            ProcessDestroy(p);
            // ProcessDestroy calls SchedulerRemoveProcess
            SpinlockAcquire(g_allProcLock);
            // Restart scan — list was modified.
            i = 0;
        }
        else
        {
            ++i;
        }
    }
    SpinlockRelease(g_allProcLock);
}

// Perform a context switch from `oldProc` to `newProc`.
static void DoSwitch(Process* oldProc, Process* newProc)
{
    __asm__ volatile("cli");

    uint32_t cpu = ThisCpu();
    g_perCpu[cpu].currentProcess = newProc;
    newProc->state = ProcessState::Running;
    g_perCpu[cpu].sliceStartTick = g_lapicTickCount;

    GdtSetTssRsp0ForCpu(cpu, newProc->kernelStackTop);
    SetSyscallStack(cpu, newProc->kernelStackTop);

    // Validate FxsaveArea alignment.
    auto oldFxAddr = reinterpret_cast<uintptr_t>(&oldProc->fxsave);
    auto newFxAddr = reinterpret_cast<uintptr_t>(&newProc->fxsave);
    if ((oldFxAddr & 0xF) || (newFxAddr & 0xF))
    {
        SerialPrintf("SCHED FATAL: FxsaveArea misaligned! old=%p new=%p\n",
                     (void*)oldFxAddr, (void*)newFxAddr);
        for (;;) __asm__ volatile("hlt");
    }

    context_switch(&oldProc->savedCtx, &newProc->savedCtx,
                   &oldProc->fxsave, &newProc->fxsave);
}

void SchedulerTimerTick()
{
    if (!g_schedulerRunning)
        return;

    // Only BSP (CPU 0) does wakeup checks and reaping to avoid contention.
    uint32_t cpu = ThisCpu();
    if (cpu == 0)
    {
        CheckBlockedWakeups();
        ReapTerminated();
    }

    Process* cur = g_perCpu[cpu].currentProcess;
    if (!cur)
        return;

    // Idle — if something became ready, switch to it.
    if (cur == g_perCpu[cpu].idleProcess)
    {
        SpinlockAcquire(g_readyLock);
        Process* next = PickNextLocked();
        SpinlockRelease(g_readyLock);
        if (next)
            DoSwitch(cur, next);
        return;
    }

    // Check timeslice.
    if (g_lapicTickCount - g_perCpu[cpu].sliceStartTick < SCHED_TIMESLICE_MS)
        return;

    // Timeslice expired — put current back in queue and switch.
    SpinlockAcquire(g_readyLock);
    if (cur->state == ProcessState::Running)
    {
        cur->state = ProcessState::Ready;
        ReadyQueueInsertLocked(cur);
    }

    Process* next = PickNextLocked();
    SpinlockRelease(g_readyLock);

    if (!next || next == cur)
    {
        // Nothing else — keep running.
        cur->state = ProcessState::Running;
        g_perCpu[cpu].sliceStartTick = g_lapicTickCount;
        return;
    }

    DoSwitch(cur, next);
}

void SchedulerYield()
{
    uint32_t cpu = ThisCpu();
    Process* old = g_perCpu[cpu].currentProcess;
    if (!old)
        return;

    SpinlockAcquire(g_readyLock);
    if (old->state == ProcessState::Running)
    {
        old->state = ProcessState::Ready;
        ReadyQueueInsertLocked(old);
    }

    Process* next = PickNextLocked();
    SpinlockRelease(g_readyLock);

    if (!next || next == old)
    {
        // If the process is Blocked/Terminated, it must NOT continue running.
        // Switch to the idle process so the CPU is available for other work
        // and the blocked process can be properly rescheduled when unblocked.
        if (old->state == ProcessState::Blocked ||
            old->state == ProcessState::Terminated)
        {
            next = g_perCpu[cpu].idleProcess;
            DoSwitch(old, next);
            return;
        }
        if (old->state == ProcessState::Ready)
            old->state = ProcessState::Running;
        return;
    }

    DoSwitch(old, next);
}

[[noreturn]] void SchedulerExitCurrentProcess(int status)
{
    uint32_t cpu = ThisCpu();
    Process* proc = g_perCpu[cpu].currentProcess;
    SerialPrintf("SCHED: '%s' (pid %u) exited with status %d\n",
                 proc->name, proc->pid, status);

    proc->state = ProcessState::Terminated;

    SpinlockAcquire(g_readyLock);
    if (proc->schedNext)
        ReadyQueueRemoveLocked(proc);
    Process* next = PickNextLocked();
    SpinlockRelease(g_readyLock);

    if (!next) next = g_perCpu[cpu].idleProcess;

    g_perCpu[cpu].currentProcess = next;
    next->state = ProcessState::Running;
    g_perCpu[cpu].sliceStartTick = g_lapicTickCount;
    GdtSetTssRsp0ForCpu(cpu, next->kernelStackTop);
    SetSyscallStack(cpu, next->kernelStackTop);

    context_switch(&proc->savedCtx, &next->savedCtx,
                   &proc->fxsave, &next->fxsave);

    __builtin_unreachable();
}

[[noreturn]] void SchedulerStart()
{
    SerialPrintf("SCHED: starting scheduler, %u processes ready\n",
                 SchedulerReadyCount());

    uint32_t cpu = ThisCpu();

    SpinlockAcquire(g_readyLock);
    Process* first = PickNextLocked();
    SpinlockRelease(g_readyLock);

    if (!first) first = g_perCpu[cpu].idleProcess;

    g_perCpu[cpu].currentProcess = first;
    first->state = ProcessState::Running;
    g_perCpu[cpu].sliceStartTick = g_lapicTickCount;
    GdtSetTssRsp0ForCpu(cpu, first->kernelStackTop);
    SetSyscallStack(cpu, first->kernelStackTop);

    VmmSwitchPageTable(first->pageTable);

    if (first->fsBase)
    {
        uint32_t lo = static_cast<uint32_t>(first->fsBase);
        uint32_t hi = static_cast<uint32_t>(first->fsBase >> 32);
        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000100U));
    }

    SerialPrintf("SCHED: CPU%u entering user mode for '%s' (pid %u)\n",
                 cpu, first->name, first->pid);

    g_schedulerRunning = true;
    SwitchToUserMode(first->stackTop, first->elf.entryPoint);

    __builtin_unreachable();
}

// AP entry into the scheduler — called from SmpActivateAPs via the AP wake path.
[[noreturn]] void SchedulerStartAp()
{
    uint32_t cpu = ThisCpu();

    // Wait for BSP to set g_schedulerRunning.
    while (!__atomic_load_n(&g_schedulerRunning, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");

    // Try to pick a process from the global queue.
    SpinlockAcquire(g_readyLock);
    Process* first = PickNextLocked();
    SpinlockRelease(g_readyLock);

    if (!first) first = g_perCpu[cpu].idleProcess;

    g_perCpu[cpu].currentProcess = first;
    first->state = ProcessState::Running;
    g_perCpu[cpu].sliceStartTick = g_lapicTickCount;
    GdtSetTssRsp0ForCpu(cpu, first->kernelStackTop);
    SetSyscallStack(cpu, first->kernelStackTop);

    VmmSwitchPageTable(first->pageTable);

    if (first == g_perCpu[cpu].idleProcess)
    {
        SerialPrintf("SCHED: CPU%u entering idle\n", cpu);
        __asm__ volatile("sti");
        for (;;)
            __asm__ volatile("hlt" ::: "memory");
    }

    if (first->fsBase)
    {
        uint32_t lo = static_cast<uint32_t>(first->fsBase);
        uint32_t hi = static_cast<uint32_t>(first->fsBase >> 32);
        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000100U));
    }

    SerialPrintf("SCHED: CPU%u entering user mode for '%s' (pid %u)\n",
                 cpu, first->name, first->pid);

    SwitchToUserMode(first->stackTop, first->elf.entryPoint);
    __builtin_unreachable();
}

void SchedulerSetCpuEnv(uint32_t cpuIndex, KernelCpuEnv* env)
{
    g_perCpu[cpuIndex].cpuEnv = env;
}

void SchedulerInitApIdle(uint32_t cpuIndex)
{
    auto* idle = static_cast<Process*>(kmalloc(sizeof(Process)));
    __builtin_memset(idle, 0, sizeof(Process));
    idle->pid = 0;
    idle->state = ProcessState::Ready;
    char name[] = "idle0";
    name[4] = static_cast<char>('0' + (cpuIndex % 10));
    __builtin_memcpy(idle->name, name, 6);
    idle->kernelStackBase = reinterpret_cast<uint64_t>(g_idleStacks[cpuIndex]);
    idle->kernelStackTop  = reinterpret_cast<uint64_t>(g_idleStacks[cpuIndex]) + sizeof(g_idleStacks[cpuIndex]);
    idle->savedCtx.rsp = idle->kernelStackTop - 8;
    idle->savedCtx.rip = reinterpret_cast<uint64_t>(&IdleLoop);
    idle->savedCtx.rflags = 0x202;
    idle->savedCtx.cr3 = VmmKernelCR3().pml4.raw();
    idle->pageTable = VmmKernelCR3();
    g_perCpu[cpuIndex].idleProcess = idle;
}

Process* ProcessCurrent()
{
    return g_perCpu[ThisCpu()].currentProcess;
}

uint16_t SchedulerAllocPid()
{
    return g_nextPid++;
}

} // namespace brook
