#include "scheduler.h"
#include "process.h"
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
extern "C" void context_switch(brook::SavedContext* oldCtx, brook::SavedContext* newCtx);

// Enter user mode for the first time (existing function in syscall.cpp).
namespace brook { void SwitchToUserMode(uint64_t userRsp, uint64_t userRip); }

namespace brook {

// ---------------------------------------------------------------------------
// Scheduler state
// ---------------------------------------------------------------------------

// Circular doubly-linked ready queue. Head points to the next process to run.
static Process* g_readyHead  = nullptr;

// The currently running process (may be nullptr only during init).
static Process* g_currentProcess = nullptr;

// The idle process — runs when nothing else is ready.
static Process* g_idleProcess = nullptr;

// Tick at which the current timeslice started.
static uint64_t g_sliceStartTick = 0;

// All processes (for blocked-process scanning).
static Process* g_allProcesses[MAX_PROCESSES] = {};
static uint32_t g_processCount = 0;

// Next PID to allocate.
static uint16_t g_nextPid = 1;

// ---------------------------------------------------------------------------
// Ready queue operations (circular doubly-linked list)
// ---------------------------------------------------------------------------

static void ReadyQueueInsert(Process* proc)
{
    if (!g_readyHead)
    {
        proc->schedNext = proc;
        proc->schedPrev = proc;
        g_readyHead = proc;
    }
    else
    {
        // Insert before head (i.e. at the tail of the queue).
        Process* tail = g_readyHead->schedPrev;
        proc->schedNext = g_readyHead;
        proc->schedPrev = tail;
        tail->schedNext = proc;
        g_readyHead->schedPrev = proc;
    }
}

static void ReadyQueueRemove(Process* proc)
{
    if (proc->schedNext == proc)
    {
        // Only element.
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
// Idle process — halts until next interrupt
// ---------------------------------------------------------------------------

// The idle process runs entirely in kernel mode (ring 0). It just halts.
// It has a minimal kernel stack but no user-space mapping.
static uint8_t g_idleStack[4096] __attribute__((aligned(16)));

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
    // Create the idle process (pid 0). It doesn't go in the ready queue —
    // the scheduler falls back to it when the queue is empty.
    g_idleProcess = static_cast<Process*>(kmalloc(sizeof(Process)));
    __builtin_memset(g_idleProcess, 0, sizeof(Process));

    g_idleProcess->pid = 0;
    g_idleProcess->state = ProcessState::Ready;
    __builtin_memcpy(g_idleProcess->name, "idle", 5);

    // Idle runs on a static kernel stack, ring 0 only.
    g_idleProcess->kernelStackBase = reinterpret_cast<uint64_t>(g_idleStack);
    g_idleProcess->kernelStackTop  = reinterpret_cast<uint64_t>(g_idleStack) + sizeof(g_idleStack);

    // Set up saved context so context_switch "returns" into IdleLoop.
    g_idleProcess->savedCtx.rsp = g_idleProcess->kernelStackTop - 8;
    g_idleProcess->savedCtx.rip = reinterpret_cast<uint64_t>(&IdleLoop);
    g_idleProcess->savedCtx.rflags = 0x202; // IF set
    g_idleProcess->savedCtx.cr3 = VmmKernelCR3().pml4.raw();

    SerialPuts("SCHED: scheduler initialised\n");
}

void SchedulerAddProcess(Process* proc)
{
    proc->state = ProcessState::Ready;
    ReadyQueueInsert(proc);

    // Track in global list for blocked-process scanning.
    if (g_processCount < MAX_PROCESSES)
        g_allProcesses[g_processCount++] = proc;

    SerialPrintf("SCHED: added '%s' (pid %u) to ready queue\n",
                 proc->name, proc->pid);
}

void SchedulerRemoveProcess(Process* proc)
{
    if (proc->state == ProcessState::Ready)
        ReadyQueueRemove(proc);

    // Remove from global list.
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        if (g_allProcesses[i] == proc)
        {
            g_allProcesses[i] = g_allProcesses[--g_processCount];
            break;
        }
    }
}

void SchedulerBlock(Process* proc)
{
    proc->state = ProcessState::Blocked;
    if (proc->schedNext) // still in ready queue
        ReadyQueueRemove(proc);

    SerialPrintf("SCHED: blocked '%s' (pid %u), wakeup at tick %lu\n",
                 proc->name, proc->pid, proc->wakeupTick);

    // If we just blocked the current process, we must reschedule now.
    if (proc == g_currentProcess)
        SchedulerYield();
}

void SchedulerUnblock(Process* proc)
{
    if (proc->state != ProcessState::Blocked)
        return;

    proc->state = ProcessState::Ready;
    proc->wakeupTick = 0;
    ReadyQueueInsert(proc);
}

uint32_t SchedulerReadyCount()
{
    if (!g_readyHead) return 0;
    uint32_t count = 1;
    for (Process* p = g_readyHead->schedNext; p != g_readyHead; p = p->schedNext)
        ++count;
    return count;
}

// ---------------------------------------------------------------------------
// Context switch logic
// ---------------------------------------------------------------------------

// Pick the next process to run. Returns idle if queue is empty.
static Process* PickNext()
{
    if (g_readyHead)
    {
        Process* next = g_readyHead;
        // Rotate the queue so the next call picks the following process.
        g_readyHead = g_readyHead->schedNext;
        return next;
    }
    return g_idleProcess;
}

// Check blocked processes for timed wakeups.
static void CheckBlockedWakeups()
{
    uint64_t now = g_lapicTickCount;
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (p->state == ProcessState::Blocked && p->wakeupTick != 0 && now >= p->wakeupTick)
            SchedulerUnblock(p);
    }
}

// Perform a context switch from `oldProc` to `newProc`.
static void DoSwitch(Process* oldProc, Process* newProc)
{
    g_currentProcess = newProc;
    newProc->state = ProcessState::Running;
    g_sliceStartTick = g_lapicTickCount;

    // Update TSS RSP0 so ring 3→0 transitions land on the new process's
    // kernel stack.
    GdtSetTssRsp0(newProc->kernelStackTop);

    // Save/restore SSE/FPU state around the GPR context switch.
    __asm__ volatile("fxsave %0" : "=m"(oldProc->fxsave));

    // The actual register save/restore + CR3 switch happens in asm.
    context_switch(&oldProc->savedCtx, &newProc->savedCtx);

    // When we return here, we've been switched BACK to oldProc.
    // Restore our SSE state.
    __asm__ volatile("fxrstor %0" : : "m"(g_currentProcess->fxsave));
}

void SchedulerTimerTick()
{
    CheckBlockedWakeups();

    if (!g_currentProcess)
        return;

    // Don't preempt the idle process — it yields naturally via hlt.
    if (g_currentProcess == g_idleProcess)
    {
        // If something became ready, switch to it.
        if (g_readyHead)
        {
            Process* next = PickNext();
            DoSwitch(g_idleProcess, next);
        }
        return;
    }

    // Check if timeslice has expired.
    if (g_lapicTickCount - g_sliceStartTick < SCHED_TIMESLICE_MS)
        return;

    // Timeslice expired — put current back in queue and switch.
    if (g_currentProcess->state == ProcessState::Running)
    {
        g_currentProcess->state = ProcessState::Ready;
        ReadyQueueInsert(g_currentProcess);
    }

    Process* old = g_currentProcess;
    Process* next = PickNext();
    if (next == old)
    {
        // Same process — just reset timeslice.
        old->state = ProcessState::Running;
        g_sliceStartTick = g_lapicTickCount;
        return;
    }

    DoSwitch(old, next);
}

void SchedulerYield()
{
    if (!g_currentProcess)
        return;

    // Put current back in ready queue (unless blocked/terminated).
    Process* old = g_currentProcess;
    if (old->state == ProcessState::Running)
    {
        old->state = ProcessState::Ready;
        ReadyQueueInsert(old);
    }

    Process* next = PickNext();
    if (!next || next == old)
    {
        // Nothing else to run — keep running.
        if (old->state == ProcessState::Ready)
        {
            old->state = ProcessState::Running;
            ReadyQueueRemove(old);
        }
        return;
    }

    DoSwitch(old, next);
}

[[noreturn]] void SchedulerExitCurrentProcess(int status)
{
    Process* proc = g_currentProcess;
    SerialPrintf("SCHED: '%s' (pid %u) exited with status %d\n",
                 proc->name, proc->pid, status);

    proc->state = ProcessState::Terminated;

    // Remove from ready queue if still there.
    if (proc->schedNext)
        ReadyQueueRemove(proc);

    // Pick next process.
    Process* next = PickNext();
    if (!next) next = g_idleProcess;

    g_currentProcess = next;
    next->state = ProcessState::Running;
    g_sliceStartTick = g_lapicTickCount;
    GdtSetTssRsp0(next->kernelStackTop);

    // We can't call DoSwitch because we don't want to save state for the
    // terminated process. Just restore the next process's context directly.
    context_switch(&proc->savedCtx, &next->savedCtx);

    // Should never reach here.
    __builtin_unreachable();
}

[[noreturn]] void SchedulerStart()
{
    SerialPrintf("SCHED: starting scheduler, %u processes ready\n",
                 SchedulerReadyCount());

    Process* first = PickNext();
    if (!first) first = g_idleProcess;

    g_currentProcess = first;
    first->state = ProcessState::Running;
    g_sliceStartTick = g_lapicTickCount;
    GdtSetTssRsp0(first->kernelStackTop);

    // For the first process we need to set up CR3 and enter user mode.
    // The process's savedCtx.rip points to a trampoline that does iretq
    // into ring 3.
    VmmSwitchPageTable(first->pageTable);

    if (first->fsBase)
    {
        uint32_t lo = static_cast<uint32_t>(first->fsBase);
        uint32_t hi = static_cast<uint32_t>(first->fsBase >> 32);
        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000100U));
    }

    SerialPrintf("SCHED: entering user mode for '%s' (pid %u)\n",
                 first->name, first->pid);
    SwitchToUserMode(first->stackTop, first->elf.entryPoint);

    __builtin_unreachable();
}

Process* ProcessCurrent()
{
    return g_currentProcess;
}

uint16_t SchedulerAllocPid()
{
    return g_nextPid++;
}

} // namespace brook
