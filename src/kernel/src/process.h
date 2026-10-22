#pragma once

#include <stdint.h>
#include "memory/address.h"
#include "memory/address_space.h"
#include "input.h"
#include "spinlock.h"
#include "fd_table.h"
#include "credential_ids.h"
#include "signal_return.h"

namespace brook {

struct Vnode;
struct FsContext;
struct KRwLock;
struct SupplementaryGroups {
    volatile uint32_t references;
    uint32_t count;
    uint32_t ids[1]; // immutable after publication, variable allocation
};
static constexpr uint32_t MAX_SUPPLEMENTARY_GROUPS = 65536;
void FdReleaseOwnedEntry(const FdEntry& entry);

// MAX_FDS, FdType and FdEntry are defined in fd_table.h (extracted for
// host-side unit testing — see BRO-156).

// Maximum concurrent processes.
static constexpr uint32_t MAX_PROCESSES = 1024;

// Program break limit (max brk heap size per process). Browser/WebContent
// workloads can exceed 64 MB through many small allocator requests before
// falling back to mmap.
static constexpr uint64_t PROGRAM_BREAK_SIZE = 128 * 1024 * 1024; // 128 MB

// Default user stack size.
static constexpr uint64_t USER_STACK_SIZE = 8 * 1024 * 1024; // 8 MB (Linux default)

// Per-process kernel stack size (64 pages = 256 KB).
// Must be large enough for syscalls (VFS/FatFS stack depth), interrupts,
// and signal delivery frames (~560 bytes each).
// Each process's kernel stack is also used as its syscall stack (gs:8).
// The allocation includes unmapped guard pages at both ends for overflow detection.
static constexpr uint64_t KERNEL_STACK_SIZE = 256 * 1024;
static constexpr uint64_t KERNEL_STACK_PAGES = KERNEL_STACK_SIZE / 4096;

// Scheduler time slice in milliseconds (~10ms).
static constexpr uint64_t SCHED_TIMESLICE_MS = 10;

// User-space virtual address where ELF binaries are loaded.
// This is in the lower canonical half (user space).
static constexpr uint64_t USER_LOAD_BASE = 0x400000;

// User mmap region starts at 256MB (above ELF load area, below stack).
static constexpr uint64_t USER_MMAP_BASE = 0x10000000ULL;   // 256 MB
static constexpr uint64_t USER_MMAP_END  = 0x700000000000ULL; // well below stack

// User stack top (grows down from here).
static constexpr uint64_t USER_STACK_TOP = 0x7FFFFFFFE000ULL;

// ---------------------------------------------------------------------------
// Signal handler (Linux-compatible sigaction layout)
// ---------------------------------------------------------------------------

struct KernelSigaction {
    uint64_t handler;    // SIG_DFL=0, SIG_IGN=1, or handler address
    uint64_t flags;      // SA_RESTORER, SA_SIGINFO, etc.
    uint64_t restorer;   // User-space __restore_rt trampoline
    uint64_t mask;       // Signals blocked during handler
};
static_assert(sizeof(KernelSigaction)==32);
static_assert(__builtin_offsetof(KernelSigaction,handler)==0);
static_assert(__builtin_offsetof(KernelSigaction,flags)==8);
static_assert(__builtin_offsetof(KernelSigaction,restorer)==16);
static_assert(__builtin_offsetof(KernelSigaction,mask)==24);

// SA_* flag constants
static constexpr uint64_t SA_SIGINFO  = 0x00000004;
static constexpr uint64_t SA_ONSTACK  = 0x08000000;
static constexpr uint64_t SA_RESTORER = 0x04000000;
static constexpr uint64_t SA_RESTART  = 0x10000000;

// Group dispositions are snapshotted/exchanged under one short IRQ-safe lock.
// Replacement is a trusted kernel-local value; perform user copies outside it.
KernelSigaction ReadSignalAction(uint16_t tgid,uint32_t index);
KernelSigaction ExchangeSignalAction(uint16_t tgid,uint32_t index,const KernelSigaction* replacement);
void ResetSignalActions(uint16_t tgid,bool preserveIgnore=false);
void CopySignalActions(uint16_t destination,uint16_t source);

// ---------------------------------------------------------------------------
// Signal frame structures (Linux x86_64 ABI compatible)
// ---------------------------------------------------------------------------
// These must match the Linux kernel's layout so musl/glibc's sigreturn works.

struct SignalMcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip;
    uint64_t eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;          // NULL (no FPU state saved)
    uint64_t __reserved1[8];
};
static_assert(__builtin_offsetof(SignalMcontext, ss) == 150,
              "Linux x86-64 sigcontext SS must be the fourth selector");
static_assert(sizeof(SignalMcontext) == 256, "Linux x86-64 sigcontext layout");

struct SignalStack {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  __pad;
    uint64_t ss_size;
};

struct SignalUcontext {
    uint64_t        uc_flags;
    uint64_t        uc_link;
    SignalStack      uc_stack;
    SignalMcontext   uc_mcontext;
    uint64_t        uc_sigmask;
};

struct SignalInfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t __pad;
    uint8_t _data[128 - 16];
};

struct SignalFrame {
    uint64_t        pretcode;   // sa_restorer (handler returns here)
    SignalUcontext  uc;
    SignalInfo      info;
};

// Kernel stack frame layout matching BrookSyscallDispatcher's push order.
// Used by SyscallCheckSignals before SYSRET or full-register signal IRET.
struct SyscallFrame {
    uint64_t rbx;       // [RSP+0]
    uint64_t r15;       // [RSP+8]
    uint64_t r14;       // [RSP+16]
    uint64_t r13;       // [RSP+24]
    uint64_t r12;       // [RSP+32]
    uint64_t r11;       // [RSP+40]
    uint64_t r10;       // [RSP+48]
    uint64_t r9;        // [RSP+56]
    uint64_t r8;        // [RSP+64]
    uint64_t rdi;       // [RSP+72]
    uint64_t rsi;       // [RSP+80]
    uint64_t rdx;       // [RSP+88]
    uint64_t rbp;       // [RSP+96]
    uint64_t rflags;    // [RSP+104]  (user RFLAGS, popped into R11)
    uint64_t rcx;       // [RSP+112]  (user RIP, restored via sysret)
    uint64_t rsp;       // [RSP+120]  (user RSP)
    SyscallReturnState returnState; // +128 mode, +136 RCX, +144 IRET frame
};
static_assert(__builtin_offsetof(SyscallFrame,returnState)==128);
static_assert(__builtin_offsetof(SyscallFrame,returnState)+__builtin_offsetof(SyscallReturnState,rcx)==136);
static_assert(__builtin_offsetof(SyscallFrame,returnState)+__builtin_offsetof(SyscallReturnState,rip)==144);
static_assert(sizeof(SyscallFrame)==192);

// ---------------------------------------------------------------------------
// Process scheduling state
// ---------------------------------------------------------------------------

enum class ProcessState : uint8_t
{
    Ready,          // In the run queue, eligible for scheduling
    Running,        // Currently executing on CPU
    Blocked,        // Waiting (sleep, I/O, mutex) — not in run queue
    Stopped,        // Suspended by signal (SIGTSTP/SIGSTOP) — not in run queue
    Terminated,     // Finished, awaiting cleanup
};

// ---------------------------------------------------------------------------
// Saved CPU context — stored on context switch
// ---------------------------------------------------------------------------
// Layout must match the context_switch asm (push/pop order).

struct SavedContext
{
    // General-purpose registers
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    // RIP is implicitly saved as the return address on the kernel stack
    // RSP is the stack pointer when we switch away
    uint64_t rsp;       // Kernel stack pointer at switch point
    uint64_t rip;       // Resume address (set for first switch, then implicit)
    uint64_t rflags;

    // Segment bases
    uint64_t cr3;       // Page table physical address
    uint64_t fsBase;    // FS segment base (TLS)
    uint64_t gsBase;    // User GS base; inactive MSR_KERNEL_GS_BASE in kernel
};

static_assert(__builtin_offsetof(SavedContext, gsBase) == 88, "context_switch.S GS offset");

// XSAVE area for x87 + SSE + AVX state.  Brook enables CR4.OSXSAVE and
// XCR0[2] for AVX-capable userland, so fxsave/fxrstor is not sufficient:
// it omits the upper halves of YMM registers and corrupts AVX-optimised code
// across context switches.
struct alignas(64) FxsaveArea
{
    uint8_t data[1088];
};

// ---------------------------------------------------------------------------
// File descriptor entry
// ---------------------------------------------------------------------------
// FdType and FdEntry are defined in fd_table.h (extracted for host-side unit
// testing — see BRO-156).

struct TtyDevicePair {
    void* readPipe;   // PipeBuffer* for stdin
    void* writePipe;  // PipeBuffer* for stdout
};

// ---------------------------------------------------------------------------
// ELF binary descriptor (populated by the ELF loader)
// ---------------------------------------------------------------------------

struct ElfBinary
{
    uint64_t baseAddress;       // Virtual base where binary was loaded
    uint64_t allocatedSize;     // Image virtual span; may contain unmapped holes
    uint64_t entryPoint;        // e_entry
    uint64_t programBreakLow;   // End of loaded segments (heap starts here)
    uint64_t programBreakHigh;  // programBreakLow + PROGRAM_BREAK_SIZE

    // Program headers (needed for AT_PHDR auxv)
    uint64_t phdrVaddr;         // Virtual address of program headers in memory
    uint16_t phdrNum;           // Number of program headers
    uint16_t phdrEntSize;       // Size of each program header entry

    // TLS template
    uint8_t* tlsInitData;       // Initial TLS data (PT_TLS p_vaddr in loaded image)
    uint64_t tlsInitSize;       // Size of initialized TLS data (p_filesz)
    uint64_t tlsTotalSize;      // Total TLS size including BSS (p_memsz)
    uint16_t tlsAlign;          // TLS alignment

    // Dynamic linking: PT_INTERP path (empty if statically linked)
    char interpPath[128];
};

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

// Sentinel written to Process::magic at allocation; zeroed at kfree.  Any
// scheduler hot-path that derefs a Process* asserts this value to catch
// use-after-free promptly instead of triggering a non-canonical #PF on a
// truncated/poisoned pointer field.  Value chosen to be obviously non-zero,
// non-canonical-as-an-address, and ASCII-distinctive in serial dumps.
static constexpr uint64_t PROCESS_MAGIC = 0xB0000B5042524F4FULL; // "B   BROO"

enum class ProcessLifecycleRole : uint8_t { Application = 0, Service = 1 };

struct Process
{
    // Magic word — used by DoSwitch and SchedulerBlock to catch use-after-free
    // when a Process struct has been freed and its pages reused.  Set on
    // allocation, zeroed on free.  See PROCESS_MAGIC below.
    uint64_t magic;

    uint16_t pid;
    uint16_t parentPid;          // Parent thread-group ID (0 if no parent)
    uint16_t pgid;               // Process group ID
    uint16_t sid;                // Session ID
    uint16_t tgid;               // Thread group ID (= leader's PID; same as PID for main thread)
    uint32_t uid;                // Real user ID
    uint32_t euid;               // Effective user ID
    uint32_t gid;                // Real group ID
    uint32_t egid;               // Effective group ID
    uint32_t suid;               // Saved set-user-ID
    uint32_t sgid;               // Saved set-group-ID
    SupplementaryGroups* groups; // per-task immutable shared list, owns one ref
    ProcessState state;
    uint8_t  schedPriority;  // Initial scheduler priority (0=RT, 1=High, 2=Normal, 3=Low)
    uint16_t noNewPrivs;         // Irreversible per-task exec privilege restriction; credential lock
    int32_t  runningOnCpu;   // CPU index (-1 = not running, used for double-schedule detection)
    int32_t  cpuAffinity;    // CPU affinity pin (-1 = any CPU, >=0 = pinned to that CPU)
    volatile uint64_t tlbCpuMask;  // Bitmask of CPUs that have this process's CR3 loaded in TLB
    // Address-space ownership and its monotonic TLB footprint are shared by
    // tasks independently of group leadership. Use AddressSpaceTlbMaskPtr for
    // user shootdowns; the per-task mask above misses sibling CPUs.
    AddressSpaceState* addressSpace;
    Process* destroyNext;
    ProcessCleanupCompletion* destroyCompletion;
    // BRO-173/175: liveness refcount — counts external holders of a raw
    // Process* that could deref this struct (a CPU currently running it, a
    // SchedulerKillThreadGroup leader's snapshot, the compositor's VFB
    // registration).  The reaper frees a Process ONLY when state==Terminated
    // AND refCount==0, replacing the old free-gate flag soup (reapable +
    // runningOnCpu==-1 + groupKillOwned + compositorRegistered-defer) with one
    // monotonic invariant: never free while any reference is outstanding.
    // ORTHOGONAL to `state` (which answers "may it run?", not "is it freeable?").
    volatile int32_t refCount;
    // Unreaped thread count keeps the group leader and shared FD table alive.
    // Address-space memory has its own references, including across exec.
    volatile int32_t asLiveThreads;
    // BRO-176: a monotonically-increasing incarnation id assigned at creation,
    // NEVER reused (unlike pid or the struct pointer, both of which recycle
    // under churn).  A thread records its leader's incarnation at creation
    // (leaderIncarnation); at teardown the decrement of the leader's
    // asLiveThreads is skipped unless the leader struct's current incarnation
    // still matches, so a freed+reused leader slot can never have an unrelated
    // incarnation's count corrupted.  In practice the asLiveThreads gate keeps a
    // leader alive while any thread references it, so incarnations always match;
    // the check is a cheap correctness guard that documents that invariant.
    uint64_t incarnation;
    uint64_t leaderIncarnation;
    volatile bool reapable;  // Set after context_switch completes away from this process
    volatile bool compositorRegistered; // True while compositor holds a reference to this process's VFB
    // BRO-173: set while a SchedulerKillThreadGroup leader owns this thread's
    // teardown.  While true, ONLY the owning leader (Phase 2, after
    // KRwLockCleanupOnExit) may mark this process reapable — DrainPostSwitch
    // and signal paths must NOT, otherwise the reaper could free the Process
    // out from under the leader's still-pending cleanup (use-after-free).
    volatile bool groupKillOwned;
    // BRO-173: death-latch set on the thread-group LEADER (pid==tgid) when the
    // group begins exiting (exit_group / SchedulerKillThreadGroup), under
    // g_allProcLock.  ProcessCreateThread checks it (also under g_allProcLock)
    // and refuses to spawn a new thread into a dying group — otherwise a thread
    // born after the kill snapshot is never terminated, the group never
    // empties, the leader is never reaped, and the parent's wait/fork loop
    // livelocks (the "Terminated leader keeps creating threads" symptom).
    volatile bool tgidExiting;
    // Linux wait status: normal exit code in bits 8..15, terminating signal
    // in bits 0..6. Store the cause explicitly; exit(128) is not a signal.
    int32_t waitStatus;

    uint64_t launchId; // UI launch token; inherited by fork/threads and preserved across exec
    uint64_t debugJobId; // Debug job lineage; inherited by fork/threads, preserved across exec

    // CPU time accounting (in LAPIC ticks, ~1ms each)
    volatile uint64_t userTicks;     // Ticks spent in user mode
    volatile uint64_t sysTicks;      // Ticks spent in kernel mode

    // Threading
    bool     isThread;           // True if this is a thread (not the group leader)
    Process* threadLeader;       // Points to the thread group leader (self if leader)
    uint64_t clearChildTid;      // User address to write 0 + futex_wake on exit (set_tid_address / CLONE_CHILD_CLEARTID)
    uint64_t parentSetTid;       // User address where parent's clone() stores child TID (CLONE_PARENT_SETTID)
    Process* vforkParent;        // If non-null, wake this process when we exec/exit (CLONE_VFORK)

    // Crash-dump hook: when set, an unhandled synchronous fault in this
    // process is redirected to a user-mode writer thread entered at this VA.
    // Leader-owned; threads inherit via threadLeader. See files/crash-dump-plan.md.
    uint64_t crashEntry;         // 0 = not registered
    bool     crashInProgress;    // Set on fault; prevents recursive dumps

    // Scheduler linked-list pointers (circular doubly-linked ready queue)
    Process* schedNext;
    Process* schedPrev;
    volatile uint32_t inReadyQueue;  // 0 or 1 — debug guard against double-insert

    // Blocking info (e.g. nanosleep wakeup tick)
    uint64_t wakeupTick;        // ApicTickCount() elapsed-ms deadline (0 = N/A)

    // BRO-176 lost-enqueue diagnostic: a small ring of recent scheduler events
    // touching this process. Each entry packs (tick<<16 | site<<8 | state).
    // Dumped by the SCHED STALL detector to show the exact op sequence that left
    // a process state==Ready but absent from the policy ready queue. Pure
    // diagnostic; written O(1) at the key sched sites.
    uint64_t schedTrace[12];
    uint8_t  schedTraceHead;

    // Protected by scheduler readyLock. Sleeping filesystem locks defer fatal
    // cancellation through the complete kernel operation, including gaps between
    // locks (e.g. FAT cache loading). Each queued/held lock owns a Process ref.
    uint32_t kernelRegionDepth;
    bool kernelOperationProtected;
    bool terminalPending;
    bool interruptibleWait; // readyLock: registered wait may unwind on terminalPending
    int pendingWaitStatus;

    // Mutex wait-queue linkage (used by KMutex when process is blocked on a lock)
    Process* syncNext;          // Next process in sync wait queue (mutex/rwlock/semaphore)
    volatile uint32_t pendingWakeup; // Set by KMutexUnlock before SchedulerUnblock;
                                     // checked by SchedulerBlock to avoid lost wakeups.

    // RwLock tracking for cleanup on thread exit
    KRwLock* blockedOnRwLock;   // Non-null if blocked waiting on this rwlock
    KRwLock* heldWriteLock;     // Non-null if holding this rwlock for write
    KRwLock* heldReadLock;      // Non-null if holding this rwlock for read (BRO-162)
    bool     blockedAsWriter;   // True if on write wait queue, false if on read wait queue

    // Saved CPU context (written by context_switch asm)
    SavedContext savedCtx;
    FxsaveArea   fxsave;

    // Per-process kernel stack (for ring 3→0 transitions and syscalls)
    uint64_t kernelStackBase;   // Bottom of kernel stack allocation
    uint64_t kernelStackTop;    // Top of kernel stack (initial RSP0)

    // ELF binary info is task-local; mappings and allocation cursors belong to AS.
    ElfBinary elf;
    uint64_t initialEntry;
    // Per-thread transient fault backing; never inherited by fork/clone.
    Vnode* activeFileFaultVnode;

    // User stack
    uint64_t stackBase;         // Bottom of stack allocation (lowest address)
    uint64_t stackTop;          // Top of stack (highest usable address)

    // TLS
    uint64_t fsBase;            // FS segment base (TLS)

    // File descriptors. Points to a heap-allocated FdEntry[MAX_FDS]. Shared
    // across CLONE_FILES threads of the same thread group (all threads' fds
    // pointer points at the leader's table). fork() allocates a new table
    // and copies entries. The leader (or fork root) frees the table when
    // the process is destroyed.
    FdEntry* fds;
    SpinLock fdLock;             // Protects fds array (alloc/free/dup races)

    // Per-process virtual framebuffer (for compositor)
    // When non-null, fb mmap maps this buffer instead of the physical FB.
    uint32_t* fbVirtual;        // Kernel-mapped virtual framebuffer
    MappingBacking* fbBacking; // retained immutable source snapshot
    KMutex framebufferLock; // source lifetime/geometry, independent of exec AS replacement
    uint32_t  fbVirtualSize;    // Size in bytes
    uint32_t  fbVfbWidth;       // VFB width in pixels
    uint32_t  fbVfbHeight;      // VFB height in pixels
    uint32_t  fbVfbStride;      // VFB stride in pixels (= fbVfbWidth)

    // Per-process GPU (virgl) context id for app GL rendering, or 0 if none.
    // Allocated by GPU_CTX_CREATE (syscall 0xB10); torn down on process exit.
    int32_t   gpuAppCtx;

    // Compositor placement: position and downscale factor on the physical FB.
    // The virtual FB is the full resolution the process thinks it has.
    // The compositor blits the VFB at (fbDestX, fbDestY) with 1:1 pixel mapping.
    int16_t   fbDestX;          // Destination X on physical FB
    int16_t   fbDestY;          // Destination Y on physical FB
    uint8_t   fbScale;          // Downscale factor when blitting to phys FB (1=1:1, 2=half, etc.)
    volatile uint8_t fbDirty;   // Set by process (via write to fb fd), cleared by compositor
    volatile uint32_t fbExitColor; // Non-zero: compositor should fill region with this color and clear it

    // Process name (for debug output)
    char name[32];

    // Absolute path of the current executable for /proc/self/exe.
    char exePath[256];

    // Advisory initial-namespace name only. Never used for lookup authority.
    char cwdDisplay[256];

    // True if this is a kernel-mode thread (ring 0, kernel CR3, no user stack).
    bool isKernelThread;

    // TTY mode: true = canonical (line buffered), false = raw (char at a time)
    bool ttyCanonical;
    bool ttyEcho;
    bool straceEnabled;  // Per-process syscall tracing
    bool straceErrorsOnly; // Only log syscalls that return errors
    uint32_t straceFilter; // 0=all, bitmask of StraceCategory for selective tracing

    // Signal state
    uint64_t sigMask;           // Blocked signals bitmask (bit N = signal N+1)
    uint64_t sigPending;        // Pending signals bitmask
    struct PendingSignalSender { int32_t pid; uint32_t uid; int32_t code; } sigSenders[64];
    uint64_t sigSavedMask;      // Signal mask saved before handler (restored by sigreturn)

    // Signal alternate stack
    uint64_t sigAltstackSp;     // Alternate stack pointer (0 = disabled)
    uint32_t sigAltstackSize;   // Alternate stack size
    uint32_t sigAltstackFlags;  // SS_DISABLE, SS_ONSTACK, etc.

    // Alarm timer (SIGALRM delivery)
    uint64_t alarmTick;         // LAPIC tick at which to deliver SIGALRM (0 = disabled)

    // Signal delivery state
    bool     inSignalHandler;   // Currently executing a user signal handler
    bool     inSignalHandlerOnAltStack; // Current handler entered on sigaltstack
    bool     sigReturnPending;  // Set by sys_rt_sigreturn, handled by SyscallCheckSignals
    uint64_t currentSyscallNum;  // Syscall number (RAX) saved per-process for SA_RESTART

    // Fork child state: when true, the trampoline enters user mode at
    // forkReturnRip with RAX=0 (child's fork() return value).
    bool isForkChild;
    bool stopReported;          // Stopped event consumed; protected by scheduler ready lock
    bool continuedPending;     // Unconsumed SIGCONT event, same lock
    bool resumePending;        // Deferred stop resumption until CPU ownership clears
    uint8_t stopSignal;         // Actual signal for a stopped-child event
    // Stable copy of the user register snapshot taken at syscall entry. The
    // syscall path copies per-CPU %gs fields here before re-enabling interrupts
    // so fork/clone are not racing another syscall on the same CPU.
    uint64_t syscallUserRip;
    uint64_t syscallUserRsp;
    uint64_t syscallUserRflags;
    uint64_t syscallUserRbx;
    uint64_t syscallUserRbp;
    uint64_t syscallUserR12;
    uint64_t syscallUserR13;
    uint64_t syscallUserR14;
    uint64_t syscallUserR15;
    uint64_t syscallUserRdi;
    uint64_t syscallUserRsi;
    uint64_t syscallUserRdx;
    uint64_t syscallUserR8;
    uint64_t syscallUserR9;
    uint64_t syscallUserR10;
    uint64_t forkReturnRip;     // User-mode RIP to resume at (instruction after syscall)
    uint64_t forkReturnRsp;     // User-mode RSP at time of fork
    uint64_t forkReturnRflags;  // User-mode RFLAGS at time of fork
    // Callee-saved registers that must be preserved across syscall for child
    uint64_t forkRbx;
    uint64_t forkRbp;
    uint64_t forkR12;
    uint64_t forkR13;
    uint64_t forkR14;
    uint64_t forkR15;
    // Caller-saved registers — Linux preserves all regs across fork
    uint64_t forkRdi;
    uint64_t forkRsi;
    uint64_t forkRdx;
    uint64_t forkR8;
    uint64_t forkR9;
    uint64_t forkR10;

    // Per-process input queue (WM mode: compositor pushes keyboard events here
    // instead of leaving them in the global input device ring).
    static constexpr uint32_t INPUT_QUEUE_SIZE = 64;
    InputEvent inputQueue[INPUT_QUEUE_SIZE];
    volatile uint32_t inputHead;  // Write index (compositor)
    volatile uint32_t inputTail;  // Read index (process/syscall)

    // Kernel launcher classification, immutable after publication. Fork and
    // threads inherit it; exec preserves it. This grants no Linux credentials.
    ProcessLifecycleRole lifecycleRole;

    // Owned reference; access only through process_fs.h publication helpers.
    FsContext* fsContext;
};

// User backing pages belong to the shared address space, not the allocating
// thread. A thread's PID can be reused while its mappings remain live; charging
// pages to that PID lets the next process's PmmKillPid free a sibling's memory.
// The group leader stays reserved until all address-space users have exited.
static inline uint16_t ProcessAddressSpacePid(const Process* p)
{
    return p->addressSpace ? p->addressSpace->allocationPid : p->pid;
}

// The TLB footprint mask lives in the referenced address space, independent of thread-group
// leadership or exec. Use it for all user-page shootdowns.
static inline volatile uint64_t* AddressSpaceTlbMaskPtr(Process* p)
{
    return &p->addressSpace->tlbCpuMask;
}

// Push an input event to a process's per-process queue (non-blocking).
inline void ProcessInputPush(Process* proc, const InputEvent& ev)
{
    if (!proc || proc->magic != PROCESS_MAGIC ||
        proc->state == ProcessState::Terminated)
        return;
    uint32_t head = __atomic_load_n(&proc->inputHead, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&proc->inputTail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1) % Process::INPUT_QUEUE_SIZE;
    if (next == tail) return; // full — drop
    proc->inputQueue[head] = ev;
    __atomic_store_n(&proc->inputHead, next, __ATOMIC_RELEASE);
}

// Pop an input event from a process's per-process queue (non-blocking).
inline bool ProcessInputPop(Process* proc, InputEvent* out)
{
    uint32_t tail = __atomic_load_n(&proc->inputTail, __ATOMIC_ACQUIRE);
    if (tail == __atomic_load_n(&proc->inputHead, __ATOMIC_ACQUIRE))
        return false;
    *out = proc->inputQueue[tail];
    __atomic_store_n(&proc->inputTail, (tail + 1) % Process::INPUT_QUEUE_SIZE, __ATOMIC_RELEASE);
    return true;
}

// Kernel thread entry point signature.
using KernelThreadFn = void (*)(void* arg);

// CLONE_FILES threads share the leader's table and must share its lock too.
// The leader remains alive until its last address-space thread is reaped.
inline SpinLock* ProcessFdLock(Process* proc)
{
    if (proc->isThread && proc->threadLeader && proc->fds == proc->threadLeader->fds)
        return &proc->threadLeader->fdLock;
    return &proc->fdLock;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Get the current process (for syscall handlers).
Process* ProcessCurrent();

// Find a process by PID (returns nullptr if not found).
Process* ProcessFindByPid(uint16_t pid);

// Create a new process, loading an ELF binary from a memory buffer.
// Returns null on failure.
// If stdFds is non-null, it points to 3 FdEntry structs used for fd 0/1/2
// instead of the default DevKeyboard/serial setup. This allows the caller
// to pre-wire pipe fds for terminal emulation.
Process* ProcessCreate(const uint8_t* elfData, uint64_t elfSize,
                       int argc, const char* const* argv,
                       int envc, const char* const* envp,
                       const FdEntry* stdFds = nullptr,
                       uint32_t launchUid = 0, uint32_t launchGid = 0);

struct ExecutableImage;
// Explicit dedicated-image variants preserve the raw-buffer API and ABI.
Process* ProcessCreateExecutable(const ExecutableImage& image,
    int argc, const char* const* argv, int envc, const char* const* envp,
    const FdEntry* stdFds, uint32_t launchUid, uint32_t launchGid);
uint64_t ProcessExecExecutable(Process* proc, const ExecutableImage& image,
    int argc, const char* const* argv, int envc, const char* const* envp,
    uint64_t* outStackPtr);

uint64_t ProcessExecPreparedExecutable(Process*, const ExecutableImage&,
    const ExecutableImage& interpreter, int argc, const char* const* argv,
    int envc, const char* const* envp, uint64_t* outStackPtr);

CredentialIds ProcessReadIds(Process* proc, bool group);
int ProcessChangeIds(Process* proc, bool group, CredentialChange operation,
                     uint32_t real, uint32_t effective = CredentialUnchanged,
                     uint32_t saved = CredentialUnchanged);
SupplementaryGroups* ProcessGroupsAcquire(Process* proc);
void ProcessGroupsRelease(SupplementaryGroups* groups);
void ProcessGroupsReplace(Process* proc, SupplementaryGroups* owned);
// One credential snapshot for leaf-file authorization. Root receives read/write
// access here; executable authorization needs its own root rules.
struct FileCredentialAccess {
    bool privileged;
    bool owner;
    bool group;
    uint8_t permissions;
};
FileCredentialAccess ProcessFileAccess(Process* proc, uint32_t owner, uint32_t group, uint16_t mode, bool realIds = false);
// Per-task, inherited on fork/clone and preserved across exec.
void ProcessSetNoNewPrivs(Process* proc);
bool ProcessGetNoNewPrivs(Process* proc);
int ProcessCheckFileAccess(Process* proc, uint32_t owner, uint32_t group, uint16_t mode, uint8_t required, bool realIds = false);
uint8_t ProcessFilePermissions(Process* proc, uint32_t owner, uint32_t group, uint16_t mode);

// Create a kernel-mode thread. Runs `fn(arg)` in ring 0 with the kernel's
// page table. Has its own kernel stack and is scheduled like any process.
Process* KernelThreadCreate(const char* name, KernelThreadFn fn, void* arg,
                            uint8_t priority = 2);

// Destroy a process and free all its resources.
void ProcessDestroy(Process* proc);
// Synchronous unwind before SchedulerAddProcess; never use for published tasks.
void ProcessDiscardUnpublished(Process* proc);

// BRO-173/175 liveness refcount.  ProcessRef takes a reference on a Process the
// caller is about to hold a raw pointer to across a window where the reaper
// could otherwise free it; ProcessUnref drops it.  The reaper only frees a
// Terminated process when its refCount has fallen to 0.  ProcessRef callers
// must already hold a valid reference OR g_allProcLock (so the struct cannot be
// freed underneath the increment).  Neither call frees inline — freeing is done
// solely by the reaper in its safe (CPU0, non-ISR) context.
static inline void ProcessRef(Process* p)
{
    if (p) __atomic_add_fetch(&p->refCount, 1, __ATOMIC_ACQ_REL);
}
static inline void ProcessUnref(Process* p)
{
    if (p) __atomic_sub_fetch(&p->refCount, 1, __ATOMIC_ACQ_REL);
}

// Fork the current process, creating a child with a copy of its address space.
// The caller must provide the user-mode context so the child can resume.
// Returns the child Process* on success, or null on failure.
Process* ProcessFork(Process* parent, uint64_t userRip,
                     uint64_t userRsp, uint64_t userRflags,bool shareFs=false);

// Create a new thread in the same address space as parent (CLONE_VM|CLONE_THREAD).
// Shares the page table (no copy), shares fd table, uses the provided user stack.
// Returns the thread Process* on success, or null on failure.
Process* ProcessCreateThread(Process* parent, uint64_t userRip,
                             uint64_t userRsp, uint64_t userRflags,
                             uint64_t tlsBase,bool shareFs=true);

// Create a new user-mode thread inside `target`'s address space, running at
// `entry` with a freshly-allocated user stack.  `argBytes` (length `argLen`)
// is copied onto the top of the new stack; the entry point receives its
// address in RDI.  The new thread shares target's page table, fd table,
// tgid, and signal handlers — effectively a synchronous CLONE_VM|CLONE_FILES
// initiated by the kernel.  Used for crash-dump writers and similar
// in-process injection tasks; reusable primitive (future: debugger attach,
// profiler inject, hot-patching).
//
// Returns the new thread's tid on success, or 0 on failure.  The caller
// must not hold any per-process locks on `target`.
uint16_t CreateRemoteThread(Process* target, uint64_t entry,
                            uint32_t stackSize,
                            const void* argBytes, uint32_t argLen);

// Replace the current process's address space with a new ELF binary.
// On success, returns the new user entry point (and sets *outStackPtr).
// On failure, returns 0 (the process is left in a broken state and should exit).
uint64_t ProcessExec(Process* proc, const uint8_t* elfData, uint64_t elfSize,
                     int argc, const char* const* argv,
                     int envc, const char* const* envp,
                     uint64_t* outStackPtr);

// File descriptor operations
int       FdAlloc(Process* proc, FdType type, void* handle);
void      FdFree(Process* proc, int fd);
FdEntry*  FdGet(Process* proc, int fd);

// Atomically read-and-clear an fd slot, returning its prior owning state in
// *out. Returns true for exactly one caller across threads racing to close
// the same fd, so the handle is unref'd/freed once (BRO-156).
bool      FdClaim(Process* proc, int fd, FdClaimResult* out);

// Pinned fget/fput path (BRO-156). FdGetRef pins a live slot and returns a
// pointer that stays valid (slot can't be reused, handle can't be freed) until
// the matching FdPut. Every FdGetRef that returns non-null MUST be balanced by
// exactly one FdPut on every return path (use the FdRef RAII guard). FdPut runs
// any deferred close teardown when it drops the last pin.
FdEntry*  FdGetRef(Process* proc, int fd);
void      FdPut(Process* proc, int fd);

// Close a slot honouring outstanding pins: claims-and-clears immediately if
// unpinned (ClaimedNow, *out filled), or marks it closing and defers teardown
// to the last FdPut if pinned (Deferred). NotFound if already gone.
FdCloseResult FdClose(Process* proc, int fd, FdClaimResult* out);

// Tear down the handle named by a claimed/finalized fd snapshot (per-type
// unref/free + waiter wakeups). Defined in syscall.cpp; called from both the
// immediate-close path and FdPut's deferred-close path. Must run outside the
// fd table lock.
void FinalizeClosedFd(const FdClaimResult& c);

// Close all file descriptors for a process (called at exit time).
// Properly handles pipe refcounting and wakes blocked readers/writers.
void ProcessReleaseFileFault(Process* proc);
void ProcessCloseAllFds(Process* proc);
void ProcessCloseCloexecFds(Process* proc); // close FD_CLOEXEC fds on execve

// Signal delivery: send a signal to a process.
// Returns 0 on success, -ESRCH if not found, -EINVAL if bad signal.
int ProcessSendSignal(Process* proc, int signum, const SignalInfo* info = nullptr);
bool ProcessDequeueSignal(Process* proc, int signum, SignalInfo* info);
// Non-allocating queue only; caller may hold scheduler locks. No wake/lock inversion.
void ProcessQueueChildSignal(Process* parent,Process* child);
uint64_t ProcessPendingSignals(Process*);
// Scheduler commit only: caller holds ready and registry locks.
void ProcessCommitExecIdentity(Process*);

// Send a signal to all processes in the given process group.
// Returns the number of processes signalled.
int ProcessSendSignalToGroup(uint16_t pgid, int signum);

} // namespace brook

// Called from asm syscall return path (extern "C").
// Checks for pending signals and modifies the kernel stack frame to redirect
// to the user signal handler. Also handles sigreturn frame restoration.
// Returns the value that should be in RAX on return to userspace.
extern "C" int64_t SyscallCheckSignals(brook::SyscallFrame* frame, int64_t syscallResult);
