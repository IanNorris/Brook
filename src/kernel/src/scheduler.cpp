#include "scheduler.h"
#include "process.h"
#include "panic.h"
#include "cpu.h"
#include "smp.h"
#include "apic.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "memory/address.h"
#include "gdt.h"
#include "serial.h"
#include "spinlock.h"
#include "sched_ops.h"
#include "profiler.h"
#include "device.h"
#include "sync/krwlock.h"

#include <stdint.h>

// LAPIC tick counter (defined in apic.cpp, volatile because ISR-modified).
namespace brook { extern volatile uint64_t g_lapicTickCount; }

// BRO-208: futex diagnostics (defined in syscall.cpp) for the hang dump.
extern "C" void FutexDumpWaiters(uint16_t tgidFilter);
extern "C" void FutexDumpTrace(uint16_t tgidFilter, uint32_t maxEntries);

// Context switch — implemented in context_switch.S
extern "C" void context_switch(brook::SavedContext* oldCtx, brook::SavedContext* newCtx,
                                brook::FxsaveArea* oldFx, brook::FxsaveArea* newFx,
                                volatile int32_t* oldRunningOnCpu);

// Futex wake — implemented in syscall.cpp, called for clear_child_tid on thread exit
extern "C" int64_t FutexWake(uint64_t owner, uint64_t uaddr, uint32_t maxWake,
                              uint32_t wake_bitset = 0xFFFFFFFFu);

// Ext2 lock state diagnostic — implemented in ext2_vfs.cpp
extern "C" void Ext2DumpLockState();

// FatFS lock state diagnostic — implemented in fatfs_vfs.cpp
extern "C" void FatFsDumpLockState();

// Enter user mode for the first time (existing function in syscall.cpp).
namespace brook { void SwitchToUserMode(uint64_t userRsp, uint64_t userRip); }

namespace brook {

extern "C" int ProcessDumpFreeLog(void* ptr);  // BRO-176 diag (process.cpp)

// ---------------------------------------------------------------------------
// Interrupt-safe spinlock for scheduler
// ---------------------------------------------------------------------------
// This spinlock saves/restores RFLAGS.IF to prevent deadlock when the timer
// ISR fires while a syscall path holds the lock on the same CPU.

struct SchedLock {
    volatile uint32_t next   = 0;
    volatile uint32_t serving = 0;
};

// ~25 million cycles at 2.5GHz ≈ 10ms — generous for any SchedLock
// critical section, which should be <1µs.
static constexpr uint64_t SCHEDLOCK_TIMEOUT_SPINS = 50000000ULL;

static inline uint64_t SchedLockAcquire(SchedLock& lock, const char* caller = __builtin_FUNCTION())
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    uint32_t ticket = __atomic_fetch_add(&lock.next, 1, __ATOMIC_RELAXED);
    uint64_t spins = 0;
    while (__atomic_load_n(&lock.serving, __ATOMIC_ACQUIRE) != ticket)
    {
        __asm__ volatile("pause" ::: "memory");
        if (++spins > SCHEDLOCK_TIMEOUT_SPINS)
        {
            // Deadlock detected. Re-enable interrupts for serial output.
            __asm__ volatile("sti" ::: "memory");
            // Use direct serial write to avoid any lock dependencies
            SerialPrintf("\n*** SCHEDLOCK DEADLOCK: %s waiting for ticket %u, "
                         "serving %u (spun %llu times) ***\n",
                         caller, ticket,
                         __atomic_load_n(&lock.serving, __ATOMIC_RELAXED),
                         (unsigned long long)spins);
            KernelPanic("SchedLock deadlock detected in %s "
                        "(ticket=%u serving=%u)",
                        caller, ticket,
                        __atomic_load_n(&lock.serving, __ATOMIC_RELAXED));
        }
    }
    return flags;
}

static inline void SchedLockRelease(SchedLock& lock, uint64_t savedFlags)
{
    __atomic_fetch_add(&lock.serving, 1, __ATOMIC_RELEASE);
    if (savedFlags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

// Release the lock WITHOUT touching RFLAGS.IF. Used by the held-across-pick
// dispatch path (DoSwitch): the scheduler decision holds g_readyLock with IF=0
// from PickNextLocked through the runningOnCpu claim, then drops the lock here
// immediately before context_switch — IF must stay 0 across the switch (the
// resumed thread restores its own IF), exactly as the pre-existing commit path
// did. This closes the BRO-176 lost-enqueue window: previously the lock was
// released (re-enabling IF) BEFORE DoSwitch's claim, leaving a gap in which
// (a) the picked proc was out of the ready queue but not yet claimed, and
// (b) a timer could divert this CPU. Both vanish when pick+claim are one
// IF=0 critical section.
static inline void SchedLockReleaseRaw(SchedLock& lock)
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
    Process*         pendingRequeue;   // Set before context_switch; consumed after
    Process*         pendingRetire;    // Terminated process to mark reapable after context_switch
    volatile uint64_t busyTicks;      // ticks spent running non-idle processes
    volatile uint64_t idleTicks;      // ticks spent in idle process
};

static PerCpuSchedState g_perCpu[SCHED_MAX_CPUS] = {};

// ---------------------------------------------------------------------------
// Reschedule IPI — kick an idle CPU to pick up a newly-enqueued process
// ---------------------------------------------------------------------------

static void KickIdleCpu()
{
    uint32_t cpuCount = brook::SmpGetCpuCount();
    if (cpuCount <= 1) return;

    uint32_t self = brook::SmpCurrentCpuIndex();

    // Find a CPU running its idle process and send it a reschedule IPI.
    // Skip self — the caller will pick up work via its own timer tick.
    for (uint32_t i = 0; i < cpuCount; ++i) {
        if (i == self) continue;
        Process* cur = g_perCpu[i].currentProcess;
        if (cur && cur == g_perCpu[i].idleProcess) {
            const brook::CpuInfo* ci = brook::SmpGetCpu(i);
            if (ci && ci->online) {
                brook::ApicSendRescheduleIpi(ci->apicId);
                return;
            }
        }
    }
}

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

static void CopyProcessNameForLog(const Process* proc, char out[33])
{
    if (!proc)
    {
        out[0] = '?';
        out[1] = '\0';
        return;
    }

    uint32_t i = 0;
    for (; i < 32 && proc->name[i]; ++i)
    {
        char ch = proc->name[i];
        out[i] = (ch >= 32 && ch <= 126) ? ch : '?';
    }
    out[i] = '\0';
}

// ---------------------------------------------------------------------------
// Scheduler state
// ---------------------------------------------------------------------------

// Pluggable scheduling policy (loaded at init, called through vtable).
static const SchedOps* g_schedOps = nullptr;
static uint8_t g_schedStateStorage[8192] __attribute__((aligned(16)));
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

// BRO-179: reverse-map diagnostic. Given a physical frame, scan EVERY live
// process's user page table (PML4[0..255]) for a present leaf PTE that still
// maps it, and print pid + VA + flags. Authoritative (unlike PMM mapCount,
// which is decoupled from actual PTE teardown). Intended to be called ONLY from
// the RSVD-#PF panic path, where all APs are already halted — so it does NOT
// take g_allProcLock (the owner CPU may hold it) and reads page tables straight
// through the direct map. Every table pointer is range-checked so a corrupt
// entry cannot fault the walker itself. Reports the leaking mapper behind the
// freed-while-mapped frame recycling (SIG1 root).
extern "C" void ProcessDumpFrameMappers(uint64_t targetPhys)
{
    constexpr uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL; // PTE physical-address bits
    targetPhys &= ~0xFFFULL;
    // Bound: physical frames live below the top of installed RAM. Use the
    // direct-map window as a sanity ceiling (anything mapped is < 0x4000_0000_0000).
    auto physOk = [](uint64_t p) -> bool {
        return p != 0 && p < 0x0000400000000000ULL; // 64 TiB ceiling, generous
    };
    auto tbl = [](uint64_t physTable) -> const uint64_t* {
        return reinterpret_cast<const uint64_t*>(DIRECT_MAP_BASE + physTable);
    };

    SerialPrintf("  --- BRO179 reverse-map: who maps phys 0x%lx? ---\n", targetPhys);
    uint32_t hits = 0;
    uint32_t n = g_processCount;
    if (n > MAX_PROCESSES) n = MAX_PROCESSES;
    for (uint32_t pi = 0; pi < n && hits < 32; ++pi)
    {
        Process* p = g_allProcesses[pi];
        if (!p || p->magic != PROCESS_MAGIC) continue;
        uint64_t pml4Phys = p->pageTable.pml4.raw();
        if (!physOk(pml4Phys)) continue;
        const uint64_t* pml4 = tbl(pml4Phys);

        for (uint64_t i4 = 0; i4 < 256 && hits < 32; ++i4) // user half only
        {
            uint64_t e4 = pml4[i4];
            if (!(e4 & VMM_PRESENT)) continue;
            uint64_t pdptPhys = e4 & PHYS_MASK;
            if (!physOk(pdptPhys)) continue;
            const uint64_t* pdpt = tbl(pdptPhys);
            for (uint64_t i3 = 0; i3 < 512 && hits < 32; ++i3)
            {
                uint64_t e3 = pdpt[i3];
                if (!(e3 & VMM_PRESENT) || (e3 & (1ULL << 7))) continue; // skip 1G
                uint64_t pdPhys = e3 & PHYS_MASK;
                if (!physOk(pdPhys)) continue;
                const uint64_t* pd = tbl(pdPhys);
                for (uint64_t i2 = 0; i2 < 512 && hits < 32; ++i2)
                {
                    uint64_t e2 = pd[i2];
                    if (!(e2 & VMM_PRESENT) || (e2 & (1ULL << 7))) continue; // skip 2M
                    uint64_t ptPhys = e2 & PHYS_MASK;
                    if (!physOk(ptPhys)) continue;
                    const uint64_t* pt = tbl(ptPhys);
                    for (uint64_t i1 = 0; i1 < 512 && hits < 32; ++i1)
                    {
                        uint64_t e1 = pt[i1];
                        if (!(e1 & VMM_PRESENT)) continue;
                        if ((e1 & PHYS_MASK) != targetPhys) continue;
                        uint64_t va = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                        SerialPrintf("    MAPPER pid=%u va=0x%lx pte=0x%lx (W=%d U=%d COW-pid=%lu)\n",
                                     (unsigned)p->pid, va, e1,
                                     (int)((e1 >> 1) & 1), (int)((e1 >> 2) & 1),
                                     (e1 & PTE_PID_MASK) >> PTE_PID_SHIFT);
                        ++hits;
                    }
                }
            }
        }
    }
    if (hits == 0)
        SerialPrintf("    (no live user PTE maps this frame — stale-TLB-only write,"
                     " or mapper already torn down)\n");
    SerialPrintf("  --- end reverse-map (%u mapper PTE(s)) ---\n", hits);
}

// Next PID to allocate.
static uint16_t g_nextPid = 1;

// PID recycling: stack of freed pids in the [1, MAX_PROCESSES) range.
// Without recycling g_nextPid grows unboundedly past MAX_PROCESSES, which
// breaks any g_sigHandlers[tgid] / g_pidToProcess[tgid] indexing. We keep
// PIDs bounded so signal-handler tables never overflow.
static uint16_t g_pidFreeStack[MAX_PROCESSES];
static uint32_t g_pidFreeCount = 0;
static SchedLock g_pidLock;

// Guard: timer ticks are ignored until SchedulerStart sets this.
static volatile bool g_schedulerRunning = false;

// ---------------------------------------------------------------------------
// BRO-176 DIAGNOSTIC: asLiveThreads inc/dec ring.
// Records every asLiveThreads increment (SchedulerAddProcess) and decrement
// attempt (ProcessDestroy), including decrements SKIPPED by the incarnation
// guard — the prime suspect for a leaked count that wedges a Terminated leader
// permanently unreaped (the reap-stall hang). Dumped by SchedulerDumpHang
// (Ctrl+F12) and on demand. Lock-free (atomic seq), like the PMM free-log.
// TEMPORARY — strip with the rest of the BRO-176 instrumentation.
// ---------------------------------------------------------------------------
struct AsLiveEvent {
    uint64_t seq;
    void*    leader;        // leader Process* the op targeted
    uint16_t actingPid;     // the thread/proc whose lifecycle drove the op
    uint16_t leaderPid;
    uint32_t leaderIncarn;  // leader->incarnation at op time
    uint32_t procLeaderIncarn; // proc->leaderIncarnation (decrement guard key)
    int32_t  result;        // resulting asLiveThreads value
    uint8_t  op;            // 0=inc, 1=dec-applied, 2=dec-SKIPPED(mismatch), 3=dec-bad-leader
};
static constexpr uint32_t ASLIVE_RING_SIZE = 4096;
static AsLiveEvent g_asLiveRing[ASLIVE_RING_SIZE];
static volatile uint64_t g_asLiveSeq = 0;

extern "C" void SchedulerRecordAsLive(void* leader, uint16_t actingPid, uint16_t leaderPid,
                                      uint32_t leaderIncarn, uint32_t procLeaderIncarn,
                                      int32_t result, uint8_t op)
{
    uint64_t s = __atomic_fetch_add(&g_asLiveSeq, 1, __ATOMIC_RELAXED);
    AsLiveEvent& e = g_asLiveRing[s & (ASLIVE_RING_SIZE - 1)];
    e.seq = s; e.leader = leader; e.actingPid = actingPid; e.leaderPid = leaderPid;
    e.leaderIncarn = leaderIncarn; e.procLeaderIncarn = procLeaderIncarn;
    e.result = result; e.op = op;
}

// BRO-176 diagnostic: dump the asLiveThreads inc/dec history for one leader
// (by pointer), so a leaked count's unmatched increment is visible. Lock-free
// read (best-effort during a hang). op: 0=inc 1=dec 2=dec-SKIPPED 3=dec-badleader.
static void DumpAsLiveHistory(void* leader)
{
    auto puts = [](const char* str){ if (str) while (*str) SerialPutChar(*str++); };
    auto dec = [](int64_t v){ if(v<0){SerialPutChar('-');v=-v;} char b[20]; int i=0;
        if(!v)b[i++]='0'; while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i)SerialPutChar(b[--i]); };
    const char* opName[4] = { "INC      ", "DEC      ", "DEC-SKIP ", "DEC-BADLDR" };
    uint64_t total = g_asLiveSeq;
    uint64_t start = (total > ASLIVE_RING_SIZE) ? (total - ASLIVE_RING_SIZE) : 0;
    int shown = 0;
    for (uint64_t s = start; s < total; ++s)
    {
        AsLiveEvent& e = g_asLiveRing[s & (ASLIVE_RING_SIZE - 1)];
        if (e.leader != leader) continue;
        puts("      asLive["); dec((int64_t)e.seq); puts("] ");
        puts(e.op < 4 ? opName[e.op] : "?");
        puts(" actingPid="); dec(e.actingPid);
        puts(" ldrPid="); dec(e.leaderPid);
        puts(" ldrIncarn="); dec(e.leaderIncarn);
        puts(" procLdrIncarn="); dec(e.procLeaderIncarn);
        puts(" -> count="); dec(e.result);
        SerialPutChar('\n');
        if (++shown >= 64) { puts("      ...(truncated)\n"); break; }
    }
    if (shown == 0) puts("      (no asLiveThreads events recorded for this leader)\n");
}

// Cumulative stats (for /proc/stat)
static volatile uint64_t g_totalForks = 0;
static volatile uint64_t g_reapedUserTicks = 0;
static volatile uint64_t g_reapedSysTicks = 0;

// EWMA load averages (fixed-point: value * 1000)
// Linux uses e^(-5/60), e^(-5/300), e^(-5/900) ≈ 0.920, 0.983, 0.994
// We approximate with integer math: new = old * decay + sample * (1000-decay)
// All divided by 1000 for scaling.
static volatile uint32_t g_loadAvg1  = 0;  // 1-minute EWMA * 1000
static volatile uint32_t g_loadAvg5  = 0;  // 5-minute EWMA * 1000
static volatile uint32_t g_loadAvg15 = 0;  // 15-minute EWMA * 1000
static constexpr uint32_t LOAD_DECAY_1  = 920;  // e^(-5/60) * 1000
static constexpr uint32_t LOAD_DECAY_5  = 983;  // e^(-5/300) * 1000
static constexpr uint32_t LOAD_DECAY_15 = 994;  // e^(-5/900) * 1000
static constexpr uint64_t LOAD_SAMPLE_INTERVAL = 5000; // 5 seconds in ms ticks

// ---------------------------------------------------------------------------
// Ready queue operations — delegate to the pluggable policy module.
// Caller must hold g_readyLock.
// ---------------------------------------------------------------------------

// BRO-176 lost-enqueue diagnostic: scheduler-event trace sites.
enum SchedTraceSite : uint8_t {
    STR_ENQUEUE = 1,       // ReadyQueueInsertLocked → policy Enqueue
    STR_REMOVE,            // ReadyQueueRemoveLocked → policy Remove
    STR_READY_UNBLOCK,     // SchedulerUnblock set state=Ready + enqueue
    STR_UNBLOCK_DEFER_RR,  // SchedulerUnblock: state already Running/Ready → pendingWakeup
    STR_UNBLOCK_DEFER_CPU, // SchedulerUnblock: Blocked but runningOnCpu!=-1 → pendingWakeup
    STR_BLOCK,             // SchedulerBlock set state=Blocked
    STR_BLOCK_SKIP,        // SchedulerBlock early-return on pendingWakeup
    STR_READY_PREEMPT,     // SchedulerPreempt set state=Ready (requeueOld)
    STR_READY_YIELD,       // SchedulerYield set state=Ready (requeue)
    STR_REQUEUE_ENQ,       // DrainPostSwitch requeued (state==Ready → enqueue)
    STR_REQUEUE_SKIP,      // DrainPostSwitch saw pendingRequeue but state!=Ready
    STR_RUN,               // DoSwitch set state=Running
    STR_PICK_RETURN,       // 13: PickNextLocked about to RETURN proc to DoSwitch
    STR_PICK_SKIP_RUN,     // 14: PickNextLocked skipped proc (runningOnCpu!=-1) + raw re-enqueue
    STR_DS_BAIL_ENQ,       // 15: DoSwitch double-schedule bail re-enqueued newProc (Ready)
    STR_DS_BAIL_SKIP,      // 16: DoSwitch double-schedule bail, newProc state!=Ready (no enq)
    STR_REMOVE_PROC,       // 17: SchedulerRemoveProcess removed proc (destroy)
};

static inline void SchedTrace(Process* proc, uint8_t site)
{
    if (!proc) return;
    uint64_t e = (g_lapicTickCount << 16) | ((uint64_t)site << 8)
               | ((uint64_t)(uint8_t)proc->state);
    uint8_t h = proc->schedTraceHead;
    proc->schedTrace[h % 12] = e;
    proc->schedTraceHead = (uint8_t)(h + 1);
}

static void ReadyQueueInsertLocked(Process* proc)
{
    // Idle processes (pid=0) are never managed by the policy module.
    if (proc->pid == 0) return;

    // Guard: process must not already be running on a CPU.
    int32_t cpu = __atomic_load_n(&proc->runningOnCpu, __ATOMIC_ACQUIRE);
    if (cpu != -1)
    {
        // BRO-176: print SAFELY. A corrupted/freed-and-reused Process struct is a
        // prime suspect here, so do not blindly dereference proc->name (it #GP'd
        // before). Validate the magic first; dump raw struct words either way so
        // we can see whether this is a live-but-mis-stated proc or freed garbage.
        bool magicOk = (proc->magic == PROCESS_MAGIC);
        SerialPrintf("SCHED BUG: inserting RUNNING proc=%p magic=%s pid=%u runningOnCpu=%d "
                     "state=%d refCount=%d reapable=%d incarnation=%u\n",
                     (void*)proc, magicOk ? "OK" : "CORRUPT",
                     magicOk ? proc->pid : 0xFFFF, cpu, (int)proc->state,
                     (int)proc->refCount, (int)proc->reapable,
                     magicOk ? proc->incarnation : 0);
        if (magicOk)
            SerialPrintf("            name='%s' tgid=%u isThread=%d\n",
                         proc->name, proc->tgid, (int)proc->isThread);
        // Dump the first 8 words of the struct for forensic comparison against
        // PROCESS_MAGIC / freed-poison patterns.
        const uint64_t* raw = reinterpret_cast<const uint64_t*>(proc);
        for (int w = 0; w < 8; ++w)
            SerialPrintf("            [%p+0x%x] = 0x%lx\n",
                         (void*)proc, w * 8, raw[w]);
        // BRO-179 forensic: if any struct word carries the 0xDFDF poison marker,
        // decode the frame's ORIGINAL owner PID + free-seq and dump its alloc/
        // free callstack history — naming whose freed frame was reused as this
        // Process struct (the cross-domain SIG1 culprit). The poison may be the
        // HEAP's (a kfree over this live struct) or the PMM frame poison, so try
        // both decoders.
        for (int w = 0; w < 8; ++w)
        {
            HeapDecodePoison(raw[w]);
            PmmDecodePoison(raw[w]);
        }
        // BRO-176: was this exact Process* recently freed? If so, name the free
        // site — that is the premature-free path (signature 2).
        if (ProcessDumpFreeLog((void*)proc) == 0)
            SerialPrintf("            (proc not in recent free-log — not a freed-struct reuse, "
                         "or evicted from ring)\n");
        for (;;) __asm__ volatile("hlt");
    }
    SchedTrace(proc, STR_ENQUEUE);
    g_schedOps->Enqueue(g_schedState, proc->pid);
}

static void ReadyQueueRemoveLocked(Process* proc)
{
    if (proc->pid == 0) return; // idle never in policy queue
    SchedTrace(proc, STR_REMOVE);
    g_schedOps->Remove(g_schedState, proc->pid);
}

static bool ProcessCanRunOnCpu(Process* proc, uint32_t cpu)
{
    if (!proc || proc->pid == 0 || proc->isKernelThread)
        return true;
    int32_t affinity = __atomic_load_n(&proc->cpuAffinity, __ATOMIC_ACQUIRE);
    return affinity < 0 || affinity == static_cast<int32_t>(cpu);
}

// Retained for future sched_setaffinity — not called during normal scheduling
// now that TLB shootdown handles cross-CPU invalidation.
[[maybe_unused]]
static void PinUserAddressSpaceToCpu(Process* proc, uint32_t cpu)
{
    if (!proc || proc->pid == 0 || proc->isKernelThread)
        return;

    int32_t expected = -1;
    __atomic_compare_exchange_n(&proc->cpuAffinity, &expected,
                                static_cast<int32_t>(cpu),
                                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static Process* PickNextLocked(uint32_t cpu)
{
    Process* skipped[SCHED_MAX_PIDS];
    uint32_t skippedCount = 0;
    uint32_t tries = g_schedOps->ReadyCount(g_schedState);

    for (uint32_t i = 0; i < tries; ++i)
    {
        uint16_t pid = g_schedOps->PickNext(g_schedState);
        if (pid == SCHED_PID_NONE)
            break;

        // BRO-176/SIG2: bounds-check the pid BEFORE indexing g_pidToProcess.
        // PickNext is only supposed to return pids < SCHED_MAX_PIDS, but if the
        // policy state is corrupt it can return an out-of-range value; indexing
        // the 1024-entry array with it was an out-of-bounds read that returned a
        // stack/garbage pointer the scheduler then treated as a live Process*
        // (the "inserting RUNNING proc=<stack addr>" crash). Skip + log loudly.
        if (pid >= SCHED_MAX_PIDS)
        {
            static uint64_t s_lastOob = 0;
            uint64_t now = g_lapicTickCount;
            if (now - s_lastOob >= 100)
            {
                s_lastOob = now;
                SerialPrintf("SCHED: PickNext returned OOB pid=%u (>= %u) — policy state "
                             "corrupt; skipping\n", (unsigned)pid, (unsigned)SCHED_MAX_PIDS);
            }
            continue;
        }

        Process* proc = g_pidToProcess[pid];
        // SIG2: validate the mapped pointer is a plausible HEAP Process* (heap
        // starts at 0xFFFFC080..., stacks/VMALLOC live below it) with good magic
        // BEFORE dereferencing ->state. A corrupt g_pidToProcess slot holding a
        // stack/VMALLOC address would otherwise be read as a fake Ready process.
        if (proc)
        {
            uint64_t pv = reinterpret_cast<uint64_t>(proc);
            bool heapish = pv >= 0xFFFFC08000000000ULL && (pv & 0x7) == 0;
            if (!heapish || proc->magic != PROCESS_MAGIC)
            {
                static uint64_t s_lastBad = 0;
                uint64_t now = g_lapicTickCount;
                if (now - s_lastBad >= 100)
                {
                    s_lastBad = now;
                    SerialPrintf("SCHED: g_pidToProcess[%u] = %p is not a valid Process* "
                                 "(corrupt mapping); scrubbing\n", (unsigned)pid, (void*)proc);
                }
                g_pidToProcess[pid] = nullptr;  // scrub the poisoned slot
                continue;
            }
        }
        // Drop stale / non-runnable entries entirely (PickNext already
        // dequeued the pid; do NOT re-enqueue it).  A pid can linger in the
        // queue as Terminated (exit_group marks Terminated without dequeuing)
        // or reference a freed/empty slot.  Scheduling such an entry was a
        // path into the use-after-free; skip it so only genuinely Ready
        // processes are ever returned.
        if (!proc || proc->state != ProcessState::Ready)
            continue;
        // BRO-176 lost-enqueue root fix: never hand DoSwitch a process that is
        // still marked running on another CPU. RrPickNext already removed this
        // pid from the queue, and PickNext only gates on state==Ready — but a
        // process can be Ready while runningOnCpu is briefly still set during the
        // pick->switch window (runningOnCpu is cleared in context_switch.S only
        // after the old context is saved). If we returned it, DoSwitch would hit
        // its double-schedule guard, decline to run it, and (depending on a
        // racy state re-check) could fail to put it back — leaking it Ready but
        // unqueued (the schedstress slow-leak strand). Instead, re-enqueue it
        // here and keep looking. We must use the policy Enqueue directly (not
        // ReadyQueueInsertLocked, which halts on runningOnCpu!=-1); it is
        // idempotent via the queued-flag, and bounded by `tries` so an
        // all-running queue just returns null and the CPU retries next tick.
        if (__atomic_load_n(&proc->runningOnCpu, __ATOMIC_ACQUIRE) != -1)
        {
            SchedTrace(proc, STR_PICK_SKIP_RUN);
            g_schedOps->Enqueue(g_schedState, proc->pid);
            continue;
        }
        if (ProcessCanRunOnCpu(proc, cpu))
        {
            for (uint32_t j = 0; j < skippedCount; ++j)
                ReadyQueueInsertLocked(skipped[j]);
            SchedTrace(proc, STR_PICK_RETURN);
            return proc;
        }

        if (skippedCount < SCHED_MAX_PIDS)
            skipped[skippedCount++] = proc;
    }

    for (uint32_t j = 0; j < skippedCount; ++j)
        ReadyQueueInsertLocked(skipped[j]);
    return nullptr;
}

// Drain per-CPU bookkeeping that was set before a context_switch.
// Must be called after every context_switch resumption point (DoSwitch,
// ProcessTrampoline, KernelThreadTrampoline).
static void DrainPostSwitch(uint32_t cpu)
{
    // Defensive: a corrupt-stack panic was observed where this function was
    // entered with cpu = 0x80040f0c — the low 32 bits of context_switch's
    // .Lresume label.  Root cause: after .Lresume executes `ret`, the saved
    // kernel stack of the resuming thread had a return address that pointed
    // five bytes past `call SmpCurrentCpuIndex` in the post-context_switch
    // continuation, skipping the call.  EAX retained the value left by
    // context_switch's `mov 0x38(%rsi), %rax` (= the saved RIP, .Lresume),
    // and `mov %eax, %edi` propagated that into the cpu argument here.
    //
    // The underlying stack corruption is rare and not reliably reproducible.
    // Re-derive cpu from the APIC and log loudly so a recurrence is visible
    // rather than a fatal #PF on g_perCpu[cpu].pendingRetire.
    if (cpu >= SCHED_MAX_CPUS)
    {
        uint32_t recovered = SmpCurrentCpuIndex();
        SerialPrintf("SCHED: DrainPostSwitch entered with bogus cpu=0x%x — "
                     "recovering via APIC -> %u (kernel stack corruption?)\n",
                     cpu, recovered);
        cpu = (recovered < SCHED_MAX_CPUS) ? recovered : 0;
    }

    // Mark any terminated process as safe to reap — by this point the CPU's
    // RSP is on the NEW process's kernel stack, so the old stack is unused.
    Process* retired = g_perCpu[cpu].pendingRetire;
    g_perCpu[cpu].pendingRetire = nullptr;
    if (retired)
    {
        // BRO-173/175: `reapable` now means only "switched away cleanly, no
        // longer on a kernel stack".  It is DECOUPLED from whether other
        // subsystems still reference the proc — those hold a liveness refCount,
        // and the reaper additionally requires refCount==0 before freeing.  So
        // we can always mark a retired Terminated proc reapable here; the
        // compositor's VFB reference (and any kill-group reference) keeps it
        // alive via refCount until released.  Previously this deferred reapable
        // while compositorRegistered, which livelocked if the compositor never
        // unregistered the proc (BRO-175).
        __atomic_store_n(&retired->reapable, true, __ATOMIC_RELEASE);
    }

    // Re-enqueue the process we were switched away from.
    Process* toRequeue = g_perCpu[cpu].pendingRequeue;
    g_perCpu[cpu].pendingRequeue = nullptr;
    if (toRequeue)
    {
        uint64_t rlf = SchedLockAcquire(g_readyLock);
        if (toRequeue->state == ProcessState::Ready)
            ReadyQueueInsertLocked(toRequeue);
        else
            SchedTrace(toRequeue, STR_REQUEUE_SKIP);
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
    {
        // BRO-173/175: drain per-CPU post-switch bookkeeping here too.  When an
        // exiting/blocking thread switches directly to idle (PickNextLocked
        // returned nothing), idle is the resumed process — and unlike the
        // trampolines and DoSwitch it would otherwise NEVER call
        // DrainPostSwitch, so the exited thread's pendingRetire is never
        // consumed and its `reapable` flag is never set.  That left Terminated
        // threads stuck unreapable (reaper saw runCpu=-1, refCount=0, but
        // reapable=0) and livelocked the parent's wait/fork loop under heavy
        // thread churn.  Draining at the top of every idle iteration closes it.
        DrainPostSwitch(ThisCpu());
        __asm__ volatile("sti\n\thlt" ::: "memory");
    }
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
    if (!idle) KernelPanic("SCHED: OOM allocating BSP idle process");
    __builtin_memset(idle, 0, sizeof(Process));
    idle->magic = PROCESS_MAGIC;
    // Safe x87/SSE defaults for xrstor
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
    DbgPrintf("SCHED: fork child '%s' (pid %u) entering user mode rip=0x%lx rsp=0x%lx\n",
                 proc->name, proc->pid, proc->forkReturnRip, proc->forkReturnRsp);

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

    __asm__ volatile("cli" ::: "memory");

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
    // BRO-176/SIG2: g_pidToProcess[] and the policy state (RrState) are protected
    // by g_readyLock — PickNextLocked reads g_pidToProcess[pid] and the RR ops
    // mutate RrState all under it. These two writes were previously done WITHOUT
    // the lock, racing those locked readers/writers on other CPUs and corrupting
    // the intrusive ready-list (head/pid), which surfaced as a wild stack-region
    // pointer being scheduled as a Process*. Serialize them under g_readyLock.
    {
        uint64_t rlf0 = SchedLockAcquire(g_readyLock);
        if (proc->pid < SCHED_MAX_PIDS)
            g_pidToProcess[proc->pid] = proc;
        g_schedOps->InitProcess(g_schedState, proc->pid, proc->schedPriority);
        SchedLockRelease(g_readyLock, rlf0);
    }

    // BRO-173: serialize thread birth against group exit.  A new thread of an
    // already-exiting group must NOT become runnable — it was born after the
    // exit_group kill snapshot, so nothing would ever terminate it, the group
    // would never empty, and the leader/parent would livelock.  We check the
    // group leader's death-latch under g_allProcLock (the same lock
    // SchedulerKillThreadGroup sets it under), and if the group is exiting we
    // register the thread already-Terminated+reapable so it is reaped instead
    // of run.
    //
    // The check uses the thread's OWN generation-specific leader pointer, NOT a
    // scan for any process with a matching tgid: pids/tgids are reused, so a
    // tgid scan can false-positive against a still-unreaped corpse from a prior
    // generation and stillbirth a healthy new group's threads (which then hang
    // the new leader in pthread_create forever).  The leader pointer is stable
    // while it has live threads (it is parked in exit_group) and is validated
    // by magic; a dangling/invalid leader is treated as not-exiting (the normal
    // teardown paths still apply).
    bool stillborn = false;
    uint64_t alf1 = SchedLockAcquire(g_allProcLock);
    if (proc->isThread && !proc->isKernelThread)
    {
        Process* ldr = proc->threadLeader;
        if (ldr && ldr->magic == PROCESS_MAGIC
            && __atomic_load_n(&ldr->tgidExiting, __ATOMIC_ACQUIRE))
        {
            stillborn = true;
        }
    }
    if (stillborn)
    {
        proc->state = ProcessState::Terminated;
        proc->exitStatus = 0;
        __atomic_store_n(&proc->reapable, true, __ATOMIC_RELEASE);
        SerialPrintf("SCHED: thread pid=%u stillborn into exiting tgid=%u\n",
                     proc->pid, proc->tgid);
    }
    else
    {
        uint64_t rlf1 = SchedLockAcquire(g_readyLock);
        ReadyQueueInsertLocked(proc);
        SchedLockRelease(g_readyLock, rlf1);
    }

    if (g_processCount < MAX_PROCESSES)
    {
        g_allProcesses[g_processCount++] = proc;
        ++g_totalForks;
        // BRO-176: register a thread against its leader's address space at the
        // exact moment it becomes visible in g_allProcesses (so it is guaranteed
        // to be reaped via ProcessDestroy, which decrements).  This keeps the
        // leader alive — and thus the shared page table / user pages mapped —
        // until every thread sharing the address space is gone, closing the
        // pick->switch UAF where a Terminated-but-pick-pending sibling resumes
        // on a freed cr3.  Balanced 1:1 with the decrement in ProcessDestroy.
        if (proc->isThread && !proc->isKernelThread)
        {
            Process* ldr = proc->threadLeader;
            if (ldr && ldr->magic == PROCESS_MAGIC)
            {
                int32_t after = __atomic_add_fetch(&ldr->asLiveThreads, 1, __ATOMIC_ACQ_REL);
                SchedulerRecordAsLive(ldr, proc->pid, ldr->pid, (uint32_t)ldr->incarnation,
                                      (uint32_t)proc->leaderIncarnation, after, /*op=inc*/0);
            }
        }
    }
    else
    {
        SchedLockRelease(g_allProcLock, alf1);
        KernelPanic("SCHED: g_allProcesses full (MAX_PROCESSES=%u). "
                    "Process '%s' pid=%u not registered.\n",
                    MAX_PROCESSES, proc->name, proc->pid);
    }
    SchedLockRelease(g_allProcLock, alf1);

    DbgPrintf("SCHED: added '%s' (pid %u) to ready queue\n",
                 proc->name, proc->pid);

    KickIdleCpu();
}

void SchedulerRemoveProcess(Process* proc)
{
    uint64_t rlf2 = SchedLockAcquire(g_readyLock);
    SchedTrace(proc, STR_REMOVE_PROC);
    // BRO-173: ALWAYS unlink from the policy ready-queue, not just when
    // state==Ready.  A process being destroyed is typically Terminated (it was
    // reaped), but it may still be linked in the policy's intrusive
    // doubly-linked queue — e.g. a thread that was Ready (enqueued) when
    // exit_group marked it Terminated; Terminated does not dequeue.  Skipping
    // the remove here left dangling next/prev links and a stale readyCount in
    // the RR queue; once the pid was freed and reused the list corrupted,
    // PickNext then handed back a freed/wrong pid and the scheduler jumped
    // through a use-after-freed Process (0xcc-poison panic).  Remove is
    // idempotent (no-op if the pid isn't queued), so this is always safe.
    ReadyQueueRemoveLocked(proc);
    // BRO-176/SIG2: clear the pid->proc map under the SAME lock that guards it
    // (PickNextLocked reads it).  Previously this was done after the lock was
    // released, racing the locked reader and leaving a window where a reused pid
    // could observe a stale mapping.  Cleared here, before SchedulerFreePid makes
    // the pid available for reuse.
    if (proc->pid > 0 && proc->pid < SCHED_MAX_PIDS)
        g_pidToProcess[proc->pid] = nullptr;
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

    SchedulerFreePid(proc->pid);
}

void SchedulerBlock(Process* proc)
{
    // UAF guard — same rationale as in DoSwitch.
    {
        uint64_t v = (uint64_t)proc;
        if (v == 0 || (v >> 47) != 0x1FFFFULL)
        {
            KernelPanic("SCHED: SchedulerBlock proc %p not canonical kernel-half\n", proc);
        }
        if (proc->magic != PROCESS_MAGIC)
        {
            KernelPanic("SCHED: SchedulerBlock proc %p has bad magic 0x%lx — UAF?\n",
                        proc, proc->magic);
        }
    }

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
        SchedTrace(proc, STR_BLOCK_SKIP);
        SchedLockRelease(g_readyLock, rlf3);
        if (flags & 0x200)
            __asm__ volatile("sti" ::: "memory");
        return;
    }

    proc->state = ProcessState::Blocked;
    SchedTrace(proc, STR_BLOCK);
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

void SchedulerStop(Process* proc)
{
    // Like SchedulerBlock but sets Stopped instead of Blocked.
    // Used by SIGTSTP/SIGTTIN/SIGTTOU default handlers.
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    uint64_t rlf = SchedLockAcquire(g_readyLock);
    proc->state = ProcessState::Stopped;
    ReadyQueueRemoveLocked(proc);
    SchedLockRelease(g_readyLock, rlf);

    uint32_t cpu = ThisCpu();
    if (proc == g_perCpu[cpu].currentProcess)
        SchedulerYield();

    if (flags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

void SchedulerUnblock(Process* proc)
{
    uint64_t procAddr = reinterpret_cast<uint64_t>(proc);
    if (!proc || (procAddr >> 47) != 0x1FFFFULL)
        return;
    if (proc->magic != PROCESS_MAGIC)
        return;

    uint64_t rlf4 = SchedLockAcquire(g_readyLock);
    // Accept Blocked or Stopped processes for unblocking/resuming
    if (proc->state != ProcessState::Blocked && proc->state != ProcessState::Stopped)
    {
        // If the process is still Running or Ready, it's in the window between
        // setting pollWaiter (inside the socket spinlock) and calling
        // SchedulerBlock.  Set pendingWakeup so the imminent SchedulerBlock
        // returns immediately instead of sleeping.
        //
        // Running: process on another CPU, about to call SchedulerBlock.
        // Ready:   process was preempted after releasing the socket spinlock
        //          but before reaching SchedulerBlock; when it next runs it
        //          will call SchedulerBlock and must not block.
        //
        // This mirrors the KMutexUnlock pattern which sets pendingWakeup
        // directly before calling SchedulerUnblock.
        if (proc->state == ProcessState::Running ||
            proc->state == ProcessState::Ready)
            __atomic_store_n(&proc->pendingWakeup, 1, __ATOMIC_RELEASE);
        SchedTrace(proc, STR_UNBLOCK_DEFER_RR);
        SchedLockRelease(g_readyLock, rlf4);
        return;
    }
    if (proc->state == ProcessState::Blocked &&
        __atomic_load_n(&proc->runningOnCpu, __ATOMIC_ACQUIRE) != -1)
    {
        // Process is Blocked but still mid-context-switch on another CPU.
        // We can't insert it into the ready queue yet.  Set pendingWakeup
        // so CheckBlockedWakeups (timer tick) will retry the unblock once
        // the context switch completes and runningOnCpu is cleared.
        __atomic_store_n(&proc->pendingWakeup, 1, __ATOMIC_RELEASE);
        SchedTrace(proc, STR_UNBLOCK_DEFER_CPU);
        SchedLockRelease(g_readyLock, rlf4);
        return;
    }
    proc->state = ProcessState::Ready;
    proc->wakeupTick = 0;
    __atomic_store_n(&proc->pendingWakeup, 0, __ATOMIC_RELEASE);
    SchedTrace(proc, STR_READY_UNBLOCK);
    ReadyQueueInsertLocked(proc);
    SchedLockRelease(g_readyLock, rlf4);

    KickIdleCpu();
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

static bool ThreadGroupHasLivePeerLocked(Process* proc)
{
    if (!proc) return false;
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (p && p != proc && p->tgid == proc->tgid &&
            p->state != ProcessState::Terminated)
        {
            // Only non-terminated peers block leader fd cleanup and reaping.
            // Threads share the page table with the leader — destroying it
            // while a running/sleeping thread still references it causes
            // corruption. Once a peer is Terminated it no longer executes,
            // so the leader can safely close its fds (pipe write ends, etc.)
            // without waiting for the peer to be reaped from g_allProcesses.
            return true;
        }
    }
    return false;
}

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
            && __atomic_load_n(&p->runningOnCpu, __ATOMIC_ACQUIRE) == -1
            && __atomic_load_n(&p->refCount, __ATOMIC_ACQUIRE) == 0
            && p != g_perCpu[cpu].currentProcess)
        {
            if (!p->isThread && ThreadGroupHasLivePeerLocked(p))
            {
                ++i;
                continue;
            }

            // Threads (isThread=true) are never wait()-able children —
            // auto-reap them regardless of parent state. Without this,
            // c-ares resolver threads (CLONE_THREAD) from curl accumulate
            // as zombies because parentPid points to the grandparent
            // (nix-fetch) which is alive and never calls waitpid for them.
            if (p->isThread)
            {
                // Thread — auto-reap immediately
            }
            // If parentPid != 0, check if the parent still exists.
            // If the parent is gone, reparent to 0 so we can reap.
            else if (p->parentPid != 0)
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

            // BRO-176: do NOT reap a thread-group LEADER (which frees the shared
            // user address space in ProcessDestroy) while any thread sharing that
            // address space is still un-reaped.  asLiveThreads counts those
            // threads, INCLUDING a Terminated sibling still in the pick->switch
            // window (picked Ready, runningOnCpu transiently -1) — which slips
            // past both ThreadGroupHasLivePeerLocked (it is Terminated) and the
            // Phase-2 quiesce-wait (its runningOnCpu reads -1).  Gating the
            // leader's reap here keeps the shared page table + user pages alive
            // until every such thread has been switched-away and reaped, so a
            // thread resuming via DoSwitch never loads a freed cr3 (BRO-176 UAF).
            if (!p->isThread &&
                __atomic_load_n(&p->asLiveThreads, __ATOMIC_ACQUIRE) > 0)
            {
                ++i;
                continue;
            }

            SchedLockRelease(g_allProcLock, alf);
            DbgPrintf("SCHED: reaping '%s' (pid %u) compReg=%d reapable=%d\n",
                       p->name, p->pid,
                       __atomic_load_n(&p->compositorRegistered, __ATOMIC_ACQUIRE),
                       __atomic_load_n(&p->reapable, __ATOMIC_ACQUIRE));
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
static void DoSwitch(Process* oldProc, Process* newProc, bool requeueOld = false,
                     bool holdingReadyLock = false)
{
    __asm__ volatile("cli");

    // Use-after-free / corruption guard.  Both pointers must be canonical
    // kernel-half addresses pointing at a live Process struct.  If a stale
    // pointer made it onto the run queue or wakeup list, deref'ing fields
    // here would otherwise produce a non-canonical fault deep inside the
    // dispatch path; the magic check turns that into a clear, attributable
    // panic naming the offending pointer.
    auto validate = [](Process* p, const char* who) {
        uint64_t v = (uint64_t)p;
        if (v == 0 || (v >> 47) != 0x1FFFFULL)
        {
            KernelPanic("SCHED: %s pointer %p not canonical kernel-half\n", who, p);
        }
        if (p->magic != PROCESS_MAGIC)
        {
            KernelPanic("SCHED: %s=%p has bad magic 0x%lx (expected 0x%lx) — UAF?\n",
                        who, p, p->magic, PROCESS_MAGIC);
        }
        // State must be a valid enum value (0..4 in our enum).
        uint32_t s = (uint32_t)p->state;
        if (s > 4)
        {
            KernelPanic("SCHED: %s=%p (pid=%u) has corrupt state=%u\n",
                        who, p, p->pid, s);
        }
    };
    validate(oldProc, "oldProc");
    validate(newProc, "newProc");

    uint32_t cpu = ThisCpu();

    // Double-schedule detection: newProc must not already be running on another CPU.
    int32_t prevCpu = __atomic_exchange_n(&newProc->runningOnCpu, (int32_t)cpu, __ATOMIC_ACQ_REL);
    if (prevCpu != -1)
    {
        // Restore the original runningOnCpu — we're not taking this process.
        __atomic_store_n(&newProc->runningOnCpu, prevCpu, __ATOMIC_RELEASE);

        // Held-lock dispatch invariant: when the caller hands us a process it
        // picked under g_readyLock (still held here), that process was removed
        // from the policy ready queue AND verified runningOnCpu==-1 inside the
        // same uninterrupted IF=0 critical section. Nothing can have claimed it
        // since. If prevCpu!=-1 fires anyway the lock discipline is broken — and
        // the lock-free bail path below would self-deadlock re-acquiring
        // g_readyLock. Fail loudly instead.
        if (holdingReadyLock)
        {
            KernelPanic("SCHED: held-lock claim race on pid=%u: runningOnCpu=%d, "
                        "expected -1 (g_readyLock discipline broken)",
                        newProc->pid, prevCpu);
        }

        // BRO-176 lost-enqueue fix: newProc was REMOVED from the ready queue by
        // the caller's PickNextLocked (PickNext only checks state==Ready, NOT
        // runningOnCpu, so a process that is Ready but still marked running on
        // another CPU during the pick->switch window can be selected here). We
        // are declining to run it. If we just return, newProc is left
        // state==Ready but absent from the policy ready queue — leaked forever
        // (the schedstress slow-leak strand the SCHED STALL/SCHEDTRACE caught:
        // workers vanish one at a time and throughput decays). Put it back.
        //
        // We must enqueue via the policy directly: ReadyQueueInsertLocked would
        // trip its runningOnCpu!=-1 halt-guard (we just restored
        // runningOnCpu=prevCpu). That is safe — the policy's queued-flag makes
        // Enqueue idempotent (no double-link if prevCpu also requeues it), and
        // DoSwitch's own double-schedule guard prevents any premature pick from
        // actually running it while prevCpu still owns it. If newProc is
        // concurrently blocked/terminated, a later PickNext drops it from the
        // queue (it checks state==Ready), so a stale enqueue self-heals.
        if (newProc->state == ProcessState::Ready && newProc->pid != 0)
        {
            uint64_t rlfDs = SchedLockAcquire(g_readyLock);
            g_schedOps->Enqueue(g_schedState, newProc->pid);
            SchedLockRelease(g_readyLock, rlfDs);
            SchedTrace(newProc, STR_DS_BAIL_ENQ);
        }
        else
        {
            SchedTrace(newProc, STR_DS_BAIL_SKIP);
        }

        // Rate-limit: this path fires repeatedly when a hot process is
        // contended between CPUs (e.g. a long-running nar-unpack).  It's a
        // benign avoided-race, not a bug; log once per second per CPU.
        static uint64_t lastLogMs[SCHED_MAX_CPUS] = {};
        uint64_t nowMs = g_lapicTickCount;
        if (nowMs - lastLogMs[cpu] >= 1000)
        {
            lastLogMs[cpu] = nowMs;
            SerialPrintf("SCHED: double-schedule avoided: '%s' (pid %u) on CPU%d, "
                         "CPU%u will retry. oldProc='%s' pid=%u\n",
                         newProc->name, newProc->pid, prevCpu, cpu,
                         oldProc->name, oldProc->pid);
        }

        // If old process can still run, just continue it.
        if (oldProc->state == ProcessState::Running ||
            oldProc->state == ProcessState::Ready)
        {
            oldProc->state = ProcessState::Running;
            __asm__ volatile("sti");
            return;
        }

        // Old process is blocked/terminated — switch to our own idle.
        Process* idle = g_perCpu[cpu].idleProcess;
        if (idle && idle != newProc)
        {
            newProc = idle;
            int32_t idlePrev = __atomic_exchange_n(&idle->runningOnCpu, (int32_t)cpu, __ATOMIC_ACQ_REL);
            if (idlePrev != -1)
            {
                // Even our idle is busy — just hlt and wait for next tick.
                __atomic_store_n(&idle->runningOnCpu, idlePrev, __ATOMIC_RELEASE);
                __asm__ volatile("sti");
                return;
            }
        }
        else
        {
            __asm__ volatile("sti");
            return;
        }
    }

    g_perCpu[cpu].currentProcess = newProc;
    if (g_perCpu[cpu].cpuEnv) {
        g_perCpu[cpu].cpuEnv->currentPid = newProc->pid;
        g_perCpu[cpu].cpuEnv->currentProcess = reinterpret_cast<uint64_t>(newProc);
    }
    newProc->state = ProcessState::Running;
    SchedTrace(newProc, STR_RUN);
    __atomic_store_n(&newProc->runningOnCpu, static_cast<int32_t>(cpu), __ATOMIC_RELEASE);
    // Track which CPUs have this process's TLB entries loaded
    __atomic_or_fetch(&newProc->tlbCpuMask, 1ULL << cpu, __ATOMIC_RELEASE);
    // BRO-176/SIG1: also record this CPU in the address-space footprint mask
    // (leader-owned union) so shootdowns reach every sibling-thread CPU.
    __atomic_or_fetch(AddressSpaceTlbMaskPtr(newProc), 1ULL << cpu, __ATOMIC_RELEASE);
    g_perCpu[cpu].sliceStartTick = g_lapicTickCount;

    // Store requeue info in per-CPU state BEFORE context_switch.
    g_perCpu[cpu].pendingRequeue = requeueOld ? oldProc : nullptr;

    // BRO-173/175: if we are switching away from a process that is already
    // Terminated, hand it to the retire path so the resuming process's
    // DrainPostSwitch marks it reapable once it is fully off its kernel stack.
    // This closes a race in the self-exit path: SchedulerExitCurrentProcess
    // sets state=Terminated with interrupts still ENABLED (it does SIGCHLD /
    // reparent work under g_allProcLock before the cli + its own pendingRetire
    // store).  A timer tick landing in that window descheduls the Terminated
    // proc through here (SchedulerTimerTick's Terminated branch) — it would
    // otherwise be switched out Terminated/runningOnCpu=-1 but never marked
    // reapable (its own pendingRetire store at the tail of
    // SchedulerExitCurrentProcess is never reached), wedging the reaper and
    // livelocking the parent's wait/fork loop.  The leader hits this far more
    // often than a plain thread because of the extra SIGCHLD/reparent work that
    // widens the interrupts-enabled window.
    if (oldProc != newProc &&
        oldProc->state == ProcessState::Terminated &&
        !g_perCpu[cpu].pendingRetire)
    {
        g_perCpu[cpu].pendingRetire = oldProc;
    }

    // Held-lock dispatch release point. The run-queue decision is now fully
    // committed under one IF=0 hold of g_readyLock: newProc was de-queued in
    // PickNextLocked, claimed (runningOnCpu=cpu), marked Running, and the
    // outgoing requeue/retire intents are recorded in per-CPU state. Everything
    // remaining (TSS/syscall-stack/CR3 setup + context_switch) is per-CPU and
    // intentionally runs with IF still 0 — the resumed thread restores its own
    // IF. Drop the lock RAW (no sti) so we never re-enable interrupts in the
    // pick->switch gap that caused the BRO-176 lost-enqueue strand.
    if (holdingReadyLock)
        SchedLockReleaseRaw(g_readyLock);

    GdtSetTssRsp0ForCpu(cpu, newProc->kernelStackTop);
    SetSyscallStack(cpu, newProc->kernelStackTop);

    // Validate XSAVE area alignment.
    auto oldFxAddr = reinterpret_cast<uintptr_t>(&oldProc->fxsave);
    auto newFxAddr = reinterpret_cast<uintptr_t>(&newProc->fxsave);
    if ((oldFxAddr & 0x3F) || (newFxAddr & 0x3F))
    {
        SerialPrintf("SCHED FATAL: XSAVE area misaligned! old=%p new=%p\n",
                     (void*)oldFxAddr, (void*)newFxAddr);
        for (;;) __asm__ volatile("hlt");
    }

    // Clear old process's TLB CPU mask bit — after CR3 switch, this CPU's TLB
    // no longer has the old process's entries (different address spaces).
    if (oldProc != newProc)
        __atomic_and_fetch(&oldProc->tlbCpuMask, ~(1ULL << cpu), __ATOMIC_RELEASE);

    // Update per-CPU currentCr3 so TLB shootdown can determine which CPUs
    // are running a given address space (critical for CoW fork correctness).
    SmpSetCurrentCr3(cpu, newProc->savedCtx.cr3);

    ProfilerContextSwitch(oldProc->pid, newProc->pid);

    context_switch(&oldProc->savedCtx, &newProc->savedCtx,
                   &oldProc->fxsave, &newProc->fxsave,
                   &oldProc->runningOnCpu);

    // --- We return here when another CPU (or this one) switches back to us ---

    DrainPostSwitch(ThisCpu());
}

void SchedulerTimerTick(bool allowPreempt)
{
    if (!g_schedulerRunning)
        return;

    // Drain pendingRetire on EVERY CPU, every tick. Without this, a process
    // that exits on CPU N while idle is running may leave pendingRetire set
    // until the next DoSwitch — which might not happen for a long time if
    // no processes are ready. The child's reapable flag stays false and
    // waitpid spins forever.
    uint32_t cpu = ThisCpu();
    DrainPostSwitch(cpu);

    // Only BSP (CPU 0) does wakeup checks, reaping, and policy ticks.
    if (cpu == 0)
    {
        CheckBlockedWakeups();

        // Check alarm timers for all processes
        uint64_t now = g_lapicTickCount;
        uint64_t alf_alarm = SchedLockAcquire(g_allProcLock);
        for (uint32_t i = 0; i < g_processCount; ++i)
        {
            Process* p = g_allProcesses[i];
            if (p->alarmTick != 0 && now >= p->alarmTick &&
                p->state != ProcessState::Terminated)
            {
                p->alarmTick = 0; // one-shot
                ProcessSendSignal(p, 14); // SIGALRM
            }
        }
        SchedLockRelease(g_allProcLock, alf_alarm);

        ReapTerminated();

        // Periodically verify device registry integrity (~every 1000 ticks ≈ 1s).
        // This catches silent BSS corruption early before it causes a #GP.
        static uint32_t s_devCheckCounter = 0;
        if (++s_devCheckCounter >= 1000)
        {
            s_devCheckCounter = 0;
            DeviceCheckIntegrity();
        }

        // Notify policy of time passing (for anti-starvation boosts etc.).
        uint64_t rlf_tick = SchedLockAcquire(g_readyLock);
        g_schedOps->Tick(g_schedState, g_lapicTickCount);
        SchedLockRelease(g_readyLock, rlf_tick);

        // Sample load average every 5 seconds (on CPU 0 only)
        if (g_lapicTickCount % LOAD_SAMPLE_INTERVAL == 0)
        {
            uint32_t running = 0;
            uint64_t alf_load = SchedLockAcquire(g_allProcLock);
            for (uint32_t i = 0; i < g_processCount; ++i)
            {
                Process* p = g_allProcesses[i];
                if (!p) continue;
                if (p->state == ProcessState::Ready || p->state == ProcessState::Running)
                    ++running;
            }
            SchedLockRelease(g_allProcLock, alf_load);

            uint32_t sample = running * 1000;
            g_loadAvg1  = (g_loadAvg1  * LOAD_DECAY_1  + sample * (1000 - LOAD_DECAY_1))  / 1000;
            g_loadAvg5  = (g_loadAvg5  * LOAD_DECAY_5  + sample * (1000 - LOAD_DECAY_5))  / 1000;
            g_loadAvg15 = (g_loadAvg15 * LOAD_DECAY_15 + sample * (1000 - LOAD_DECAY_15)) / 1000;
        }
    }

    Process* cur = g_perCpu[cpu].currentProcess;
    if (!cur)
        return;

    // CPU time accounting: charge one tick to the running process.
    // allowPreempt is true when the timer interrupted user mode (ring 3).
    if (cur != g_perCpu[cpu].idleProcess)
    {
        if (allowPreempt)
            cur->userTicks++;
        else
            cur->sysTicks++;
        g_perCpu[cpu].busyTicks++;
    }
    else
    {
        g_perCpu[cpu].idleTicks++;
    }

    // Idle — if something became ready, switch to it.
    if (cur == g_perCpu[cpu].idleProcess)
    {
        uint64_t rlf7 = SchedLockAcquire(g_readyLock);
        Process* next = PickNextLocked(cpu);
        uint32_t readyN = g_schedOps->ReadyCount(g_schedState);
        if (next)
        {
            // Hold g_readyLock through DoSwitch's claim — it releases the lock
            // (raw, IF stays 0) right before context_switch.
            DoSwitch(cur, next, /* requeueOld */ false, /* holdingReadyLock */ true);
        }
        else
        {
            SchedLockRelease(g_readyLock, rlf7);
            if (cpu == 0)
            {
            // BRO-176 HANG detector: this CPU is idle and PickNext found nothing,
            // yet a process may be marked state==Ready but missing from the policy
            // ready queue — a genuine strand would idle every CPU forever. But a
            // process is LEGITIMATELY Ready-but-unqueued for a brief window during
            // preemption: SchedulerPreempt/Yield set state=Ready and stash the proc
            // in g_perCpu[].pendingRequeue; DrainPostSwitch enqueues it a few
            // instructions later on the resuming CPU. Sampling during that window is
            // a FALSE positive. To report only REAL strands, require (a) the proc is
            // not any CPU's pendingRequeue, not running, and (b) the SAME pid stays
            // stranded across two consecutive checks (the rate-limit interval is far
            // longer than the requeue window, which never survives it).
            static uint64_t s_lastStallLog = 0;
            static uint16_t s_lastStrandedPid = SCHED_PID_NONE;
            uint64_t now = g_lapicTickCount;
            if (now - s_lastStallLog >= 500)
            {
                uint64_t alf = SchedLockAcquire(g_allProcLock);
                Process* stranded = nullptr;
                for (uint32_t i = 0; i < g_processCount; ++i)
                {
                    Process* p = g_allProcesses[i];
                    if (!(p && p->magic == PROCESS_MAGIC
                          && p->state == ProcessState::Ready
                          && ProcessCanRunOnCpu(p, cpu)))
                        continue;
                    // Skip if running or mid-context-switch on any CPU.
                    if (__atomic_load_n(&p->runningOnCpu, __ATOMIC_ACQUIRE) != -1)
                        continue;
                    // Skip the legitimate preemption requeue window: a proc parked
                    // in any CPU's pendingRequeue is about to be enqueued by that
                    // CPU's DrainPostSwitch — not a strand.
                    bool pendingRequeue = false;
                    for (uint32_t c = 0; c < SCHED_MAX_CPUS; ++c)
                        if (g_perCpu[c].pendingRequeue == p) { pendingRequeue = true; break; }
                    if (pendingRequeue)
                        continue;
                    stranded = p;
                    break;
                }
                SchedLockRelease(g_allProcLock, alf);
                // Only log a strand that PERSISTS: the same pid must be caught on
                // two consecutive checks (>=500 ticks apart). A transient requeue
                // window cannot survive one interval, so this kills false positives.
                uint16_t prevStrandedPid = s_lastStrandedPid;
                s_lastStrandedPid = stranded ? stranded->pid : SCHED_PID_NONE;
                s_lastStallLog = now;
                if (stranded && stranded->pid == prevStrandedPid)
                {
                    SerialPrintf("SCHED STALL: pid=%u '%s' state=Ready but PickNext "
                                 "returned null (RR readyCount=%u) — persists, real strand!\n",
                                 stranded->pid, stranded->name, readyN);
                    // Dump the policy's internal view to distinguish a LOGIC desync
                    // (queued flag stale) from STRUCT CORRUPTION (listLen != readyCount).
                    if (g_schedOps->DebugDump)
                    {
                        brook::SchedDebugInfo di = {};
                        uint64_t rlfd = SchedLockAcquire(g_readyLock);
                        g_schedOps->DebugDump(g_schedState, stranded->pid, &di);
                        SchedLockRelease(g_readyLock, rlfd);
                        SerialPrintf("  RR[pid=%u]: queued=%u active=%u next=%u prev=%u | "
                                     "head=%u tail=%u readyCount=%u listLen=%u%s\n",
                                     stranded->pid, di.queued, di.active, di.nextPid,
                                     di.prevPid, di.head, di.tail, di.readyCount,
                                     di.listLen,
                                     (di.listLen != di.readyCount)
                                         ? "  <<< STRUCT CORRUPT (listLen != readyCount)"
                                         : (di.queued
                                             ? "  <<< LOGIC DESYNC (queued but not picked)"
                                             : "  <<< NOT ENQUEUED (queued=0)"));
                    }
                    // Dump the stranded process's recent scheduler-event ring:
                    // the op sequence reveals exactly where state became Ready
                    // without a following ENQUEUE (the lost-enqueue site).
                    SerialPrintf("  SCHEDTRACE pid=%u (oldest->newest):\n", stranded->pid);
                    for (uint32_t k = 0; k < 12; ++k)
                    {
                        uint8_t slot = (uint8_t)((stranded->schedTraceHead + k) % 12);
                        uint64_t e = stranded->schedTrace[slot];
                        if (e == 0) continue;
                        SerialPrintf("    site=%u state=%u tick=%lu\n",
                                     (unsigned)((e >> 8) & 0xFF),
                                     (unsigned)(e & 0xFF),
                                     (unsigned long)(e >> 16));
                    }
                    SerialPrintf("  (sites 1=ENQ 2=REM 3=RDY_UNBLK 4=DEFER_RR 5=DEFER_CPU "
                                 "6=BLOCK 7=BLOCK_SKIP 8=PREEMPT 9=YIELD 10=REQ_ENQ "
                                 "11=REQ_SKIP 12=RUN 13=PICK_RET 14=PICK_SKIP_RUN "
                                 "15=DS_BAIL_ENQ 16=DS_BAIL_SKIP 17=REM_PROC)\n");
                }
            }
            }
        }
        return;
    }

    // A Terminated process MUST be descheduled immediately, regardless of
    // whether the tick interrupted user or kernel mode.  exit_group /
    // SchedulerKillThreadGroup marks sibling threads Terminated and then
    // spin-waits for runningOnCpu == -1.  If the victim is a pure user-mode
    // CPU spinner, ticks arrive with allowPreempt=true and the old code fell
    // through to the timeslice path, where `cur->state != Running` returned
    // WITHOUT descheduling — so the Terminated spinner kept burning its CPU
    // forever, the quiesce-wait never completed, and the teardown
    // reference/reap was leaked (BRO-173/175 residual stall).  Handle it here,
    // up front, for both modes.
    if (cur->state == ProcessState::Terminated)
    {
        uint64_t rlf_term = SchedLockAcquire(g_readyLock);
        ReadyQueueRemoveLocked(cur);
        Process* next = PickNextLocked(cpu);
        if (next)
        {
            DoSwitch(cur, next, /* requeueOld */ false, /* holdingReadyLock */ true);
        }
        else
        {
            SchedLockRelease(g_readyLock, rlf_term);
            Process* idle = g_perCpu[cpu].idleProcess;
            if (idle && idle != cur)
                DoSwitch(cur, idle, /* requeueOld */ false);
        }
        return;
    }

    // Brook currently treats kernel code as non-preemptible. Timer ticks still
    // sample, account, wake sleepers, and dispatch away from idle, but a tick
    // that interrupted a syscall/driver path must not deschedule the process
    // while it owns filesystem, VFS, or device-driver locks.
    if (!allowPreempt)
        return;

    // Check timeslice (per-process, from policy module).
    uint64_t timeslice = g_schedOps->Timeslice(g_schedState, cur->pid);
    if (g_lapicTickCount - g_perCpu[cpu].sliceStartTick < timeslice)
        return;

    // Only preempt if the process is still Running. It might have been
    // marked Blocked (by SchedulerBlock in a syscall) between the lock
    // release and the yield — the timer fired in that window.
    // Also deschedule Stopped processes (SIGTSTP/SIGSTOP).
    if (cur->state == ProcessState::Stopped)
    {
        // Stopped process — remove from ready queue and switch away
        uint64_t rlf_stop = SchedLockAcquire(g_readyLock);
        ReadyQueueRemoveLocked(cur);
        Process* next = PickNextLocked(cpu);
        if (next)
        {
            DoSwitch(cur, next, /* requeueOld */ false, /* holdingReadyLock */ true);
        }
        else
        {
            SchedLockRelease(g_readyLock, rlf_stop);
            // No other process — switch to idle
            Process* idle = g_perCpu[cpu].idleProcess;
            if (idle && idle != cur)
                DoSwitch(cur, idle, /* requeueOld */ false);
        }
        return;
    }
    if (cur->state != ProcessState::Running)
        return;

    // Timeslice expired — notify policy, pick next, and switch.
    uint64_t rlf8 = SchedLockAcquire(g_readyLock);
    g_schedOps->TimesliceExpired(g_schedState, cur->pid);
    Process* next = PickNextLocked(cpu);

    if (!next)
    {
        SchedLockRelease(g_readyLock, rlf8);
        // Nothing else — keep running.
        g_perCpu[cpu].sliceStartTick = g_lapicTickCount;
        return;
    }

    // state=Ready set under g_readyLock; DoSwitch claims next + releases the
    // lock (raw) right before context_switch.
    cur->state = ProcessState::Ready;
    SchedTrace(cur, STR_READY_PREEMPT);
    DoSwitch(cur, next, /* requeueOld */ true, /* holdingReadyLock */ true);
}

void SchedulerYield()
{
    uint32_t cpu = ThisCpu();
    Process* old = g_perCpu[cpu].currentProcess;
    if (!old)
        return;
    uint64_t rlf9 = SchedLockAcquire(g_readyLock);
    Process* next = PickNextLocked(cpu);

    if (!next)
    {
        SchedLockRelease(g_readyLock, rlf9);
        // If the process is Blocked/Terminated, it must NOT continue running.
        // Switch to the idle process so the CPU is available for other work
        // and the blocked process can be properly rescheduled when unblocked.
        if (old->state == ProcessState::Blocked ||
            old->state == ProcessState::Terminated ||
            old->state == ProcessState::Stopped)
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
    {
        old->state = ProcessState::Ready;
        SchedTrace(old, STR_READY_YIELD);
    }
    // DoSwitch claims next + releases g_readyLock (raw) before context_switch.
    DoSwitch(old, next, requeue, /* holdingReadyLock */ true);
}

extern "C" void SchedulerSleepMs(uint32_t ms)
{
    if (ms == 0) { SchedulerYield(); return; }

    Process* proc = ProcessCurrent();
    // Pre-scheduler / kernel-context with no current process: best-effort
    // busy spin so we don't deadlock early boot callers.
    if (!proc)
    {
        for (volatile uint32_t i = 0; i < ms * 25000; i++)
            __asm__ volatile("pause");
        return;
    }

    proc->wakeupTick = g_lapicTickCount + ms;
    SchedulerBlock(proc);
}

[[noreturn]] void SchedulerExitCurrentProcess(int status)
{
    uint32_t cpu = ThisCpu();
    Process* proc = g_perCpu[cpu].currentProcess;

    // Release any held kernel rwlocks / remove from wait queues.
    KRwLockCleanupOnExit(proc);

    char procName[33];
    CopyProcessNameForLog(proc, procName);
    if (status != 0) {
        SerialPrintf("SCHED: '%s' (pid %u, tgid %u) exited with status %d%s\n",
                     procName, proc->pid, proc->tgid, status,
                     proc->isThread ? " [thread]" : "");
    } else {
        DbgPrintf("SCHED: '%s' (pid %u, tgid %u) exited with status %d%s\n",
                  procName, proc->pid, proc->tgid, status,
                  proc->isThread ? " [thread]" : "");
    }

    // Thread exit: clear_child_tid + futex_wake for pthread_join
    if (proc->clearChildTid)
    {
        // Verify the address is still mapped. After execve the old address
        // space is gone; if clearChildTid wasn't cleared by the exec path we'd
        // fault here. Treat an unmapped tidPtr as "nothing to do" rather than
        // panicking the kernel.
        PhysicalAddress tidPhys = VmmVirtToPhys(proc->pageTable,
            VirtualAddress(proc->clearChildTid));
        if (tidPhys)
        {
            auto* tidPtr = reinterpret_cast<volatile uint32_t*>(proc->clearChildTid);
            __atomic_store_n(tidPtr, 0, __ATOMIC_RELEASE);
            // Wake any thread waiting in futex(FUTEX_WAIT) on this address
            // (pthread_join blocks on this via FUTEX_WAIT)
            FutexWake(proc->tgid, proc->clearChildTid, 1);
        }
        else
        {
            SerialPrintf("SCHED: skip clearChildTid=0x%lx (unmapped) pid=%u\n",
                         proc->clearChildTid, proc->pid);
        }
    }

    bool hasLiveThreadPeer = false;
    if (!proc->isThread)
    {
        uint64_t alf = SchedLockAcquire(g_allProcLock);
        hasLiveThreadPeer = ThreadGroupHasLivePeerLocked(proc);
        SchedLockRelease(g_allProcLock, alf);
    }

    // Only do full process cleanup for non-thread (group leader) processes
    // after all sibling threads are gone. A plain sys_exit from the leader does
    // not terminate the thread group, and shared fds/address-space state must
    // remain valid for still-running pthreads.
    if (!proc->isThread)
    {
        if (!hasLiveThreadPeer)
        {
            // Close all FDs immediately so pipe readers/writers get unblocked.
            ProcessCloseAllFds(proc);
        }

        // Reparent any children of this process to init (parentPid=0).
        {
            uint64_t alf = SchedLockAcquire(g_allProcLock);
            for (uint32_t i = 0; i < g_processCount; i++)
            {
                if (g_allProcesses[i]->parentPid == proc->pid)
                    g_allProcesses[i]->parentPid = 0;
            }
            SchedLockRelease(g_allProcLock, alf);
        }

        // Signal the compositor
        if (proc->fbVfbWidth > 0)
            proc->fbExitColor = (status < 0) ? 0x00CC0000u : 0x00001A3Au;
        proc->fbVirtual = nullptr;
        proc->fbVirtualSize = 0;
    }

    proc->state = ProcessState::Terminated;
    proc->exitStatus = status;

    // Wake the parent process if it's blocked (likely in wait4).
    // Also send SIGCHLD to the parent.
    // Threads (isThread=true) do NOT send SIGCHLD — only process exits do.
    // In Linux, SIGCHLD is sent when the thread group leader exits, not
    // individual threads. Threads only wake their leader via futex on
    // clear_child_tid (handled above).
    if (!proc->isThread && proc->parentPid != 0)
    {
        // Send SIGCHLD (17) to the parent.
        constexpr int SIGCHLD = 17;
        uint64_t bit = 1ULL << (SIGCHLD - 1);

        uint64_t alf = SchedLockAcquire(g_allProcLock);
        for (uint32_t i = 0; i < g_processCount; i++)
        {
            if (g_allProcesses[i]->pid == proc->parentPid)
            {
                Process* parent = g_allProcesses[i];

                // Mutate `parent` ONLY while holding g_allProcLock. The reaper
                // frees a Process strictly after SchedulerRemoveProcess pulls
                // it out of g_allProcesses under this same lock, so a parent we
                // found in the array here cannot be freed until we release —
                // closing the use-after-free (BRO-158).
                //
                // We must NOT call SchedulerUnblock (which takes g_readyLock)
                // while holding g_allProcLock: SchedulerSetPolicy establishes
                // the g_readyLock -> g_allProcLock order, so the reverse would
                // deadlock. Instead set pendingWakeup and let the BSP's
                // CheckBlockedWakeups perform the unblock on the next tick. That
                // path runs on CPU 0 right alongside ReapTerminated, so it can
                // never race the reaper freeing the parent. SchedulerBlock also
                // honours pendingWakeup, so a parent racing into wait4 won't
                // miss the signal.
                __atomic_or_fetch(&parent->sigPending, bit, __ATOMIC_RELEASE);
                __atomic_store_n(&parent->pendingWakeup, 1, __ATOMIC_RELEASE);
                break;
            }
        }
        SchedLockRelease(g_allProcLock, alf);
    }

    SchedLockAcquire(g_readyLock);
    ReadyQueueRemoveLocked(proc);
    Process* next = PickNextLocked(cpu);

    if (!next) next = g_perCpu[cpu].idleProcess;

    // Held-lock exit dispatch: keep g_readyLock from the pick through the claim
    // of `next` (runningOnCpu=cpu) so there is no window in which `next` is out
    // of the ready queue but not yet owned — the BRO-176 lost-enqueue race. The
    // lock is dropped RAW (IF stays 0) right before context_switch, matching
    // DoSwitch. Interrupts are already disabled by SchedLockAcquire; the cli
    // below is redundant but harmless.
    __asm__ volatile("cli" ::: "memory");

    __atomic_store_n(&proc->runningOnCpu, (int32_t)-1, __ATOMIC_RELEASE);
    // Exiting process will never run again — clear its TLB CPU mask
    __atomic_and_fetch(&proc->tlbCpuMask, ~(1ULL << cpu), __ATOMIC_RELEASE);

    g_perCpu[cpu].currentProcess = next;
    if (g_perCpu[cpu].cpuEnv) {
        g_perCpu[cpu].cpuEnv->currentPid = next->pid;
        g_perCpu[cpu].cpuEnv->currentProcess = reinterpret_cast<uint64_t>(next);
    }
    next->state = ProcessState::Running;
    __atomic_store_n(&next->runningOnCpu, (int32_t)cpu, __ATOMIC_RELEASE);
    __atomic_or_fetch(&next->tlbCpuMask, 1ULL << cpu, __ATOMIC_RELEASE);
    __atomic_or_fetch(AddressSpaceTlbMaskPtr(next), 1ULL << cpu, __ATOMIC_RELEASE);
    // Keep per-CPU currentCr3 in sync with the address space we're switching to.
    // Without this the value stays stale (= the exiting process's CR3), and the
    // TLB-shootdown timeout-forgiveness path (apic.cpp) would mis-read it: a CPU
    // actually running `next` could be wrongly "forgiven" (its stale CR3 not
    // matching the shootdown target), leaving a stale writable COW TLB entry —
    // a COW double-free / UAF. DoSwitch updates this; this exit path must too.
    SmpSetCurrentCr3(cpu, next->savedCtx.cr3);
    g_perCpu[cpu].sliceStartTick = g_lapicTickCount;
    GdtSetTssRsp0ForCpu(cpu, next->kernelStackTop);
    SetSyscallStack(cpu, next->kernelStackTop);

    // Mark this terminated process for deferred reap — DrainPostSwitch on the
    // resumed process will set reapable once the context_switch is complete
    // and this kernel stack is no longer in use.
    g_perCpu[cpu].pendingRetire = proc;

    // Drop g_readyLock RAW now that `next` is fully claimed and committed; IF
    // stays 0 across the switch (the resumed thread restores its own IF).
    SchedLockReleaseRaw(g_readyLock);

    ProfilerContextSwitch(proc->pid, next->pid);
    context_switch(&proc->savedCtx, &next->savedCtx,
                   &proc->fxsave, &next->fxsave,
                   &proc->runningOnCpu);

    __builtin_unreachable();
}

[[noreturn]] void SchedulerStart()
{
    SerialPrintf("SCHED: starting scheduler, %u processes ready\n",
                 SchedulerReadyCount());

    uint32_t cpu = ThisCpu();

    uint64_t rlf11 = SchedLockAcquire(g_readyLock);
    Process* first = PickNextLocked(cpu);
    SchedLockRelease(g_readyLock, rlf11);

    if (!first) first = g_perCpu[cpu].idleProcess;

    g_perCpu[cpu].currentProcess = first;
    if (g_perCpu[cpu].cpuEnv) { g_perCpu[cpu].cpuEnv->currentPid = first->pid; g_perCpu[cpu].cpuEnv->currentProcess = reinterpret_cast<uint64_t>(first); }
    
    first->state = ProcessState::Running;
    __atomic_store_n(&first->runningOnCpu, (int32_t)cpu, __ATOMIC_RELEASE);
    __atomic_or_fetch(&first->tlbCpuMask, 1ULL << cpu, __ATOMIC_RELEASE);
    __atomic_or_fetch(AddressSpaceTlbMaskPtr(first), 1ULL << cpu, __ATOMIC_RELEASE);
    SmpSetCurrentCr3(cpu, first->savedCtx.cr3);  // keep tracking in sync (see exit path)
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
    // Use hlt (woken by LAPIC timer) instead of pause to avoid starving
    // the BSP which still needs to finish boot before calling SchedulerStart.
    __asm__ volatile("sti");
    while (!__atomic_load_n(&g_schedulerRunning, __ATOMIC_ACQUIRE))
        __asm__ volatile("hlt" ::: "memory");

    // Try to pick a process from the global queue.
    uint64_t rlf12 = SchedLockAcquire(g_readyLock);
    Process* first = PickNextLocked(cpu);
    SchedLockRelease(g_readyLock, rlf12);

    if (!first) first = g_perCpu[cpu].idleProcess;

    g_perCpu[cpu].currentProcess = first;
    if (g_perCpu[cpu].cpuEnv) { g_perCpu[cpu].cpuEnv->currentPid = first->pid; g_perCpu[cpu].cpuEnv->currentProcess = reinterpret_cast<uint64_t>(first); }
    
    first->state = ProcessState::Running;
    __atomic_store_n(&first->runningOnCpu, (int32_t)cpu, __ATOMIC_RELEASE);
    __atomic_or_fetch(&first->tlbCpuMask, 1ULL << cpu, __ATOMIC_RELEASE);
    __atomic_or_fetch(AddressSpaceTlbMaskPtr(first), 1ULL << cpu, __ATOMIC_RELEASE);
    SmpSetCurrentCr3(cpu, first->savedCtx.cr3);  // keep tracking in sync (see exit path)
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
    if (!idle) KernelPanic("SCHED: OOM allocating AP%u idle process", cpuIndex);
    __builtin_memset(idle, 0, sizeof(Process));
    idle->magic = PROCESS_MAGIC;
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
    // Fast migration-safe path: gs:184 holds the Process* of whichever
    // thread the *current* CPU is running.  Each CPU has its own gs base
    // (set by SWAPGS at kernel entry), so this single instruction always
    // returns *our* process — even if a timer interrupt migrates us
    // between two unrelated reads of `cpu`.  Falling back through
    // ThisCpu() + g_perCpu[] was the BRO-005 root cause: ApicGetId could
    // be cached just before a migration, the loop would return a stale
    // index, and we'd hand back a pointer to whoever was now running on
    // the *previous* CPU.
    //
    // Boot path: gs base may not be set up yet, and currentProcess will
    // read 0 — fall through to the array path so very-early callers
    // (kernel init code that uses ProcessCurrent before scheduler is
    // running) still work.

    // Safety net: if GS_BASE is zero (stray SWAPGS), auto-fix from
    // KERNEL_GS_BASE before the gs-relative load at gs:184.
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101u));
        uint64_t gsBase = (static_cast<uint64_t>(hi) << 32) | lo;
        if (__builtin_expect(gsBase == 0, 0))
        {
            uint32_t klo, khi;
            __asm__ volatile("rdmsr" : "=a"(klo), "=d"(khi) : "c"(0xC0000102u));
            uint64_t kgsBase = (static_cast<uint64_t>(khi) << 32) | klo;

            if (kgsBase) {
                WriteMsr(0xC0000101, kgsBase);
                WriteMsr(0xC0000102, 0);
            } else {
                return g_perCpu[ThisCpu()].currentProcess;
            }
        }
    }

    uint64_t cur;
    __asm__ volatile("movq %%gs:184, %0" : "=r"(cur));
    if (cur)
        return reinterpret_cast<Process*>(cur);
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

int ProcessSendSignalToGroup(uint16_t pgid, int signum)
{
    // Collect matching processes under the lock, then signal outside
    Process* targets[MAX_PROCESSES];
    uint32_t count = 0;

    uint64_t alf = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; i++)
    {
        Process* p = g_allProcesses[i];
        if (p->pgid == pgid && p->state != ProcessState::Terminated)
        {
            if (count < MAX_PROCESSES) targets[count++] = p;
        }
    }
    SchedLockRelease(g_allProcLock, alf);

    SerialPrintf("SIGNAL: SendToGroup pgid=%u sig=%d -> %u procs\n", pgid, signum, count);
    for (uint32_t i = 0; i < count; i++)
        ProcessSendSignal(targets[i], signum);

    return static_cast<int>(count);
}

uint16_t SchedulerAllocPid()
{
    uint64_t f = SchedLockAcquire(g_pidLock);
    uint16_t pid;
    if (g_pidFreeCount > 0)
    {
        pid = g_pidFreeStack[--g_pidFreeCount];
    }
    else
    {
        pid = g_nextPid++;
        if (pid >= MAX_PROCESSES)
        {
            SchedLockRelease(g_pidLock, f);
            KernelPanic("SCHED: pid %u exceeds MAX_PROCESSES=%u and no "
                        "freed pids available\n", pid, MAX_PROCESSES);
        }
    }
    SchedLockRelease(g_pidLock, f);
    return pid;
}

void SchedulerFreePid(uint16_t pid)
{
    if (pid == 0 || pid >= MAX_PROCESSES) return;
    uint64_t f = SchedLockAcquire(g_pidLock);
    if (g_pidFreeCount < MAX_PROCESSES)
        g_pidFreeStack[g_pidFreeCount++] = pid;
    SchedLockRelease(g_pidLock, f);
}

// Mark all threads in a thread group (same tgid) as terminated so they are
// reaped without running.  Called by sys_exit_group before the calling thread
// calls SchedulerExitCurrentProcess.  Threads that are currently running on
// another CPU will be caught by the scheduler at the next timer tick.
void SchedulerKillThreadGroup(uint16_t tgid, Process* caller, int exitStatus)
{
    Process* targets[MAX_PROCESSES];
    uint32_t count = 0;

    uint64_t alf = SchedLockAcquire(g_allProcLock);
    // BRO-173: latch the whole group as exiting BEFORE snapshotting members,
    // under g_allProcLock — the same lock ProcessCreateThread/SchedulerAddProcess
    // take to publish a new thread.  This serializes group-exit vs thread birth:
    // a clone that wins the lock first is in our snapshot (and gets killed); a
    // clone that loses sees the latch and is refused.  Mark every current member
    // (leaders look themselves up by pid==tgid) so the check is robust even if
    // the leader pointer is stale.
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (p && p->tgid == tgid)
            __atomic_store_n(&p->tgidExiting, true, __ATOMIC_RELEASE);
    }
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (p && p != caller && p->tgid == tgid
            && p->state != ProcessState::Terminated)
        {
            if (count < MAX_PROCESSES) {
                // BRO-173: claim teardown ownership while still holding
                // g_allProcLock.  The reaper removes a Process from
                // g_allProcesses (under this same lock) strictly before
                // freeing it, so a target we select+flag here cannot be freed
                // until we release — and once flagged, no other path will mark
                // it reapable, so our raw `targets[]` pointers stay valid
                // through the unlocked Phase 2 below.
                // BRO-173/175: take a liveness reference (instead of the old
                // groupKillOwned flag) while still holding g_allProcLock, so
                // the reaper cannot free this target until our unlocked Phase 2
                // drops the ref.  The reaper removes a Process from
                // g_allProcesses under this same lock strictly before freeing,
                // so a target we ref here cannot already be mid-free.
                ProcessRef(p);
                targets[count++] = p;
            }
        }
    }
    SchedLockRelease(g_allProcLock, alf);

    // Phase 1: mark every sibling Terminated so the remote timer tick will
    // deschedule it.  We deliberately DO NOT touch each thread's kernel-lock
    // state here: the thread may still be running on another CPU and could be
    // mid-KRwLock{Read,Write}Lock, racing KRwLockCleanupOnExit's lock-free
    // reads of heldWriteLock/blockedOnRwLock (BRO-157).  Cleanup is deferred to
    // phase 2, after each thread is confirmed quiesced.
    //
    // Publish order matters: set exitStatus first, then store state with
    // release semantics so a remote CPU that observes Terminated also observes
    // the matching exitStatus.  state is a single aligned byte (atomic on x86);
    // the release store adds the compiler/memory barrier the plain store lacked.
    for (uint32_t i = 0; i < count; ++i)
    {
        Process* p = targets[i];
        p->exitStatus = exitStatus;
        __atomic_store_n(reinterpret_cast<uint8_t*>(&p->state),
                         static_cast<uint8_t>(ProcessState::Terminated),
                         __ATOMIC_RELEASE);
        DbgPrintf("SCHED: exit_group killing thread pid=%u tgid=%u\n",
                  p->pid, p->tgid);
    }

    // Phase 2: wait for each sibling to stop executing, THEN clean up its
    // kernel-lock state and mark it reapable.
    //
    // The quiesce wait is required so the caller (leader) doesn't proceed to
    // ProcessDestroy and free the shared page table / fileMaps while a sibling
    // is still running on those shared resources.  Performing the rwlock
    // cleanup only after runningOnCpu < 0 also closes the BRO-157 race: the
    // thread is now parked at a stable point, so its heldWriteLock /
    // blockedOnRwLock fields are no longer being mutated concurrently and
    // KRwLockCleanupOnExit can read them safely.
    //
    // The timer tick handler checks for Terminated state and will deschedule
    // the thread even from kernel mode.  We use a lightweight pause loop with
    // sti to let timer interrupts fire on our CPU (so the scheduler can
    // deschedule targets on other CPUs).  Avoid SchedulerYield() here — it
    // hammers g_readyLock and can cause massive contention.
    for (uint32_t i = 0; i < count; ++i)
    {
        Process* p = targets[i];
        uint32_t attempts = 0;
        constexpr uint32_t MAX_ATTEMPTS = 10000000; // ~10s
        bool quiesced = true;
        while (__atomic_load_n(&p->runningOnCpu, __ATOMIC_ACQUIRE) >= 0)
        {
            // Ensure interrupts are enabled so timer ticks can fire
            // and deschedule the Terminated thread on its CPU.
            __asm__ volatile("sti; pause; pause; pause; pause" ::: "memory");

            if (++attempts >= MAX_ATTEMPTS)
            {
                int cpu = __atomic_load_n(&p->runningOnCpu, __ATOMIC_ACQUIRE);
                SerialPrintf("SCHED: exit_group timeout waiting for pid=%u "
                             "(stuck on cpu=%d), forcing reapable\n",
                             p->pid, cpu);
                quiesced = false;
                break;
            }
        }

        // Now that the thread is parked (or we gave up after the timeout),
        // release any kernel rwlocks it held or was waiting on.  On the forced
        // timeout path the thread is presumably wedged and won't make further
        // progress, so cleaning up is still the least-bad option.
        (void)quiesced;
        KRwLockCleanupOnExit(p);

        __atomic_store_n(&p->reapable, true, __ATOMIC_RELEASE);

        // BRO-173/175: drop the teardown reference taken in the snapshot.  Once
        // this falls to 0 (and the thread is Terminated, reapable, off-CPU) the
        // reaper is free to destroy it.  Our raw `targets[]` pointer must not be
        // dereferenced after this point.
        ProcessUnref(p);
    }
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
            && !p->isThread  // Threads are not waitable children
            && (pid == -1 || pid == static_cast<int64_t>(p->pid)))
        {
            // BRO-176: do not let waitpid reap a thread-group leader (which frees
            // the shared address space in ProcessDestroy) while any thread still
            // shares that address space — a sibling in the pick->switch window,
            // or one running musl thread-exit cleanup, would then fault on a
            // freed AS.  Gate on asLiveThreads, the same invariant the auto-reaper
            // uses; the leader becomes reapable once the last thread is reaped.
            if (__atomic_load_n(&p->asLiveThreads, __ATOMIC_ACQUIRE) != 0)
                continue;  // skip this leader; revisit once threads drain
            SchedLockRelease(g_allProcLock, alf);
            return p;
        }
    }
    SchedLockRelease(g_allProcLock, alf);
    return nullptr;
}

Process* SchedulerFindStoppedChild(uint16_t parentPid, int64_t pid)
{
    uint64_t alf = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; i++)
    {
        Process* p = g_allProcesses[i];
        if (p->parentPid == parentPid
            && p->state == ProcessState::Stopped
            && !p->stopReported
            && (pid == -1 || pid == static_cast<int64_t>(p->pid)))
        {
            SchedLockRelease(g_allProcLock, alf);
            return p;
        }
    }
    SchedLockRelease(g_allProcLock, alf);
    return nullptr;
}

bool SchedulerChildExists(uint16_t parentPid, uint16_t childPid)
{
    uint64_t alf = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; i++)
    {
        Process* p = g_allProcesses[i];
        if (p->pid == childPid && p->parentPid == parentPid && !p->isThread)
        {
            SchedLockRelease(g_allProcLock, alf);
            return true;
        }
    }
    SchedLockRelease(g_allProcLock, alf);
    return false;
}

void SchedulerDumpChildState(uint16_t parentPid, uint16_t targetPid)
{
    uint64_t alf = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; i++)
    {
        Process* p = g_allProcesses[i];
        if (p->pid == targetPid || p->parentPid == parentPid)
        {
            SerialPrintf("WAIT4-DIAG: pid=%u ppid=%u state=%d reapable=%d "
                         "isThread=%d tgid=%u name='%s'\n",
                         p->pid, p->parentPid, (int)p->state,
                         __atomic_load_n(&p->reapable, __ATOMIC_ACQUIRE),
                         p->isThread, p->tgid, p->name);
        }
    }
    SchedLockRelease(g_allProcLock, alf);
}

Process* SchedulerFindProcessByBaseName(const char* basename)
{
    if (!basename || !*basename) return nullptr;
    uint64_t alf = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p || p->state == ProcessState::Terminated) continue;
        // proc->name is the binary basename optionally followed by "_NN".
        // Match the prefix up to and excluding the spawn-index suffix.
        const char* a = p->name;
        const char* b = basename;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*b == '\0' && (*a == '\0' || *a == '_'))
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
    // Preserve ticks from reaped processes for accurate /proc/stat accounting
    g_reapedUserTicks += child->userTicks;
    g_reapedSysTicks += child->sysTicks;
    ProcessDestroy(child);
}

// BRO-176 diagnostic: NON-DESTRUCTIVE hang dump (Ctrl+F12). Walks every process
// — INCLUDING Terminated/zombie ones the panic path skips — and prints the
// reap-gate fields so a fork+exit reap-stall (live=1, system otherwise alive)
// can be diagnosed without killing the instance. Lock-free serial output
// (SerialPutChar polls the UART, takes no lock) and a best-effort lock-free read
// of g_allProcesses, so it is safe to fire from the keyboard IRQ even while the
// reaper/waitpid path is stuck. Can be triggered repeatedly. TEMPORARY.
static void HangPuts(const char* s) { if (s) while (*s) SerialPutChar(*s++); }
static void HangHex(uint64_t v)
{
    SerialPutChar('0'); SerialPutChar('x');
    for (int sh = 60; sh >= 0; sh -= 4)
    { int n = (int)((v >> sh) & 0xF); SerialPutChar((char)(n < 10 ? '0' + n : 'a' + n - 10)); }
}
static void HangDec(int64_t v)
{
    if (v < 0) { SerialPutChar('-'); v = -v; }
    char buf[20]; int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) SerialPutChar(buf[--i]);
}

extern "C" void SchedulerDumpHang()
{
    HangPuts("\n==================== BRO176 HANG DUMP (Ctrl+F12) ====================\n");
    // Per-CPU current process + saved RIP — reveals where each CPU is parked or
    // spinning (the QR panic only shows the one CPU that took the keyboard IRQ).
    uint32_t cpuCount = SmpGetCpuCount();
    if (cpuCount > SCHED_MAX_CPUS) cpuCount = SCHED_MAX_CPUS;
    HangPuts("--- per-CPU current process ---\n");
    for (uint32_t c = 0; c < cpuCount; ++c)
    {
        Process* cur = g_perCpu[c].currentProcess;
        Process* idle = g_perCpu[c].idleProcess;
        Process* req = g_perCpu[c].pendingRequeue;
        Process* ret = g_perCpu[c].pendingRetire;
        HangPuts("  CPU"); HangDec((int)c); HangPuts(": ");
        if (!cur) { HangPuts("<null>"); }
        else if (cur == idle) { HangPuts("idle"); }
        else if (cur->magic != PROCESS_MAGIC) { HangPuts("cur=CORRUPT "); HangHex((uint64_t)cur); }
        else {
            HangPuts("pid="); HangDec(cur->pid);
            HangPuts(" state="); HangDec((int)cur->state);
            HangPuts(" rip="); HangHex(cur->savedCtx.rip);
            HangPuts(" '"); for (int j = 0; j < 20 && cur->name[j]; ++j) SerialPutChar(cur->name[j]); HangPuts("'");
        }
        if (req) { HangPuts(" pendingRequeue="); HangHex((uint64_t)req); }
        if (ret) { HangPuts(" pendingRetire="); HangHex((uint64_t)ret); }
        HangPuts("\n");
    }
    HangPuts("processCount="); HangDec((int64_t)g_processCount); HangPuts("\n");
    // Best-effort, NO lock (the reaper may hold g_allProcLock while stuck).
    uint32_t n = g_processCount;
    if (n > MAX_PROCESSES) n = MAX_PROCESSES;
    for (uint32_t i = 0; i < n; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p) continue;
        bool magicOk = (p->magic == PROCESS_MAGIC);
        HangPuts("  [");
        HangDec((int64_t)i); HangPuts("] proc="); HangHex((uint64_t)p);
        HangPuts(magicOk ? " magic=OK" : " magic=BAD");
        if (!magicOk) { HangPuts(" (skipped — corrupt)\n"); continue; }
        HangPuts(" pid="); HangDec(p->pid);
        HangPuts(" tgid="); HangDec(p->tgid);
        HangPuts(" state="); HangDec((int)p->state);
        HangPuts(" isThread="); HangDec(p->isThread ? 1 : 0);
        HangPuts(" isKthread="); HangDec(p->isKernelThread ? 1 : 0);
        HangPuts(" asLiveThreads="); HangDec(__atomic_load_n(&p->asLiveThreads, __ATOMIC_RELAXED));
        HangPuts(" refCount="); HangDec(__atomic_load_n(&p->refCount, __ATOMIC_RELAXED));
        HangPuts(" reapable="); HangDec(p->reapable ? 1 : 0);
        HangPuts(" runCpu="); HangDec(__atomic_load_n(&p->runningOnCpu, __ATOMIC_RELAXED));
        HangPuts(" incarn="); HangDec((int64_t)p->incarnation);
        HangPuts(" rip="); HangHex(p->savedCtx.rip);
        HangPuts(" name='"); 
        for (int j = 0; j < 24 && p->name[j]; ++j) SerialPutChar(p->name[j]);
        HangPuts("'");
        // BRO-208: block-reason analysis — for a hung/deadlocked thread, show
        // WHERE in the kernel it blocked and WHAT on. Symbolicating savedCtx.rip
        // reveals the blocking primitive (sys_futex / KRwLockWriteLock / pipe /
        // sockets). pendingWakeup!=0 on a Blocked thread is the lost-wakeup
        // fingerprint: a wake was signalled but the thread never ran.
        {
            // NOTE: savedCtx.rip is the shared context_switch resume stub, not a
            // per-thread blocked PC — the schedTrace ring (site 6=BLOCK) and the
            // futex-waiter table below are the authoritative "blocked where/on-what".
            uint32_t pw = __atomic_load_n(&p->pendingWakeup, __ATOMIC_RELAXED);
            if (pw) { HangPuts(" pendWake="); HangDec((int64_t)pw); }
            if (p->wakeupTick)
            { HangPuts(" wakeupTick=+"); HangDec((int64_t)p->wakeupTick - (int64_t)g_lapicTickCount); }
            KRwLock* rw = p->blockedOnRwLock;
            if (rw)
            {
                HangPuts(" rwlock="); HangHex((uint64_t)rw);
                HangPuts(p->blockedAsWriter ? " asWriter" : " asReader");
                HangPuts(" rdrs="); HangDec((int64_t)rw->readerCount);
                HangPuts(" wrActive="); HangDec((int64_t)rw->writerActive);
                HangPuts(" wrWaiting="); HangDec((int64_t)rw->writersWaiting);
            }
        }
        // For a thread, show the leader's gate so we can see a stuck reap.
        if (p->isThread && p->threadLeader && p->threadLeader->magic == PROCESS_MAGIC)
        {
            HangPuts(" leaderPid="); HangDec(p->threadLeader->pid);
            HangPuts(" leaderAsLive="); HangDec(__atomic_load_n(&p->threadLeader->asLiveThreads, __ATOMIC_RELAXED));
            HangPuts(" leaderIncMatch=");
            HangDec(p->threadLeader->incarnation == p->leaderIncarnation ? 1 : 0);
        }
        HangPuts("\n");
        // BRO-208: for a Blocked thread, decode its schedTrace ring — the last 12
        // scheduler events. STR_BLOCK(6) followed by no STR_READY_UNBLOCK(3), or a
        // STR_BLOCK_SKIP(7)/STR_UNBLOCK_DEFER(4,5) tail, distinguishes a genuine
        // wait from a lost/deferred wakeup.
        if (p->state == ProcessState::Blocked)
        {
            HangPuts("    schedTrace(site@tick,state):");
            uint8_t h = p->schedTraceHead;
            for (int k = 0; k < 12; ++k)
            {
                uint8_t slot = (uint8_t)((h + k) % 12);
                uint64_t e = p->schedTrace[slot];
                if (e == 0) continue;
                HangPuts(" ["); HangDec((int64_t)((e >> 8) & 0xFF));
                HangPuts("@"); HangDec((int64_t)(e >> 16));
                HangPuts(",s"); HangDec((int64_t)(e & 0xFF)); HangPuts("]");
            }
            HangPuts("\n");
        }
        // BRO-176: a Terminated leader stuck with asLiveThreads>0 is the reap-stall
        // fingerprint — dump its full inc/dec history to name the unmatched op.
        if (!p->isThread && p->state == ProcessState::Terminated &&
            __atomic_load_n(&p->asLiveThreads, __ATOMIC_RELAXED) > 0)
        {
            HangPuts("    ^^ STUCK LEADER (Terminated, asLiveThreads>0) — inc/dec history:\n");
            DumpAsLiveHistory(p);
        }
    }
    HangPuts("STATE legend: 0=Ready 1=Running 2=Blocked 3=Stopped 4=Terminated\n");
    HangPuts("schedTrace sites: 1=ENQ 2=REM 3=RDY_UNBLK 4=DEFER_RR 5=DEFER_CPU "
             "6=BLOCK 7=BLOCK_SKIP 8=PREEMPT 9=YIELD 10=REQ_ENQ 11=REQ_SKIP 12=RUN\n");
    // BRO-208: cross-reference every futex waiter (uaddr/owner/pid/pendWake) so a
    // deadlock on a user mutex/condvar is visible: a Blocked thread parked on a
    // futex whose word no other thread will wake is the fingerprint.
    {
        FutexDumpWaiters(0);
        // BRO-208: replay the recent futex op history so a lost/misdirected WAKE
        // or a wake-before-wait ordering bug is visible as a time-series.
        FutexDumpTrace(0, 200);
    }
    HangPuts("==================== END HANG DUMP ====================\n\n");
}

uint32_t SchedulerSnapshotProcesses(ProcessSnapshot* out, uint32_t maxCount)
{
    uint32_t count = 0;
    uint64_t flags = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount && count < maxCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p) continue;
        ProcessSnapshot& s = out[count++];
        s.pid = p->pid;
        s.parentPid = p->parentPid;
        s.pgid = p->pgid;
        s.sid = p->sid;
        s.state = p->state;
        s.runningOnCpu = p->runningOnCpu;
        s.stackBase = p->stackBase;
        s.stackTop = p->stackTop;
        s.programBreak = p->programBreak;
        s.userTicks = p->userTicks;
        s.sysTicks = p->sysTicks;
        uint32_t j = 0;
        for (; j < 31 && p->name[j]; ++j)
            s.name[j] = p->name[j];
        s.name[j] = '\0';
        // Copy exePath and cwd
        j = 0;
        for (; j < 255 && p->exePath[j]; ++j)
            s.exePath[j] = p->exePath[j];
        s.exePath[j] = '\0';
        j = 0;
        for (; j < 255 && p->cwd[j]; ++j)
            s.cwd[j] = p->cwd[j];
        s.cwd[j] = '\0';
    }
    SchedLockRelease(g_allProcLock, flags);
    return count;
}

bool SchedulerSnapshotProcess(uint16_t pid, ProcessSnapshot* out)
{
    uint64_t flags = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p || p->pid != pid) continue;
        out->pid = p->pid;
        out->parentPid = p->parentPid;
        out->pgid = p->pgid;
        out->sid = p->sid;
        out->state = p->state;
        out->runningOnCpu = p->runningOnCpu;
        out->stackBase = p->stackBase;
        out->stackTop = p->stackTop;
        out->programBreak = p->programBreak;
        out->userTicks = p->userTicks;
        out->sysTicks = p->sysTicks;
        uint32_t j = 0;
        for (; j < 31 && p->name[j]; ++j)
            out->name[j] = p->name[j];
        out->name[j] = '\0';
        j = 0;
        for (; j < 255 && p->exePath[j]; ++j)
            out->exePath[j] = p->exePath[j];
        out->exePath[j] = '\0';
        j = 0;
        for (; j < 255 && p->cwd[j]; ++j)
            out->cwd[j] = p->cwd[j];
        out->cwd[j] = '\0';
        SchedLockRelease(g_allProcLock, flags);
        return true;
    }
    SchedLockRelease(g_allProcLock, flags);
    return false;
}

// Return the PID of the Nth active process (0-indexed). Returns false if index is out of range.
bool SchedulerGetPidByIndex(uint32_t index, uint16_t* outPid)
{
    uint64_t flags = SchedLockAcquire(g_allProcLock);
    uint32_t found = 0;
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p) continue;
        if (found == index)
        {
            *outPid = p->pid;
            SchedLockRelease(g_allProcLock, flags);
            return true;
        }
        ++found;
    }
    SchedLockRelease(g_allProcLock, flags);
    return false;
}

uint64_t SchedulerGetTotalForks() { return g_totalForks; }

void SchedulerGetReapedTicks(uint64_t& userTicks, uint64_t& sysTicks)
{
    userTicks = g_reapedUserTicks;
    sysTicks = g_reapedSysTicks;
}

void SchedulerGetProcessCounts(uint32_t& total, uint32_t& running)
{
    uint64_t flags = SchedLockAcquire(g_allProcLock);
    total = 0;
    running = 0;
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p) continue;
        ++total;
        if (p->state == ProcessState::Ready || p->state == ProcessState::Running)
            ++running;
    }
    SchedLockRelease(g_allProcLock, flags);
}

void SchedulerGetLoadAvg(uint32_t& avg1, uint32_t& avg5, uint32_t& avg15)
{
    avg1 = g_loadAvg1;
    avg5 = g_loadAvg5;
    avg15 = g_loadAvg15;
}

void SchedulerGetCpuTicks(uint32_t cpuIndex, uint64_t& busyTicks, uint64_t& idleTicks)
{
    if (cpuIndex < SCHED_MAX_CPUS)
    {
        busyTicks = g_perCpu[cpuIndex].busyTicks;
        idleTicks = g_perCpu[cpuIndex].idleTicks;
    }
    else
    {
        busyTicks = 0;
        idleTicks = 0;
    }
}

Process* SchedulerGetCpuProcess(uint32_t cpuIndex)
{
    if (cpuIndex < SCHED_MAX_CPUS)
        return g_perCpu[cpuIndex].currentProcess;
    return nullptr;
}

static void FillFdSnapshot(FdSnapshot* out, const FdEntry& fde)
{
    out->type = static_cast<uint8_t>(fde.type);
    out->flags = fde.statusFlags;
    out->seekPos = fde.seekPos;
    // Copy dirPath if it has content, otherwise describe by type
    if (fde.dirPath[0])
    {
        uint32_t j = 0;
        for (; j < 63 && fde.dirPath[j]; ++j)
            out->path[j] = fde.dirPath[j];
        out->path[j] = '\0';
    }
    else
    {
        static const char* typeNames[] = {
            "none", "file", "fb", "kbd", "pipe", "/dev/null", "/dev/urandom",
            "mem", "socket", "tty", "eventfd", "dsp", "epoll", "timerfd",
            "memfd", "unix", "klog"
        };
        uint8_t t = static_cast<uint8_t>(fde.type);
        const char* name = (t < sizeof(typeNames)/sizeof(typeNames[0])) ? typeNames[t] : "?";
        uint32_t j = 0;
        for (; j < 63 && name[j]; ++j)
            out->path[j] = name[j];
        out->path[j] = '\0';
    }
}

bool SchedulerGetFdByIndex(uint16_t pid, uint32_t index, int* outFd, FdSnapshot* outSnap)
{
    uint64_t flags = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p || p->pid != pid) continue;
        if (!p->fds) { SchedLockRelease(g_allProcLock, flags); return false; }

        SpinLockAcquire(&p->fdLock);
        uint32_t found = 0;
        for (uint32_t fd = 0; fd < MAX_FDS; ++fd)
        {
            if (p->fds[fd].type == FdType::None) continue;
            if (found == index)
            {
                *outFd = static_cast<int>(fd);
                FillFdSnapshot(outSnap, p->fds[fd]);
                SpinLockRelease(&p->fdLock);
                SchedLockRelease(g_allProcLock, flags);
                return true;
            }
            ++found;
        }
        SpinLockRelease(&p->fdLock);
        SchedLockRelease(g_allProcLock, flags);
        return false;
    }
    SchedLockRelease(g_allProcLock, flags);
    return false;
}

bool SchedulerGetFdInfo(uint16_t pid, int fd, FdSnapshot* outSnap)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return false;
    uint64_t flags = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p || p->pid != pid) continue;
        if (!p->fds) { SchedLockRelease(g_allProcLock, flags); return false; }

        SpinLockAcquire(&p->fdLock);
        if (p->fds[fd].type == FdType::None)
        {
            SpinLockRelease(&p->fdLock);
            SchedLockRelease(g_allProcLock, flags);
            return false;
        }
        FillFdSnapshot(outSnap, p->fds[fd]);
        SpinLockRelease(&p->fdLock);
        SchedLockRelease(g_allProcLock, flags);
        return true;
    }
    SchedLockRelease(g_allProcLock, flags);
    return false;
}

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

// Panic-safe process enumeration — no locks, assumes all other CPUs halted.
uint32_t PanicGetProcessCount()
{
    return g_processCount;
}

Process* PanicGetProcess(uint32_t index)
{
    if (index >= g_processCount) return nullptr;
    return g_allProcesses[index];
}

void SchedulerDumpThreadStates()
{
    static const char* stateNames[] = {
        "READY", "RUNNING", "BLOCKED", "ZOMBIE", "STOPPED", "SLEEPING"
    };
    SerialPrintf("\n=== THREAD STATE DUMP ===\n");
    uint64_t flags = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p) continue;
        const char* st = "?";
        int si = static_cast<int>(p->state);
        if (si >= 0 && si <= 5) st = stateNames[si];
        SerialPrintf("  pid=%u tgid=%u '%s' state=%s cpu=%d syscall=%lu wakeup=%lu\n",
                     p->pid, p->tgid, p->name, st,
                     p->runningOnCpu, p->currentSyscallNum, p->wakeupTick);
    }
    extern volatile uint64_t g_lapicTickCount;
    SerialPrintf("  now=%lu\n", g_lapicTickCount);
    Ext2DumpLockState();
    FatFsDumpLockState();
    SerialPrintf("=========================\n\n");
    SchedLockRelease(g_allProcLock, flags);
}

} // namespace brook
