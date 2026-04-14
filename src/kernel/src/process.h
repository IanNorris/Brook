#pragma once

#include <stdint.h>
#include "memory/address.h"
#include "input.h"

namespace brook {

// Maximum number of open file descriptors per process.
static constexpr uint32_t MAX_FDS = 64;

// Maximum concurrent processes.
static constexpr uint32_t MAX_PROCESSES = 64;

// Program break limit (max heap size per process).
static constexpr uint64_t PROGRAM_BREAK_SIZE = 64 * 1024 * 1024; // 64 MB

// Default user stack size.
static constexpr uint64_t USER_STACK_SIZE = 128 * 1024; // 128 KB

// Per-process kernel stack size (16 pages = 64 KB).
// Must be large enough for syscalls (VFS/FatFS stack depth) and interrupts.
// Each process's kernel stack is also used as its syscall stack (gs:8).
// The allocation includes unmapped guard pages at both ends for overflow detection.
static constexpr uint64_t KERNEL_STACK_SIZE = 64 * 1024;
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

// SA_* flag constants
static constexpr uint64_t SA_SIGINFO  = 0x00000004;
static constexpr uint64_t SA_RESTORER = 0x04000000;

// Per-process signal handlers (indexed by [pid][signal-1])
extern KernelSigaction g_sigHandlers[MAX_PROCESSES][64];

// ---------------------------------------------------------------------------
// Signal frame structures (Linux x86_64 ABI compatible)
// ---------------------------------------------------------------------------
// These must match the Linux kernel's layout so musl/glibc's sigreturn works.

struct SignalMcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip;
    uint64_t eflags;
    uint16_t cs, gs, fs, __pad0;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;          // NULL (no FPU state saved)
    uint64_t __reserved1[8];
};

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
// Used by SyscallCheckSignals to modify saved registers before sysret.
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
};

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
};

// FXSAVE area must be 16-byte aligned, 512 bytes.
struct alignas(16) FxsaveArea
{
    uint8_t data[512];
};

// ---------------------------------------------------------------------------
// File descriptor entry
// ---------------------------------------------------------------------------

enum class FdType : uint8_t
{
    None = 0,
    Vnode,         // Regular VFS file
    DevFramebuf,   // /dev/fb0
    DevKeyboard,   // /dev/keyboard
    Pipe,          // pipe() read/write end
    DevNull,       // /dev/null — discard writes, EOF on read
    SyntheticMem,  // In-memory synthetic file (e.g. /etc/passwd)
};

struct FdEntry
{
    FdType   type;
    uint8_t  flags;        // O_NONBLOCK, pipe direction, etc.
    uint8_t  fdFlags;      // FD-level flags: FD_CLOEXEC (bit 0)
    uint8_t  _pad;
    uint32_t refCount;
    uint32_t statusFlags;  // Linux O_* flags from open (for F_GETFL/F_SETFL)
    void*    handle;       // VFS Vnode* or device-specific state
    uint64_t seekPos;      // Current file offset (for lseek)
    char     dirPath[64];  // For directory fds: path prefix for openat resolution
};

// ---------------------------------------------------------------------------
// ELF binary descriptor (populated by the ELF loader)
// ---------------------------------------------------------------------------

struct ElfBinary
{
    uint64_t baseAddress;       // Virtual base where binary was loaded
    uint64_t allocatedSize;     // Total bytes mapped
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

struct Process
{
    uint16_t pid;
    uint16_t parentPid;          // Parent process PID (0 if no parent)
    uint16_t pgid;               // Process group ID
    uint16_t sid;                // Session ID
    ProcessState state;
    uint8_t  schedPriority;  // Initial scheduler priority (0=RT, 1=High, 2=Normal, 3=Low)
    int32_t  runningOnCpu;   // CPU index (-1 = not running, used for double-schedule detection)
    volatile bool reapable;  // Set after context_switch completes away from this process
    volatile bool compositorRegistered; // True while compositor holds a reference to this process's VFB
    int32_t exitStatus;      // Exit status (stored when process exits, for wait4)

    // Scheduler linked-list pointers (circular doubly-linked ready queue)
    Process* schedNext;
    Process* schedPrev;
    volatile uint32_t inReadyQueue;  // 0 or 1 — debug guard against double-insert

    // Blocking info (e.g. nanosleep wakeup tick)
    uint64_t wakeupTick;        // g_lapicTickCount value to unblock at (0 = N/A)

    // Mutex wait-queue linkage (used by KMutex when process is blocked on a lock)
    Process* syncNext;          // Next process in sync wait queue (mutex/rwlock/semaphore)
    volatile uint32_t pendingWakeup; // Set by KMutexUnlock before SchedulerUnblock;
                                     // checked by SchedulerBlock to avoid lost wakeups.

    // Saved CPU context (written by context_switch asm)
    SavedContext savedCtx;
    FxsaveArea   fxsave;

    // Per-process kernel stack (for ring 3→0 transitions and syscalls)
    uint64_t kernelStackBase;   // Bottom of kernel stack allocation
    uint64_t kernelStackTop;    // Top of kernel stack (initial RSP0)

    // Per-process page table
    PageTable pageTable;

    // ELF binary info
    ElfBinary elf;
    uint64_t  initialEntry;     // Actual entry point (interpreter entry if dynamically linked)

    // Memory
    uint64_t programBreak;      // Current brk() position
    uint64_t mmapNext;          // Next free user-space mmap virtual address

    // User stack
    uint64_t stackBase;         // Bottom of stack allocation (lowest address)
    uint64_t stackTop;          // Top of stack (highest usable address)

    // TLS
    uint64_t fsBase;            // FS segment base (TLS)

    // File descriptors
    FdEntry  fds[MAX_FDS];

    // Per-process virtual framebuffer (for compositor)
    // When non-null, fb mmap maps this buffer instead of the physical FB.
    uint32_t* fbVirtual;        // Kernel-mapped virtual framebuffer
    uint32_t  fbVirtualSize;    // Size in bytes
    uint32_t  fbVfbWidth;       // VFB width in pixels
    uint32_t  fbVfbHeight;      // VFB height in pixels
    uint32_t  fbVfbStride;      // VFB stride in pixels (= fbVfbWidth)

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

    // Working directory (for relative path resolution)
    char cwd[64];

    // True if this is a kernel-mode thread (ring 0, kernel CR3, no user stack).
    bool isKernelThread;

    // TTY mode: true = canonical (line buffered), false = raw (char at a time)
    bool ttyCanonical;
    bool ttyEcho;
    bool straceEnabled;  // Per-process syscall tracing

    // Signal state
    uint64_t sigMask;           // Blocked signals bitmask (bit N = signal N+1)
    uint64_t sigPending;        // Pending signals bitmask
    uint64_t sigSavedMask;      // Signal mask saved before handler (restored by sigreturn)

    // Signal alternate stack
    uint64_t sigAltstackSp;     // Alternate stack pointer (0 = disabled)
    uint32_t sigAltstackSize;   // Alternate stack size
    uint32_t sigAltstackFlags;  // SS_DISABLE, SS_ONSTACK, etc.

    // Alarm timer (SIGALRM delivery)
    uint64_t alarmTick;         // LAPIC tick at which to deliver SIGALRM (0 = disabled)

    // Signal delivery state
    bool     inSignalHandler;   // Currently executing a user signal handler
    bool     sigReturnPending;  // Set by sys_rt_sigreturn, handled by SyscallCheckSignals

    // Fork child state: when true, the trampoline enters user mode at
    // forkReturnRip with RAX=0 (child's fork() return value).
    bool isForkChild;
    bool stopReported;          // True after wait4 reported this process as stopped
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
};

// Push an input event to a process's per-process queue (non-blocking).
inline void ProcessInputPush(Process* proc, const InputEvent& ev)
{
    uint32_t next = (proc->inputHead + 1) % Process::INPUT_QUEUE_SIZE;
    if (next == proc->inputTail) return; // full — drop
    proc->inputQueue[proc->inputHead] = ev;
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
                       const FdEntry* stdFds = nullptr);

// Create a kernel-mode thread. Runs `fn(arg)` in ring 0 with the kernel's
// page table. Has its own kernel stack and is scheduled like any process.
Process* KernelThreadCreate(const char* name, KernelThreadFn fn, void* arg,
                            uint8_t priority = 2);

// Destroy a process and free all its resources.
void ProcessDestroy(Process* proc);

// Fork the current process, creating a child with a copy of its address space.
// The caller must provide the user-mode context so the child can resume.
// Returns the child Process* on success, or null on failure.
Process* ProcessFork(Process* parent, uint64_t userRip,
                     uint64_t userRsp, uint64_t userRflags);

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

// Close all file descriptors for a process (called at exit time).
// Properly handles pipe refcounting and wakes blocked readers/writers.
void ProcessCloseAllFds(Process* proc);
FdEntry*  FdGet(Process* proc, int fd);

// Signal delivery: send a signal to a process.
// Returns 0 on success, -ESRCH if not found, -EINVAL if bad signal.
int ProcessSendSignal(Process* proc, int signum);

// Send a signal to all processes in the given process group.
// Returns the number of processes signalled.
int ProcessSendSignalToGroup(uint16_t pgid, int signum);

} // namespace brook

// Called from asm syscall return path (extern "C").
// Checks for pending signals and modifies the kernel stack frame to redirect
// to the user signal handler. Also handles sigreturn frame restoration.
// Returns the value that should be in RAX on return to userspace.
extern "C" int64_t SyscallCheckSignals(brook::SyscallFrame* frame, int64_t syscallResult);
