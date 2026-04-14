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
    Process*         pendingRetire;    // Terminated process to mark reapable after context_switch
};

static PerCpuSchedState g_perCpu[SCHED_MAX_CPUS] = {};

// Helpers
static inline uint32_t ThisCpu() { return SmpCurrentCpuIndex(); }

// Drain per-CPU bookkeeping after context_switch — forward declared, defined below.
static void DrainPostSwitch(uint32_t cpu);

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

// Policy registry — modules register here; we can switch at runtime.
static constexpr uint32_t MAX_SCHED_POLICIES = 8;
static const SchedOps* g_registeredPolicies[MAX_SCHED_POLICIES] = {};
static uint32_t g_registeredPolicyCount = 0;

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

// Drain per-CPU bookkeeping that was set before a context_switch.
// Must be called after every context_switch resumption point (DoSwitch,
// ProcessTrampoline, KernelThreadTrampoline).
static void DrainPostSwitch(uint32_t cpu)
{
    // Mark any terminated process as safe to reap — by this point the CPU's
    // RSP is on the NEW process's kernel stack, so the old stack is unused.
    Process* retired = g_perCpu[cpu].pendingRetire;
    g_perCpu[cpu].pendingRetire = nullptr;
    if (retired)
        __atomic_store_n(&retired->reapable, true, __ATOMIC_RELEASE);

    // Re-enqueue the process we were switched away from.
    Process* toRequeue = g_perCpu[cpu].pendingRequeue;
    g_perCpu[cpu].pendingRequeue = nullptr;
    if (toRequeue)
    {
        uint64_t rlf = SchedLockAcquire(g_readyLock);
        if (toRequeue->state == ProcessState::Ready)
            ReadyQueueInsertLocked(toRequeue);
        SchedLockRelease(g_readyLock, rlf);
    }
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
    // The statically-linked default (sched_rr) is always available.
    // Dynamic modules can register additional policies at runtime.
    g_schedOps = GetSchedOps();
    SchedulerRegisterPolicy(g_schedOps);  // register built-in as first policy
    if (g_schedOps->stateSize > sizeof(g_schedStateStorage))
    {
        SerialPrintf("SCHED FATAL: policy state %lu > storage %lu\n",
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
// we must manually drain per-CPU bookkeeping that DoSwitch set up.
static void ProcessTrampoline()
{
    uint32_t cpu = ThisCpu();
    DrainPostSwitch(cpu);

    Process* proc = g_perCpu[cpu].currentProcess;
    DbgPrintf("SCHED: CPU%u entering user mode for '%s' (pid %u)\n",
                 cpu, proc->name, proc->pid);

    __asm__ volatile("sti");

    SwitchToUserMode(proc->stackTop, proc->initialEntry);
    __builtin_unreachable();
}

// Trampoline for forked child processes.
// Returns to the instruction after the fork() syscall with RAX=0.
// Uses SYSRET to enter user mode (same mechanism the parent's syscall
// return would use), with RCX=user RIP, R11=user RFLAGS.
static void ForkChildTrampoline()
{
    uint32_t cpu = ThisCpu();
    DrainPostSwitch(cpu);

    Process* proc = g_perCpu[cpu].currentProcess;
    DbgPrintf("SCHED: CPU%u fork child '%s' (pid %u) entering user mode at rip=0x%lx\n",
                 cpu, proc->name, proc->pid, proc->forkReturnRip);

    uint64_t userRip = proc->forkReturnRip;
    uint64_t userRsp = proc->forkReturnRsp;
    uint64_t userRflags = proc->forkReturnRflags;
    uint64_t userRbx = proc->forkRbx;
    uint64_t userRbp = proc->forkRbp;
    uint64_t userR12 = proc->forkR12;
    uint64_t userR13 = proc->forkR13;
    uint64_t userR14 = proc->forkR14;
    uint64_t userR15 = proc->forkR15;
    uint64_t userRdi = proc->forkRdi;
    uint64_t userRsi = proc->forkRsi;
    uint64_t userRdx = proc->forkRdx;
    uint64_t userR8  = proc->forkR8;
    uint64_t userR9  = proc->forkR9;
    uint64_t userR10 = proc->forkR10;
    proc->isForkChild = false;

    __asm__ volatile("sti");

    // Enter user mode via IRETQ with ALL registers restored.
    // Linux preserves every register across fork except RAX (0 for child).
    // We use IRETQ instead of SYSRET because SYSRET faults are delivered in
    // ring 0 with the user RSP — making crash diagnostics unreliable and
    // potentially corrupting state.  IRETQ faults are delivered properly
    // via the TSS RSP0 stack.
    //
    // We build a struct on the stack and load from it to avoid
    // register pressure issues with 15 operands.
    struct ForkRegs {
        uint64_t rip, rflags, rsp, rbx, rbp, r12, r13, r14, r15;
        uint64_t rdi, rsi, rdx, r8, r9, r10;
    } regs = { userRip, userRflags, userRsp, userRbx, userRbp,
               userR12, userR13, userR14, userR15,
               userRdi, userRsi, userRdx, userR8, userR9, userR10 };

    __asm__ volatile(
        "mov %[base], %%rax\n\t"
        "mov 24(%%rax), %%rbx\n\t"     // restore RBX
        "mov 32(%%rax), %%rbp\n\t"     // restore RBP
        "mov 40(%%rax), %%r12\n\t"     // restore R12
        "mov 48(%%rax), %%r13\n\t"     // restore R13
        "mov 56(%%rax), %%r14\n\t"     // restore R14
        "mov 64(%%rax), %%r15\n\t"     // restore R15
        "mov 72(%%rax), %%rdi\n\t"     // restore RDI
        "mov 80(%%rax), %%rsi\n\t"     // restore RSI
        "mov 88(%%rax), %%rdx\n\t"     // restore RDX
        "mov 96(%%rax), %%r8\n\t"      // restore R8
        "mov 104(%%rax), %%r9\n\t"     // restore R9
        "mov 112(%%rax), %%r10\n\t"    // restore R10
        "mov 8(%%rax), %%r11\n\t"      // R11 = user RFLAGS (preserved)
        "mov 0(%%rax), %%rcx\n\t"      // RCX = user RIP (preserved)
        // Build IRETQ frame: push SS, RSP, RFLAGS, CS, RIP
        "pushq $0x23\n\t"              // SS = user data segment
        "pushq 16(%%rax)\n\t"          // RSP = user stack
        "pushq 8(%%rax)\n\t"           // RFLAGS
        "pushq $0x2B\n\t"              // CS = user code segment
        "pushq 0(%%rax)\n\t"           // RIP = user return address
        "xor %%eax, %%eax\n\t"         // RAX = 0 (fork child return)
        "swapgs\n\t"
        "iretq\n\t"
        :: [base] "r"(&regs)
        : "memory"
    );
    __builtin_unreachable();
}

// Trampoline for kernel threads. Like ProcessTrampoline but stays in ring 0.
// fn and arg are stored at the top of the kernel stack by KernelThreadCreate.
void KernelThreadTrampoline()
{
    uint32_t cpu = ThisCpu();

    DrainPostSwitch(cpu);

    Process* proc = g_perCpu[cpu].currentProcess;
    DbgPrintf("SCHED: CPU%u starting kernel thread '%s' (pid %u)\n",
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

    // Kernel threads store fn/arg at kernelStackTop[-16] and [-8], so RSP
    // must start below those slots to avoid the function prologue overwriting them.
    proc->savedCtx.rsp = proc->isKernelThread
        ? proc->kernelStackTop - 24   // below fn/arg slots (16 bytes) + alignment
        : proc->kernelStackTop - 8;

    if (proc->isKernelThread)
        proc->savedCtx.rip = reinterpret_cast<uint64_t>(&KernelThreadTrampoline);
    else if (proc->isForkChild)
        proc->savedCtx.rip = reinterpret_cast<uint64_t>(&ForkChildTrampoline);
    else
        proc->savedCtx.rip = reinterpret_cast<uint64_t>(&ProcessTrampoline);

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

    DbgPrintf("SCHED: added '%s' (pid %u) to ready queue\n",
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

    // Check for a pending wakeup that raced with us (e.g. KMutexUnlock
    // calling SchedulerUnblock before we got here).  If set, the waker
    // already transferred mutex ownership; we should NOT block.
    if (__atomic_load_n(&proc->pendingWakeup, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&proc->pendingWakeup, 0, __ATOMIC_RELEASE);
        SchedLockRelease(g_readyLock, rlf3);
        if (flags & 0x200)
            __asm__ volatile("sti" ::: "memory");
        return;
    }

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
    if (proc->state != ProcessState::Blocked)
    {
        SchedLockRelease(g_readyLock, rlf4);
        return;
    }
    if (__atomic_load_n(&proc->runningOnCpu, __ATOMIC_ACQUIRE) != -1)
    {
        // Process is Blocked but still mid-context-switch on another CPU.
        // We can't insert it into the ready queue yet.  Set pendingWakeup
        // so CheckBlockedWakeups (timer tick) will retry the unblock once
        // the context switch completes and runningOnCpu is cleared.
        __atomic_store_n(&proc->pendingWakeup, 1, __ATOMIC_RELEASE);
        SchedLockRelease(g_readyLock, rlf4);
        return;
    }
    proc->state = ProcessState::Ready;
    proc->wakeupTick = 0;
    __atomic_store_n(&proc->pendingWakeup, 0, __ATOMIC_RELEASE);
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
        if (p->state != ProcessState::Blocked) continue;
        if (__atomic_load_n(&p->runningOnCpu, __ATOMIC_ACQUIRE) != -1) continue;

        // Timed wakeup OR deferred wakeup from SchedulerUnblock race
        bool timedWake = (p->wakeupTick != 0 && now >= p->wakeupTick);
        bool pendingWake = __atomic_load_n(&p->pendingWakeup, __ATOMIC_ACQUIRE) != 0;

        if (timedWake || pendingWake)
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
static volatile uint32_t g_reapInProgress = 0;

static void ReapTerminated()
{
    // Guard against re-entry: if PmmKillPid→SerialPrintf→serial-lock-sti
    // lets the timer fire again while we're mid-reap, don't nest.
    if (__atomic_exchange_n(&g_reapInProgress, 1, __ATOMIC_ACQUIRE))
        return;

    uint32_t cpu = ThisCpu();
    uint64_t alf = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; )
    {
        Process* p = g_allProcesses[i];
        if (p->state == ProcessState::Terminated
            && __atomic_load_n(&p->reapable, __ATOMIC_ACQUIRE)
            && p != g_perCpu[cpu].currentProcess)
        {
            // If parentPid != 0, check if the parent still exists.
            // If the parent is gone, reparent to 0 so we can reap.
            if (p->parentPid != 0)
            {
                bool parentAlive = false;
                for (uint32_t j = 0; j < g_processCount; j++)
                {
                    if (g_allProcesses[j]->pid == p->parentPid
                        && g_allProcesses[j]->state != ProcessState::Terminated)
                    {
                        parentAlive = true;
                        break;
                    }
                }
                if (parentAlive)
                {
                    // Parent may still call wait4 — skip for now
                    ++i;
                    continue;
                }
                // Parent is gone — reparent to init (0) for reaping
                p->parentPid = 0;
            }

            SchedLockRelease(g_allProcLock, alf);
            DbgPrintf("SCHED: reaping '%s' (pid %u)\n", p->name, p->pid);
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

    __atomic_store_n(&g_reapInProgress, 0, __ATOMIC_RELEASE);
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
    DrainPostSwitch(ThisCpu());
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

    // Close all FDs immediately so pipe readers/writers get unblocked.
    // This must happen before marking as Terminated — the parent may be
    // blocked on a pipe read that this process had the write end of.
    ProcessCloseAllFds(proc);

    // Reparent any children of this process to init (parentPid=0).
    // This ensures they can be reaped even after this parent is gone.
    {
        uint64_t alf = SchedLockAcquire(g_allProcLock);
        for (uint32_t i = 0; i < g_processCount; i++)
        {
            if (g_allProcesses[i]->parentPid == proc->pid)
                g_allProcesses[i]->parentPid = 0;
        }
        SchedLockRelease(g_allProcLock, alf);
    }

    // Signal the compositor to fill this process's screen region with an exit
    // status colour on its next pass. No VFB access needed — avoids races with
    // the reaper freeing VFB pages.
    //   Red  for abnormal termination (negative status = signal/fault)
    //   Blue for normal exit
    if (proc->fbVfbWidth > 0)
        proc->fbExitColor = (status < 0) ? 0x00CC0000u : 0x00001A3Au;

    // Null out VFB pointer BEFORE marking as Terminated to prevent the
    // compositor from blitting freed memory (race with ReapTerminated).
    proc->fbVirtual = nullptr;
    proc->fbVirtualSize = 0;

    proc->state = ProcessState::Terminated;
    proc->exitStatus = status;

    // Wake the parent process if it's blocked (likely in wait4).
    // Also send SIGCHLD to the parent.
    if (proc->parentPid != 0)
    {
        uint64_t alf = SchedLockAcquire(g_allProcLock);
        for (uint32_t i = 0; i < g_processCount; i++)
        {
            if (g_allProcesses[i]->pid == proc->parentPid)
            {
                Process* parent = g_allProcesses[i];
                SchedLockRelease(g_allProcLock, alf);

                // Send SIGCHLD (17) to parent
                constexpr int SIGCHLD = 17;
                uint64_t bit = 1ULL << (SIGCHLD - 1);
                __atomic_or_fetch(&parent->sigPending, bit, __ATOMIC_RELEASE);

                // Set pendingWakeup in case parent hasn't blocked yet
                __atomic_store_n(&parent->pendingWakeup, 1, __ATOMIC_RELEASE);
                if (parent->state == ProcessState::Blocked)
                    SchedulerUnblock(parent);
                goto parent_done;
            }
        }
        SchedLockRelease(g_allProcLock, alf);
    }
parent_done:

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

    // Mark this terminated process for deferred reap — DrainPostSwitch on the
    // resumed process will set reapable once the context_switch is complete
    // and this kernel stack is no longer in use.
    g_perCpu[cpu].pendingRetire = proc;

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

    DbgPrintf("SCHED: CPU%u entering user mode for '%s' (pid %u)\n",
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

    SwitchToUserMode(first->stackTop, first->initialEntry);

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

    DbgPrintf("SCHED: CPU%u entering user mode for '%s' (pid %u)\n",
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

    SwitchToUserMode(first->stackTop, first->initialEntry);    __builtin_unreachable();
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

Process* ProcessFindByPid(uint16_t pid)
{
    uint64_t alf = SchedLockAcquire(g_allProcLock);
    Process* result = nullptr;
    for (uint32_t i = 0; i < g_processCount; i++)
    {
        if (g_allProcesses[i]->pid == pid)
        {
            result = g_allProcesses[i];
            break;
        }
    }
    SchedLockRelease(g_allProcLock, alf);
    return result;
}

uint16_t SchedulerAllocPid()
{
    return g_nextPid++;
}

Process* SchedulerFindTerminatedChild(uint16_t parentPid, int64_t pid)
{
    uint64_t alf = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; i++)
    {
        Process* p = g_allProcesses[i];
        if (p->parentPid == parentPid
            && p->state == ProcessState::Terminated
            && __atomic_load_n(&p->reapable, __ATOMIC_ACQUIRE)
            && (pid == -1 || pid == static_cast<int64_t>(p->pid)))
        {
            SchedLockRelease(g_allProcLock, alf);
            return p;
        }
    }
    SchedLockRelease(g_allProcLock, alf);
    return nullptr;
}

void SchedulerReapChild(Process* child)
{
    DbgPrintf("SCHED: reaping child '%s' (pid %u)\n", child->name, child->pid);
    ProcessDestroy(child);
}

// ---------------------------------------------------------------------------
// Dynamic policy registration and switching
// ---------------------------------------------------------------------------

void SchedulerRegisterPolicy(const SchedOps* ops)
{
    if (!ops || !ops->name)
    {
        SerialPuts("SCHED: register — null policy\n");
        return;
    }
    // Check for duplicate
    for (uint32_t i = 0; i < g_registeredPolicyCount; ++i)
    {
        if (g_registeredPolicies[i] == ops) return; // already registered
        // Compare names
        const char* a = g_registeredPolicies[i]->name;
        const char* b = ops->name;
        bool same = true;
        for (uint32_t j = 0; a[j] || b[j]; ++j)
        {
            if (a[j] != b[j]) { same = false; break; }
        }
        if (same)
        {
            // Replace existing registration with new pointer
            g_registeredPolicies[i] = ops;
            SerialPrintf("SCHED: updated policy '%s'\n", ops->name);
            return;
        }
    }
    if (g_registeredPolicyCount >= MAX_SCHED_POLICIES)
    {
        SerialPrintf("SCHED: policy registry full, cannot register '%s'\n", ops->name);
        return;
    }
    g_registeredPolicies[g_registeredPolicyCount++] = ops;
    SerialPrintf("SCHED: registered policy '%s' (state=%lu bytes)\n",
                 ops->name, ops->stateSize);
}

static bool StrEq(const char* a, const char* b)
{
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

bool SchedulerSwitchPolicy(const char* name)
{
    const SchedOps* newOps = nullptr;
    for (uint32_t i = 0; i < g_registeredPolicyCount; ++i)
    {
        if (StrEq(g_registeredPolicies[i]->name, name))
        {
            newOps = g_registeredPolicies[i];
            break;
        }
    }
    if (!newOps)
    {
        SerialPrintf("SCHED: policy '%s' not registered\n", name);
        return false;
    }
    if (newOps == g_schedOps)
    {
        SerialPrintf("SCHED: already using '%s'\n", name);
        return true;
    }
    if (newOps->stateSize > sizeof(g_schedStateStorage))
    {
        SerialPrintf("SCHED: policy '%s' state %lu > storage %lu\n",
                     name, newOps->stateSize, sizeof(g_schedStateStorage));
        return false;
    }

    // Switch under the scheduler lock — migrate all active processes.
    uint64_t flags = SchedLockAcquire(g_readyLock);

    const SchedOps* oldOps = g_schedOps;
    SerialPrintf("SCHED: switching '%s' → '%s'\n", oldOps->name, newOps->name);

    // Initialize new policy state
    __builtin_memset(g_schedStateStorage, 0, sizeof(g_schedStateStorage));
    g_schedOps = newOps;
    newOps->Init(g_schedState);

    // Re-register all active processes and enqueue ready ones
    uint64_t allFlags = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p) continue;
        // Skip idle processes (pid 0 or idle-named)
        if (p->name[0] == 'i' && p->name[1] == 'd' &&
            p->name[2] == 'l' && p->name[3] == 'e')
            continue;
        newOps->InitProcess(g_schedState, p->pid, 2); // default priority
        if (p->state == ProcessState::Ready)
            newOps->Enqueue(g_schedState, p->pid);
    }
    SchedLockRelease(g_allProcLock, allFlags);

    SchedLockRelease(g_readyLock, flags);

    SerialPrintf("SCHED: now using '%s' (%u ready)\n",
                 newOps->name, newOps->ReadyCount(g_schedState));
    return true;
}

const char* SchedulerPolicyName()
{
    return g_schedOps ? g_schedOps->name : "none";
}

} // namespace brook
