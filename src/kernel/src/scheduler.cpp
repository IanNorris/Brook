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
#include "spinlock.h"
#include "sched_ops.h"

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
// Interrupt-safe spinlock for scheduler
// ---------------------------------------------------------------------------
// This spinlock saves/restores RFLAGS.IF to prevent deadlock when the timer
// ISR fires while a syscall path holds the lock on the same CPU.

struct SchedLock {
    volatile uint32_t next   = 0;
    volatile uint32_t serving = 0;
};

static inline uint64_t SchedLockAcquire(SchedLock& lock)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    uint32_t ticket = __atomic_fetch_add(&lock.next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock.serving, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");
    return flags;
}

static inline void SchedLockRelease(SchedLock& lock, uint64_t savedFlags)
{
    __atomic_fetch_add(&lock.serving, 1, __ATOMIC_RELEASE);
    if (savedFlags & 0x200)
        __asm__ volatile("sti" ::: "memory");
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
    Process*         pendingRequeue;   // Set before context_switch; consumed after
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

// Pluggable scheduling policy (loaded at init, called through vtable).
static const SchedOps* g_schedOps = nullptr;
static uint8_t g_schedStateStorage[4096] __attribute__((aligned(16)));
static void*   g_schedState = g_schedStateStorage;

// PID → Process* lookup (for converting PickNext pid back to Process*).
static Process* g_pidToProcess[SCHED_MAX_PIDS] = {};

// Global scheduler lock (interrupt-safe, protects all policy calls).
static SchedLock g_readyLock;

// All processes (for blocked-process scanning).
static Process* g_allProcesses[MAX_PROCESSES] = {};
static uint32_t g_processCount = 0;
static SchedLock g_allProcLock;

// Next PID to allocate.
static uint16_t g_nextPid = 1;

// Guard: timer ticks are ignored until SchedulerStart sets this.
static volatile bool g_schedulerRunning = false;

// ---------------------------------------------------------------------------
// Ready queue operations — delegate to the pluggable policy module.
// Caller must hold g_readyLock.
// ---------------------------------------------------------------------------

static void ReadyQueueInsertLocked(Process* proc)
{
    // Idle processes (pid=0) are never managed by the policy module.
    if (proc->pid == 0) return;

    // Guard: process must not already be running on a CPU.
    int32_t cpu = __atomic_load_n(&proc->runningOnCpu, __ATOMIC_ACQUIRE);
    if (cpu != -1)
    {
        SerialPrintf("SCHED BUG: inserting RUNNING proc '%s' (pid %u) into ready queue! "
                     "runningOnCpu=%d state=%d\n",
                     proc->name, proc->pid, cpu, (int)proc->state);
        for (;;) __asm__ volatile("hlt");
    }
    g_schedOps->Enqueue(g_schedState, proc->pid);
}

static void ReadyQueueRemoveLocked(Process* proc)
{
    if (proc->pid == 0) return; // idle never in policy queue
    g_schedOps->Remove(g_schedState, proc->pid);
}

static Process* PickNextLocked()
{
    uint16_t pid = g_schedOps->PickNext(g_schedState);
    if (pid == SCHED_PID_NONE) return nullptr;
    return g_pidToProcess[pid];
}

// ---------------------------------------------------------------------------
// Idle process — halts until next interrupt (one per CPU)
// ---------------------------------------------------------------------------

static uint8_t g_idleStacks[SCHED_MAX_CPUS][65536] __attribute__((aligned(16)));

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
    // Load the scheduling policy module.
    // For now, use the statically-linked default (sched_rr).
    // Future: load from INIT.RC via ModuleLoad("sched_mlfq.mod").
    g_schedOps = GetSchedOps();
    if (g_schedOps->stateSize > sizeof(g_schedStateStorage))
    {
        SerialPrintf("SCHED FATAL: policy state %zu > storage %zu\n",
                     g_schedOps->stateSize, sizeof(g_schedStateStorage));
        for (;;) __asm__ volatile("hlt");
    }
    g_schedOps->Init(g_schedState);
    SerialPrintf("SCHED: loaded policy '%s'\n", g_schedOps->name);

    // Create idle process for BSP (CPU 0).
    auto* idle = static_cast<Process*>(kmalloc(sizeof(Process)));
    __builtin_memset(idle, 0, sizeof(Process));
    // Safe FPU/SSE defaults for fxrstor
    idle->fxsave.data[0] = 0x7F; idle->fxsave.data[1] = 0x03;   // FCW = 0x037F
    idle->fxsave.data[24] = 0x80; idle->fxsave.data[25] = 0x1F; // MXCSR = 0x1F80

    idle->pid = 0;
    idle->state = ProcessState::Ready;
    idle->runningOnCpu = -1;
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
// Because context_switch jumps here instead of returning to DoSwitch,
// we must manually drain the pending requeue that DoSwitch set up.
static void ProcessTrampoline()
{
    uint32_t cpu = ThisCpu();

    // Drain the per-CPU requeue set by DoSwitch before context_switch.
    Process* toRequeue = g_perCpu[cpu].pendingRequeue;
    g_perCpu[cpu].pendingRequeue = nullptr;
    if (toRequeue)
    {
        uint64_t rlf = SchedLockAcquire(g_readyLock);
        if (toRequeue->state == ProcessState::Ready)
            ReadyQueueInsertLocked(toRequeue);
        SchedLockRelease(g_readyLock, rlf);
    }

    Process* proc = g_perCpu[cpu].currentProcess;
    SerialPrintf("SCHED: CPU%u entering user mode for '%s' (pid %u)\n",
                 cpu, proc->name, proc->pid);

    __asm__ volatile("sti");

    SwitchToUserMode(proc->stackTop, proc->elf.entryPoint);
    __builtin_unreachable();
}

// Trampoline for kernel threads. Like ProcessTrampoline but stays in ring 0.
// fn and arg are stored at the top of the kernel stack by KernelThreadCreate.
void KernelThreadTrampoline()
{
    uint32_t cpu = ThisCpu();

    // Drain pending requeue (same as ProcessTrampoline).
    Process* toRequeue = g_perCpu[cpu].pendingRequeue;
    g_perCpu[cpu].pendingRequeue = nullptr;
    if (toRequeue)
    {
        uint64_t rlf = SchedLockAcquire(g_readyLock);
        if (toRequeue->state == ProcessState::Ready)
            ReadyQueueInsertLocked(toRequeue);
        SchedLockRelease(g_readyLock, rlf);
    }

    Process* proc = g_perCpu[cpu].currentProcess;
    SerialPrintf("SCHED: CPU%u starting kernel thread '%s' (pid %u)\n",
                 cpu, proc->name, proc->pid);

    __asm__ volatile("sti");

    // Read fn and arg from the top of the kernel stack (placed by KernelThreadCreate).
    auto* stackSlots = reinterpret_cast<uint64_t*>(proc->kernelStackBase + KERNEL_STACK_SIZE);
    KernelThreadFn fn = reinterpret_cast<KernelThreadFn>(stackSlots[-2]);
    void* arg = reinterpret_cast<void*>(stackSlots[-1]);

    fn(arg);

    // If fn returns, terminate this thread.
    SchedulerExitCurrentProcess(0);
}

void SchedulerAddProcess(Process* proc)
{
    proc->state = ProcessState::Ready;

    proc->savedCtx.rsp = proc->kernelStackTop - 8;
    proc->savedCtx.rip = proc->isKernelThread
        ? reinterpret_cast<uint64_t>(&KernelThreadTrampoline)
        : reinterpret_cast<uint64_t>(&ProcessTrampoline);
    proc->savedCtx.rflags = 0x202;
    proc->savedCtx.cr3 = proc->pageTable.pml4.raw();
    proc->savedCtx.fsBase = proc->fsBase;

    // Register with pid lookup and policy module.
    if (proc->pid < SCHED_MAX_PIDS)
        g_pidToProcess[proc->pid] = proc;
    g_schedOps->InitProcess(g_schedState, proc->pid, proc->schedPriority);

    uint64_t rlf1 = SchedLockAcquire(g_readyLock);
    ReadyQueueInsertLocked(proc);
    SchedLockRelease(g_readyLock, rlf1);

    uint64_t alf1 = SchedLockAcquire(g_allProcLock);
    if (g_processCount < MAX_PROCESSES)
        g_allProcesses[g_processCount++] = proc;
    SchedLockRelease(g_allProcLock, alf1);

    SerialPrintf("SCHED: added '%s' (pid %u) to ready queue\n",
                 proc->name, proc->pid);
}

void SchedulerRemoveProcess(Process* proc)
{
    uint64_t rlf2 = SchedLockAcquire(g_readyLock);
    if (proc->state == ProcessState::Ready)
        ReadyQueueRemoveLocked(proc);
    SchedLockRelease(g_readyLock, rlf2);

    uint64_t alf2 = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        if (g_allProcesses[i] == proc)
        {
            g_allProcesses[i] = g_allProcesses[--g_processCount];
            break;
        }
    }
    SchedLockRelease(g_allProcLock, alf2);
}

void SchedulerBlock(Process* proc)
{
    // Disable interrupts across the entire block+yield sequence to prevent
    // SchedulerTimerTick from firing between setting Blocked and yielding,
    // which would overwrite the Blocked state with Ready.
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    uint64_t rlf3 = SchedLockAcquire(g_readyLock);
    proc->state = ProcessState::Blocked;
    ReadyQueueRemoveLocked(proc);
    g_schedOps->VoluntaryYield(g_schedState, proc->pid);
    SchedLockRelease(g_readyLock, rlf3);

    uint32_t cpu = ThisCpu();
    if (proc == g_perCpu[cpu].currentProcess)
        SchedulerYield();

    // Restore interrupts after yield (the context_switch + iretq path
    // will re-enable interrupts for us, but if we didn't yield we need
    // to restore).
    if (flags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

void SchedulerUnblock(Process* proc)
{
    uint64_t rlf4 = SchedLockAcquire(g_readyLock);
    // The process must be fully Blocked (state=Blocked AND no longer running
    // on any CPU). Between SchedulerBlock setting state=Blocked and DoSwitch
    // clearing runningOnCpu, the process is in a transient state visible to
    // other CPUs — we must not unblock it until the context switch completes.
    if (proc->state != ProcessState::Blocked ||
        __atomic_load_n(&proc->runningOnCpu, __ATOMIC_ACQUIRE) != -1)
    {
        SchedLockRelease(g_readyLock, rlf4);
        return;
    }
    proc->state = ProcessState::Ready;
    proc->wakeupTick = 0;
    ReadyQueueInsertLocked(proc);
    SchedLockRelease(g_readyLock, rlf4);
}

uint32_t SchedulerReadyCount()
{
    uint64_t rlf5 = SchedLockAcquire(g_readyLock);
    uint32_t count = g_schedOps->ReadyCount(g_schedState);
    SchedLockRelease(g_readyLock, rlf5);
    return count;
}

Process* SchedulerCurrentProcess()
{
    uint32_t cpu = ThisCpu();
    return g_perCpu[cpu].currentProcess;
}

// ---------------------------------------------------------------------------
// Context switch logic
// ---------------------------------------------------------------------------

// Check blocked processes for timed wakeups (called with NO locks held).
static void CheckBlockedWakeups()
{
    uint64_t now = g_lapicTickCount;
    Process* toUnblock[MAX_PROCESSES];
    uint32_t unblockCount = 0;

    uint64_t alf3 = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (p->state == ProcessState::Blocked && p->wakeupTick != 0
            && now >= p->wakeupTick
            && __atomic_load_n(&p->runningOnCpu, __ATOMIC_ACQUIRE) == -1)
        {
            if (unblockCount < MAX_PROCESSES)
                toUnblock[unblockCount++] = p;
        }
    }
    SchedLockRelease(g_allProcLock, alf3);

    for (uint32_t i = 0; i < unblockCount; ++i)
        SchedulerUnblock(toUnblock[i]);
}

// Reap terminated processes.
static void ReapTerminated()
{
    uint32_t cpu = ThisCpu();
    uint64_t alf = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; )
    {
        Process* p = g_allProcesses[i];
        if (p->state == ProcessState::Terminated && p != g_perCpu[cpu].currentProcess)
        {
            SchedLockRelease(g_allProcLock, alf);
            SerialPrintf("SCHED: reaping '%s' (pid %u)\n", p->name, p->pid);
            ProcessDestroy(p);
            // ProcessDestroy calls SchedulerRemoveProcess
            alf = SchedLockAcquire(g_allProcLock);
            // Restart scan — list was modified.
            i = 0;
        }
        else
        {
            ++i;
        }
    }
    SchedLockRelease(g_allProcLock, alf);
}

// Perform a context switch from `oldProc` to `newProc`.
// If `requeueOld` is true, the old process is re-inserted into the ready
// queue **after** context_switch has saved its state — this prevents the
// race where another CPU picks the process while it's still mid-switch.
//
// IMPORTANT: We store requeueOld info in per-CPU state, NOT on the stack.
// After context_switch, the resumed process returns into a *previous*
// DoSwitch invocation with that invocation's stack-local variables.
// Per-CPU state is tied to the physical CPU and is NOT saved/restored.
static void DoSwitch(Process* oldProc, Process* newProc, bool requeueOld = false)
{
    __asm__ volatile("cli");

    uint32_t cpu = ThisCpu();

    // Double-schedule detection: newProc must not already be running on another CPU.
    int32_t prevCpu = __atomic_exchange_n(&newProc->runningOnCpu, (int32_t)cpu, __ATOMIC_ACQ_REL);
    if (prevCpu != -1)
    {
        SerialPrintf("SCHED FATAL: double-schedule! '%s' (pid %u) already on CPU%d, "
                     "now CPU%u. oldProc='%s' pid=%u\n",
                     newProc->name, newProc->pid, prevCpu, cpu,
                     oldProc->name, oldProc->pid);
        for (;;) __asm__ volatile("hlt");
    }

    // Mark old process as no longer running on this CPU.
    __atomic_store_n(&oldProc->runningOnCpu, (int32_t)-1, __ATOMIC_RELEASE);

    g_perCpu[cpu].currentProcess = newProc;
    newProc->state = ProcessState::Running;
    g_perCpu[cpu].sliceStartTick = g_lapicTickCount;

    // Store requeue info in per-CPU state BEFORE context_switch.
    g_perCpu[cpu].pendingRequeue = requeueOld ? oldProc : nullptr;

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

    // --- We return here when another CPU (or this one) switches back to us ---
    // Read the per-CPU pendingRequeue that was set by THIS physical CPU
    // just before the context_switch that resumed us.
    uint32_t resumedCpu = ThisCpu();
    Process* toRequeue = g_perCpu[resumedCpu].pendingRequeue;
    g_perCpu[resumedCpu].pendingRequeue = nullptr;

    if (toRequeue)
    {
        uint64_t rlf6 = SchedLockAcquire(g_readyLock);
        if (toRequeue->state == ProcessState::Ready)
            ReadyQueueInsertLocked(toRequeue);
        SchedLockRelease(g_readyLock, rlf6);
    }
}

void SchedulerTimerTick()
{
    if (!g_schedulerRunning)
        return;

    // Only BSP (CPU 0) does wakeup checks, reaping, and policy ticks.
    uint32_t cpu = ThisCpu();
    if (cpu == 0)
    {
        CheckBlockedWakeups();
        ReapTerminated();
        // Notify policy of time passing (for anti-starvation boosts etc.).
        uint64_t rlf_tick = SchedLockAcquire(g_readyLock);
        g_schedOps->Tick(g_schedState, g_lapicTickCount);
        SchedLockRelease(g_readyLock, rlf_tick);
    }

    Process* cur = g_perCpu[cpu].currentProcess;
    if (!cur)
        return;

    // Idle — if something became ready, switch to it.
    if (cur == g_perCpu[cpu].idleProcess)
    {
        uint64_t rlf7 = SchedLockAcquire(g_readyLock);
        Process* next = PickNextLocked();
        SchedLockRelease(g_readyLock, rlf7);
        if (next)
            DoSwitch(cur, next);
        return;
    }

    // Check timeslice (per-process, from policy module).
    uint64_t timeslice = g_schedOps->Timeslice(g_schedState, cur->pid);
    if (g_lapicTickCount - g_perCpu[cpu].sliceStartTick < timeslice)
        return;

    // Only preempt if the process is still Running. It might have been
    // marked Blocked (by SchedulerBlock in a syscall) between the lock
    // release and the yield — the timer fired in that window.
    if (cur->state != ProcessState::Running)
        return;

    // Timeslice expired — notify policy, pick next, and switch.
    uint64_t rlf8 = SchedLockAcquire(g_readyLock);
    g_schedOps->TimesliceExpired(g_schedState, cur->pid);
    Process* next = PickNextLocked();
    SchedLockRelease(g_readyLock, rlf8);

    if (!next)
    {
        // Nothing else — keep running.
        g_perCpu[cpu].sliceStartTick = g_lapicTickCount;
        return;
    }

    cur->state = ProcessState::Ready;
    DoSwitch(cur, next, /* requeueOld */ true);
}

void SchedulerYield()
{
    uint32_t cpu = ThisCpu();
    Process* old = g_perCpu[cpu].currentProcess;
    if (!old)
        return;

    uint64_t rlf9 = SchedLockAcquire(g_readyLock);
    Process* next = PickNextLocked();
    SchedLockRelease(g_readyLock, rlf9);

    if (!next)
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
        // Nothing else to run — keep current.
        return;
    }

    // Re-enqueue old process after context_switch saves its state.
    bool requeue = (old->state == ProcessState::Running);
    if (requeue)
        old->state = ProcessState::Ready;
    DoSwitch(old, next, requeue);
}

[[noreturn]] void SchedulerExitCurrentProcess(int status)
{
    uint32_t cpu = ThisCpu();
    Process* proc = g_perCpu[cpu].currentProcess;
    SerialPrintf("SCHED: '%s' (pid %u) exited with status %d\n",
                 proc->name, proc->pid, status);

    proc->state = ProcessState::Terminated;

    uint64_t rlf10 = SchedLockAcquire(g_readyLock);
    ReadyQueueRemoveLocked(proc);
    Process* next = PickNextLocked();
    SchedLockRelease(g_readyLock, rlf10);

    if (!next) next = g_perCpu[cpu].idleProcess;

    __atomic_store_n(&proc->runningOnCpu, (int32_t)-1, __ATOMIC_RELEASE);

    g_perCpu[cpu].currentProcess = next;
    next->state = ProcessState::Running;
    __atomic_store_n(&next->runningOnCpu, (int32_t)cpu, __ATOMIC_RELEASE);
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

    uint64_t rlf11 = SchedLockAcquire(g_readyLock);
    Process* first = PickNextLocked();
    SchedLockRelease(g_readyLock, rlf11);

    if (!first) first = g_perCpu[cpu].idleProcess;

    g_perCpu[cpu].currentProcess = first;
    first->state = ProcessState::Running;
    __atomic_store_n(&first->runningOnCpu, (int32_t)cpu, __ATOMIC_RELEASE);
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

    if (first->isKernelThread)
    {
        // Kernel threads stay in ring 0. Switch to the thread's own kernel
        // stack, enable interrupts, and call the trampoline.
        uint64_t newRsp = first->kernelStackTop - 16; // below fn/arg slots
        __asm__ volatile(
            "movq %0, %%rsp\n\t"
            "sti\n\t"
            "call *%1\n\t"
            "ud2\n\t"
            :: "r"(newRsp),
               "r"(reinterpret_cast<uint64_t>(&KernelThreadTrampoline))
            : "memory"
        );
        __builtin_unreachable();
    }

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
    uint64_t rlf12 = SchedLockAcquire(g_readyLock);
    Process* first = PickNextLocked();
    SchedLockRelease(g_readyLock, rlf12);

    if (!first) first = g_perCpu[cpu].idleProcess;

    g_perCpu[cpu].currentProcess = first;
    first->state = ProcessState::Running;
    __atomic_store_n(&first->runningOnCpu, (int32_t)cpu, __ATOMIC_RELEASE);
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

    if (first->isKernelThread)
    {
        uint64_t newRsp = first->kernelStackTop - 16;
        __asm__ volatile(
            "movq %0, %%rsp\n\t"
            "sti\n\t"
            "call *%1\n\t"
            "ud2\n\t"
            :: "r"(newRsp),
               "r"(reinterpret_cast<uint64_t>(&KernelThreadTrampoline))
            : "memory"
        );
        __builtin_unreachable();
    }

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
    // Safe FPU/SSE defaults for fxrstor
    idle->fxsave.data[0] = 0x7F; idle->fxsave.data[1] = 0x03;   // FCW = 0x037F
    idle->fxsave.data[24] = 0x80; idle->fxsave.data[25] = 0x1F; // MXCSR = 0x1F80
    idle->pid = 0;
    idle->state = ProcessState::Ready;
    idle->runningOnCpu = -1;
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
