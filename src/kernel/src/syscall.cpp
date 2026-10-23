#include "process_fs.h"
#include "mount_namespace.h"
#include "tmpfs.h"
#include "tmpfs_mount_options.h"
#include "process_user_namespace.h"
#include "unix_lifetime_trace.h"
#include "file_locks.h"
#include "memfd_seals.h"
// syscall.cpp -- SYSCALL/SYSRET dispatcher, syscall table, and user-mode entry.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "syscall.h"
#include "robust_futex.h"
#include "posix_timer.h"
#include "sysv_sem.h"
#include "signal_fd.h"
#include "unix_credentials.h"
#include "timeout_value.h"
#include "timerfd_state.h"
#include "dsp_geometry.h"
#include "futex_timeout.h"
#include "sync_file.h"
#include "gpu_reservation.h"
#include "busybox_applets.h"
#include "cpu.h"
#include "gdt.h"
#include "serial.h"
#include "panic.h"
#include "process.h"
#include "desktop_identity.h"
#include "exec_limits.h"
#include "drm_map_tokens.h"
#include "drm_backing_diagnostics.h"
#include "sync/ksemaphore.h"
#include "exec_image.h"
#include "fpu_irq.h"
#include "scheduler.h"
#include "spinlock.h"
#include "sync/futex_waiter_pool.h"
#include "sync/kmutex.h"
#include "string.h"
#include "memory/virtual_memory.h"
#include "memory/mapping_transaction.h"
#include "memory/file_page_cache.h"
#include "memory/file_object.h"
#include "memory/user_mapping.h"
#include "memory/mapping_fault.h"
#include "memory/mapping_fault_test.h"
#include "register_test.h"
#include "memory/physical_memory.h"
#include "vfs.h"
#include "ext2_vfs.h"
#include "tty.h"
#include "input.h"
#include "pipe.h"
#include "memory/heap.h"
#include "memory/object_cache.h"
#include "epoll_diagnostics.h"
#include "compositor.h"
#include "compositor_scene.h"
#include "window.h"
#include "gpu_app.h"
#include "gpu_compositor.h"
#include "terminal.h"
#include "net.h"
#include "rtc.h"
#include "kvmclock.h"
#include "audio.h"
#include "dsp_write_plan.h"
#include "dsp_producer_timing.h"
#include "syscall_timing.h"
#include "profiler.h"
#include "smp.h"
#include "apic.h"
#include "serial_writer.h"
#include "debug_overlay.h"
#include "user_copy.h"
#include "gs_paranoid.h"

// Forward declaration
extern "C" __attribute__((naked)) void ReturnToKernel();
extern "C" bool MemFdHandleUserFault(uint64_t cr2, uint64_t errCode);
extern "C" bool FileMapHandleUserFault(uint64_t cr2, uint64_t errCode);

using brook::UserBufferReadable;
using brook::UserBufferWritable;
using brook::CopyToUser;
using brook::CopyUserCString;

// ---------------------------------------------------------------------------
// Random helpers: RDRAND with TSC fallback
// ---------------------------------------------------------------------------

// Software PRNG fallback using TSC + LCG mixing (used when rdrand unavailable).
static uint64_t SoftRandU64()
{
    uint64_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = (hi << 32) | lo;
    tsc ^= tsc << 13;
    tsc ^= tsc >> 7;
    tsc ^= tsc << 17;
    return tsc * 6364136223846793005ULL + 1442695040888963407ULL;
}

static bool RdrandU64(uint64_t* out)
{
    if (!CpuHasRdrand()) return false;
    uint64_t val;
    uint8_t ok;
    for (int i = 0; i < 10; i++)
    {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        if (ok) { *out = val; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// C dispatch wrapper — reads syscall number from GS:120, applies strace.
// Must be extern "C" for the assembly call instruction.
// ---------------------------------------------------------------------------

namespace brook { int64_t SyscallDispatchInternal(uint64_t num, uint64_t a0, uint64_t a1,
                                                   uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5); }

extern "C" int64_t SyscallDispatchC(uint64_t a0, uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t num;
    asm volatile("movq %%gs:120, %0" : "=r"(num));

    return brook::SyscallDispatchInternal(num, a0, a1, a2, a3, a4, a5);
}

extern "C" void SyscallSnapshotCurrentC()
{
    uint64_t procPtr;
    uint64_t userRip, userRsp, userRflags;
    uint64_t userRbx, userRbp, userR12, userR13, userR14, userR15;
    uint64_t userRdi, userRsi, userRdx, userR8, userR9, userR10;
    uint64_t syscallNum;

    asm volatile("movq %%gs:184, %0" : "=r"(procPtr));
    asm volatile("movq %%gs:48,  %0" : "=r"(userRip));
    asm volatile("movq %%gs:56,  %0" : "=r"(userRsp));
    asm volatile("movq %%gs:64,  %0" : "=r"(userRflags));
    asm volatile("movq %%gs:72,  %0" : "=r"(userRbx));
    asm volatile("movq %%gs:80,  %0" : "=r"(userRbp));
    asm volatile("movq %%gs:88,  %0" : "=r"(userR12));
    asm volatile("movq %%gs:96,  %0" : "=r"(userR13));
    asm volatile("movq %%gs:104, %0" : "=r"(userR14));
    asm volatile("movq %%gs:112, %0" : "=r"(userR15));
    asm volatile("movq %%gs:128, %0" : "=r"(userRdi));
    asm volatile("movq %%gs:136, %0" : "=r"(userRsi));
    asm volatile("movq %%gs:144, %0" : "=r"(userRdx));
    asm volatile("movq %%gs:152, %0" : "=r"(userR8));
    asm volatile("movq %%gs:160, %0" : "=r"(userR9));
    asm volatile("movq %%gs:168, %0" : "=r"(userR10));
    asm volatile("movq %%gs:120, %0" : "=r"(syscallNum));

    auto* proc = reinterpret_cast<brook::Process*>(procPtr);
    if (!proc)
        return;

    proc->syscallUserRip = userRip;
    proc->syscallUserRsp = userRsp;
    proc->syscallUserRflags = userRflags;
    proc->syscallUserRbx = userRbx;
    proc->syscallUserRbp = userRbp;
    proc->syscallUserR12 = userR12;
    proc->syscallUserR13 = userR13;
    proc->syscallUserR14 = userR14;
    proc->syscallUserR15 = userR15;
    proc->syscallUserRdi = userRdi;
    proc->syscallUserRsi = userRsi;
    proc->syscallUserRdx = userRdx;
    proc->syscallUserR8 = userR8;
    proc->syscallUserR9 = userR9;
    proc->syscallUserR10 = userR10;
    proc->currentSyscallNum = syscallNum;
}

// ---------------------------------------------------------------------------
// SYSCALL dispatcher -- naked assembly, pointed to by LSTAR MSR.
// ---------------------------------------------------------------------------

extern "C" __attribute__((naked, used)) void BrookSyscallDispatcher()
{
    __asm__ volatile(
        "swapgs\n\t"
        "movq %%r11, %%gs:24\n\t"      // stash user RFLAGS
        "mov %%rsp, %%r11\n\t"          // r11 = user RSP
        "mov %%gs:8, %%rsp\n\t"
        "and $~0xF, %%rsp\n\t"
        // Keep all existing frame/RBP offsets and 16-byte call alignment.
        "sub $64, %%rsp\n\t"
        "movq $0, (%%rsp)\n\t"          // this entry defaults to SYSRET
        "push %%r11\n\t"               // [1] user RSP
        "push %%rcx\n\t"               // [2] user return address
        "movq %%gs:24, %%r11\n\t"       // reload user RFLAGS
        "push %%r11\n\t"               // [3] user RFLAGS
        "push %%rbp\n\t"               // [4] user RBP
        "mov %%rsp, %%rbp\n\t"
        "push %%rdx\n\t"               // [5]
        "push %%rsi\n\t"               // [6]
        "push %%rdi\n\t"               // [7]
        "push %%r8\n\t"                // [8]
        "push %%r9\n\t"                // [9]
        "push %%r10\n\t"               // [10]
        "push %%r11\n\t"               // [11]
        "push %%r12\n\t"               // [12]
        "push %%r13\n\t"               // [13]
        "push %%r14\n\t"               // [14]
        "push %%r15\n\t"               // [15]
        "push %%rbx\n\t"               // [16] — 16 pushes = 128 bytes, aligned
        "mov %%r10, %%rcx\n\t"
        "movq %%rax, %%gs:120\n\t"       // save syscall number for debug
        "mov %%gs:16, %%r12\n\t"
        // Save user context in KernelCpuEnv for fork().
        // Use R15 as temp (saved at [rbp-88]).
        "mov 16(%%rbp), %%r15\n\t"       // user RCX (return addr)
        "movq %%r15, %%gs:48\n\t"        // -> syscallUserRip
        "mov 24(%%rbp), %%r15\n\t"       // user RSP
        "movq %%r15, %%gs:56\n\t"        // -> syscallUserRsp
        "mov 8(%%rbp), %%r15\n\t"        // user RFLAGS (R11)
        "movq %%r15, %%gs:64\n\t"        // -> syscallUserRflags
        // Save callee-saved registers for fork child
        "mov -96(%%rbp), %%r15\n\t"      // RBX at [rbp-96]
        "movq %%r15, %%gs:72\n\t"        // -> syscallUserRbx
        "mov 0(%%rbp), %%r15\n\t"        // user RBP at [rbp+0]
        "movq %%r15, %%gs:80\n\t"        // -> syscallUserRbp
        "mov -64(%%rbp), %%r15\n\t"      // R12 at [rbp-64]
        "movq %%r15, %%gs:88\n\t"        // -> syscallUserR12
        "mov -72(%%rbp), %%r15\n\t"      // R13 at [rbp-72]
        "movq %%r15, %%gs:96\n\t"        // -> syscallUserR13
        "mov -80(%%rbp), %%r15\n\t"      // R14 at [rbp-80]
        "movq %%r15, %%gs:104\n\t"       // -> syscallUserR14
        "mov -88(%%rbp), %%r15\n\t"      // R15 at [rbp-88]
        "movq %%r15, %%gs:112\n\t"       // -> syscallUserR15
        // Caller-saved registers (needed for fork — Linux preserves all regs)
        "mov -24(%%rbp), %%r15\n\t"      // RDI at [rbp-24]
        "movq %%r15, %%gs:128\n\t"       // -> syscallUserRdi
        "mov -16(%%rbp), %%r15\n\t"      // RSI at [rbp-16]
        "movq %%r15, %%gs:136\n\t"       // -> syscallUserRsi
        "mov -8(%%rbp), %%r15\n\t"       // RDX at [rbp-8]
        "movq %%r15, %%gs:144\n\t"       // -> syscallUserRdx
        "mov -32(%%rbp), %%r15\n\t"      // R8 at [rbp-32]
        "movq %%r15, %%gs:152\n\t"       // -> syscallUserR8
        "mov -40(%%rbp), %%r15\n\t"      // R9 at [rbp-40]
        "movq %%r15, %%gs:160\n\t"       // -> syscallUserR9
        "mov -48(%%rbp), %%r15\n\t"      // R10 at [rbp-48]
        "movq %%r15, %%gs:168\n\t"       // -> syscallUserR10
        // Syscalls preserve user vector state even though the C handlers may
        // use SSE. Reuse the IRQ stack-save convention before the first C
        // call: R14 retains the unchanged GPR-frame base, R15 the 64-byte
        // aligned XSAVE area. Both are callee-saved across blocking switches.
        // At most 1215 additional bytes are reserved on the 256 KiB stack;
        // the save area sits above the C stack and below the GPR frame.
        BROOK_FPU_SAVE_IRQ
        "call SyscallSnapshotCurrentC\n\t"
        "mov -24(%%rbp), %%rdi\n\t"      // a0
        "mov -16(%%rbp), %%rsi\n\t"      // a1
        "mov -8(%%rbp), %%rdx\n\t"       // a2
        "mov -48(%%rbp), %%rcx\n\t"      // a3 (user R10)
        "mov -32(%%rbp), %%r8\n\t"       // a4
        "mov -40(%%rbp), %%r9\n\t"       // a5
        "sti\n\t"
        "call SyscallDispatchC\n\t"
        "cli\n\t"
        // Check for pending signals before returning to userspace.
        // Pass the saved-register frame and syscall return value.
        "mov %%r14, %%rdi\n\t"         // original SyscallFrame, above XSAVE area
        "mov %%rax, %%rsi\n\t"         // RSI = syscall result
        "call SyscallCheckSignals\n\t"  // returns new RAX
        "jmp .Lsyscall_return\n\t"
        ".Lsyscall_invalid:\n\t"
        "mov $-38, %%rax\n\t"
        ".Lsyscall_return:\n\t"
        // XRSTOR uses EDX:EAX as its mask. Preserve the final syscall result
        // (including rt_sigreturn's restored RAX) in a saved scratch register.
        "mov %%rax, %%r13\n\t"
        BROOK_FPU_RESTORE_IRQ
        "mov %%r13, %%rax\n\t"
        "pop %%rbx\n\t"                // [16]
        "pop %%r15\n\t"                // [15]
        "pop %%r14\n\t"                // [14]
        "pop %%r13\n\t"                // [13]
        "pop %%r12\n\t"                // [12]
        "pop %%r11\n\t"                // [11]
        "pop %%r10\n\t"                // [10]
        "pop %%r9\n\t"                 // [9]
        "pop %%r8\n\t"                 // [8]
        "pop %%rdi\n\t"                // [7]
        "pop %%rsi\n\t"                // [6]
        "pop %%rdx\n\t"                // [5]
        "pop %%rbp\n\t"                // [4]
        // RSP is now frame+104. Signal return must restore RCX and R11 as
        // ordinary GPRs; SYSRET would replace them with RIP and RFLAGS.
        "cmpq $0, 24(%%rsp)\n\t"        // returnState.mode at frame+128
        "jne .Lsigreturn_iret\n\t"
        "pop %%r11\n\t"                // [3] user RFLAGS
        "pop %%rcx\n\t"                // [2] user return address
        // Every high bit must be clear for Brook's low-47-bit user range.
        // Bit 47 alone misses noncanonical values with higher bits set.
        // The scratch push reuses the consumed RIP slot, preserving RAX/RSP.
        "push %%rax\n\t"
        "mov %%rcx, %%rax\n\t"
        "shr $47, %%rax\n\t"
        "test %%rax, %%rax\n\t"
        "pop %%rax\n\t"
        "jnz .Lsysret_bad_rcx\n\t"
        "swapgs\n\t"
        "mov (%%rsp), %%rsp\n\t"       // [1] user RSP
        ".byte 0x48\n\t"
        "sysret\n\t"
        ".Lsysret_bad_rcx:\n\t"
        "mov $0x3F8, %%dx\n\t"
        "mov $0x58, %%al\n\t"
        "outb %%al, (%%dx)\n\t"
        "int3\n\t"
        "cli\n\t"
        ".Lsysret_halt:\n\t"
        "hlt\n\t"
        "jmp .Lsysret_halt\n\t"
        ".Lsigreturn_iret:\n\t"
        "mov 32(%%rsp), %%rcx\n\t"      // frame+136, original signal RCX
        "add $40, %%rsp\n\t"            // frame+144, validated IRET frame
        IRETQ_NORMALIZE_RFLAGS
        "swapgs\n\t"
        "iretq\n\t"
        ::: "memory"
    );
}

namespace brook {

// ---------------------------------------------------------------------------
// Profiling counters (atomic, dumped on title changes)
// ---------------------------------------------------------------------------
static volatile uint64_t g_profOpenCount = 0;
static volatile uint64_t g_profMmapCount = 0;
extern volatile uint64_t g_profFaultCount;  // defined in idt.cpp
static volatile uint64_t g_profCacheHit = 0;
static volatile uint64_t g_profCacheMiss = 0;
static volatile uint64_t g_profPreloadPages = 0;

// ---------------------------------------------------------------------------
// Error codes (Linux)
// ---------------------------------------------------------------------------

static constexpr int64_t ENOENT  = 2;
static constexpr int64_t EEXIST  = 17;
static constexpr int64_t ESRCH   = 3;
static constexpr int64_t E2BIG   = 7;
static constexpr int64_t EPERM   = 1;
static constexpr int64_t EBADF   = 9;
static constexpr int64_t ENOMEM  = 12;
static constexpr int64_t EFAULT  = 14;
static constexpr int64_t ENODEV  = 19;
static constexpr int64_t EINVAL  = 22;
static constexpr int64_t ENOEXEC = 8;
static constexpr int64_t EMFILE  = 24;
static constexpr int64_t EPIPE   = 32;
static constexpr int64_t ERANGE  = 34;
static constexpr int64_t ENOSYS  = 38;
static constexpr int64_t EAGAIN  = 11;
static constexpr int64_t EINTR   = 4;
static constexpr int64_t ENOTDIR = 20;
static constexpr int64_t EIO     = 5;
static constexpr int64_t ENOTCONN = 107;
static constexpr int64_t EAFNOSUPPORT = 97;
static constexpr int64_t ECONNREFUSED = 111;
static constexpr int64_t ETIMEDOUT    = 110;
static constexpr int64_t ELOOP       = 40;

// Temporary masks survive the blocking C handler until signal selection on
// syscall return. A PID can be reused after a killed waiter, so pair each slot
// with its process incarnation. Only that process installs/consumes its slot;
// this state is neither shared by threads nor inherited by fork/exec.
struct WaitSignalMask {
    uint64_t incarnation;
    uint64_t original;
    bool active;
};
static WaitSignalMask g_waitSignalMasks[MAX_PROCESSES] = {};

static int InstallWaitSignalMask(uint64_t address, uint64_t size)
{
    if (!address) return 0;
    if (size != sizeof(uint64_t)) return -EINVAL;
    if (!UserBufferReadable(address, sizeof(uint64_t))) return -EFAULT;
    Process* proc = ProcessCurrent();
    if (!proc || proc->pid >= MAX_PROCESSES) return -ESRCH;
    uint64_t mask = *reinterpret_cast<const uint64_t*>(address);
    // SIGKILL and SIGSTOP cannot be blocked, including through these APIs.
    mask &= ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));
    auto& saved = g_waitSignalMasks[proc->pid];
    saved.incarnation = proc->incarnation;
    saved.original = proc->sigMask;
    saved.active = true;
    proc->sigMask = mask;
    return 0;
}

// Consumed by SyscallCheckSignals before choosing a signal under the temporary
// mask. The signal frame must restore this original mask via rt_sigreturn.
extern "C" bool SyscallTakeWaitSignalMask(Process* proc, uint64_t* original)
{
    if (!proc || proc->pid >= MAX_PROCESSES) return false;
    auto& saved = g_waitSignalMasks[proc->pid];
    bool active = saved.active && saved.incarnation == proc->incarnation;
    saved.active = false;
    if (active) *original = saved.original;
    return active;
}

// Check if the current process has deliverable signals pending.
// Call after SchedulerBlock() returns to decide whether to return -EINTR.
static bool HasPendingSignals()
{
    Process* proc = ProcessCurrent();
    if (!proc) return false;
    return __atomic_load_n(&proc->terminalPending, __ATOMIC_ACQUIRE) ||
           ((ProcessPendingSignals(proc) | PosixTimerPending(proc)) & ~proc->sigMask) != 0;
}

// Child exit also queues default-ignored SIGCHLD to wake wait4. Blocking
// reads may discard that notification only when no user handler needs it.
// In particular, libuv's exec-handshake read must preserve caught SIGCHLD
// so its handler can notify the event loop and reap the exited child.
static bool HasReadInterruptSignal()
{
    Process* proc = ProcessCurrent();
    if (!proc) return false;
    constexpr uint64_t childBit = 1ULL << (17 - 1);
    if ((ProcessPendingSignals(proc) & ~proc->sigMask & childBit) && proc->tgid < MAX_PROCESSES) {
        uint64_t handler = ReadSignalAction(proc->tgid,17 - 1).handler;
        if (handler == 0 || handler == 1)
            ProcessDequeueSignal(proc, 17, nullptr);
    }
    return HasPendingSignals();
}

static void WakeProcess(Process* proc)
{
    uint64_t addr = reinterpret_cast<uint64_t>(proc);
    if (!proc || (addr >> 47) != 0x1FFFFULL) return;
    if (proc->magic != PROCESS_MAGIC) return;
    __atomic_store_n(&proc->pendingWakeup, 1, __ATOMIC_RELEASE);
    SchedulerUnblock(proc);
}

#include "pipe_poll.inc"

static void SyncFileRef(SyncFileData* file) { __atomic_fetch_add(&file->references,1,__ATOMIC_RELAXED); }
static void SyncFileUnref(SyncFileData* file) { if(__atomic_fetch_sub(&file->references,1,__ATOMIC_ACQ_REL)==1)kfree(file); }

// EventFd data structure (used in read/write/poll/close)
struct EventFdData {
    KMutex lock; // graph -> eventfd; never acquire graph while holding this
    uint64_t counter;
    uint64_t inGeneration;
    uint64_t outGeneration;
    uint64_t identity;
    uint32_t flags;
    volatile uint32_t refCount;
    Process* readerWaiter;
    Process* epollWaiter;  // process blocked in epoll_wait watching this eventfd
};
static uint64_t g_eventFdIdentity = 0;
static void EventFdInit(EventFdData* efd, uint64_t counter, uint32_t flags) {
    __builtin_memset(efd, 0, sizeof(*efd));
    efd->counter = counter;
    efd->flags = flags;
    efd->refCount = 1;
    efd->identity = __atomic_add_fetch(&g_eventFdIdentity, 1, __ATOMIC_RELAXED);
}
struct EventFdGuard {
    EventFdData* efd;
    explicit EventFdGuard(EventFdData* value) : efd(value) { KMutexLock(&efd->lock); }
    ~EventFdGuard() { KMutexUnlock(&efd->lock); }
};
struct EventFdSnapshot {
    uint64_t identity, inGeneration, outGeneration;
    uint32_t ready;
};
static EventFdSnapshot EventFdReady(EventFdData* efd, uint32_t events) {
    EventFdGuard guard(efd);
    uint32_t ready = 0;
    if (efd->counter && (events & 1)) ready |= 1;
    if (efd->counter < ~0ULL - 1 && (events & 4)) ready |= 4;
    return {efd->identity, efd->inGeneration, efd->outGeneration, ready};
}
static constexpr uint32_t EFD_SEMAPHORE = 0x01;
static constexpr uint32_t EFD_CLOEXEC = 0x80000;
static constexpr uint32_t EFD_NONBLOCK  = 0x800;

// timerfd constants and data — defined here so sys_read/sys_poll can use them
static constexpr int TFD_NONBLOCK     = 0x800;
static constexpr int TFD_CLOEXEC      = 0x80000;
static constexpr int TFD_TIMER_ABSTIME = 1;
static uint64_t ClockNowNs(uint64_t clockid);
static uint64_t WaitWakeupTick(uint64_t deadlineNs, uint64_t safetyMs);
struct TimerWaitNode;
struct TimerFdData {
    KMutex lock; // graph -> timer; no graph acquisition under this mutex
    brook::TimerFdState state;
    int clockId;
    volatile uint32_t refCount;
    TimerWaitNode* waiters;
    uint64_t rearmGeneration;
};
struct TimerFdGuard {
    TimerFdData* timer;
    explicit TimerFdGuard(TimerFdData* t) : timer(t) { KMutexLock(&t->lock); }
    ~TimerFdGuard() { KMutexUnlock(&timer->lock); }
};
static bool TimerFdReady(TimerFdData* timer) {
    TimerFdGuard guard(timer);
    timer->state.refresh(ClockNowNs(timer->clockId));
    return timer->state.pending != 0;
}
// A selected-clock deadline is never a LAPIC tick or a monotonic timestamp.
static uint64_t TimerFdDeadline(TimerFdData* timer, uint64_t* generation = nullptr) {
    TimerFdGuard guard(timer);
    const uint64_t now = ClockNowNs(timer->clockId);
    timer->state.refresh(now);
    if (generation) *generation = __atomic_load_n(&timer->rearmGeneration, __ATOMIC_RELAXED);
    const uint64_t mono = ClockNowNs(1);
    if (timer->state.pending) return mono;
    if (!timer->state.armed) return ~0ULL;
    const uint64_t remaining = timer->state.deadline - now;
    return remaining > ~0ULL - mono ? ~0ULL : mono + remaining;
}

struct EpollEvent {
    uint32_t events;
    union {
        int      fd;
        uint64_t u64;
        uint32_t u32;
        void*    ptr;
    } data;
} __attribute__((packed)); // Linux x86_64 ABI: packed to 12 bytes

static constexpr int EPOLL_MAX_FDS = MAX_FDS;

struct EpollEntry {
    int      fd;      // watched fd (-1 = free)
    uint32_t events;  // requested events
    uint64_t data;    // user data (uint64)
    uint64_t eventFdIdentity = 0; // zero means initial/MOD delivery is pending
    uint64_t inGeneration = 0;
    uint64_t outGeneration = 0;
};

struct EpollInstance {
    EpollEntry entries[EPOLL_MAX_FDS];
    int        count;
    volatile uint32_t refCount; // +1 per fd sharing this epoll instance
    Process*   waiter; // process blocked in epoll_wait
};

// memfd constants and data — defined here so sys_read/sys_write/fstat can use them
static constexpr uint32_t MFD_CLOEXEC       = 0x0001u;
static constexpr uint32_t MFD_ALLOW_SEALING = 0x0002u;

// Sparse / lazy memfd backing.
//
// GTK's gdk-wayland speculatively ftruncates ~700 MiB for an SHM cache and
// then mmap()s the whole region, but only ever touches a tiny slice. Eagerly
// allocating the full backing would OOM the guest. Instead we keep a per-fd
// page-index array: pageMap[i] is either 0 (unbacked, reads-as-zero) or the
// physical address of a 4 KiB page. Pages are allocated on first access —
// either via kernel sys_read/sys_write to the fd, or via a user #PF on a
// MAP_SHARED mapping (handled in idt.cpp). Multiple processes sharing the
// fd share the same physical pages, which is the wl_shm contract.
struct MemFdData {
    uint64_t  inode;            // stable backing-object identity, shared by dup/fork/SCM_RIGHTS
    uint64_t* pageMap;          // [pageMapCount] entries; raw PhysicalAddress.value, 0 = unbacked
    uint64_t  pageMapCount;     // number of slots; covers `capacity` bytes
    uint64_t  size;             // logical size set by truncate/write/fallocate
    uint64_t  capacity;         // pageMapCount * 4096
    volatile uint32_t lock;     // spinlock protecting pageMap allocations + grow
    volatile uint32_t refCount; // +1 per fd, +1 per live mmap VMA
    MemFdSealState seals{};     // protected by lock; one lease per mapping backing
};

static uint64_t NextMemFdInode()
{
    static uint64_t next = 1;
    uint64_t candidate=__atomic_load_n(&next,__ATOMIC_RELAXED);
    for(;;) {
        // The high owner bit distinguishes shared futex keys from task IDs.
        // Saturate instead of ever reusing an identity after wraparound.
        if(candidate>=(uint64_t{1}<<62))return 0;
        if(__atomic_compare_exchange_n(&next,&candidate,candidate+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return candidate;
    }
}

static inline void MfdLock(MemFdData* mfd)
{
    if (Process* self = ProcessCurrent()) SchedulerKernelRegionEnter(self);
    while (__atomic_exchange_n(&mfd->lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}
static inline void MfdUnlock(MemFdData* mfd)
{
    __atomic_store_n(&mfd->lock, 0, __ATOMIC_RELEASE);
    if (Process* self = ProcessCurrent()) SchedulerKernelRegionLeave(self);
}

// Grow pageMap to cover at least `needed` bytes. Does NOT allocate any
// physical pages — those are demand-paged. Caller must hold mfd->lock.
static bool MemFdGrowLocked(MemFdData* mfd, uint64_t needed)
{
    if (needed <= mfd->capacity) return true;
    // Cap at 16 GiB of address space so the index array itself stays small
    // (16 GiB / 4 KiB * 8 = 32 MiB index). Past this is almost certainly an
    // app pathology and -ENOMEM is the right answer.
    static constexpr uint64_t MEMFD_MAX = 16ULL * 1024 * 1024 * 1024;
    if (needed > MEMFD_MAX) {
        SerialPrintf("MemFdGrow: needed=%lu > MEMFD_MAX (%lu), returning ENOMEM\n",
                     needed, MEMFD_MAX);
        return false;
    }

    uint64_t newCount = (needed + 4095) / 4096;
    uint64_t bytes = newCount * sizeof(uint64_t);
    auto* newMap = static_cast<uint64_t*>(kmalloc(static_cast<uint32_t>(bytes)));
    if (!newMap) {
        SerialPrintf("MemFdGrow: kmalloc(%lu) FAILED for index of %lu pages\n",
                     bytes, newCount);
        return false;
    }
    memcpy(newMap, mfd->pageMap, mfd->pageMapCount * sizeof(uint64_t));
    memset(newMap + mfd->pageMapCount, 0, (newCount - mfd->pageMapCount) * sizeof(uint64_t));
    if (mfd->pageMap) kfree(mfd->pageMap);
    mfd->pageMap = newMap;
    mfd->pageMapCount = newCount;
    mfd->capacity = newCount * 4096;
    return true;
}

static bool MemFdGrow(MemFdData* mfd, uint64_t needed)
{
    MfdLock(mfd);
    bool ok = MemFdGrowLocked(mfd, needed);
    MfdUnlock(mfd);
    return ok;
}

// Get-or-allocate the physical page backing page index `pageIdx`. Returns
// PhysicalAddress(0) on OOM or out-of-range. New pages are zero-filled.
static PhysicalAddress MemFdGetOrAllocPage(MemFdData* mfd, uint64_t pageIdx, uint16_t pid)
{
    (void)pid; // Backing belongs to the object, which can outlive its creator.
    MfdLock(mfd);
    if (pageIdx >= mfd->pageMapCount) { MfdUnlock(mfd); return PhysicalAddress(0); }
    uint64_t raw = mfd->pageMap[pageIdx];
    if (raw == 0) {
        PhysicalAddress phys = PmmAllocPage(MemTag::User, 0);
        if (!phys) { MfdUnlock(mfd); return PhysicalAddress(0); }
        auto* va = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        memset(va, 0, 4096);
        raw = phys.raw();
        mfd->pageMap[pageIdx] = raw;
    }
    MfdUnlock(mfd);
    return PhysicalAddress(raw);
}

static inline void MemFdRef(MemFdData* mfd)
{
    if (mfd) __atomic_fetch_add(&mfd->refCount, 1, __ATOMIC_RELEASE);
}

static inline void MemFdUnref(MemFdData* mfd)
{
    if (!mfd) return;
    uint32_t prev = __atomic_fetch_sub(&mfd->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1) {
        if (mfd->pageMap) {
            for (uint64_t i = 0; i < mfd->pageMapCount; i++) {
                if (mfd->pageMap[i]) PmmFreePage(PhysicalAddress(mfd->pageMap[i]));
            }
            kfree(mfd->pageMap);
        }
        kfree(mfd);
    }
}

// A backing can be shared by forked/split VMAs. Only its final destruction
// retires admission, after all mapping/fault snapshots have released it.
static void MemFdReleaseMapping(void* object) {
    auto* mfd=static_cast<MemFdData*>(object);
    MfdLock(mfd);if(!mfd->seals.mappings)__builtin_trap();--mfd->seals.mappings;MfdUnlock(mfd);
    MemFdUnref(mfd);
}

struct KernelOperationGuard {
    Process* proc;
    explicit KernelOperationGuard(Process* p) : proc(p) { if (proc) SchedulerKernelRegionEnter(proc); }
    ~KernelOperationGuard() { if (proc) SchedulerKernelRegionLeave(proc); }
};

// Publish a complete independent descriptor while retaining object ownership
// under fdLock, so a sibling close cannot race partially initialized flags.
static int MemFdInstall(Process* proc, MemFdData* mfd, uint32_t flags)
{
    if (!proc || !proc->fds) return -EMFILE;
    FdEntry entry{};
    entry.type = FdType::MemFd;
    entry.handle = mfd;
    entry.statusFlags = flags & ~(0x80000u | 0x40u | 0x80u | 0x200u);
    entry.fdFlags = (flags & 0x80000u) ? 1 : 0;
    int fd = FdTableDuplicatePinned(proc->fds, ProcessFdLock(proc), &entry, 0,
        entry.fdFlags, [](FdEntry* e) { MemFdRef(static_cast<MemFdData*>(e->handle)); });
    return fd < 0 ? -EMFILE : fd;
}

static int64_t MemFdReadAt(MemFdData* mfd, void* buffer, uint64_t count, uint64_t* offset)
{
    MfdLock(mfd);
    uint64_t pos = *offset;
    uint64_t available = pos < mfd->size ? mfd->size - pos : 0;
    uint64_t total = count < available ? count : available;
    auto* dst = static_cast<uint8_t*>(buffer);
    for (uint64_t done = 0; done < total;) {
        uint64_t index = pos / 4096, inPage = pos & 4095;
        uint64_t n = 4096 - inPage;
        if (n > total - done) n = total - done;
        uint64_t raw = index < mfd->pageMapCount ? mfd->pageMap[index] : 0;
        if (raw) memcpy(dst + done, reinterpret_cast<void*>(PhysToVirt(PhysicalAddress(raw)).raw() + inPage), n);
        else memset(dst + done, 0, n);
        pos += n; done += n;
    }
    *offset = pos;
    MfdUnlock(mfd);
    return total;
}

static int64_t MemFdWriteAt(MemFdData* mfd, const void* buffer, uint64_t count,
                            uint64_t* offset, bool append)
{
    if (!count) return 0;
    MfdLock(mfd);
    uint64_t pos = append ? mfd->size : *offset;
    if (pos > 0x7fffffffffffffffULL || count > 0x7fffffffffffffffULL - pos) { MfdUnlock(mfd); return -EINVAL; }
    if(int error=MemFdSealedWrite(mfd->seals,mfd->size,pos+count)) {MfdUnlock(mfd);return error;}
    if (!MemFdGrowLocked(mfd, pos + count)) { MfdUnlock(mfd); return -ENOMEM; }
    auto* src = static_cast<const uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < count) {
        uint64_t index = pos / 4096, inPage = pos & 4095;
        uint64_t n = 4096 - inPage;
        if (n > count - done) n = count - done;
        uint64_t raw = mfd->pageMap[index];
        if (!raw) {
            PhysicalAddress page = PmmAllocPage(MemTag::User, 0);
            if (!page) break;
            raw = page.raw();
            memset(reinterpret_cast<void*>(PhysToVirt(page).raw()), 0, 4096);
            mfd->pageMap[index] = raw;
        }
        memcpy(reinterpret_cast<void*>(PhysToVirt(PhysicalAddress(raw)).raw() + inPage), src + done, n);
        pos += n; done += n;
    }
    if (pos > mfd->size) mfd->size = pos;
    *offset = pos;
    MfdUnlock(mfd);
    return done || !count ? static_cast<int64_t>(done) : -ENOMEM;
}

static inline void VnodeRef(Vnode* vn)
{
    if (!vn) return;
    // Detect use-after-free: kfree poisons memory with 0xDF bytes.
    // If the ops pointer is the poison pattern, the vnode was already freed.
    uintptr_t opsVal = reinterpret_cast<uintptr_t>(vn->ops);
    if (opsVal == 0xdfdfdfdfdfdfdfdfULL || vn->refCount == 0) {
        SerialPrintf("BUG: VnodeRef on freed/zero-ref vnode %p (ops=%p refCount=%u)\n",
                     vn, vn->ops, vn->refCount);
        return; // refuse to ref a dead vnode
    }
    VfsRetain(vn);
}

// Try to take a reference on a vnode that might be concurrently freed.
// Returns true if the ref was taken (refCount was > 0 and is now +1).
// Returns false if refCount was already 0 (vnode is being freed).

static inline void VnodeUnref(Vnode* vn)
{
    if (!vn) return;
    uint32_t prev = __atomic_fetch_sub(&vn->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1)
        VfsClose(vn);
}

// User #PF demand-paging hook for memfd MAP_SHARED mappings. Returns true
// if the fault was a memfd-backed page that we just paged in; false if
// this fault is unrelated (caller should fall through to the kill path).
extern "C" bool MemFdHandleUserFault(uint64_t cr2, uint64_t errCode);
extern "C" bool FileMapHandleUserFault(uint64_t cr2, uint64_t errCode);

// External wrappers for fork() in process.cpp, which doesn't see MemFdData.
void MemFdHandleRef(void* handle) { MemFdRef(static_cast<MemFdData*>(handle)); }
void MemFdHandleUnref(void* handle) { MemFdUnref(static_cast<MemFdData*>(handle)); }

static inline void EventFdRef(EventFdData* efd)
{
    if (efd) __atomic_fetch_add(&efd->refCount, 1, __ATOMIC_RELEASE);
}

static inline bool EventFdUnref(EventFdData* efd)
{
    if (!efd) return false;
    uint32_t prev = __atomic_fetch_sub(&efd->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1) {
        kfree(efd);
        return true;
    }
    return false;
}

static inline void TimerFdRef(TimerFdData* tfd)
{
    if (tfd) __atomic_fetch_add(&tfd->refCount, 1, __ATOMIC_RELEASE);
}

static inline bool TimerFdUnref(TimerFdData* tfd)
{
    if (!tfd) return false;
    uint32_t prev = __atomic_fetch_sub(&tfd->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1) {
        kfree(tfd);
        return true;
    }
    return false;
}

// Each syscall owns its registrations and retains the timer independently of
// numeric fd reuse. Rearm/disarm wakes every reader, poller and epoll waiter.
struct TimerWaitNode {
    TimerFdData* timer;
    Process* process;
    TimerWaitNode* nextTimer;
    TimerWaitNode* nextGroup;
    uint64_t observedGeneration;
};
struct TimerWaitGroup {
    TimerWaitNode* nodes = nullptr;
    bool allocationFailed = false;
    void add(TimerFdData* timer, Process* process) {
        for (auto* n = nodes; n; n = n->nextGroup) if (n->timer == timer) return;
        auto* n = static_cast<TimerWaitNode*>(kmalloc(sizeof(TimerWaitNode)));
        if (!n) { allocationFailed = true; return; }
        TimerFdRef(timer);
        TimerFdGuard guard(timer);
        *n = {timer, process, timer->waiters, nodes, 0};
        timer->waiters = n; nodes = n;
    }
    uint64_t deadline(uint64_t outer) {
        for (auto* n = nodes; n; n = n->nextGroup) {
            const uint64_t candidate = TimerFdDeadline(n->timer, &n->observedGeneration);
            if (candidate < outer) outer = candidate;
        }
        // A rearm wake can be consumed while acquiring a later timer's
        // contended mutex. Check every snapshot again without sleeping locks.
        // A rearm after this pass leaves pendingWakeup set until the block.
        for (auto* n = nodes; n; n = n->nextGroup)
            if (__atomic_load_n(&n->timer->rearmGeneration, __ATOMIC_ACQUIRE) != n->observedGeneration)
                return ClockNowNs(1);
        return outer;
    }
    ~TimerWaitGroup() {
        while (nodes) {
            auto* n = nodes; nodes = n->nextGroup;
            {
                TimerFdGuard guard(n->timer);
                auto** link = &n->timer->waiters;
                while (*link && *link != n) link = &(*link)->nextTimer;
                if (*link) *link = n->nextTimer;
            }
            TimerFdUnref(n->timer); kfree(n);
        }
    }
};
static void TimerFdWakeLocked(TimerFdData* timer) {
    for (auto* n = timer->waiters; n; n = n->nextTimer) WakeProcess(n->process);
}

static inline void EpollFdRef(EpollInstance* ep)
{
    if (ep) __atomic_fetch_add(&ep->refCount, 1, __ATOMIC_RELEASE);
}

static inline bool EpollFdUnref(EpollInstance* ep)
{
    if (!ep) return false;
    uint32_t prev = __atomic_fetch_sub(&ep->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1) {
        kfree(ep);
        return true;
    }
    return false;
}

void EventFdHandleRef(void* handle) { EventFdRef(static_cast<EventFdData*>(handle)); }
void EpollFdHandleRef(void* handle) { EpollFdRef(static_cast<EpollInstance*>(handle)); }
void TimerFdHandleRef(void* handle) { TimerFdRef(static_cast<TimerFdData*>(handle)); }

// DspState defined here so DspHandleRef can access refCount.
// /dev/dsp ioctl constants and handling code are further below.
struct DspState {
    uint32_t sampleRate;
    uint8_t  channels;
    uint8_t  bitsPerSample;
    uint32_t fragmentSize;   // bytes per fragment
    uint32_t ringBytes;      // zero = default geometry; explicit geometry is native-rate only
    uint32_t bufferOffset;   // write cursor into staging buffer
    uint8_t* buffer;         // staging buffer (kmalloc'd)
    uint32_t bufferSize;     // total staging buffer size
    AudioStreamId mixerStreamId; // generation-tagged mixer ownership
    uint64_t pointerEpoch;    // GETOPTR origin after reset/format negotiation
    uint64_t pointerBaseFrames;
    uint64_t controlEpoch;    // cancels drains on reset, pause or reconfiguration
    KMutex ioLock;           // shared by dup/fork; never held while waiting for FIFO space
    uint8_t* inputBuffer;    // bounded user-copy staging
    uint32_t refCount;       // fork-safe refcount (1 on creation)
};

struct DspIoGuard {
    DspState* state;
    explicit DspIoGuard(DspState* value) : state(value) { KMutexLock(&state->ioLock); }
    ~DspIoGuard() { KMutexUnlock(&state->ioLock); }
};

void DspHandleRef(void* handle)
{
    if (!handle) return;
    auto* dsp = static_cast<DspState*>(handle);
    __atomic_fetch_add(&dsp->refCount, 1, __ATOMIC_ACQ_REL);
}

void* DevKlogDeepCopy(void* handle)
{
    if (!handle) return nullptr;
    auto* src = static_cast<uint64_t*>(handle);
    auto* dst = static_cast<uint64_t*>(kmalloc(sizeof(uint64_t)));
    if (dst) *dst = *src;
    return dst;
}

void* DevTtyDeepCopy(void* handle)
{
    if (!handle) return nullptr;
    auto* src = static_cast<TtyDevicePair*>(handle);
    auto* dst = static_cast<TtyDevicePair*>(kmalloc(sizeof(TtyDevicePair)));
    if (!dst) return nullptr;
    dst->readPipe = src->readPipe;
    dst->writePipe = src->writePipe;
    // Bump pipe refcounts for the new copy
    if (dst->readPipe)
        __atomic_fetch_add(&static_cast<PipeBuffer*>(dst->readPipe)->readers, 1, __ATOMIC_RELEASE);
    if (dst->writePipe)
        __atomic_fetch_add(&static_cast<PipeBuffer*>(dst->writePipe)->writers, 1, __ATOMIC_RELEASE);
    return dst;
}

struct FileMappingBacking {
    Vnode* vnode;
    uint64_t size;
    bool pinned;
};
// Historical fault entrypoints share one resolver: the mapping's backing kind
// may change while preparation sleeps, so retries must dispatch afresh.

extern "C" bool MemFdHandleUserFault(uint64_t address,uint64_t error) {
    return ResolveUserMappingFault(address,error)==MappingFaultResult::Resolved;
}

// ---------------------------------------------------------------------------
// File Page Cache — global cache of clean file-backed pages.
// Key: (globally unique vnode cacheId, file_page_index). Maps to a physical page.
// Used by demand-paged MAP_PRIVATE file mappings to share identical read-only
// pages across processes and enable 64KB read-ahead batches.
// ---------------------------------------------------------------------------

// Preload file pages into the cache in bulk.
// Reads directly into physical pages via 64KB VfsRead chunks, avoiding a
// single large bounce buffer. Handles files up to 4MB.
static uint32_t PageCachePreload(Vnode* vn, uint64_t startPageIdx,
                                  uint64_t fileSize, uint32_t maxPages)
{
    // FileObject-backed faults use that inode's coherent resident cache and
    // never consult this legacy cache. Preloading here would create unused
    // duplicate pages and pin the vnode (and its resident cache) indefinitely.
    // Uncached vnodes likewise must not read batches that Adopt will discard.
    if (!vn->cacheId || (vn->ops->file_object && vn->ops->file_object(vn))) return 0;
    static constexpr uint32_t MAX_PRELOAD_PAGES = 4096; // 16MB
    static constexpr uint32_t CHUNK_PAGES = 16;         // 64KB per VfsRead

    if (maxPages > MAX_PRELOAD_PAGES) maxPages = MAX_PRELOAD_PAGES;

    // Cap by file size
    uint64_t maxFilePages = (fileSize + 4095) / 4096;
    if (startPageIdx >= maxFilePages) return 0;
    if (startPageIdx + maxPages > maxFilePages)
        maxPages = static_cast<uint32_t>(maxFilePages - startPageIdx);

    // Fast check: if first page is already cached, entire file likely is
    PhysicalAddress probe = FilePageCacheLookup(vn, startPageIdx);
    if (probe) { PmmUnrefPage(probe); return maxPages; }

    // 64KB bounce buffer for disk reads
    auto* readBuf = static_cast<uint8_t*>(kmalloc(CHUNK_PAGES * 4096));
    if (!readBuf) return 0;

    uint32_t totalCached = 0;

    for (uint32_t base = 0; base < maxPages; base += CHUNK_PAGES) {
        uint32_t chunkPages = maxPages - base;
        if (chunkPages > CHUNK_PAGES) chunkPages = CHUNK_PAGES;

        // Allocate physical pages for this chunk
        PhysicalAddress pages[CHUNK_PAGES];
        for (uint32_t i = 0; i < chunkPages; ++i) {
            pages[i] = PmmAllocPage(MemTag::User, 0);
            if (!pages[i]) { chunkPages = i; break; }
        }
        if (chunkPages == 0) break;

        // Bulk read from disk
        uint64_t off = (startPageIdx + base) * 4096;
        uint64_t chunkBytes = static_cast<uint64_t>(chunkPages) * 4096;
        __builtin_memset(readBuf, 0, chunkBytes);
        int got = VfsRead(vn, readBuf, chunkBytes, &off);
        if (got < 0) got = 0;

        // Scatter into physical pages and insert into cache
        for (uint32_t i = 0; i < chunkPages; ++i) {
            auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(pages[i]).raw());
            uint64_t srcOff = i * 4096;
            if (srcOff < static_cast<uint64_t>(got)) {
                uint64_t toCopy = static_cast<uint64_t>(got) - srcOff;
                if (toCopy > 4096) toCopy = 4096;
                __builtin_memcpy(kp, readBuf + srcOff, toCopy);
                if (toCopy < 4096)
                    __builtin_memset(kp + toCopy, 0, 4096 - toCopy);
            } else {
                __builtin_memset(kp, 0, 4096);
            }
            // Preload needs only the cache's reference, not a caller alias.
            auto canonical = FilePageCacheAdopt(vn, startPageIdx + base + i, pages[i], false);
            if (canonical) PmmUnrefPage(canonical);
        }
        totalCached += chunkPages;
    }

    kfree(readBuf);
    __atomic_fetch_add(&g_profPreloadPages, totalCached, __ATOMIC_RELAXED);
    return totalCached;
}

// Demand-page the current user mapping. File VMAs own a Vnode reference,
// so the backing remains valid even after userspace closes the original fd.
// Uses the global page cache for sharing and 64KB read-ahead.
// No metadata pointer survives unlocking. The transaction owns both prepared
// frames and the immutable backing pin through I/O, validation and retirement.
static MappingFaultResult ResolveMappingFaultForSpace(Process* actor,AddressSpaceState* space,uint64_t address,uint64_t error,uint32_t retryBudget=0) {
    if (error & 1) return MappingFaultResult::Unhandled;
    if (!actor || !space || space->kernel) return MappingFaultResult::Unhandled;
    const uint64_t page=address&~uint64_t{4095};
    const uint32_t access=(error&2)?2u:((error&16)?4u:1u);
    for(uint32_t attempt=0;!retryBudget || attempt<retryBudget;++attempt) {
        MappingTransaction operation(actor,space);
        if(!operation.LockFaultRange(page))return MappingFaultResult::Unhandled;MappingFaultSnapshot snapshot{};
        if (!operation.CaptureRange(page,access,snapshot)) return MappingFaultResult::Unhandled;
        if(snapshot.range.backing && snapshot.range.backing->file && snapshot.range.backing->file->mappingError)
            return MappingFaultResult::IoError;
        // A peer may have resolved this fault or replaced its range while we
        // prepared I/O. Use the current range and leaf, not the original fault
        // backend. Returning retries the instruction (including a subsequent
        // ordinary CoW fault), without publishing any stale prepared page.
        const uint64_t leaf=VmmReadLeaf(operation.Space()->pageTable,VirtualAddress(page));
        if (leaf & VMM_PRESENT) {
            const bool cow=(leaf&PTE_COW_BIT) && snapshot.range.sharing==MappingSharing::Private &&
                snapshot.range.leafOwnership==MappingLeafOwnership::Managed;
            return ((leaf&VMM_USER) && (!(access&4) || !(leaf&VMM_NO_EXEC)) &&
                (!(access&2) || (leaf&VMM_WRITABLE) || cow))?MappingFaultResult::Resolved:MappingFaultResult::Unhandled;
        }
        const auto kind=snapshot.range.kind;
        if (kind!=MappingKind::File && kind!=MappingKind::MemFd && kind!=MappingKind::Anonymous &&
            kind!=MappingKind::Stack && kind!=MappingKind::Heap && kind!=MappingKind::Image) return MappingFaultResult::Unhandled;
        const uint64_t batch=kind==MappingKind::File?FILE_OBJECT_PAGE_BATCH:1;
        operation.Unlock();
        if (!operation.Prepare(0,batch,1)) return MappingFaultResult::OutOfMemory;
        bool cachedPrivate=false;
        auto prepared=MappingFaultResult::Resolved;
        if (!MappingFaultTestBarrier(operation.Space(),page,1)) return MappingFaultResult::Unhandled;
        if (kind==MappingKind::File) {
            const auto* file=static_cast<FileMappingBacking*>(snapshot.range.backing->object);
            FileObject* object=file->vnode->ops->file_object ? file->vnode->ops->file_object(file->vnode) : nullptr;
            if(object) {
                // These references remain cancellation-owned until staged;
                // all I/O and target-file locking precede AS publication.
                struct BatchPins {
                    PhysicalAddress pages[FILE_OBJECT_PAGE_BATCH]{};
                    ~BatchPins() { for(auto physical:pages)if(physical)PmmUnrefPage(physical); }
                } pins;
                const uint64_t offset=snapshot.range.offset+page-snapshot.range.begin;
                uint64_t remaining=(snapshot.range.end-page)/4096;
                const uint32_t count=remaining<batch?static_cast<uint32_t>(remaining):batch;
                bool loaded=false;
                prepared=FileObjectFaultPages(object,file->vnode,offset/4096,count,pins.pages,&loaded);
                for(uint32_t i=0;i<count;++i)if(pins.pages[i]) {
                    __atomic_fetch_add(i==0&&loaded?&g_profCacheMiss:&g_profCacheHit,1,__ATOMIC_RELAXED);
                    if(!operation.StagePage(i,pins.pages[i],true))return MappingFaultResult::Unhandled;
                    pins.pages[i]=PhysicalAddress{}; // transaction owns the pin
                }
            } else {
              VnodeStat stat{};
              const int statResult=VfsStat(file->vnode,&stat);
              if(statResult)prepared=statResult==-ENOMEM?MappingFaultResult::OutOfMemory:MappingFaultResult::IoError;
              for (uint64_t i=0;prepared==MappingFaultResult::Resolved && i<batch && page+i*4096<snapshot.range.end;++i) {
                const uint64_t offset=snapshot.range.offset+(page+i*4096-snapshot.range.begin);
                if (offset>=stat.size) { if (!i)prepared=MappingFaultResult::EndOfFile;break; }
                auto physical=FilePageCacheLookup(file->vnode,offset/4096);
                if (physical) {
                    __atomic_fetch_add(&g_profCacheHit,1,__ATOMIC_RELAXED);
                    if (!operation.StagePage(i,physical,true)) { PmmUnrefPage(physical);return MappingFaultResult::Unhandled; }
                } else if (!i) {
                    __atomic_fetch_add(&g_profCacheMiss,1,__ATOMIC_RELAXED);
                    physical=PmmAllocPage(MemTag::User,operation.Space()->allocationPid);
                    if (!physical) { prepared=MappingFaultResult::OutOfMemory;break; }
                    auto* bytes=reinterpret_cast<void*>(PhysToVirt(physical).raw());memset(bytes,0,4096);
                    uint64_t readOffset=offset;
                    const int readResult=VfsRead(file->vnode,bytes,4096,&readOffset);
                    if (readResult<0) {
                        PmmUnrefPage(physical);
                        prepared=readResult==-ENOMEM?MappingFaultResult::OutOfMemory:MappingFaultResult::IoError;break;
                    }
                    // Preparation is cancellation-protected by the transaction.
                    // Stage the canonical winner only after competing inserts
                    // converge, rather than retaining an uncached losing frame.
                    physical=FilePageCacheAdopt(file->vnode,offset/4096,physical,
                        snapshot.range.sharing==MappingSharing::Private ||
                        !(snapshot.range.maximumProtection&2));
                    if (!physical) { prepared=MappingFaultResult::OutOfMemory;break; }
                    if (!operation.StagePage(i,physical,true)) { PmmUnrefPage(physical);return MappingFaultResult::Unhandled; }
                }
              }
            }
            cachedPrivate=snapshot.range.sharing==MappingSharing::Private;
        } else if (kind==MappingKind::MemFd) {
            auto* backing=static_cast<MemFdData*>(snapshot.range.backing->object);
            const uint64_t offset=snapshot.range.offset+page-snapshot.range.begin;
            if (offset>=__atomic_load_n(&backing->size,__ATOMIC_ACQUIRE)) return MappingFaultResult::Unhandled;
            auto physical=MemFdGetOrAllocPage(backing,offset/4096,space->allocationPid);
            if (!physical || !operation.StagePage(0,physical,false)) return MappingFaultResult::Unhandled;
        } else if (!operation.PrepareZeroed(MemTag::User,operation.Space()->allocationPid)) return MappingFaultResult::Unhandled;
        if (!MappingFaultTestBarrier(operation.Space(),page,2)) return MappingFaultResult::Unhandled;
        if(!operation.LockFaultRange(page))return MappingFaultResult::OutOfMemory;
        if (!operation.ValidateRange(snapshot,page,access)) continue;
        // EOF/IO observations are only authoritative for this same VMA and file
        // generation. A remap, protection change or resize retries preparation.
        if(prepared!=MappingFaultResult::Resolved)return prepared;
        auto result=operation.PublishFault(snapshot,0,VirtualAddress(page),access,cachedPrivate);
        if (result==MappingFaultCommit::Stale) continue;
        if (result==MappingFaultCommit::Failed) return MappingFaultResult::Unhandled;
        for (uint64_t i=1;i<batch;++i)
            if (operation.PreparedPage(i)) (void)operation.PublishFault(snapshot,i,VirtualAddress(page+i*4096),access,cachedPrivate);
        return MappingFaultResult::Resolved;
    }
    return MappingFaultResult::Unhandled; // bounded remote cleanup under remap churn
}
MappingFaultResult ResolveUserMappingFault(uint64_t address,uint64_t error) {
    auto* actor=ProcessCurrent();
    return ResolveMappingFaultForSpace(actor,actor?actor->addressSpace:nullptr,address,error);
}
extern "C" bool FileMapHandleUserFault(uint64_t address,uint64_t error) {
    return ResolveUserMappingFault(address,error)==MappingFaultResult::Resolved;
}

// ---------------------------------------------------------------------------
// AF_UNIX path sockets — structs and helpers defined here so
// sys_read/sys_write/sys_close can use them
// ---------------------------------------------------------------------------

static constexpr int AF_UNIX           = 1;
static constexpr int UNIX_SOCK_NONBLOCK = 0x800;
static constexpr int UNIX_SOCK_CLOEXEC  = 0x80000;
static constexpr int UNIX_MAX_SERVERS   = 32;
static constexpr int UNIX_ACCEPT_QUEUE  = 8;
static constexpr int UNIX_FD_QUEUE_CAP  = 64;

// Snapshot of an FdEntry passed across an AF_UNIX connection via SCM_RIGHTS.
// The handle refcount is bumped on send (via the type-specific Ref helper),
// transferred to the receiver on recv (no net refcount change), or dropped
// on socket close (matching refcount decrement).
struct UnixFdSnap {
    FdType   type;
    uint8_t  flags;
    uint8_t  fdFlags;
    uint32_t statusFlags;
    void*    handle;
};

static void UnixFdSnapRelease(const UnixFdSnap* snap);

static UnixCredentials UnixCurrentCredentials(Process* process)
{
    return {static_cast<int32_t>((process->tgid ? process->tgid : process->pid)), process->euid, process->egid};
}
static uint64_t UnixCurrentIncarnation(Process* process)
{
    return process->isThread?process->leaderIncarnation:process->incarnation;
}

// FIFO of in-flight fds for one direction of a connected AF_UNIX pair.
// Shared between sender (enqueue) and receiver (dequeue).
struct UnixFdQueue {
    UnixFdSnap   msgs[UNIX_FD_QUEUE_CAP];
    uint64_t offsets[UNIX_FD_QUEUE_CAP];
    // Sender identity belongs to bytes, not the socket's connection-time peer.
    // Forked children can send through an inherited endpoint.
    struct CredentialSpan { uint64_t end; UnixCredentials sender; } credentials[UNIX_FD_QUEUE_CAP];
    uint32_t credentialHead, credentialTail;
    uint64_t written, read;
    volatile int head;
    volatile int tail;
    volatile uint32_t refCount; // one ref per UnixSocketData pointing at us
};

// The direction's pipe lock protects BOTH bytes and descriptor ownership.
// Publishing descriptors separately lets a receiver steal them before their
// bytes arrive; unsynchronized receivers can also install the same ref twice.
static int64_t UnixStreamTryWrite(PipeBuffer* pipe, UnixFdQueue* queue,
                                  const char* bytes, uint64_t length,
                                  const UnixFdSnap* descriptors=nullptr, int count=0,
                                  const UnixCredentials* explicitCredentials=nullptr) {
    auto* process = ProcessCurrent();
    const UnixCredentials sender=explicitCredentials?*explicitCredentials:UnixCredentials{
        static_cast<int32_t>(process->tgid ? process->tgid : process->pid),
        ProcessReadIds(process,false).real,ProcessReadIds(process,true).real};
    SpinLockAcquire(&pipe->lock);
    bool merge = false;
    if (queue && queue->credentialHead != queue->credentialTail) {
        auto& previous = queue->credentials[(queue->credentialHead + UNIX_FD_QUEUE_CAP - 1) % UNIX_FD_QUEUE_CAP];
        merge = previous.sender.pid == sender.pid && previous.sender.uid == sender.uid && previous.sender.gid == sender.gid;
    }
    const bool credentialRoom = !queue || merge ||
        (queue->credentialHead + 1) % UNIX_FD_QUEUE_CAP != queue->credentialTail;
    int64_t result=-EAGAIN;
    if (!pipe->readers || pipe->unixReadShutdown || pipe->unixWriteShutdown) result=-EPIPE;
    else if (!length) result=0;
    else if (pipe->space() && credentialRoom &&
             (!count || (queue && (queue->head-queue->tail+UNIX_FD_QUEUE_CAP)%UNIX_FD_QUEUE_CAP+count<UNIX_FD_QUEUE_CAP))) {
        uint32_t n=pipe->space();if (length<n)n=static_cast<uint32_t>(length);
        if (n) {
            for(int i=0;i<count;++i) {
                queue->msgs[queue->head]=descriptors[i];
                queue->offsets[queue->head]=queue->written;
                queue->head=(queue->head+1)%UNIX_FD_QUEUE_CAP;
            }
            uint32_t first=pipe->capacity-pipe->head;if(first>n)first=n;
            memcpy(pipe->data+pipe->head,bytes,first);
            if(n>first)memcpy(pipe->data,bytes+first,n-first);
            pipe->head=(pipe->head+n)%pipe->capacity;
            if(queue) {
                queue->written += n;
                if (merge) queue->credentials[(queue->credentialHead + UNIX_FD_QUEUE_CAP - 1) % UNIX_FD_QUEUE_CAP].end = queue->written;
                else {
                    queue->credentials[queue->credentialHead] = {queue->written, sender};
                    queue->credentialHead = (queue->credentialHead + 1) % UNIX_FD_QUEUE_CAP;
                }
            }
        }
        result=n;
    }
    SpinLockRelease(&pipe->lock);return result;
}
static uint32_t UnixStreamTryRead(PipeBuffer* pipe, UnixFdQueue* queue,
                                  char* bytes, uint64_t length,
                                  UnixFdSnap* descriptors, int& count, UnixCredentials* sender) {
    count=0;SpinLockAcquire(&pipe->lock);
    uint32_t n=pipe->count();if(length<n)n=static_cast<uint32_t>(length);
    if(queue && n) {
        if (sender && queue->credentialTail != queue->credentialHead) {
            auto& span = queue->credentials[queue->credentialTail];
            *sender = span.sender;
            if (span.end - queue->read < n) n = static_cast<uint32_t>(span.end - queue->read);
        }
        // Stop before the next distinct descriptor group. A one-byte receive
        // must never collect rights attached to a later byte in the stream.
        bool first=true;uint64_t offset=0;
        for(int i=queue->tail;i!=queue->head;i=(i+1)%UNIX_FD_QUEUE_CAP) {
            uint64_t at=queue->offsets[i];
            if(first) {offset=at;first=false;}
            else if(at!=offset) {
                if(at<queue->read+n)n=static_cast<uint32_t>(at-queue->read);
                break;
            }
        }
        while(queue->tail!=queue->head && queue->offsets[queue->tail]<queue->read+n) {
            descriptors[count++]=queue->msgs[queue->tail];
            queue->tail=(queue->tail+1)%UNIX_FD_QUEUE_CAP;
        }
        queue->read+=n;
        while (queue->credentialTail != queue->credentialHead &&
               queue->credentials[queue->credentialTail].end <= queue->read)
            queue->credentialTail = (queue->credentialTail + 1) % UNIX_FD_QUEUE_CAP;
    }
    uint32_t first=pipe->capacity-pipe->tail;if(first>n)first=n;
    if(n)memcpy(bytes,pipe->data+pipe->tail,first);
    if(n>first)memcpy(bytes+first,pipe->data,n-first);
    pipe->tail=(pipe->tail+n)%pipe->capacity;
    SpinLockRelease(&pipe->lock);return n;
}

static inline int UnixFdQueueCount(const UnixFdQueue* q)
{
    return (q->head - q->tail + UNIX_FD_QUEUE_CAP) % UNIX_FD_QUEUE_CAP;
}
[[maybe_unused]] static inline bool UnixFdQueueFull(const UnixFdQueue* q)
{
    return UnixFdQueueCount(q) >= UNIX_FD_QUEUE_CAP - 1;
}

struct UnixPendingConn {
    UnixCredentials clientCredentials;
    uint64_t clientIncarnation,clientLaunchId;
    PipeBuffer* serverRx;     // server reads this (client→server data)
    PipeBuffer* serverTx;     // server writes this (server→client data)
    UnixFdQueue* serverRxFds; // server reads these fds (client→server)
    UnixFdQueue* serverTxFds; // server writes these fds (server→client)
    // Owns one server endpoint and one reference to each ancillary queue until
    // accept transfers them, or listener teardown releases them. Registry lock
    // serializes publication, dequeue and teardown of these slots.
    bool        used;
};

struct UnixSocketData {
    enum class State : uint8_t { Unbound, Listening, Connected };
    State   state;
    bool    nonblock;
    bool    passCredentials; // SO_PASSCRED; accessed atomically through shared fds
    volatile uint32_t refCount; // +1 per fd pointing at this struct
    uint32_t socketType;       // creation type (without NONBLOCK/CLOEXEC flags)
    UnixCredentials peerCredentials; // connection/listen snapshot; registry lock protects copies
    uint64_t peerIncarnation,peerLaunchId;
    bool acceptedEndpoint; // Only accept(), never connect() or socketpair().
    char    path[108];
    Vnode* boundNode; // Owns an inode pin even after unlink; close never unlinks.
    uint64_t boundDev, boundIno;
    // Routing path may become the server's name during connect. Preserve the
    // locally bound address separately for getsockname and accepted endpoints.
    uint8_t localAddress[110];
    uint32_t localAddressLength;
    UnixPendingConn pending[UNIX_ACCEPT_QUEUE];
    int             pendingCount;
    Process*        acceptWaiter;
    // Process inside epoll_wait watching this listening socket for readable
    // (= new pending connection). Cleared + SchedulerUnblock'd on connect.
    Process* volatile epollWaiter;
    PipeBuffer* rxPipe;
    PipeBuffer* txPipe;
    UnixFdQueue* incomingFds; // fds arriving on rxPipe
    UnixFdQueue* peerIncomingFds; // fds we post to, drained by peer's recvmsg
};

struct SockAddrUn {
    uint16_t sun_family; // AF_UNIX = 1
    char     sun_path[108];
};

static SpinLock g_unixRegistryLock;
// Explicitly armed with "unixlife on" after workload gates. No UART traffic.
extern "C" { UnixLifeCore g_unixLifeTrace{}; }
static IrqSpinLock g_unixLifeLock;
void UnixLifeControl(bool enabled)
{
    uint64_t flags = IrqSpinLockAcquire(&g_unixLifeLock);
    // Re-arm clears bounded state; readers should extract only while stopped.
    if (enabled) memset(&g_unixLifeTrace, 0, sizeof(g_unixLifeTrace));
    __atomic_store_n(&g_unixLifeTrace.enabled, enabled ? 1u : 0u, __ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_unixLifeLock, flags);
}
static void UnixLife(UnixLifeEvent event, void* handle, uint32_t previous,
                     uint32_t next, int fd, uint64_t caller, uint64_t size = 0)
{
    if (!__atomic_load_n(&g_unixLifeTrace.enabled, __ATOMIC_RELAXED)) return;
    Process* current = ProcessCurrent();
    UnixLifeRecord record{0, reinterpret_cast<uint64_t>(handle), caller, 0, size,
        previous, next, current ? current->pid : uint16_t{0},
        current ? (current->tgid ? current->tgid : current->pid) : uint16_t{0},
        fd, event, 0};
    uint64_t flags = IrqSpinLockAcquire(&g_unixLifeLock);
    if (g_unixLifeTrace.enabled) {
        if (event == UnixLifeEvent::Allocate) g_unixLifeTrace.Watch(record);
        else g_unixLifeTrace.Event(record);
    }
    IrqSpinLockRelease(&g_unixLifeLock, flags);
}
void UnixLifeHeap(void* address, uint64_t size, UnixLifeEvent event, uint64_t caller)
{
    if (!__atomic_load_n(&g_unixLifeTrace.enabled, __ATOMIC_RELAXED)) return;
    Process* current = ProcessCurrent();
    UnixLifeRecord record{0, 0, caller, reinterpret_cast<uint64_t>(address), size,
        0, 0, current ? current->pid : uint16_t{0},
        current ? (current->tgid ? current->tgid : current->pid) : uint16_t{0},
        -1, event, 0};
    uint64_t flags = IrqSpinLockAcquire(&g_unixLifeLock);
    if (g_unixLifeTrace.enabled) g_unixLifeTrace.Heap(record, event == UnixLifeEvent::HeapFree || event == UnixLifeEvent::HeapWriteFree);
    IrqSpinLockRelease(&g_unixLifeLock, flags);
}
#define UNIX_LIFE(event, object, before, after, fd) \
    UnixLife(UnixLifeEvent::event, object, before, after, fd, \
             reinterpret_cast<uint64_t>(__builtin_return_address(0)))

void UnixLifeClaim(int fd, const FdClaimResult& claim)
{
    if (claim.type == FdType::UnixSocket)
        UNIX_LIFE(Claim, claim.handle, 0, 0, fd);
}

void UnixLifePublished(int fd, const FdEntry& entry)
{
    if (entry.type == FdType::UnixSocket)
        UNIX_LIFE(Publish, entry.handle, 0, 0, fd);
}

static UnixSocketData* g_unixServers[UNIX_MAX_SERVERS];
// Name operations can perform filesystem I/O. Serialize them with a sleeping
// lock; never retain the registry spinlock across lookup or inode creation.
static KMutex g_unixNameLock;
struct UnixNameGuard {
    UnixNameGuard() { KMutexLock(&g_unixNameLock); }
    ~UnixNameGuard() { KMutexUnlock(&g_unixNameLock); }
};

static UnixSocketData* UnixFindServer(const char* path, const VnodeStat* node = nullptr)
{
    SpinLockAcquire(&g_unixRegistryLock);
    UnixSocketData* found = nullptr;
    for (int i = 0; i < UNIX_MAX_SERVERS; i++) {
        auto* candidate = g_unixServers[i];
        if (!candidate || candidate->state != UnixSocketData::State::Listening) continue;
        const bool match = node ? candidate->boundNode &&
            candidate->boundDev == node->dev && candidate->boundIno == node->ino :
            !candidate->boundNode && strcmp(candidate->path, path) == 0;
        if (match) {
            uint32_t previous = __atomic_fetch_add(&candidate->refCount, 1, __ATOMIC_RELAXED);
            UNIX_LIFE(RegistryPin, candidate, previous, previous + 1, -1);
            found = candidate;
            break;
        }
    }
    SpinLockRelease(&g_unixRegistryLock);

    return found;
}

static bool UnixRegisterServer(UnixSocketData* usd)
{
    SpinLockAcquire(&g_unixRegistryLock);
    int available = -1;
    for (int i = 0; i < UNIX_MAX_SERVERS; i++) {
        if (g_unixServers[i] == usd) { SpinLockRelease(&g_unixRegistryLock); return true; }
        if (!g_unixServers[i]) available = i;
    }
    if (available >= 0) g_unixServers[available] = usd;
    SpinLockRelease(&g_unixRegistryLock);
    return available >= 0;
}

void UnixSocketHandleRef(void* handle)
{
    if (!handle) return;
    auto* usd = static_cast<UnixSocketData*>(handle);
    uint32_t previous = __atomic_fetch_add(&usd->refCount, 1, __ATOMIC_RELEASE);
    UNIX_LIFE(Retain, usd, previous, previous + 1, -1);
}

static void UnixFdQueueUnref(UnixFdQueue* q)
{
    if (!q) return;
    uint32_t prev = __atomic_fetch_sub(&q->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev > 1) return;
    while (q->head != q->tail) {
        UnixFdSnap& m = q->msgs[q->tail];
        q->tail = (q->tail + 1) % UNIX_FD_QUEUE_CAP;
        UnixFdSnapRelease(&m);
    }
    kfree(q);
}

static inline void PipeWakeEpollRaw(PipeBuffer* pipe)
{
    if (!pipe) return;
    if (pipe->terminalReader) WakeProcess(pipe->terminalReader);
    Process* ew = pipe->epollWaiter;
    if (ew) {
        pipe->epollWaiter = nullptr;
        WakeProcess(ew);
    }
}

void PipeReleaseEnd(PipeBuffer* pipe, bool writer)
{
    if (!pipe) return;
    PipeBufferHold(pipe);
    // Finish every access before surrendering this endpoint. An opposite-end
    // closer cannot destroy the object while we still hold its lock/reference.
    SpinLockAcquire(&pipe->lock);
    if (writer)
        __atomic_fetch_sub(&pipe->writers, 1, __ATOMIC_RELEASE);
    else
        __atomic_fetch_sub(&pipe->readers, 1, __ATOMIC_RELEASE);
    Process* waiter = writer ? pipe->readerWaiter : pipe->writerWaiter;
    if (writer) pipe->readerWaiter = nullptr;
    else pipe->writerWaiter = nullptr;
    if (waiter) WakeProcess(waiter);
    PipeWakeEpollRaw(pipe);
    bool destroy = __atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0 &&
                   __atomic_load_n(&pipe->writers, __ATOMIC_ACQUIRE) == 0;
    SpinLockRelease(&pipe->lock);
    PipePollNotify(pipe, 1 | 4);
    PipeBufferDrop(pipe, destroy);
}

#include "named_fifo.inc"

// The receiver owns delivery, even while the sender still references the
// queue. Stop new writes and detach all queued capabilities under the stream
// lock; releasing them can recursively close sockets, so do that unlocked.
static void UnixDiscardIncoming(PipeBuffer* pipe, UnixFdQueue* queue)
{
    if (!pipe || !queue) return;
    UnixFdSnap discarded[UNIX_FD_QUEUE_CAP];
    int count = 0;
    SpinLockAcquire(&pipe->lock);
    pipe->unixReadShutdown = true;
    while (queue->tail != queue->head) {
        discarded[count++] = queue->msgs[queue->tail];
        queue->tail = (queue->tail + 1) % UNIX_FD_QUEUE_CAP;
    }
    SpinLockRelease(&pipe->lock);
    for (int i = 0; i < count; ++i) UnixFdSnapRelease(&discarded[i]);
}

static void UnixPendingRelease(const UnixPendingConn& pending)
{
    UnixDiscardIncoming(pending.serverRx, pending.serverRxFds);
    PipeReleaseEnd(pending.serverRx, false);
    PipeReleaseEnd(pending.serverTx, true);
    UnixFdQueueUnref(pending.serverRxFds);
    UnixFdQueueUnref(pending.serverTxFds);
}

static void UnixSocketHandleUnref(void* handle)
{
    if (!handle) return;
    auto* usd = static_cast<UnixSocketData*>(handle);
    UnixPendingConn abandoned[UNIX_ACCEPT_QUEUE]{};
    SpinLockAcquire(&g_unixRegistryLock);
    uint32_t prev = __atomic_fetch_sub(&usd->refCount, 1, __ATOMIC_ACQ_REL);
    // Trace before unlocking: another release could free the object afterward.
    UNIX_LIFE(Release, usd, prev, prev - 1, -1);
    if (prev == 1) {
        for (int i = 0; i < UNIX_MAX_SERVERS; ++i)
            if (g_unixServers[i] == usd) g_unixServers[i] = nullptr;
        for (int i = 0; i < UNIX_ACCEPT_QUEUE; ++i) {
            abandoned[i] = usd->pending[i];
            usd->pending[i] = {};
        }
        usd->pendingCount = 0;
    }
    SpinLockRelease(&g_unixRegistryLock);
    if (prev > 1) return;
    for (const auto& pending : abandoned)
        if (pending.used) UnixPendingRelease(pending);
    UnixDiscardIncoming(usd->rxPipe, usd->incomingFds);
    PipeReleaseEnd(usd->rxPipe, false);
    PipeReleaseEnd(usd->txPipe, true);

    UnixFdQueueUnref(usd->incomingFds);
    UnixFdQueueUnref(usd->peerIncomingFds);
    if (usd->boundNode) VfsClose(usd->boundNode);
    UNIX_LIFE(DirectFree, usd, 0, 0, -1);
    kfree(usd);
}

static int UnixInstallSocket(Process* process, UnixSocketData* socket, uint32_t flags,
                             const char* site)
{
    FdEntry owned{};
    owned.type = FdType::UnixSocket;
    owned.handle = socket;
    owned.statusFlags = flags & 0x800;
    owned.fdFlags = (flags & 0x80000) ? 1 : 0;
    (void)site;
    int fd = FdTablePublishOwned(process->fds, ProcessFdLock(process), owned, 0, -1, nullptr, UnixLifePublished);
    return fd;
}

// Wake any process blocked inside epoll_wait on this pipe. Called from
// sys_write / sys_sendmsg / sys_close after mutating pipe state in a way
// that could transition a watched fd to ready.
static inline void PipeWakeEpoll(PipeBuffer* pipe)
{
    if (!pipe) return;
    SpinLockAcquire(&pipe->lock);PipeWakeEpollRaw(pipe);SpinLockRelease(&pipe->lock);
    PipePollNotify(pipe, 1 | 4);
}

static void UnixShutdownDirection(PipeBuffer* pipe, bool writer)
{
    SpinLockAcquire(&pipe->lock);
    if (writer) __atomic_store_n(&pipe->unixWriteShutdown,true,__ATOMIC_RELEASE);
    else __atomic_store_n(&pipe->unixReadShutdown,true,__ATOMIC_RELEASE);
    // Wake both local and peer operations, including the thread which is
    // cancelling a blocked receive. References remain until actual close.
    auto* reader=pipe->readerWaiter;pipe->readerWaiter=nullptr;
    auto* waitingWriter=pipe->writerWaiter;pipe->writerWaiter=nullptr;
    if(reader) WakeProcess(reader);
    if(waitingWriter) WakeProcess(waitingWriter);
    PipeWakeEpollRaw(pipe);
    SpinLockRelease(&pipe->lock);
    PipePollNotify(pipe, 1 | 4);
}

// Linux poll/epoll bits share these values. Closed directions remain ready:
// reads return EOF after queued bytes; writes fail immediately with EPIPE.
static uint32_t UnixSocketReady(UnixSocketData* socket,uint32_t events)
{
    if(socket->state==UnixSocketData::State::Listening)
        return (events&1) && socket->pendingCount ? 1 : 0;
    if(socket->state!=UnixSocketData::State::Connected || !socket->rxPipe || !socket->txPipe)return 0;
    auto* rx=socket->rxPipe;auto* tx=socket->txPipe;
    SpinLockAcquire(&rx->lock);
    bool readClosed=rx->unixReadShutdown || rx->unixWriteShutdown || !rx->writers;
    bool readable=rx->count() || readClosed;
    SpinLockRelease(&rx->lock);
    SpinLockAcquire(&tx->lock);
    bool writeClosed=tx->unixReadShutdown || tx->unixWriteShutdown || !tx->readers;
    bool writable=tx->space() || writeClosed;
    SpinLockRelease(&tx->lock);
    return ((events&1)&&readable?1u:0u) | ((events&4)&&writable?4u:0u) |
        ((events&0x2000)&&readClosed?0x2000u:0u) | (readClosed&&writeClosed?0x10u:0u);
}

// Same for a listening AF_UNIX socket: wake epoll waiter when a new
// connection is posted to the pending queue.
static inline void UnixListenWakeEpoll(UnixSocketData* usd)
{
    if (!usd) return;
    Process* ew = usd->epollWaiter;
    if (ew) {
        usd->epollWaiter = nullptr;
        WakeProcess(ew);
    }
}

// ---------------------------------------------------------------------------
// OSS /dev/dsp state and ioctl definitions
// ---------------------------------------------------------------------------

// OSS ioctl numbers (from <sys/soundcard.h>)
static constexpr uint64_t SNDCTL_DSP_RESET     = 0x5000;
static constexpr uint64_t SNDCTL_DSP_SYNC      = 0x5001;
static constexpr uint64_t SNDCTL_DSP_SPEED     = 0xC0045002; // _IOWR('P', 2, int)
static constexpr uint64_t SNDCTL_DSP_SETFMT    = 0xC0045005; // _IOWR('P', 5, int)
static constexpr uint64_t SNDCTL_DSP_CHANNELS  = 0xC0045006; // _IOWR('P', 6, int)
static constexpr uint64_t SNDCTL_DSP_STEREO    = 0xC0045003; // _IOWR('P', 3, int)
static constexpr uint64_t SNDCTL_DSP_GETOSPACE = 0x8010500C; // _IOR('P', 12, audio_buf_info), 16-byte payload
static constexpr uint64_t SNDCTL_DSP_GETODELAY = 0x80045017; // queued output bytes
static constexpr uint64_t SNDCTL_DSP_GETOPTR  = 0x800C5012; // count_info, 12-byte payload
static constexpr uint64_t SNDCTL_DSP_GETCAPS   = 0x8004500F; // _IOR('P', 0x0F, int)
static constexpr uint64_t SNDCTL_DSP_SETTRIGGER= 0x40045010; // _IOW('P', 0x10, int)
static constexpr uint64_t SNDCTL_DSP_GETFMTS   = 0x8004500B; // _IOR('P', 0x0B, int)
static constexpr uint64_t SNDCTL_DSP_SETFRAGMENT= 0xC004500A;// _IOWR('P', 0x0A, int)
static constexpr uint64_t SNDCTL_DSP_GETBLKSIZE = 0xC0045004;// _IOWR('P', 4, int)
static constexpr uint64_t SNDCTL_DSP_LOW_WATER = 0x40045022;// OSS4 _IOW('P', 34, int), poll threshold in input bytes

// OSS audio formats
static constexpr int AFMT_U8     = 0x00000008;
static constexpr int AFMT_S16_LE = 0x00000010;
// OSS capabilities
static constexpr int DSP_CAP_TRIGGER = 0x00001000;

// DspState is defined earlier in the file (near DspHandleRef).

static constexpr uint32_t DSP_DEFAULT_RATE     = 44100;
static constexpr uint8_t  DSP_DEFAULT_CHANNELS = 2;
static constexpr uint8_t  DSP_DEFAULT_BITS     = 16;
static constexpr uint32_t DSP_HW_RATE          = 44100; // hardware playback rate
static constexpr uint32_t DSP_BUFFER_SIZE      = 65536; // 64KB staging buffer
static_assert(DSP_BUFFER_SIZE == DspWritePlan::InputBytesLimit && DSP_HW_RATE == DspWritePlan::HardwareRate);

// Caller holds ioLock and has checked that no input remains outstanding.
static int DspChangeFormat(DspState* dsp, const AudioStreamProgress& progress,
                           uint32_t rate, uint8_t channels, uint8_t bits)
{
    if (dsp->ringBytes && rate != MIXER_HW_RATE) return -95;
    const uint32_t frames = dsp->ringBytes ? dsp->ringBytes / (channels * (bits / 8)) : MIXER_RING_FRAMES;
    if (!frames || frames > MIXER_RING_FRAMES) return -EINVAL;
    uint64_t period = (uint64_t(dsp->fragmentSize) * MIXER_HW_RATE + uint64_t(channels) * (bits / 8) * rate - 1)
        / (uint64_t(channels) * (bits / 8) * rate);
    if (period > frames) period = frames;
    if (!AudioMixerSetCapacity(dsp->mixerStreamId, frames, static_cast<uint32_t>(period))) return -16;
    ++dsp->controlEpoch;
    dsp->pointerEpoch = progress.epoch; dsp->pointerBaseFrames = progress.completedFrames;
    dsp->sampleRate = rate; dsp->channels = channels; dsp->bitsPerSample = bits;
    return 0;
}

// RAII guard for the pinned fget/fput path (BRO-156). Constructing it pins the
// fd; destruction (on *every* return path, including the blocking ones) runs
// the matching FdPut, which performs any deferred-close teardown. Use this
// instead of a bare FdGet anywhere the handle is dereferenced across a point
// where the operation can sleep — a sibling thread could otherwise close() the
// fd and free the handle mid-use.
struct FdRef
{
    Process* proc;
    int      fd;
    FdEntry* fde;

    FdRef(Process* p, int f) : proc(p), fd(f), fde(FdGetRef(p, f)) {}
    ~FdRef() { if (fde) FdPut(proc, fd); }

    FdRef(const FdRef&)            = delete;
    FdRef& operator=(const FdRef&) = delete;

    explicit operator bool() const { return fde != nullptr; }
    FdEntry* operator->() const     { return fde; }
    FdEntry* get() const            { return fde; }
};

static int64_t UnixSocketWriteSome(UnixSocketData* socket,bool nonblock,
                                   const char* bytes,uint64_t length,
                                   const UnixFdSnap* descriptors=nullptr,int count=0,bool noSignal=false,
                                   const UnixCredentials* explicitCredentials=nullptr) {
    if(socket->state!=UnixSocketData::State::Connected || !socket->txPipe)return -ENOTCONN;
    auto* pipe=socket->txPipe;
    for(;;) {
        auto result=UnixStreamTryWrite(pipe,socket->peerIncomingFds,bytes,length,descriptors,count,explicitCredentials);
        if(result!=-EAGAIN) {
            if(result==-EPIPE && !noSignal)ProcessSendSignal(ProcessCurrent(),13);
            if(result>0) {
                if(auto* reader=pipe->readerWaiter) {pipe->readerWaiter=nullptr;WakeProcess(reader);}
                PipeWakeEpoll(pipe);
            }
            return result;
        }
        if(nonblock || socket->nonblock)return -EAGAIN;
        auto* self=ProcessCurrent();pipe->writerWaiter=self;
        self->wakeupTick=ApicTickCount()+10;SchedulerBlock(self);
        if(HasPendingSignals()) {
            if(pipe->writerWaiter==self)pipe->writerWaiter=nullptr;
            return -EINTR;
        }
    }
}
static int64_t UnixSocketReadSome(UnixSocketData* socket,bool nonblock,
                                  char* bytes,uint64_t length,UnixFdSnap* descriptors,int& count,
                                  UnixCredentials* sender=nullptr) {
    count=0;
    if(socket->state!=UnixSocketData::State::Connected || !socket->rxPipe)return -ENOTCONN;
    if(!length)return 0;
    auto* pipe=socket->rxPipe;
    for(;;) {
        auto n=UnixStreamTryRead(pipe,socket->incomingFds,bytes,length,descriptors,count,sender);
        if(n) {
            if(auto* writer=pipe->writerWaiter) {pipe->writerWaiter=nullptr;WakeProcess(writer);}
            PipeWakeEpoll(pipe);return n;
        }
        if(!__atomic_load_n(&pipe->writers,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&pipe->unixReadShutdown,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&pipe->unixWriteShutdown,__ATOMIC_ACQUIRE))return 0;
        if(nonblock || socket->nonblock)return -EAGAIN;
        auto* self=ProcessCurrent();pipe->readerWaiter=self;
        self->wakeupTick=ApicTickCount()+10;SchedulerBlock(self);
        if(HasReadInterruptSignal()) {
            if(pipe->readerWaiter==self)pipe->readerWaiter=nullptr;
            return -EINTR;
        }
    }
}

// Descriptor numbers are reusable: redirected standard streams have the
// same metadata and terminal behavior as the object they now reference.
static bool IsTerminalFd(const FdEntry* fde)
{
    return fde->type == FdType::DevKeyboard ||
           (fde->type == FdType::Vnode && !fde->handle) ||
           fde->type == FdType::DevTty ||
           (fde->type == FdType::Pipe && (fde->flags & 0x04));
}

// ---------------------------------------------------------------------------
// sys_write (1)
// ---------------------------------------------------------------------------

// Procfs fd links name capabilities; opening their printable link text is never
// a substitute. This scope supports the current fd table only, not foreign PIDs.
static int OpenProcMemFd(Process* proc, const char* path, uint64_t flags, bool* matched)
{
    *matched = false;
    if (strncmp(path, "/proc/", 6)) return -ENOENT;
    const char* part = path + 6;
    bool self = false;
    if (!strncmp(part, "self/", 5)) { self = true; part += 5; }
    else {
        uint64_t pid = 0;
        if (*part < '0' || *part > '9') return -ENOENT;
        while (*part >= '0' && *part <= '9') {
            if (pid > 65535) { *matched = true; return -ENOENT; }
            pid = pid * 10 + *part++ - '0';
        }
        if (*part++ != '/') return -ENOENT;
        self = pid == proc->pid || pid == proc->tgid;
    }
    if (strncmp(part, "fd/", 3)) return -ENOENT;
    if (!self) { *matched=true;return -13; }
    part += 3;
    uint64_t number = 0;
    if (*part < '0' || *part > '9') return -ENOENT;
    while (*part >= '0' && *part <= '9') {
        if (number >= MAX_FDS) { *matched=true;return -ENOENT; }
        number = number * 10 + *part++ - '0';
    }
    if (number >= MAX_FDS) { *matched=true;return -ENOENT; }
    KernelOperationGuard operation(proc);
    FdRef source(proc, static_cast<int>(number));
    if (!source) { *matched=true;return -ENOENT; }
    // Only memfds belong to this explicit broker. Directory capabilities,
    // including suffix traversal, must reach retained procfs magic following.
    if (source.get()->type != FdType::MemFd || !source.get()->handle)return -95;
    *matched=true;
    if (*part) return -20;
    if ((flags & 3) == 3) return -EINVAL;
    if (flags & 0x20000) return -40; // O_NOFOLLOW on a magic symlink
    if (flags & 0x10000) return -20; // O_DIRECTORY
    if (flags & (0x200000 | 0x200 | 0x40 | 0x80)) return -95;
    return MemFdInstall(proc, static_cast<MemFdData*>(source.get()->handle), static_cast<uint32_t>(flags));
}

// Shared write dispatch used by sys_write (user buffer) and sys_sendfile
// (kernel bounce buffer). bufAddr is treated as a raw readable byte pointer;
// the CALLER is responsible for validating it (sys_write checks the user
// buffer; sendfile passes a kernel bounce buffer that is always readable).
// Handles every fd type: serial, vnode, framebuffer, /dev/null, dsp, tty,
// pipe, eventfd, socket, memfd, unix-socket.
// One append syscall commits at most this prefix. A short write permits callers
// to retry without holding inode/backend locks across user faults or allocation.
static constexpr uint64_t AppendStageLimit = 64 * 1024;
// Backend/VFS byte results are signed int. Bound ordinary writes rather than
// narrowing a 64-bit request (which can wrap to zero or a negative result).
static constexpr uint64_t VnodeWriteLimit = 0x7ffff000ULL;
static int64_t AppendUserBuffer(Vnode* vn, uint64_t address, uint64_t count, uint64_t* offset)
{
    if (!count) return 0;
    uint64_t size = count < AppendStageLimit ? count : AppendStageLimit;
    auto* staged = kmalloc(size);
    if (!staged) return -ENOMEM;
    const uint64_t copied=CopyFromUserBytes(staged,address,size);
    if(!copied) { kfree(staged);return -EFAULT; }
    int64_t result = VfsAppend(vn, staged, copied, offset);
    kfree(staged);
    return result;
}

static int64_t WriteToPinnedFdByType(Process* proc,FdEntry* fde,uint64_t fd,
                                     uint64_t bufAddr,uint64_t count,bool userSource)
{
    // Debug serial writes directly, bypassing the async ring buffer.
    // Hold the serial lock across the entire write so multi-CPU output
    // doesn't interleave character-by-character.
    if (fde->type == FdType::DebugSerial)
    {
        const char* buf = reinterpret_cast<const char*>(bufAddr);
        brook::SerialLock();
        for (uint64_t i = 0; i < count; i++)
        {
            if (buf[i]) // skip null bytes (binary padding in buffers)
                brook::SerialPutChar(buf[i]);
        }
        brook::SerialUnlock();
        return static_cast<int64_t>(count);
    }

    // For fd 0/1/2: check if they've been redirected via dup2 first
    if (fd == 1 || fd == 2)
    {
        // Redirected if FD entry is a pipe or other non-default type.
        // Default: Vnode with null handle = serial output
        if (fde->type == FdType::Vnode && !fde->handle)
        {
            const char* buf = reinterpret_cast<const char*>(bufAddr);
            if (count > 0)
            {
                uint64_t remaining = count;
                while (remaining > 0)
                {
                    uint32_t chunk = remaining > 0xffffffffULL
                        ? 0xffffffffU
                        : static_cast<uint32_t>(remaining);
                    brook::SerialWriterEnqueue(buf, chunk);
                    buf += chunk;
                    remaining -= chunk;
                }
            }
            return static_cast<int64_t>(count);
        }
        // Fall through to FD-table-based write below. (Historically we
        // mirrored redirected stdout/stderr to the serial log here, but
        // that corrupts binary pipelines — e.g. curl | xz | nar-unpack
        // dumps the compressed NAR stream onto the serial console — and
        // makes debug output confusing. If you need pipe content in logs,
        // explicitly tee to the inherited debug-serial descriptor.)
    }

    if(auto* opened=FdNamedFifo(fde)) {
        if((fde->statusFlags&3)==0)return -EBADF;
        FdEntry endpoint=*fde;endpoint.type=FdType::Pipe;endpoint.handle=opened->state->pipe;endpoint.flags=1|0x10;
        return WriteToPinnedFdByType(proc,&endpoint,fd,bufAddr,count,userSource);
    }
    // The pin above also protects handles across blocking writes; its
    // destructor performs any deferred-close teardown on every return path.
    if (fde->type == FdType::Vnode && fde->handle)
    {
        if ((fde->statusFlags & 3) == 0) return -EBADF;
        auto* vn = static_cast<Vnode*>(fde->handle);
        int64_t ret = (fde->statusFlags & 0x400)
            ? AppendUserBuffer(vn, bufAddr, count, &fde->seekPos)
            : VfsWrite(vn, reinterpret_cast<const void*>(bufAddr),
                       count < VnodeWriteLimit ? count : VnodeWriteLimit, &fde->seekPos);
        // O_SYNC includes O_DSYNC (0x1000). A stronger full-file sync is valid
        // for either flag and must complete before reporting a durable write.
        if (ret > 0 && (fde->statusFlags & 0x1000)) {
            int error = VfsFsync(vn);
            if (error) return error;
        }
        // Note: VfsWrite already updates *offset (fde->seekPos), don't double-update
        return ret;
    }

    // Write to /dev/fb0 signals a frame is complete (sets dirty flag).
    if (fde->type == FdType::DevFramebuf)
    {
        proc->fbDirty = 1;
        CompositorWake();
        return static_cast<int64_t>(count);
    }

    // /dev/null — discard all writes
    if (fde->type == FdType::DevNull || fde->type == FdType::DevUrandom)
        return static_cast<int64_t>(count);

    // /dev/dsp — pass PCM data through the audio mixer.
    // All streams are resampled to 44100 Hz stereo 16-bit, then submitted
    // to the mixer which accumulates and flushes to hardware.
    // Block when the mixer ring is full so we apply backpressure instead
    // of silently dropping samples (which causes audible stutter/gaps).
    if (fde->type == FdType::DevDsp && fde->handle)
    {
        auto* dsp = static_cast<DspState*>(fde->handle);
        DspWritePlan plan;
        DspProducerCall timing(proc->pid,proc->incarnation,dsp->mixerStreamId,count);
        auto finish = [&](int64_t error) -> int64_t {
            const uint64_t bytes = plan.ConsumedBytes();
            return timing.Result(bytes ? static_cast<int64_t>(bytes) : error);
        };
        for (;;) {
            SpinLockAcquire(ProcessFdLock(proc));
            const bool nonblock = (fde->statusFlags & 0x800) != 0;
            SpinLockRelease(ProcessFdLock(proc));
            {
                const auto mutexBefore=timing.Mark();
                DspIoGuard held(dsp);timing.Mutex(mutexBefore);
                AudioStreamProgress progress{};
                if (!AudioMixerGetProgress(dsp->mixerStreamId, &progress)) return finish(-EBADF);
                if (progress.discontinuity) return finish(-32); // EPIPE: explicit RESET required.
                const uint32_t rate = dsp->sampleRate ? dsp->sampleRate : DSP_HW_RATE;
                const uint32_t frameBytes = (dsp->bitsPerSample / 8) * dsp->channels;
                if (!frameBytes) return finish(-EINVAL);
                if (!plan.Active() || !plan.Matches(rate, dsp->channels, dsp->bitsPerSample,
                                                   dsp->controlEpoch, progress.epoch)) {
                    // A reset/pause/reconfiguration cannot splice the rest of an
                    // already accepted chunk into a different stream epoch.
                    if (plan.Accepted()) return finish(0);
                    if (count / frameBytes == 0) return timing.Result(0);
                    if (!plan.Begin(count, rate, dsp->channels, dsp->bitsPerSample,
                                    progress.capacityFrames, dsp->controlEpoch, progress.epoch))
                        return timing.Result(-EINVAL);
                }
                const uint32_t offered = plan.Offer(AudioMixerAvailableFrames(dsp->mixerStreamId));
                if (offered) {
                    // Re-stage under ioLock on each attempt. Nothing aliases
                    // these shared buffers across a wait or another writer.
                    // Conversion keeps its original output offset even when
                    // the hardware rate differs from the input rate.
                    if (userSource) {
                        if (!CopyFromUser(dsp->inputBuffer, bufAddr, plan.InputBytes())) return finish(-EFAULT);
                    } else {
                        memcpy(dsp->inputBuffer, reinterpret_cast<const void*>(bufAddr), plan.InputBytes());
                    }
                    auto* out = reinterpret_cast<int16_t*>(dsp->buffer);
                    if (!plan.Convert(dsp->inputBuffer, out, offered)) return finish(-EIO);
                    const uint32_t accepted = AudioMixerSubmit(out, offered, dsp->mixerStreamId);
                    if (!plan.Accept(accepted)) return finish(-EIO);
                    if (accepted) {
                        // Preserve the existing bounded full-write contract for
                        // blocking users, but refill available prefixes instead
                        // of waiting for the entire converted chunk to fit.
                        if (!plan.Remaining() || nonblock) return finish(0);
                        continue;
                    }
                }
            }
            // The description mutex is released while waiting, so a separate
            // thread can reset, pause or enable this same open description.
            // Prefix acceptance wins over a later signal/error, as for write(2).
            if (nonblock) return finish(-EAGAIN);
            if (HasReadInterruptSignal()) return finish(-EINTR);
            Process* self = SchedulerCurrentProcess();
            if (self) {
                const auto before=timing.Mark();
                self->wakeupTick = ApicTickCount() + 5; SchedulerBlockInterruptible(self);
                timing.Sleep(before);
            }
        }
    }

    // Write to /dev/tty (DevKeyboard) — route to serial + TTY framebuffer
    if (fde->type == FdType::DevKeyboard)
    {
        const char* buf = reinterpret_cast<const char*>(bufAddr);
        if (count > 0)
        {
            brook::SerialLock();
            for (uint64_t i = 0; i < count; i++)
                brook::SerialPutChar(buf[i]);
            brook::SerialUnlock();
            for (uint64_t i = 0; i < count; i++)
                TtyPutChar(buf[i]);
        }
        return static_cast<int64_t>(count);
    }

#include "pipe_write.inc"

    // Write to eventfd — adds value to counter
    if (fde->type == FdType::SignalFd) return -EINVAL;
    if (fde->type == FdType::SyncFile) return -EBADF;

    if (fde->type == FdType::EventFd && fde->handle)
    {
        if (count != 8) return -EINVAL;
        uint64_t val;
        if (!CopyFromUser(&val, bufAddr, sizeof(val))) return -EFAULT;
        if (val == 0xFFFFFFFFFFFFFFFFULL) return -EINVAL;
        auto* efd = static_cast<EventFdData*>(fde->handle);
        for (;;) {
            bool accepted = false;
            {
                EventFdGuard guard(efd);
                if (val <= (~0ULL - 1) - efd->counter) {
                    efd->counter += val;
                    ++efd->inGeneration; // every write, including zero
                    accepted = true;
                }
            }
            if (accepted) break;
            if (fde->statusFlags & EFD_NONBLOCK) return -EAGAIN;
            // Existing eventfd wait hooks have one slot; bounded polling also
            // supplies progress to writers waiting for counter capacity.
            Process* self = ProcessCurrent();
            self->wakeupTick = ApicTickCount() + 10;
            SchedulerBlockInterruptible(self);
            if (HasPendingSignals()) return -EINTR;
        }
        // Wake blocked reader
        Process* reader = efd->readerWaiter;
        if (reader) {
            efd->readerWaiter = nullptr;
            WakeProcess(reader);
        }
        // Wake epoll waiter watching this eventfd for readability
        Process* ew = efd->epollWaiter;
        if (ew) {
            efd->epollWaiter = nullptr;
            WakeProcess(ew);
        }
        return 8;
    }

    // Write to socket (TCP stream or connected UDP datagram).
    if (fde->type == FdType::Socket && fde->handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        if (brook::SockIsStream(sockIdx))
        {
            return brook::SockSend(sockIdx,
                                   reinterpret_cast<const void*>(bufAddr),
                                   static_cast<uint32_t>(count));
        }
        // UDP: write() requires a prior connect() so the kernel knows the peer.
        // SockSendTo with dest=nullptr falls back to the cached connect address.
        int ret = SockSendTo(sockIdx,
                             reinterpret_cast<const void*>(bufAddr),
                             static_cast<uint32_t>(count),
                             nullptr);
        if (ret < 0) return -ENOTCONN;
        return static_cast<int64_t>(count);
    }

    // Write to /dev/tty — writes to stdout pipe (rendered by terminal thread)
    if (fde->type == FdType::DevTty && fde->handle)
    {
        auto* pair = static_cast<TtyDevicePair*>(fde->handle);
        auto* pipe = static_cast<PipeBuffer*>(pair->writePipe);
        const char* src = reinterpret_cast<const char*>(bufAddr);
        uint64_t written = 0;

        while (written < count)
        {
            if (__atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0)
                return written > 0 ? static_cast<int64_t>(written) : -EPIPE;

            uint32_t chunk = pipe->write(src + written,
                static_cast<uint32_t>(count - written > 4096 ? 4096 : count - written));
            written += chunk;
            if (written > 0)
            {
                Process* reader = pipe->readerWaiter;
                if (reader)
                {
                    pipe->readerWaiter = nullptr;
                    WakeProcess(reader);
                }
                PipeWakeEpoll(pipe);
                break;
            }
            Process* self = ProcessCurrent();
            pipe->writerWaiter = self;
            // Re-check after registering waiter
            chunk = pipe->write(src + written,
                static_cast<uint32_t>(count - written > 4096 ? 4096 : count - written));
            if (chunk > 0)
            {
                pipe->writerWaiter = nullptr;
                written += chunk;
                Process* reader = pipe->readerWaiter;
                if (reader) { pipe->readerWaiter = nullptr; WakeProcess(reader); }
                PipeWakeEpoll(pipe);
                break;
            }
            self->wakeupTick = ApicTickCount() + 10;
            SchedulerBlock(self);
            if (HasPendingSignals())
                return written > 0 ? static_cast<int64_t>(written) : -EINTR;
        }
        return static_cast<int64_t>(written);
    }

    if (fde->type == FdType::MemFd && fde->handle) {
        if ((fde->statusFlags & 3) == 0) return -EBADF;
        return MemFdWriteAt(static_cast<MemFdData*>(fde->handle),
            reinterpret_cast<const void*>(bufAddr), count, &fde->seekPos,
            (fde->statusFlags & 0x400) != 0);
    }

    // Write to AF_UNIX connected socket.
    if (fde->type == FdType::UnixSocket && fde->handle) {
        auto* socket=static_cast<UnixSocketData*>(fde->handle);
        return UnixSocketWriteSome(socket,(fde->statusFlags&0x800)!=0,
            reinterpret_cast<const char*>(bufAddr),count);
    }

    // No fd-type handler above matched — unknown/unsupported descriptor.
    // (This is the terminal return of WriteToPinnedFdByType, not sys_write.)
    return -EBADF;
}

static int64_t WriteToFdByType(uint64_t fd,uint64_t bufAddr,uint64_t count,bool userSource=true)
{
    Process* proc=ProcessCurrent();if(!proc)return -EBADF;
    FdRef ref(proc,static_cast<int>(fd));if(!ref)return -EBADF;
    return WriteToPinnedFdByType(proc,ref.get(),fd,bufAddr,count,userSource);
}

static int64_t sys_write(uint64_t fd, uint64_t bufAddr, uint64_t count,
                          uint64_t, uint64_t, uint64_t)
{
    // Validate the user buffer once, up front. EFAULT if any page in the
    // range is unmapped — without this, a bad user pointer faults the kernel.
    if (count > 0 && !UserBufferReadable(bufAddr, count))
        return -EFAULT;
    return WriteToFdByType(fd, bufAddr, count);
}


// ---------------------------------------------------------------------------
// sys_read (0)
// ---------------------------------------------------------------------------

// SockRecv holds a socket spinlock while copying its receive ring. Never pass
// a userspace pointer: even a previously validated writable page may need CoW
// or be remapped before that copy, and fault resolution can sleep on mapping
// mutexes. Stage resident kernel bytes, then copy out after the socket unlocks.
static int64_t ReceiveTcpToUser(int sockIdx, uint64_t address, uint64_t count)
{
    if (!count) return 0;
    const uint32_t size = static_cast<uint32_t>(count < 65536 ? count : 65536);
    if (!UserBufferWritable(address, size)) return -EFAULT;
    KernelOperationGuard operation(ProcessCurrent());
    auto* staged = static_cast<uint8_t*>(kmalloc(size));
    if (!staged) return -ENOMEM;
    int result = brook::SockRecv(sockIdx, staged, size);
    if (result > 0 && !CopyToUser(address, staged, static_cast<uint32_t>(result)))
        result = -EFAULT;
    kfree(staged);
    return result;
}

static int64_t sys_read(uint64_t fd, uint64_t bufAddr, uint64_t count,
                         uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    // Pin the slot for the whole operation: the pipe/socket/tty/timerfd/eventfd
    // paths below block while holding fde->handle, so a bare FdGet would expose
    // a use-after-free if a sibling closes the fd (BRO-156). The guard's
    // destructor runs FdPut on every return path.
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref) return -EBADF;
    FdEntry* fde = ref.get();

    // Validate user buffer up-front — every read path below ultimately
    // writes to bufAddr. If the user munmapped or never mapped the range,
    // a kernel #PF would be unrecoverable. UserBufferReadable confirms
    // every page in [bufAddr, bufAddr+count) is mapped; user pages are
    // always writable in our model so checking presence is sufficient.
    if (count > 0 && !UserBufferReadable(bufAddr, count))
        return -EFAULT;

    if (fde->type == FdType::Vnode && fde->handle && !FdNamedFifo(fde))
    {
        auto* vn = static_cast<Vnode*>(fde->handle);
        int64_t ret = VfsRead(vn, reinterpret_cast<void*>(bufAddr),
                              static_cast<uint32_t>(count), &fde->seekPos);
        // VfsRead already updates seekPos via the offset pointer; don't double-add.
        return ret;
    }

    // /dev/null — always EOF
    if (fde->type == FdType::DevNull)
        return 0;

    // /dev/urandom — fill buffer with RDRAND random bytes
    if (fde->type == FdType::DevUrandom)
    {
        // Validate user buffer is in user address space
        if (bufAddr == 0 || bufAddr >= 0x0000800000000000ULL)
            return -EFAULT;
        if (count > 0 && (bufAddr + count - 1) >= 0x0000800000000000ULL)
            return -EFAULT;

        auto* dst = reinterpret_cast<uint8_t*>(bufAddr);
        uint64_t filled = 0;
        while (filled < count) {
            uint64_t rval;
            if (!RdrandU64(&rval))
                rval = SoftRandU64();
            uint64_t remain = count - filled;
            uint64_t chunk = (remain < 8) ? remain : 8;
            for (uint64_t i = 0; i < chunk; i++)
                dst[filled + i] = static_cast<uint8_t>(rval >> (i * 8));
            filled += chunk;
        }
        return static_cast<int64_t>(count);
    }

    // /dev/klog — read new lines from the kernel log ring buffer
    if (fde->type == FdType::DevKlog && fde->handle)
    {
        if (bufAddr == 0 || bufAddr >= 0x0000800000000000ULL) return -EFAULT;
        if (count > 0 && (bufAddr + count - 1) >= 0x0000800000000000ULL) return -EFAULT;

        auto* cursor = static_cast<uint32_t*>(fde->handle);
        auto* dst = reinterpret_cast<char*>(bufAddr);

        // Read available lines from the ring buffer into a temp buffer
        static constexpr uint32_t KLOG_LINE_LEN = 161; // 160 chars + null
        static constexpr uint32_t KLOG_BATCH    = 8;  // 8 × 161 = ~1.3KB stack
        char lineBuf[KLOG_BATCH][KLOG_LINE_LEN];

        uint64_t written = 0;

        for (;;) {
            uint32_t nread = DebugOverlayReadFrom(cursor, reinterpret_cast<char*>(lineBuf),
                                                   KLOG_BATCH, KLOG_LINE_LEN);
            if (nread == 0) break;

            for (uint32_t i = 0; i < nread; i++) {
                uint32_t len = 0;
                while (len < KLOG_LINE_LEN - 1 && lineBuf[i][len]) len++;

                // Need space for line + newline
                if (written + len + 1 > count) goto done;

                for (uint32_t j = 0; j < len; j++)
                    dst[written++] = lineBuf[i][j];
                dst[written++] = '\n';
            }
        }

    done:
        if (written == 0 && !(fde->statusFlags & 0x800)) { // O_NONBLOCK
            // Blocking mode: sleep briefly and retry once
            Process* self = ProcessCurrent();
            self->wakeupTick = ApicTickCount() + 100;
            SchedulerBlock(self);

            uint32_t nread = DebugOverlayReadFrom(cursor, reinterpret_cast<char*>(lineBuf),
                                                   KLOG_BATCH, KLOG_LINE_LEN);
            for (uint32_t i = 0; i < nread; i++) {
                uint32_t len = 0;
                while (len < KLOG_LINE_LEN - 1 && lineBuf[i][len]) len++;
                if (written + len + 1 > count) break;
                for (uint32_t j = 0; j < len; j++)
                    dst[written++] = lineBuf[i][j];
                dst[written++] = '\n';
            }
        }

        return static_cast<int64_t>(written);
    }

    // Synthetic in-memory files (/etc/passwd, /etc/group, etc.)
    if (fde->type == FdType::SyntheticMem && fde->handle)
    {
        auto* content = static_cast<const char*>(fde->handle);
        uint64_t contentLen = 0;
        while (content[contentLen]) contentLen++;

        uint64_t pos = fde->seekPos;
        if (pos >= contentLen) return 0; // EOF

        uint64_t avail = contentLen - pos;
        uint64_t toRead = (count < avail) ? count : avail;
        auto* dst = reinterpret_cast<char*>(bufAddr);
        for (uint64_t i = 0; i < toRead; i++)
            dst[i] = content[pos + i];
        fde->seekPos += toRead;
        return static_cast<int64_t>(toRead);
    }

    if (fde->type == FdType::DevKeyboard)
    {
        auto* buf = reinterpret_cast<uint8_t*>(bufAddr);

        // Non-blocking raw scancode mode (DOOM uses ioctl cmd=1 to enable this)
        if (fde->flags & 1)
        {
            uint64_t bytesRead = 0;
            while (bytesRead < count)
            {
                InputEvent ev;
                bool got;
                if (WmIsActive())
                    got = ProcessInputPop(proc, &ev);
                else
                    got = InputPollEvent(&ev);
                if (!got) break;

                uint8_t sc = ev.scanCode;
                bool release = (ev.type == InputEventType::KeyRelease);

                // Translate synthetic extended codes back to PS/2 E0-prefixed
                // two-byte sequences so userspace (DOOM, Quake) sees real scancodes.
                uint8_t ps2 = 0;
                switch (sc)
                {
                case SC_EXT_UP:     ps2 = 0x48; break;
                case SC_EXT_DOWN:   ps2 = 0x50; break;
                case SC_EXT_LEFT:   ps2 = 0x4B; break;
                case SC_EXT_RIGHT:  ps2 = 0x4D; break;
                case SC_EXT_HOME:   ps2 = 0x47; break;
                case SC_EXT_END:    ps2 = 0x4F; break;
                case SC_EXT_INSERT: ps2 = 0x52; break;
                case SC_EXT_DELETE: ps2 = 0x53; break;
                case SC_EXT_PGUP:   ps2 = 0x49; break;
                case SC_EXT_PGDN:   ps2 = 0x51; break;
                }

                if (ps2)
                {
                    // Emit single-byte scancode (no E0 prefix) for compatibility
                    // with apps that read 1 byte at a time (DOOM, etc.)
                    buf[bytesRead++] = release ? (ps2 | 0x80) : ps2;
                }
                else
                {
                    if (release) sc |= 0x80;
                    buf[bytesRead++] = sc;
                }
            }
            return static_cast<int64_t>(bytesRead);
        }

        // Non-canonical (raw) terminal mode: return ASCII chars as they arrive.
        // Shells like ash use this for line editing.
        if (!proc->ttyCanonical)
        {
            bool wmMode = WmIsActive();
            uint64_t bytesRead = 0;
            while (bytesRead < count)
            {
                InputEvent ev;
                bool got = wmMode ? ProcessInputPop(proc, &ev)
                                  : InputPollEvent(&ev);
                if (!got)
                {
                    if (bytesRead > 0) break; // return what we have
                    // Register as waiter BEFORE re-checking, to close the
                    // race where a key arrives between the poll and block.
                    InputAddWaiter(proc);
                    // Re-check after registration — if data arrived between
                    // the first poll and AddWaiter, consume it immediately.
                    got = wmMode ? ProcessInputPop(proc, &ev)
                                : InputPollEvent(&ev);
                    if (got)
                    {
                        InputRemoveWaiter(proc);
                        goto got_event_nc;
                    }
                    SchedulerBlock(proc);
                    if (HasPendingSignals())
                        return bytesRead > 0 ? static_cast<int64_t>(bytesRead) : -EINTR;
                    continue;
                }
            got_event_nc:
                if (ev.type != InputEventType::KeyPress) continue;
                char c = ev.ascii;
                if (c == 0) continue; // non-printable (arrows, etc.) — skip

                // Map Enter scancode to '\n'
                if (ev.scanCode == 0x1C) c = '\n';

                // Ctrl+C → send interrupt character
                if ((ev.modifiers & INPUT_MOD_CTRL) && (ev.scanCode == 0x2E))
                    c = '\x03';

                // Ctrl+D → EOF
                if ((ev.modifiers & INPUT_MOD_CTRL) && (ev.scanCode == 0x20))
                {
                    if (bytesRead == 0) return 0; // EOF
                    break;
                }

                buf[bytesRead++] = static_cast<uint8_t>(c);

                // Don't echo here — bash handles its own echo in
                // non-canonical mode by redisplaying the line on Enter.
                // Kernel echo would duplicate every character.
            }
            return static_cast<int64_t>(bytesRead);
        }

        // Cooked terminal mode: blocking, ASCII translation, line buffering, echo.
        // Buffer a full line (until Enter), then return it.
        // This matches canonical terminal behavior that shells expect.
        static constexpr uint32_t LINE_BUF_MAX = 256;
        char lineBuf[LINE_BUF_MAX];
        uint32_t lineLen = 0;
        bool wmMode = WmIsActive();

        for (;;)
        {
            InputEvent ev;
            bool got = wmMode ? ProcessInputPop(proc, &ev)
                              : InputPollEvent(&ev);
            if (!got)
            {
                // Register as waiter BEFORE re-checking to close the race.
                InputAddWaiter(proc);
                got = wmMode ? ProcessInputPop(proc, &ev)
                             : InputPollEvent(&ev);
                if (got)
                {
                    InputRemoveWaiter(proc);
                    goto got_event_cooked;
                }
                SchedulerBlock(proc);
                if (HasPendingSignals())
                    return lineLen > 0 ? static_cast<int64_t>(lineLen) : -EINTR;
                continue;
            }
        got_event_cooked:

            // Only process key presses, ignore releases
            if (ev.type != InputEventType::KeyPress)
                continue;

            char c = ev.ascii;

            // Ctrl+C → interrupt (for now, just send '\x03')
            if ((ev.modifiers & INPUT_MOD_CTRL) && (ev.scanCode == 0x2E)) // 'C'
            {
                // Echo ^C and return empty line
                brook::SerialLock(); brook::SerialPutChar('^'); brook::SerialPutChar('C'); brook::SerialPutChar('\n'); brook::SerialUnlock();
                TtyPutChar('^'); TtyPutChar('C'); TtyPutChar('\n');
                buf[0] = '\n';
                return 1;
            }

            // Ctrl+D → EOF
            if ((ev.modifiers & INPUT_MOD_CTRL) && (ev.scanCode == 0x20)) // 'D'
            {
                if (lineLen == 0) return 0; // EOF
                // Otherwise flush current line
                break;
            }

            // Enter/Return
            if (c == '\r' || c == '\n' || ev.scanCode == 0x1C)
            {
                brook::SerialLock(); brook::SerialPutChar('\n'); brook::SerialUnlock();
                TtyPutChar('\n');
                if (lineLen < LINE_BUF_MAX)
                    lineBuf[lineLen++] = '\n';
                break;
            }

            // Backspace
            if (c == '\b' || ev.scanCode == 0x0E)
            {
                if (lineLen > 0)
                {
                    lineLen--;
                    brook::SerialLock(); brook::SerialPutChar('\b'); brook::SerialPutChar(' '); brook::SerialPutChar('\b'); brook::SerialUnlock();
                    TtyPutChar('\b'); TtyPutChar(' '); TtyPutChar('\b');
                }
                continue;
            }

            // Tab → send tab character
            if (ev.scanCode == 0x0F)
            {
                if (lineLen < LINE_BUF_MAX - 1)
                {
                    lineBuf[lineLen++] = '\t';
                    brook::SerialLock(); brook::SerialPutChar('\t'); brook::SerialUnlock();
                    TtyPutChar('\t');
                }
                continue;
            }

            // Regular printable character
            if (c >= ' ' && c <= '~')
            {
                if (lineLen < LINE_BUF_MAX - 1)
                {
                    lineBuf[lineLen++] = c;
                    brook::SerialLock(); brook::SerialPutChar(c); brook::SerialUnlock();
                    TtyPutChar(c);
                }
                continue;
            }

            // Ignore non-printable keys (arrows, function keys, etc.)
        }

        // Copy line buffer to user buffer
        uint64_t copyLen = lineLen;
        if (copyLen > count) copyLen = count;
        __builtin_memcpy(buf, lineBuf, copyLen);
        return static_cast<int64_t>(copyLen);
    }

    // Read from pipe — block until data available or all writers closed
    if (FdPipe(fde))
    {
        KernelOperationGuard operation(proc);
        if (count == 0) return 0;
        if(!FdPipeRead(fde))return -EBADF;
        auto* pipe = FdPipe(fde);
        char staged[4096];
        auto* dst = staged;

        for (;;)
        {
            uint32_t got = pipe->read(dst, static_cast<uint32_t>(
                count > 4096 ? 4096 : count));
            if (got > 0)
            {
                // Wake blocked writer
                SpinLockAcquire(&pipe->lock);
                Process* writer=pipe->writerWaiter;pipe->writerWaiter=nullptr;
                if(writer)WakeProcess(writer);
                SpinLockRelease(&pipe->lock);
                if(!CopyToUser(bufAddr,staged,got))return -EFAULT;
                return static_cast<int64_t>(got);
            }

            Process* self = ProcessCurrent();
            switch (pipe->prepareReadWait(self, (fde->statusFlags & 0x800) != 0)) {
                case PipeReadWait::Retry: continue;
                case PipeReadWait::Eof: return 0;
                case PipeReadWait::WouldBlock: return -EAGAIN;
                case PipeReadWait::Registered: break;
            }
            // Use timed wakeup to recheck writer count periodically
            // (avoids permanent deadlock if close notification was missed)
            self->wakeupTick = ApicTickCount() + 10; // recheck every ~10ms
            SchedulerBlock(self);
            pipe->removeReadWait(self);
            if (HasReadInterruptSignal()) {
                return -EINTR;
            }
        }
    }

    if(fde->type==FdType::SignalFd && fde->handle)
        return SignalFdRead(proc,static_cast<SignalFd*>(fde->handle),bufAddr,count,(fde->statusFlags&0x800)!=0);

    // Read from eventfd — returns counter as uint64, resets to 0
    if (fde->type == FdType::SyncFile) return -EINVAL;

    if (fde->type == FdType::EventFd && fde->handle)
    {
        if (count < 8) return -EINVAL;
        auto* efd = static_cast<EventFdData*>(fde->handle);

        for (;;)
        {
            bool consumed = false;
            {
                EventFdGuard guard(efd);
                if (efd->counter > 0) {
                    const bool semaphore = (efd->flags & EFD_SEMAPHORE) != 0;
                    const uint64_t val = semaphore ? 1 : efd->counter;
                    // A sleeping mutex permits fault-aware copy before state
                    // mutation. No spinlock is held across userspace copying.
                    if (!CopyToUser(bufAddr, &val, sizeof(val))) return -EFAULT;
                    efd->counter = semaphore ? efd->counter - 1 : 0;
                    ++efd->outGeneration;
                    consumed = true;
                }
            }
            if (consumed) {
                Process* ew = efd->epollWaiter;
                if (ew) { efd->epollWaiter = nullptr; WakeProcess(ew); }
                return 8;
            }
            // Non-blocking: return EAGAIN
            if (fde->statusFlags & 0x800)
                return -EAGAIN;

            // Block until counter becomes non-zero
            Process* self = ProcessCurrent();
            efd->readerWaiter = self;
            self->wakeupTick = ApicTickCount() + 10;
            SchedulerBlock(self);
            if (HasPendingSignals())
                return -EINTR;
        }
    }

    // Read from socket (TCP stream or connected UDP)
    if (fde->type == FdType::Socket && fde->handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        bool nonblock = (fde->statusFlags & 0x800) != 0 || brook::SockIsNonblock(sockIdx);
        if (brook::SockIsStream(sockIdx))
        {
            if (nonblock && brook::SockRxCount(sockIdx) == 0 && !brook::SockPollReady(sockIdx, true, false))
                return -EAGAIN;
            return ReceiveTcpToUser(sockIdx, bufAddr, count);
        }
        // UDP: read() pulls one datagram from the rx queue.  SockRecvFrom
        // returns -EAGAIN when empty; non-blocking sockets propagate that,
        // blocking sockets spin-poll briefly (matches existing TCP behaviour).
        SockAddrIn unused = {};
        for (;;) {
            int ret = SockRecvFrom(sockIdx,
                                    reinterpret_cast<void*>(bufAddr),
                                    static_cast<uint32_t>(count),
                                    &unused);
            if (ret >= 0) return ret;
            if (nonblock) return -EAGAIN;
            if (HasPendingSignals()) return -EINTR;
            // Yield briefly so the NIC ISR / poll path can deliver packets.
            __asm__ volatile("pause");
        }
    }

    // Read from timerfd — returns uint64 expiry count, blocks until armed+expired
    if (fde->type == FdType::TimerFd && fde->handle)
    {
        if (count < 8) return -EINVAL;
        auto* tfd = static_cast<TimerFdData*>(fde->handle);
        bool nonblock = (fde->statusFlags & 0x800) != 0;

        TimerWaitGroup waiters;
        if (!nonblock) waiters.add(tfd, proc);
        for (;;) {
            {
                TimerFdGuard guard(tfd);
                tfd->state.refresh(ClockNowNs(tfd->clockId));
                if (tfd->state.pending) {
                    const uint64_t total = tfd->state.pending;
                    if (!CopyToUser(bufAddr, &total, sizeof(total))) return -EFAULT;
                    tfd->state.pending = 0;
                    return 8;
                }
            }
            if (nonblock) return -EAGAIN;
            if (HasPendingSignals()) return -EINTR;
            // Assign only after every potentially sleeping timer lock. Pending
            // wakeups close the rearm-between-snapshot-and-block race.
            const uint64_t deadline = TimerFdDeadline(tfd);
            proc->wakeupTick = WaitWakeupTick(deadline, waiters.allocationFailed ? 5 : 100);
            SchedulerBlockInterruptible(proc);
            if (HasPendingSignals()) return -EINTR;
        }
    }

    if (fde->type == FdType::MemFd && fde->handle) {
        if ((fde->statusFlags & 3) == 1) return -EBADF;
        return MemFdReadAt(static_cast<MemFdData*>(fde->handle),
            reinterpret_cast<void*>(bufAddr), count, &fde->seekPos);
    }

    // Read from /dev/tty — reads from stdin pipe
    if (fde->type == FdType::DevTty && fde->handle)
    {
        auto* pair = static_cast<TtyDevicePair*>(fde->handle);
        auto* pipe = static_cast<PipeBuffer*>(pair->readPipe);
        auto* dst = reinterpret_cast<char*>(bufAddr);

        for (;;)
        {
            uint32_t got = pipe->read(dst, static_cast<uint32_t>(
                count > 4096 ? 4096 : count));
            if (got > 0)
                return static_cast<int64_t>(got);

            uint32_t wr = __atomic_load_n(&pipe->writers, __ATOMIC_ACQUIRE);
            if (wr == 0) return 0;

            Process* self = ProcessCurrent();
            pipe->readerWaiter = self;
            self->wakeupTick = ApicTickCount() + 10;
            SchedulerBlock(self);
            if (HasReadInterruptSignal()) {
                if (pipe->readerWaiter == self) pipe->readerWaiter = nullptr;
                return -EINTR;
            }
        }
    }

    // A plain read consumes (and closes) ancillary rights accompanying bytes.
    if (fde->type == FdType::UnixSocket && fde->handle) {
        auto* socket=static_cast<UnixSocketData*>(fde->handle);
        UnixFdSnap descriptors[UNIX_FD_QUEUE_CAP];int received=0;
        auto result=UnixSocketReadSome(socket,(fde->statusFlags&0x800)!=0,
            reinterpret_cast<char*>(bufAddr),count,descriptors,received);
        for(int i=0;i<received;++i)UnixFdSnapRelease(&descriptors[i]);
        return result;
    }

    return -EBADF;
}

// ---------------------------------------------------------------------------
// sys_open (2)
// ---------------------------------------------------------------------------

// Simple string comparison helper
static bool StrEq(const char* a, const char* b)
{
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

// A name owns one backing reference independently of descriptors/mappings.
// Open and unlink serialize the name-to-object binding; final backing release
// happens after dropping this namespace lock. Names are dynamically allocated.
struct NamedShmEntry { char name[256]; MemFdData* backing; NamedShmEntry* next; };
static NamedShmEntry* g_namedShm;
static KMutex g_namedShmLock{};
struct NamedShmGuard {
    NamedShmGuard() { KMutexLock(&g_namedShmLock); }
    ~NamedShmGuard() { KMutexUnlock(&g_namedShmLock); }
};
static int NamedShmValidate(const char* name) {
    const uint64_t length=strlen(name);
    if(!length)return -EINVAL;
    if(length>=sizeof(NamedShmEntry::name))return -36;
    if(StrEq(name,".") || StrEq(name,".."))return -21; // EISDIR
    for(uint64_t i=0;i<length;++i)if(name[i]=='/')return -EINVAL;
    return 0;
}
static int NamedShmOpen(Process* proc,const char* name,uint32_t flags) {
    KernelOperationGuard operation(proc);
    int error=NamedShmValidate(name);if(error)return error;
    if((flags&3)==3)return -EINVAL;
    if(flags&0x10000u)return -ENOTDIR;
    if(flags&0x200u)return -95; // mapped truncate needs coordinated invalidation
    NamedShmEntry* discard=nullptr;int fd;
    {
        NamedShmGuard held;
        NamedShmEntry* entry=g_namedShm;
        while(entry && !StrEq(entry->name,name))entry=entry->next;
        const bool create=(flags&0x40u)!=0;
        if(entry && create && (flags&0x80u))return -17;
        if(!entry && !create)return -ENOENT;
        const bool fresh=!entry;
        if(fresh) {
            entry=static_cast<NamedShmEntry*>(kmalloc(sizeof(NamedShmEntry)));
            if(!entry)return -ENOMEM;
            auto* backing=static_cast<MemFdData*>(kmalloc(sizeof(MemFdData)));
            if(!backing) { kfree(entry);return -ENOMEM; }
            *backing={NextMemFdInode(),nullptr,0,0,0,0,1};
            if(!backing->inode) {kfree(backing);kfree(entry);return -75;}
            memcpy(entry->name,name,strlen(name)+1);
            entry->backing=backing;entry->next=g_namedShm;g_namedShm=entry;
        }
        fd=MemFdInstall(proc,entry->backing,flags);
        if(fd<0 && fresh) { g_namedShm=entry->next;discard=entry; }
    }
    if(discard) { MemFdUnref(discard->backing);kfree(discard); }
    return fd;
}
static int NamedShmUnlink(const char* name) {
    KernelOperationGuard operation(ProcessCurrent());
    int error=NamedShmValidate(name);if(error)return error;
    NamedShmEntry* removed;
    {
        NamedShmGuard held;auto** link=&g_namedShm;
        while(*link && !StrEq((*link)->name,name))link=&(*link)->next;
        if(!*link)return -ENOENT;
        removed=*link;*link=removed->next;
    }
    MemFdUnref(removed->backing);kfree(removed);
    return 0;
}

// ---------------------------------------------------------------------------
// DRM render-node sysfs/devfs shim (GL shim, M0 step 2)
//
// Minimal synthetic /dev/dri + /sys topology so the unmodified Linux libdrm
// (drmGetDevices2) can enumerate /dev/dri/renderD128 as a PCI virtio-gpu device
// and open it. Backed by the real per-process GPU path (GpuAppOps); no card0 /
// KMS node. Surface determined empirically from libdrm 2.4.131 source +
// host strace (see artifacts/gl-drm-enumeration-spike.md):
//   - get_pci_path() falls back to the literal path if realpath() fails, so the
//     PCI attributes are served directly at /sys/dev/char/226:128/device/* with
//     NO symlinks — except 'subsystem', which get_subsystem_type() reads via
//     readlink() to classify the bus (we return ".../pci" -> DRM_BUS_PCI).
// All responses are gated on a 3D driver being present (GpuAppGet()).
// ---------------------------------------------------------------------------
namespace {
struct DrmSysAttr { const char* path; const char* content; };
static const DrmSysAttr kDrmSysAttrs[] = {
    { "/sys/dev/char/226:128/device/uevent",
      "DRIVER=virtio_gpu\nPCI_SLOT_NAME=0000:00:02.0\n" },
    { "/sys/dev/char/226:128/device/vendor",           "0x1af4\n" },
    { "/sys/dev/char/226:128/device/device",           "0x1050\n" },
    { "/sys/dev/char/226:128/device/subsystem_vendor", "0x1af4\n" },
    { "/sys/dev/char/226:128/device/subsystem_device", "0x1100\n" },
    { "/sys/dev/char/226:128/device/revision",         "0x00\n" },
    { nullptr, nullptr }
};
// Directories that must stat as S_IFDIR (realpath / openat / drmNodeIsDRM).
static const char* kDrmSysDirs[] = {
    "/sys/dev/char/226:128/device",
    "/sys/dev/char/226:128/device/drm",
    nullptr
};
// Marker handle so getdents64 / fstat recognise the /dev/dri synthetic dir fd.
static const char g_drmDriDirTag[] = "BROOK_DRM_DRI_DIR";
// Linux dev_t for char 226:128 (gnu_dev: minor low 8 | major<<8).
static constexpr uint64_t DRM_RENDER_RDEV = 0xE280;

static const char* DrmSysAttrContent(const char* path) {
    for (auto* a = kDrmSysAttrs; a->path; ++a)
        if (StrEq(path, a->path)) return a->content;
    return nullptr;
}
static bool DrmSysIsDir(const char* path) {
    for (auto** d = kDrmSysDirs; *d; ++d)
        if (StrEq(path, *d)) return true;
    return false;
}

// Per-fd DRM render-node state (GL shim M1). Each open("/dev/dri/renderD128")
// gets one. Holds the app's GpuApp (virgl) context id plus a per-fd GEM-handle
// table that translates the Linux DRM model (per-fd GEM handles, small ints)
// to Brook's host-global virgl resource ids. This is the isolation layer the
// architecture review called for: an app can only name resources via GEM
// handles in ITS OWN table, never a raw host-global id, and GEM_CLOSE/teardown
// are scoped per fd. RESOURCE_CREATE returns res_handle=gres (host id, used
// inside virgl command streams) and bo_handle=GEM index (used in ioctls) —
// matching the upstream virtio-gpu uABI.
// Mesa's GL3 streaming buffers exceed 256 live objects during first present.
// Reconstructed BRO-227 fix (06a8d07); keep headroom for console/UI transitions.
static constexpr uint32_t DRM_MAX_GEM = 2048;
static DrmMapTokenAllocator g_drmMapTokens;
static DrmBackingDiagnostics g_drmBackingDiagnostics;
// File contexts own handle tables; resources own only a small GPU-context
// lease. Keeping these separate prevents GEM/PRIME cross-import ownership cycles.
struct DrmGpuOwner { uint32_t references; int32_t ctxId; GpuContextConfig config; };
static void DrmOwnerRef(DrmGpuOwner* owner) {
    __atomic_fetch_add(&owner->references,1,__ATOMIC_RELAXED);
}
static void DrmOwnerUnref(DrmGpuOwner* owner) {
    if (__atomic_fetch_sub(&owner->references,1,__ATOMIC_ACQ_REL)!=1) return;
    const auto* ga=GpuAppGet();
    if (owner->ctxId>0 && ga && ga->CtxDestroy) ga->CtxDestroy(owner->ctxId);
    kfree(owner);
}
struct DrmResource {
    MappingBacking* lifetime;
    DrmGpuOwner* owner;
    uint32_t gres,size,stride,width,height,target,format;
    uint64_t backingKva;
    uint32_t backingBytes;
    KMutex backingLock;
    bool backingAttached;
    bool hostBlob;
    uint32_t blobFlags;
    GpuHostMapping hostMapping;
    bool everExported;
    KMutex reservationLock;
    GpuReservation reservation;
    DrmResource* retiredNext;
};
static void DrmCaptureComplete(void* context,bool safe,bool captured) {
    auto* resource=static_cast<DrmResource*>(context);
    KMutexLock(&resource->reservationLock);
    resource->reservation.FinishCapture(safe,captured);
    KMutexUnlock(&resource->reservationLock);
}
// Final callbacks only enqueue: GPU waits never enter mapping/FD cleanup locks.
// GEMs, PRIME capabilities, VMAs and capture requests independently own the
// canonical resource. Last-reference retirement now covers exported buffers,
// too. An uncertain capture keeps its source lease, so it cannot reach here.
static KMutex g_drmRetirementLock{};
static DrmResource* g_drmRetiredResources;
static DrmResource* g_drmQuarantinedResources;
static KSemaphore g_drmRetirementReady{};
static bool g_drmRetirementWorkerStarted;
static void DrmRetirementWorker(void*) {
    for (;;) {
        KSemaphoreWait(&g_drmRetirementReady);
        KMutexLock(&g_drmRetirementLock);
        auto* resource=g_drmRetiredResources;
        g_drmRetiredResources=resource->retiredNext;
        KMutexUnlock(&g_drmRetirementLock);
        const auto* ga=GpuAppGet();
        const bool safe=resource->reservation.Ready();
        const bool retired=safe && (!resource->gres || (ga && ga->RetireResource &&
            ga->RetireResource(resource->owner->ctxId,static_cast<int32_t>(resource->gres))==0));
        if (!retired) {
            __atomic_fetch_add(&g_drmBackingDiagnostics.quarantinedResources,1,__ATOMIC_RELAXED);
            const uint64_t bytes=(static_cast<uint64_t>(resource->backingBytes)+4095)&~uint64_t{4095};
            __atomic_fetch_add(&g_drmBackingDiagnostics.quarantinedPageBytes,bytes,__ATOMIC_RELAXED);
            __atomic_fetch_add(&g_drmBackingDiagnostics.failedRetirements,1,__ATOMIC_RELAXED);
            KMutexLock(&g_drmRetirementLock);
            resource->retiredNext=g_drmQuarantinedResources;
            g_drmQuarantinedResources=resource;
            KMutexUnlock(&g_drmRetirementLock);
            continue;
        }
        if (resource->backingKva) {
            const uint64_t pages=(static_cast<uint64_t>(resource->backingBytes)+4095)/4096;
            VmmFreePages(VirtualAddress(resource->backingKva),pages);
            __atomic_fetch_add(&g_drmBackingDiagnostics.releasedMappings,1,__ATOMIC_RELAXED);
            __atomic_fetch_add(&g_drmBackingDiagnostics.releasedPageBytes,pages*4096,__ATOMIC_RELAXED);
        }
        auto* owner=resource->owner;kfree(resource);DrmOwnerUnref(owner);
    }
}
static bool DrmEnsureRetirementWorker() {
    KMutexLock(&g_drmRetirementLock);
    if (g_drmRetirementWorkerStarted) { KMutexUnlock(&g_drmRetirementLock);return true; }
    auto* worker=KernelThreadCreate("drm_retire",DrmRetirementWorker,nullptr);
    if (!worker) { KMutexUnlock(&g_drmRetirementLock);return false; }
    g_drmRetirementWorkerStarted=true;
    KMutexUnlock(&g_drmRetirementLock);
    SchedulerAddProcess(worker);
    return true;
}
static void DrmResourceLastReference(void* object) {
    auto* resource=static_cast<DrmResource*>(object);
    KMutexLock(&g_drmRetirementLock);
    resource->retiredNext=g_drmRetiredResources;
    g_drmRetiredResources=resource;
    KMutexUnlock(&g_drmRetirementLock);
    KSemaphoreSignal(&g_drmRetirementReady);
}
static DrmResource* DrmResourceCreate(DrmGpuOwner* owner,uint32_t gres,uint32_t size,
                                    uint32_t stride,uint32_t width,uint32_t height,
                                    uint32_t target,uint32_t format) {
    if (!DrmEnsureRetirementWorker()) return nullptr;
    auto* resource=static_cast<DrmResource*>(kmalloc(sizeof(DrmResource)));
    if (!resource) return nullptr;
    memset(resource,0,sizeof(*resource));
    resource->owner=owner;resource->gres=gres;resource->size=size;
    resource->stride=stride;resource->width=width;resource->height=height;
    resource->target=target;resource->format=format;
    KMutexInit(&resource->backingLock);
    resource->lifetime=MappingBackingCreate(resource,DrmResourceLastReference);
    if (!resource->lifetime) { kfree(resource);return nullptr; }
    DrmOwnerRef(owner);
    return resource;
}
struct DrmGemObj {
    bool used;
    DrmResource* resource;
    uint64_t mapToken; // issued once for this file's GEM incarnation
};
// The exported capability owns the resource independently of its source GEM
// and file. FdRef/SCM_RIGHTS/fork references retain this capability in turn.
struct DrmPrimeObj { DrmResource* resource; uint32_t refCount; };
static inline void DrmPrimeRef(DrmPrimeObj* p) {
    if (p) __atomic_fetch_add(&p->refCount,1,__ATOMIC_RELAXED);
}
static inline void DrmPrimeUnref(DrmPrimeObj* p) {
    if (p && __atomic_fetch_sub(&p->refCount,1,__ATOMIC_ACQ_REL)==1) {
        MappingBackingUnref(p->resource->lifetime);kfree(p);
    }
}
struct DrmCtx {
    volatile uint32_t refCount;
    DrmGpuOwner* owner;
    DrmGemObj gem[DRM_MAX_GEM];
    KMutex mappingLock;
};
struct DrmMappingGuard {
    DrmCtx* context;
    explicit DrmMappingGuard(DrmCtx* value) : context(value) { KMutexLock(&context->mappingLock); }
    ~DrmMappingGuard() { KMutexUnlock(&context->mappingLock); }
};
static uint32_t DrmGemAlloc(DrmCtx* c,uint32_t gres,uint32_t size,
                           uint32_t stride,uint32_t w,uint32_t h,uint32_t target,uint32_t format) {
    for (uint32_t i=0;i<DRM_MAX_GEM;++i) {
        if (c->gem[i].used) continue;
        auto* resource=DrmResourceCreate(c->owner,gres,size,stride,w,h,target,format);
        if (!resource) return 0;
        c->gem[i]={true,resource,0};return i+1;
    }
    return 0;
}
static uint32_t DrmGemAllocImported(DrmCtx* c,DrmResource* resource) {
    for (uint32_t i=0;i<DRM_MAX_GEM;++i) {
        if (c->gem[i].used) continue;
        MappingBackingRef(resource->lifetime);
        c->gem[i]={true,resource,0};return i+1;
    }
    return 0;
}
static void DrmGemDrop(DrmGemObj* gem) {
    auto* resource=gem->resource;*gem={};
    MappingBackingUnref(resource->lifetime);
}
static DrmGemObj* DrmGemGet(DrmCtx* c, uint32_t bo_handle) {
    if (bo_handle == 0 || bo_handle > DRM_MAX_GEM) return nullptr;
    DrmGemObj* g = &c->gem[bo_handle - 1];
    return g->used ? g : nullptr;
}
} // namespace

DrmBackingDiagnostics DrmBackingDiagnosticsRead() {
    const auto& d=g_drmBackingDiagnostics;
    return {__atomic_load_n(&d.allocatedMappings,__ATOMIC_RELAXED),
            __atomic_load_n(&d.allocatedPageBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.releasedMappings,__ATOMIC_RELAXED),
            __atomic_load_n(&d.releasedPageBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.gemCloseBackingAliases,__ATOMIC_RELAXED),
            __atomic_load_n(&d.gemCloseBackingAliasPageBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.failedAllocations,__ATOMIC_RELAXED),
            __atomic_load_n(&d.failedRequestedBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.quarantinedResources,__ATOMIC_RELAXED),
            __atomic_load_n(&d.quarantinedPageBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&d.failedRetirements,__ATOMIC_RELAXED)};
}

// Own a filesystem snapshot and, when used, an independently retained dirfd
// object. Descriptor pins are dropped after acquisition, never under VFS locks.
struct AtLookup {
    FsSnapshot fs{};
    VfsPath directory{};
    const VfsPath& start() const { return directory.object?directory:fs.cwd; }
    void Clear() { VfsPathRelease(directory);directory={};FsSnapshotRelease(&fs); }
    ~AtLookup() { Clear(); }
    int Init(int fd,const char* text) {
        if(!text)return -EINVAL;
        if(!text[0])return -ENOENT;
        int error=ProcessFsSnapshot(ProcessCurrent(),&fs);if(error)return error;
        if(text[0]=='/') {directory=fs.root;VfsPathRetain(directory);return 0;}
        if(fd==-100)return 0;
        FdRef ref(ProcessCurrent(),fd);
        if(!ref)return -EBADF;
        if((ref->type!=FdType::Vnode && ref->type!=FdType::Path) || !ref->handle)return -ENOTDIR;
        auto* vnode=static_cast<Vnode*>(ref->handle);
        if(vnode->type!=VnodeType::Dir)return -ENOTDIR;
        return VfsPathFromVnode(vnode,&directory);
    }
    // Synthetic proc/dev and historical aliases only. Never feed this name to
    // normal filesystem operations. Reject hidden retained starts: a current
    // global label must not redirect a lower cwd into an overmount.
    int SyntheticName(const char* text,char* out,uint32_t capacity) const {
        if(text[0]=='/') {
            size_t bytes=strlen(text)+1;if(bytes>capacity)return -36;
            memcpy(out,text,bytes);bool allowed=false;
            int error=VfsPathBrokerAllowed(fs.root,out,&allowed);return error?error:allowed?0:-95;
        }
        char parent[512];int error=VfsPathDirectoryPath(start(),parent,sizeof(parent));
        if(error)return error;
        VfsPath visible{};error=VfsPathResolve(fs.root,fs.root,parent,&visible);
        bool same=!error && VfsPathSame(start(),visible);VfsPathRelease(visible);
        if(!same)return error?error:-95;
        size_t prefix=strlen(parent),suffix=strlen(text);
        bool slash=prefix && parent[prefix-1]!='/';
        if(prefix+slash+suffix>=capacity)return -36;
        memcpy(out,parent,prefix);if(slash)out[prefix++]='/';
        memcpy(out+prefix,text,suffix+1);bool allowed=false;
        error=VfsPathBrokerAllowed(fs.root,out,&allowed);return error?error:allowed?0:-95;
    }
    AtLookup()=default;AtLookup(const AtLookup&)=delete;AtLookup& operator=(const AtLookup&)=delete;
};


static int64_t OpenAtInternal(int dirfd,uint64_t pathAddr,uint64_t flags,uint64_t mode)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    char pathBuf[512];
    const char* path = nullptr;
    if (pathAddr < 0x0000800000000000ULL)
    {
        if (pathAddr < 0x1000 || !CopyUserCString(pathAddr, pathBuf, sizeof(pathBuf))) {
            Process* p = ProcessCurrent();
            SerialPrintf("sys_open: EFAULT pathAddr=0x%lx pid=%u (%s)\n",
                         pathAddr, p ? p->pid : 0, p ? p->name : "?");
            return -EFAULT;
        }
        path = pathBuf;
    }
    else if ((pathAddr >> 47) == 0x1FFFFULL)
    {
        // Internal callers such as openat() pass kernel-built absolute paths.
        path = reinterpret_cast<const char*>(pathAddr);
    }
    else
    {
        return -EFAULT;
    }

    AtLookup at;if(int error=at.Init(dirfd,path))return error;
    char resolvedPath[512];
    bool hasSyntheticName=!at.SyntheticName(path,resolvedPath,sizeof(resolvedPath));
    const char* lookupPath=hasSyntheticName?resolvedPath:path;
    const char* syntheticPath=hasSyntheticName?resolvedPath:"";

    if(flags&0x200000) { // O_PATH: retained metadata, never synthetic data I/O.
        const uint64_t normalized=flags&(0x200000u|0x10000u|0x20000u);
        int error=0;
        auto* node=VfsOpenAt(at.fs.root,at.start(),path,VFS_O_METADATA,0,at.fs.mask,&error,!(flags&0x20000));
        if(!node)return error?error:-ENOENT;
        if((flags&0x10000) && node->type!=VnodeType::Dir) {VfsRelease(node);return -ENOTDIR;}
        int fd=FdAlloc(proc,FdType::Path,node);
        if(fd<0) {VfsRelease(node);return -EMFILE;}
        proc->fds[fd].statusFlags=normalized;
        proc->fds[fd].fdFlags=(flags&0x80000)?1:0; // FD_CLOEXEC
        return fd;
    }

    bool procFdPath = false;
    int procFdResult = OpenProcMemFd(proc, syntheticPath, flags, &procFdPath);
    if (procFdPath) return procFdResult;

    // Only the two allowlisted synthetic devices may follow a genuine /dev
    // bind. Other synthetic services retain the general masking policy.
    static constexpr const char* canonicalDevices[]={"/dev/null","/dev/urandom"};
    for(const char* device:canonicalDevices) {
        const char* leaf=device+5;
        if(!StrEq(path,device) && !StrEq(path,leaf) && !StrEq(syntheticPath,device))continue;
        bool allowed=false;
        int error=VfsCanonicalDeviceAllowed(at.fs.root,at.start(),path,&allowed,
            !(flags&0x20000) && !((flags&0x40)&&(flags&0x80)),leaf);
        if(error)return error;
        if(allowed)syntheticPath=device;
        else if(StrEq(syntheticPath,device))syntheticPath="";
    }

    // Device paths
    if(StrEq(syntheticPath,"keyboard") || StrEq(syntheticPath,"/dev/fb0") || StrEq(syntheticPath,"/dev/keyboard") ||
       StrEq(syntheticPath,"/dev/tty") || StrEq(syntheticPath,"/dev/console") ||
       StrEq(syntheticPath,"/dev/dri/renderD128") ||
       StrEq(syntheticPath,"/dev/random") ||
       StrEq(syntheticPath,"/dev/klog") || StrEq(syntheticPath,"/dev/dsp")) {
        const char* deviceName=StrEq(syntheticPath,"keyboard")?"/dev/keyboard":syntheticPath;
        if(int error=VfsPathDeviceActivation(at.fs.root,deviceName))return error;
    }
    if (StrEq(syntheticPath, "/dev/fb0"))
    {
        int fd = FdAlloc(proc, FdType::DevFramebuf, nullptr);
        if (fd < 0) return -EMFILE;
        DbgPrintf("sys_open: /dev/fb0 → fd %d\n", fd);
        return fd;
    }

    if (StrEq(syntheticPath, "keyboard") || StrEq(syntheticPath, "/dev/keyboard"))
    {
        int fd = FdAlloc(proc, FdType::DevKeyboard, nullptr);
        if (fd < 0) return -EMFILE;
        DbgPrintf("sys_open: keyboard → fd %d\n", fd);
        return fd;
    }

    // /dev/tty — controlling terminal
    // In WM terminal mode (stdin is a pipe), dup the stdin pipe so bash's
    // /dev/tty reads go through the terminal's input pipe instead of consuming
    // raw keyboard events from the input subsystem.
    if (StrEq(syntheticPath, "/dev/tty") || StrEq(syntheticPath, "/dev/console"))
    {
        if (proc->fds[0].type == FdType::Pipe && proc->fds[0].handle &&
            proc->fds[1].type == FdType::Pipe && proc->fds[1].handle)
        {
            // Terminal mode: /dev/tty reads from stdin pipe, writes to stdout pipe
            auto* pair = static_cast<TtyDevicePair*>(kmalloc(sizeof(TtyDevicePair)));
            if (!pair) return -ENOMEM;
            pair->readPipe = proc->fds[0].handle;
            pair->writePipe = proc->fds[1].handle;
            int fd = FdAlloc(proc, FdType::DevTty, pair);
            if (fd < 0) { kfree(pair); return -EMFILE; }
            proc->fds[fd].statusFlags = 2; // O_RDWR
            auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
            auto* wp = static_cast<PipeBuffer*>(pair->writePipe);
            __atomic_fetch_add(&rp->readers, 1, __ATOMIC_RELEASE);
            __atomic_fetch_add(&wp->writers, 1, __ATOMIC_RELEASE);
            return fd;
        }
        int fd = FdAlloc(proc, FdType::DevKeyboard, nullptr);
        if (fd < 0) return -EMFILE;
        DbgPrintf("sys_open: %s → fd %d (keyboard/tty)\n", path, fd);
        return fd;
    }

    // /dev/dri/renderD128 — virtio-gpu DRM render node. A Linux-compatible
    // render node that maps the virtio-gpu DRM uABI onto Brook's native
    // per-process GPU path (GpuAppOps), so unmodified Mesa's virgl driver can
    // drive it. Only the render node (no card0/KMS) is offered. Available only
    // when a 3D driver has registered GpuAppOps. (GL shim — see
    // artifacts/gl-drm-enumeration-spike.md.)
    if (StrEq(syntheticPath, "/dev/dri/renderD128"))
    {
        if (!brook::GpuAppGet()) return -ENODEV;
        auto* dctx = static_cast<DrmCtx*>(kmalloc(sizeof(DrmCtx)));
        if (!dctx) return -ENOMEM;
        memset(dctx, 0, sizeof(DrmCtx));
        KMutexInit(&dctx->mappingLock);
        dctx->refCount = 1;
        dctx->owner=static_cast<DrmGpuOwner*>(kmalloc(sizeof(DrmGpuOwner)));
        if (!dctx->owner) { kfree(dctx);return -ENOMEM; }
        *dctx->owner={1,0,{}};
        int fd = FdAlloc(proc, FdType::DevDri, dctx);
        if (fd < 0) { DrmOwnerUnref(dctx->owner);kfree(dctx);return -EMFILE; }
        DbgPrintf("sys_open: /dev/dri/renderD128 → fd %d\n", fd);
        return fd;
    }

    // /dev/dri directory — enumerable so libdrm's drmGetDevices2 can list the
    // render node. Allocate a synthetic-directory fd (recognised by the marker
    // handle in getdents64/fstat). Only when a 3D driver is present.
    if (StrEq(syntheticPath, "/dev/dri") && brook::GpuAppGet())
    {
        int fd = FdAlloc(proc, FdType::SyntheticMem,
                         const_cast<char*>(g_drmDriDirTag));
        if (fd < 0) return -EMFILE;
        proc->fds[fd].seekPos = 0;
        proc->fds[fd].dirPath[0] = '\0';
        return fd;
    }

    // DRM sysfs attribute files (/sys/dev/char/226:128/device/{uevent,vendor,…}).
    // Served as read-only synthetic memory files for libdrm PCI device parsing.
    if (brook::GpuAppGet())
    {
        const char* drmAttr = DrmSysAttrContent(syntheticPath);
        if (drmAttr)
        {
            int fd = FdAlloc(proc, FdType::SyntheticMem, const_cast<char*>(drmAttr));
            if (fd < 0) return -EMFILE;
            proc->fds[fd].seekPos = 0;
            return fd;
        }
    }

    // /dev/null — discard writes, EOF on read
    if (StrEq(syntheticPath, "/dev/null"))
    {
        if((flags&0xc0)==0xc0)return -EEXIST; // O_CREAT|O_EXCL
        if(flags&0x10000)return -ENOTDIR;
        FdEntry owned{};owned.type=FdType::DevNull;
        owned.statusFlags=static_cast<uint32_t>(flags)&~(0x80000u|0x40u|0x80u|0x200u);
        owned.fdFlags=(flags&0x80000u)?1:0;
        int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
        return fd<0?-EMFILE:fd;
    }

    // /dev/urandom, /dev/random — RDRAND-backed random bytes
    if (StrEq(syntheticPath, "/dev/urandom") || StrEq(syntheticPath, "/dev/random"))
    {
        if((flags&0xc0)==0xc0)return -EEXIST;
        if(flags&0x10000)return -ENOTDIR;
        FdEntry owned{};owned.type=FdType::DevUrandom;
        owned.statusFlags=static_cast<uint32_t>(flags)&~(0x80000u|0x40u|0x80u|0x200u);
        owned.fdFlags=(flags&0x80000u)?1:0;
        int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
        return fd<0?-EMFILE:fd;
    }

    // /dev/klog — kernel log ring buffer reader
    if (StrEq(syntheticPath, "/dev/klog"))
    {
        // Allocate a cursor (uint32_t) to track read position in ring buffer.
        auto* cursor = static_cast<uint32_t*>(kmalloc(sizeof(uint32_t)));
        if (!cursor) return -ENOMEM;
        *cursor = 0; // start from beginning of ring
        int fd = FdAlloc(proc, FdType::DevKlog, cursor);
        if (fd < 0) { kfree(cursor); return -EMFILE; }
        DbgPrintf("sys_open: /dev/klog → fd %d\n", fd);
        return fd;
    }

    // /dev/dsp — OSS audio output
    if (StrEq(syntheticPath, "/dev/dsp"))
    {
        if (!AudioAvailable()) return -ENODEV;
        auto* dsp = static_cast<DspState*>(kmalloc(sizeof(DspState)));
        if (!dsp) return -ENOMEM;
        dsp->sampleRate    = DSP_DEFAULT_RATE;
        dsp->channels      = DSP_DEFAULT_CHANNELS;
        dsp->bitsPerSample = DSP_DEFAULT_BITS;
        dsp->fragmentSize  = 4096;
        dsp->ringBytes     = 0;
        dsp->bufferOffset  = 0;
        dsp->bufferSize    = DSP_BUFFER_SIZE;
        dsp->mixerStreamId = AudioMixerOpen();
        dsp->pointerEpoch = dsp->pointerBaseFrames = 0;
        dsp->controlEpoch = 1;
        if (!dsp->mixerStreamId) { kfree(dsp); return -16; } // EBUSY
        if (!AudioMixerSetCapacity(dsp->mixerStreamId, MIXER_RING_FRAMES, 4096 / MIXER_FRAME_BYTES)) {
            AudioMixerClose(dsp->mixerStreamId); kfree(dsp); return -EIO;
        }
        KMutexInit(&dsp->ioLock);
        dsp->refCount      = 1;
        dsp->buffer        = static_cast<uint8_t*>(kmalloc(DSP_BUFFER_SIZE));
        dsp->inputBuffer = static_cast<uint8_t*>(kmalloc(DSP_BUFFER_SIZE));
        if (!dsp->buffer || !dsp->inputBuffer) {
            AudioMixerClose(dsp->mixerStreamId);
            kfree(dsp->inputBuffer); kfree(dsp->buffer); kfree(dsp); return -ENOMEM;
        }
        FdEntry owned{};
        owned.type = FdType::DevDsp; owned.handle = dsp;
        owned.statusFlags = static_cast<uint32_t>(flags) & ~(0x80000u | 0x40u | 0x80u | 0x200u);
        owned.fdFlags = (flags & 0x80000u) ? 1 : 0;
        int fd = FdTablePublishOwned(proc->fds, ProcessFdLock(proc), owned, 0, -1, nullptr);
        if (fd < 0) {
            AudioMixerClose(dsp->mixerStreamId);
            kfree(dsp->inputBuffer); kfree(dsp->buffer); kfree(dsp); return -EMFILE;
        }
        return fd;
    }

    // Synthetic memory files: /etc/passwd, /etc/group, /proc/self/...
    {
        struct SyntheticFile { const char* path; const char* content; };
        static const SyntheticFile syntheticFiles[] = {
            { "/etc/passwd", DesktopPasswd },
            { "/etc/group",  DesktopGroup },
            { "/etc/shells", "/boot/BIN/BASH\n" },
            { "/etc/hostname", "brook\n" },
            { "/etc/os-release", "NAME=Brook\nID=brook\nPRETTY_NAME=Brook\n" },
            { "/etc/nsswitch.conf", "passwd: files\ngroup: files\nhosts: files dns\n" },
            { "/etc/hosts", "127.0.0.1 localhost brook\n" },
            { "/proc/version", "Linux version 6.1.0-brook (brook@brook) (gcc 14.2) #1 SMP\n" },
            { nullptr, nullptr }
        };

        for (auto* sf = syntheticFiles; sf->path; ++sf)
        {
            if (StrEq(syntheticPath, sf->path))
            {
                // Store pointer to static content in handle, seekPos=0
                int fd = FdAlloc(proc, FdType::SyntheticMem, const_cast<char*>(sf->content));
                if (fd < 0) return -EMFILE;
                proc->fds[fd].seekPos = 0;
                return fd;
            }
        }

        // Dynamic /etc/resolv.conf — generated from DHCP DNS server
        if (StrEq(syntheticPath, "/etc/resolv.conf"))
        {
            static char resolvBuf[128];
            auto* nif = brook::NetGetIf();
            if (nif && nif->dns) {
                uint32_t ip = brook::ntohl(nif->dns);
                // Format: "nameserver X.X.X.X\n"
                int pos = 0;
                const char* prefix = "nameserver ";
                for (int i = 0; prefix[i]; i++) resolvBuf[pos++] = prefix[i];
                // Format IP
                for (int octet = 3; octet >= 0; octet--) {
                    uint8_t b = static_cast<uint8_t>((ip >> (octet * 8)) & 0xFF);
                    if (b >= 100) resolvBuf[pos++] = '0' + b / 100;
                    if (b >= 10) resolvBuf[pos++] = '0' + (b / 10) % 10;
                    resolvBuf[pos++] = '0' + b % 10;
                    if (octet > 0) resolvBuf[pos++] = '.';
                }
                resolvBuf[pos++] = '\n';
                const char* opts = "options single-request no-aaaa\n";
                for (int i = 0; opts[i]; i++) resolvBuf[pos++] = opts[i];
                resolvBuf[pos] = '\0';
            } else {
                // Fallback: QEMU default DNS
                const char* fb = "nameserver 10.0.2.3\noptions single-request no-aaaa\n";
                int i = 0;
                while (fb[i]) { resolvBuf[i] = fb[i]; i++; }
                resolvBuf[i] = '\0';
            }
            int fd = FdAlloc(proc, FdType::SyntheticMem, resolvBuf);
            if (fd < 0) return -EMFILE;
            proc->fds[fd].seekPos = 0;
            return fd;
        }
    }

    // /proc/self/exe → return ENOENT for now (readlink handles it)
    if (StrEq(syntheticPath, "/proc/self/exe"))
        return -ENOENT;

    // /dev/shm/<name> — POSIX shared memory via memfd
    // glibc shm_open() calls open("/dev/shm/<name>", flags, mode)
    if (syntheticPath[0] == '/' &&
        syntheticPath[1] == 'd' && syntheticPath[2] == 'e' && syntheticPath[3] == 'v' &&
        syntheticPath[4] == '/' && syntheticPath[5] == 's' && syntheticPath[6] == 'h' &&
        syntheticPath[7] == 'm' && syntheticPath[8] == '/')
    {
        return NamedShmOpen(proc,syntheticPath+9,static_cast<uint32_t>(flags));
    }

    // Translate Linux open flags to VFS flags
    uint32_t vfsFlags = VFS_O_READ;
    static constexpr uint64_t LINUX_O_WRONLY = 1;
    static constexpr uint64_t LINUX_O_RDWR   = 2;
    static constexpr uint64_t LINUX_O_CREAT  = 0x40;
    static constexpr uint64_t LINUX_O_EXCL   = 0x80;
    static constexpr uint64_t LINUX_O_TRUNC  = 0x200;
    static constexpr uint64_t LINUX_O_APPEND = 0x400;
    static constexpr uint64_t LINUX_O_DIRECTORY = 0x10000;
    if (flags & LINUX_O_WRONLY || flags & LINUX_O_RDWR) vfsFlags |= VFS_O_WRITE;
    if ((flags & 3) == LINUX_O_RDWR) vfsFlags |= VFS_O_READWRITE;
    if (flags & LINUX_O_CREAT)  vfsFlags |= VFS_O_CREATE;
    if (flags & LINUX_O_EXCL)   vfsFlags |= VFS_O_EXCL;
    if (flags & LINUX_O_TRUNC)  vfsFlags |= VFS_O_TRUNC;
    if (flags & LINUX_O_APPEND) vfsFlags |= VFS_O_APPEND;

    int openError = 0;
    Vnode* vn=VfsOpenAt(at.fs.root,at.start(),path,vfsFlags,static_cast<uint16_t>(mode&07777),at.fs.mask,&openError,
                          !(flags&0x20000)); // O_NOFOLLOW
    if (!vn && openError && openError != -ENOENT) return openError;

    // Preserve /boot aliases for legacy applications, but Nix store paths
    // identify exact package contents. In particular, a missing libc.so in
    // a compiler's -L directory must not resolve to the boot disk's musl:
    // doing so links musl into otherwise glibc-based executables.
    bool nixStorePath = strncmp(lookupPath, "/nix/store/", 11) == 0;
    if (!vn && hasSyntheticName && lookupPath[0] == '/' && !nixStorePath)
    {
        // Try /boot prefix first
        char bootPath[256] = "/boot";
        uint32_t bi = 5;
        const char* p = lookupPath;
        while (*p && bi + 1 < sizeof(bootPath)) bootPath[bi++] = *p++;
        bootPath[bi] = '\0';
        if (!*p) { // Never open a silently truncated alias.
            vn=VfsOpenAt(at.fs.root,at.fs.root,bootPath,vfsFlags,static_cast<uint16_t>(mode&07777),at.fs.mask,&openError);
            if (!vn && openError && openError != -ENOENT) return openError;
        }

        if (!vn && !(flags & (LINUX_O_DIRECTORY | LINUX_O_CREAT | LINUX_O_TRUNC | LINUX_O_APPEND)) &&
            !(flags & 3))
        {
            const char* fname = lookupPath;
            for (p = lookupPath; *p; ++p)
                if (*p == '/') fname = p + 1;
            auto hasPrefix = [lookupPath](const char* prefix) {
                const char* candidate = lookupPath;
                while (*prefix) if (*candidate++ != *prefix++) return false;
                return true;
            };
            bool libraryPrefix = hasPrefix("/lib/") || hasPrefix("/lib64/") ||
                hasPrefix("/usr/lib/") || hasPrefix("/usr/lib64/");
            bool libraryName = false;
            for (const char* suffix = fname; *suffix; ++suffix) {
                if (suffix[0] == '.' && suffix[1] == 's' && suffix[2] == 'o' &&
                    (suffix[3] == '\0' || suffix[3] == '.')) {
                    const char* version = suffix + 3;
                    libraryName = true;
                    while (*version) {
                        if (*version++ != '.' || *version < '0' || *version > '9') {
                            libraryName = false; break;
                        }
                        while (*version >= '0' && *version <= '9') ++version;
                    }
                    break;
                }
            }
            // A trailing slash has no basename. In particular, a missing Git
            // refs directory must never become the /boot/lib directory.
            if (libraryPrefix && *fname && libraryName) {
                char libPath[256] = "/boot/lib/";
                uint32_t li = 10;
                while (*fname && li + 1 < sizeof(libPath)) libPath[li++] = *fname++;
                libPath[li] = '\0';
                if (!*fname) vn=VfsOpenAt(at.fs.root,at.fs.root,libPath,vfsFlags,0,at.fs.mask,&openError);
                if (vn && vn->type != VnodeType::File) {
                    VfsClose(vn);
                    vn = nullptr;
                }
            }
        }
    }

    if (!vn)
    {
        // Optional Quake pak diagnostics. Missing pak-number probes are normal
        // discovery, not kernel errors; logging thousands of them directly to
        // the UART made game startup spend substantial time with IRQs off.
        bool isPak = false;
        constexpr uint32_t fileTraceCategory = 1u << 1; // SC_FILE below
        if (proc->straceEnabled &&
            (proc->straceFilter == 0 || (proc->straceFilter & fileTraceCategory))) {
            for (const char* p = lookupPath; *p; ++p)
                if (p[0] == 'p' && p[1] == 'a' && p[2] == 'k') { isPak = true; break; }
        }
        if (isPak)
        {
            uint32_t usedFds = 0;
            for (uint32_t fi = 0; fi < MAX_FDS; fi++)
                if (proc->fds[fi].type != FdType::None) usedFds++;
            SerialPrintf("sys_open FAIL: '%s' (resolved '%s') fds=%u/%u\n",
                         path, lookupPath, usedFds, MAX_FDS);
        }
        return -ENOENT;
    }

    uint32_t lookupLength = 0;
    while (lookupPath[lookupLength]) ++lookupLength;
    if (((flags & LINUX_O_DIRECTORY) ||
         (lookupLength && lookupPath[lookupLength - 1] == '/')) && vn->type != VnodeType::Dir) {
        VfsClose(vn);
        return -ENOTDIR;
    }
    VnodeStat openedStat{};
    if(int error=VfsStat(vn,&openedStat)) {VfsClose(vn);return error;}
    if((openedStat.mode&0170000)==0010000) {
        if(int error=NamedFifoAttach(vn,static_cast<uint32_t>(flags))) {VfsClose(vn);return error;}
    }
    FdEntry openedEntry{};openedEntry.type=FdType::Vnode;openedEntry.handle=vn;
    openedEntry.statusFlags=static_cast<uint32_t>(flags);openedEntry.fdFlags=(flags&0x80000)?1:0;
    int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),openedEntry,0,-1,nullptr);
    if(fd<0) {VfsClose(vn);return -EMFILE;}

    // Keep a bounded diagnostic label; this is never a resolution prefix.
    if (vn->type == VnodeType::Dir)
    {
        uint32_t pi = 0;
        while (lookupPath[pi] && pi < 62) { proc->fds[fd].dirPath[pi] = lookupPath[pi]; pi++; }
        // Ensure trailing slash
        if (pi > 0 && proc->fds[fd].dirPath[pi-1] != '/' && pi < 63)
            proc->fds[fd].dirPath[pi++] = '/';
        proc->fds[fd].dirPath[pi] = '\0';
    }

    return fd;
}
static int64_t sys_open(uint64_t path,uint64_t flags,uint64_t mode,uint64_t,uint64_t,uint64_t)
{
    return OpenAtInternal(-100,path,flags,mode);
}


// Linux creat is precisely open(O_WRONLY|O_CREAT|O_TRUNC), including mode,
// umask, descriptor access and the regular-file truncate/mapping safeguards.
static int64_t sys_creat(uint64_t path,uint64_t mode,uint64_t,uint64_t,uint64_t,uint64_t) {
    return sys_open(path,0x1u|0x40u|0x200u,mode,0,0,0);
}

// ---------------------------------------------------------------------------
// sys_close (3)
// ---------------------------------------------------------------------------

// Tear down the handle named by an fd snapshot: per-type unref/free plus any
// waiter wakeups. Runs outside the fd table lock. Called from the immediate
// close() path (CloseProcessFd) and from FdPut when it drains the last pin on a
// slot whose close() was deferred (BRO-156). The snapshot makes this path
// independent of the slot, which by now is already cleared (or about to be).
void FinalizeClosedFd(const FdClaimResult& c)
{
    KernelOperationGuard operation(ProcessCurrent());
    if ((c.type == FdType::Vnode || c.type == FdType::Path) && c.handle)
    {
        auto* vn = static_cast<Vnode*>(c.handle);
        uint32_t prev = __atomic_fetch_sub(&vn->refCount, 1, __ATOMIC_ACQ_REL);
        if (prev <= 1)
            VfsClose(vn);
        // else: other processes still reference this vnode
    }

    if (c.type == FdType::Pipe && c.handle)
        PipeReleaseEnd(static_cast<PipeBuffer*>(c.handle), (c.flags & 1) != 0);

    if (c.type == FdType::Socket && c.handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(c.handle)) - 1;
        brook::SockUnref(sockIdx);
    }

    if (c.type == FdType::SignalFd && c.handle) SignalFdUnref(static_cast<SignalFd*>(c.handle));
    if (c.type == FdType::SyncFile && c.handle) SyncFileUnref(static_cast<SyncFileData*>(c.handle));

    if (c.type == FdType::EventFd && c.handle)
        EventFdUnref(static_cast<EventFdData*>(c.handle));

    if (c.type == FdType::DrmPrime && c.handle)
        DrmPrimeUnref(static_cast<DrmPrimeObj*>(c.handle));
    if (c.type == FdType::CompositorScene && c.handle)
        SceneHandleUnref(static_cast<SceneHandle*>(c.handle));

    if (c.type == FdType::EpollFd && c.handle)
        EpollFdUnref(static_cast<EpollInstance*>(c.handle));

    if (c.type == FdType::TimerFd && c.handle)
        TimerFdUnref(static_cast<TimerFdData*>(c.handle));

    if (c.type == FdType::MemFd && c.handle)
    {
        auto* mfd = static_cast<MemFdData*>(c.handle);
        MemFdUnref(mfd);
    }

    if (c.type == FdType::UnixSocket && c.handle)
    {
        UNIX_LIFE(Finalize, c.handle, 0, 0, -1);
        UnixSocketHandleUnref(c.handle);
    }

    if (c.type == FdType::DevDsp && c.handle)
    {
        auto* dsp = static_cast<DspState*>(c.handle);
        uint32_t prev = __atomic_fetch_sub(&dsp->refCount, 1, __ATOMIC_ACQ_REL);
        if (prev <= 1)
        {
            AudioMixerClose(dsp->mixerStreamId);
            kfree(dsp->inputBuffer);
            kfree(dsp->buffer);
            kfree(dsp);
        }
    }

    if (c.type == FdType::DevKlog && c.handle)
    {
        // DevKlog handle is a bare uint64_t* cursor — no refcount, just free.
        // Fork deep-copies it, so each process owns its own cursor.
        kfree(c.handle);
    }

    if (c.type == FdType::DevDri && c.handle)
    {
        // Drop file-owned references. Resource retirement runs independently;
        // GEM/PRIME/VMA/capture leases independently keep the producer alive.
        auto* dctx = static_cast<DrmCtx*>(c.handle);
        if (__atomic_fetch_sub(&dctx->refCount, 1, __ATOMIC_ACQ_REL) > 1) return;
        for (uint32_t i=0;i<DRM_MAX_GEM;++i)
            if (dctx->gem[i].used) DrmGemDrop(&dctx->gem[i]);
        DrmOwnerUnref(dctx->owner);
        kfree(dctx);
    }

    if (c.type == FdType::DevTty && c.handle)
    {
        // DevTty wraps a TtyDevicePair that references stdin/stdout pipes.
        // Opening /dev/tty incremented reader/writer counts on those pipes,
        // so closing must decrement them and free the pair struct.
        auto* pair = static_cast<TtyDevicePair*>(c.handle);
        PipeReleaseEnd(static_cast<PipeBuffer*>(pair->readPipe), false);
        PipeReleaseEnd(static_cast<PipeBuffer*>(pair->writePipe), true);
        kfree(pair);
    }
}

int64_t CloseProcessFd(Process* proc, int fd)
{
    KernelOperationGuard operation(ProcessCurrent());
    if (!proc) return -EBADF;
    // Close the slot honouring any in-flight pin. FdClose either:
    //   * ClaimedNow — slot was unpinned; it has been cleared and we own the
    //     snapshot, so finalize the handle now;
    //   * Deferred   — a sibling is mid-operation on this fd (pinned); the slot
    //     is marked closing and the last FdPut will finalize it — nothing to do
    //     here, and the caller still sees a successful close();
    //   * NotFound   — already closed (or a sibling won a concurrent close()).
    // This both prevents the double-unref the old FdGet()+FdFree() split allowed
    // and closes the use-after-free window where a blocking op dereferenced a
    // handle a sibling had freed (BRO-156).
    FdClaimResult c;
    switch (FdClose(proc, fd, &c))
    {
        case FdCloseResult::NotFound:
            return -EBADF;
        case FdCloseResult::ClaimedNow:
            UnixLifeClaim(fd, c);
            FinalizeClosedFd(c);
            return 0;
        case FdCloseResult::Deferred:
            return 0;
    }
    return 0;
}

// Force-close a slot at process teardown, IGNORING any outstanding pin. Used
// only by ProcessCloseAllFds, which runs after all sibling threads of the group
// are gone (scheduler.cpp gates it on !hasLiveThreadPeer). A thread killed while
// blocked in a pinned op (read/write/lseek) never runs its FdRef destructor, so
// pinCount stays >0 and the ordinary FdClose would defer the teardown forever —
// leaking the handle's bumped refcount. With no live peer left to call FdPut,
// it is safe to claim-and-clear the slot unconditionally and finalize now.
void FdReleaseOwnedEntry(const FdEntry& entry)
{
    FinalizeClosedFd({entry.type, entry.handle, entry.flags});
}

int64_t CloseProcessFdForced(Process* proc, int fd)
{
    KernelOperationGuard operation(ProcessCurrent());
    if (!proc) return -EBADF;
    FdClaimResult c;
    if (FdClaim(proc, fd, &c)) {
        UnixLifeClaim(fd, c);
        FinalizeClosedFd(c);
    }
    return 0;
}

static int64_t sys_close(uint64_t fd, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return CloseProcessFd(ProcessCurrent(), static_cast<int>(fd));
}

static int64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags,
                               uint64_t, uint64_t, uint64_t)
{
    static constexpr uint64_t CLOSE_RANGE_UNSHARE = 1ULL << 1;
    static constexpr uint64_t CLOSE_RANGE_CLOEXEC = 1ULL << 2;
    static constexpr uint64_t CLOSE_RANGE_ALLOWED =
        CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC;

    if (flags & ~CLOSE_RANGE_ALLOWED) return -EINVAL;
    if (first > last) return -EINVAL;

    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    if (last >= MAX_FDS) last = MAX_FDS - 1;
    if (first >= MAX_FDS) return 0;

    for (uint64_t fd = first; fd <= last; ++fd)
    {
        if (proc->fds[fd].type == FdType::None) continue;
        if (flags & CLOSE_RANGE_CLOEXEC)
            proc->fds[fd].fdFlags = 1;
        else
            CloseProcessFd(proc, static_cast<int>(fd));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sys_pipe (22)
// ---------------------------------------------------------------------------

static int64_t sys_pipe(uint64_t pipefdAddr, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(pipefdAddr, 2 * sizeof(int32_t))) return -EFAULT;
    auto* pipefd = reinterpret_cast<int32_t*>(pipefdAddr);

    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    // Allocate pipe buffer
    auto* pipe = PipeBufferCreate(PIPE_BUF_DEFAULT_SIZE);
    if (!pipe) return -ENOMEM;
    pipe->readers = 1;
    pipe->writers = 1;

    // Allocate read end (flags=0) and write end (flags=1)
    int readFd = FdAlloc(proc, FdType::Pipe, pipe);
    if (readFd < 0) { PipeBufferDestroy(pipe); return -EMFILE; }
    proc->fds[readFd].flags = 0;  // read end
    proc->fds[readFd].statusFlags = 0;  // O_RDONLY

    int writeFd = FdAlloc(proc, FdType::Pipe, pipe);
    if (writeFd < 0)
    {
        FdFree(proc, readFd);
        PipeBufferDestroy(pipe);
        return -EMFILE;
    }
    proc->fds[writeFd].flags = 1;  // write end
    proc->fds[writeFd].statusFlags = 1;  // O_WRONLY

    pipefd[0] = readFd;
    pipefd[1] = writeFd;

    DbgPrintf("sys_pipe: fd[%d,%d] for pid %u\n", readFd, writeFd, proc->pid);
    return 0;
}

// ---------------------------------------------------------------------------
// sys_dup (32) / sys_dup2 (33)
// ---------------------------------------------------------------------------

// Bump refcount on the underlying object for a duplicated fd entry.
static void FdBumpRefcount(const FdEntry* fde)
{
    if (!fde->handle) return;
    switch (fde->type)
    {
    case FdType::Pipe: {
        auto* pipe = static_cast<PipeBuffer*>(fde->handle);
        if (fde->flags & 1)
            __atomic_fetch_add(&pipe->writers, 1, __ATOMIC_RELEASE);
        else
            __atomic_fetch_add(&pipe->readers, 1, __ATOMIC_RELEASE);
        break;
    }
    case FdType::Path:
    case FdType::Vnode:
        __atomic_fetch_add(&static_cast<Vnode*>(fde->handle)->refCount, 1, __ATOMIC_RELEASE);
        break;
    case FdType::Socket: {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        brook::SockRef(sockIdx);
        break;
    }
    case FdType::MemFd:
        MemFdRef(static_cast<MemFdData*>(fde->handle));
        break;
    case FdType::SignalFd:
        SignalFdRef(static_cast<SignalFd*>(fde->handle));break;
    case FdType::SyncFile:
        SyncFileRef(static_cast<SyncFileData*>(fde->handle));break;
    case FdType::EventFd:
        EventFdRef(static_cast<EventFdData*>(fde->handle));
        break;
    case FdType::DevDri:
        __atomic_fetch_add(&static_cast<DrmCtx*>(fde->handle)->refCount, 1, __ATOMIC_RELEASE);
        break;
    case FdType::DrmPrime:
        DrmPrimeRef(static_cast<DrmPrimeObj*>(fde->handle));
        break;
    case FdType::CompositorScene:
        SceneHandleRef(static_cast<SceneHandle*>(fde->handle));
        break;
    case FdType::EpollFd:
        EpollFdRef(static_cast<EpollInstance*>(fde->handle));
        break;
    case FdType::TimerFd:
        TimerFdRef(static_cast<TimerFdData*>(fde->handle));
        break;
    case FdType::UnixSocket: {
        UnixSocketHandleRef(fde->handle);
        break;
    }
    case FdType::DevDsp:
        __atomic_fetch_add(&static_cast<DspState*>(fde->handle)->refCount, 1, __ATOMIC_ACQ_REL);
        break;
    default:
        break;
    }
}

// A successful copy owns its handle before it can become visible in any table.
// Deep-copy-only handle types must never be published with the source pointer.
bool FdRetainCopy(FdEntry* copy)
{
    if (copy->type == FdType::UnixSocket)
        UNIX_LIFE(CopyRetain, copy->handle, 0, 0, -1);
    if (copy->type == FdType::DevKlog && copy->handle) {
        void* handle = DevKlogDeepCopy(copy->handle);
        if (!handle) return false;
        copy->handle = handle;
    } else if (copy->type == FdType::DevTty && copy->handle) {
        void* handle = DevTtyDeepCopy(copy->handle);
        if (!handle) return false;
        copy->handle = handle;
    } else {
        FdBumpRefcount(copy);
    }
    return true;
}

static int64_t DuplicateFd(Process* proc, int oldfd, int target,
                           uint32_t minimum, uint8_t fdFlags)
{
    KernelOperationGuard operation(proc);
    FdRef oldref(proc, oldfd);
    if (!oldref) return -EBADF;
    if (target == oldfd) return target;
    FdEntry owned;
    SpinLockAcquire(ProcessFdLock(proc));
    owned = *oldref.get();
    bool retained = FdRetainCopy(&owned);
    SpinLockRelease(ProcessFdLock(proc));
    if (!retained) return -ENOMEM;
    owned.fdFlags = fdFlags;
    FdClaimResult retired{};
    FileLockCloseContext lockClose(proc);
    int result = FdTablePublishOwned(proc->fds, ProcessFdLock(proc), owned,
                                     minimum, target, &retired, UnixLifePublished,
                                     FileLockCloseContext::Closing,&lockClose);
    if (result < 0)
        FinalizeClosedFd({owned.type, owned.handle, owned.flags});
    else if (retired.type != FdType::None) {
        UnixLifeClaim(result, retired);
        FinalizeClosedFd(retired);
    }
    return result;
}

static int64_t sys_dup(uint64_t oldfd, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    return DuplicateFd(proc, static_cast<int>(oldfd), -1, 0, 0);
}

static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    int target = static_cast<int>(newfd);
    if (target < 0 || target >= static_cast<int>(MAX_FDS)) return -EBADF;
    return DuplicateFd(proc, static_cast<int>(oldfd), target, 0, 0);
}

static int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                         uint64_t, uint64_t, uint64_t)
{
    uint32_t options = static_cast<uint32_t>(flags);
    if (options & ~0x80000u) return -EINVAL;
    if (static_cast<int>(oldfd) == static_cast<int>(newfd)) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    int target = static_cast<int>(newfd);
    if (target < 0 || target >= static_cast<int>(MAX_FDS)) return -EBADF;
    return DuplicateFd(proc, static_cast<int>(oldfd), target, 0, options ? 1 : 0);
}

// ---------------------------------------------------------------------------
// sys_lseek (8)
// ---------------------------------------------------------------------------

static constexpr int SEEK_SET = 0;
static constexpr int SEEK_CUR = 1;
static constexpr int SEEK_END = 2;

static int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    // BRO-156: pin the slot for the whole op. SEEK_END dereferences fde->handle
    // across VfsStat, which can block on disk I/O — a bare FdGet would let a
    // sibling close() free the handle mid-stat. The pin also keeps the slot
    // alive for the seekPos read-modify-write below.
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref) return -EBADF;
    FdEntry* fde = ref.get();

    if(FdPipe(fde))return -29;
    if(fde->type==FdType::SignalFd)return whence<=4?0:-EINVAL; // Linux noop_llseek
    if(fde->type==FdType::SyncFile)return -29; // ESPIPE

    if(fde->type==FdType::Path)return -EBADF;
    if(fde->type==FdType::Vnode && fde->handle && static_cast<Vnode*>(fde->handle)->type==VnodeType::Namespace)return -29;
    int64_t soff = static_cast<int64_t>(offset);

    switch (static_cast<int>(whence))
    {
    case SEEK_SET:
        fde->seekPos = static_cast<uint64_t>(soff);
        break;
    case SEEK_CUR:
        fde->seekPos = static_cast<uint64_t>(static_cast<int64_t>(fde->seekPos) + soff);
        break;
    case SEEK_END:
    {
        if (fde->type == FdType::MemFd && fde->handle) {
            auto* mfd = static_cast<MemFdData*>(fde->handle);
            int64_t newPos = static_cast<int64_t>(mfd->size) + soff;
            if (newPos < 0) return -EINVAL;
            fde->seekPos = static_cast<uint64_t>(newPos);
            break;
        }
        if (fde->type != FdType::Vnode || !fde->handle) return -EINVAL;
        auto* vn = static_cast<Vnode*>(fde->handle);
        VnodeStat st{};
        if (VfsStat(vn, &st) < 0) return -EINVAL;
        int64_t newPos = static_cast<int64_t>(st.size) + soff;
        if (newPos < 0) return -EINVAL;
        fde->seekPos = static_cast<uint64_t>(newPos);
        break;
    }
    default:
        return -EINVAL;
    }

    return static_cast<int64_t>(fde->seekPos);
}

// ---------------------------------------------------------------------------
// sys_brk (12)
// ---------------------------------------------------------------------------

static int64_t sys_brk(uint64_t requested,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();if (!proc) return -ENOMEM;
    return UserMappingBreak(proc,proc->addressSpace,requested);
}

// ---------------------------------------------------------------------------
// sys_mmap (9)
// ---------------------------------------------------------------------------

enum MmapFlags : uint64_t {
    MAP_SHARED    = 0x01,
    MAP_PRIVATE   = 0x02,
    MAP_FIXED     = 0x10,
    MAP_FIXED_NOREPLACE = 0x100000,
    MAP_ANONYMOUS = 0x20,
    MAP_DENYWRITE = 0x0800,
    MAP_POPULATE  = 0x8000,
};

enum MmapProt : uint64_t {
    PROT_READ     = 0x1,
    PROT_WRITE    = 0x2,
    PROT_EXEC     = 0x4,
};

struct DeviceMappingBacking {
    uint64_t kernelBase, physicalBase;
    bool borrowedPhysical;
    MappingBacking* resourceLifetime;
};
struct MappingBackingGuard {
    MappingBacking* value = nullptr;
    ~MappingBackingGuard() { MappingBackingUnref(value); }
};
static void ReleaseFileMapping(void* object) {
    auto* backing = static_cast<FileMappingBacking*>(object);
    if (backing->pinned) VfsFileMapUnpin(backing->vnode);
    VnodeUnref(backing->vnode); kfree(backing);
}
static int PrepareAnonymousMapping(MappingTransaction& operation, const UserMappingRequest&, void*) {
    return operation.PrepareZeroed(MemTag::User,operation.Space()->allocationPid) ? 0 : -ENOMEM;
}
static int PrepareFileMapping(MappingTransaction& operation, const UserMappingRequest& request, void*) {
    if (!operation.PrepareZeroed(MemTag::User,operation.Space()->allocationPid)) return -ENOMEM;
    auto* backing = static_cast<FileMappingBacking*>(request.range.backing->object);
    uint64_t offset = request.range.offset;
    for (uint64_t page=0;page<request.preparedPages;++page) {
        auto* bytes = reinterpret_cast<uint8_t*>(PhysToVirt(operation.PreparedPage(page)).raw());
        int got = VfsRead(backing->vnode,bytes,4096,&offset);
        if (got < 0) return got;
        if (got < 4096) break; // candidates are already zeroed, including EOF tails
    }
    return 0;
}
static int PrepareDeviceMapping(MappingTransaction& operation, const UserMappingRequest& request, void*) {
    const auto* source = static_cast<DeviceMappingBacking*>(request.range.backing->object);
    for (uint64_t page=0;page<request.preparedPages;++page) {
        auto physical = source->borrowedPhysical ? PhysicalAddress(source->physicalBase+page*4096) :
            VmmVirtToPhys(KernelPageTable,VirtualAddress(source->kernelBase+page*4096));
        if (!physical) return -ENOMEM;
        if (!source->borrowedPhysical) PmmRefPage(physical);
        if (!operation.StagePage(page,physical,!source->borrowedPhysical)) {
            if (!source->borrowedPhysical) PmmUnrefPage(physical);
            return -ENOMEM;
        }
    }
    return 0;
}
static void ReleaseDeviceMapping(void* object) {
    auto* source=static_cast<DeviceMappingBacking*>(object);
    MappingBackingUnref(source->resourceLifetime);kfree(source);
}
static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset)
{
    __atomic_fetch_add(&g_profMmapCount,1,__ATOMIC_RELAXED);
    constexpr uint64_t userEnd = 0x0000800000000000ULL;
    const bool exactAddress = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    if (!length || length > userEnd || (prot & ~7ULL) ||
        (!(flags & MAP_PRIVATE) == !(flags & MAP_SHARED)) ||
        (exactAddress && ((addr & 4095) || addr < VIRTUAL_NULL_GUARD || addr>=userEnd || length > userEnd-addr)) ||
        (!(flags & MAP_ANONYMOUS) && (offset & 4095))) return -EINVAL;
    length = (length+4095)&~uint64_t{4095};
    if (!(flags & MAP_ANONYMOUS) && offset > ~uint64_t{0}-length) return -EINVAL;
    Process* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard protectedOperation(proc);
    MappingBackingGuard backing;
    UserMappingRequest request{};
    request.hint = addr; request.length = length; request.fixed = exactAddress;
    request.noReplace = (flags & MAP_FIXED_NOREPLACE) != 0;
    request.range = {0,length,offset,0,nullptr,static_cast<uint32_t>(prot),7,MappingKind::Anonymous,
                     (flags & MAP_SHARED) ? MappingSharing::Shared : MappingSharing::Private,MappingLeafOwnership::Managed};
    if (flags & MAP_ANONYMOUS) {
        request.range.offset = 0;
        if (flags & MAP_SHARED) {
            auto* mfd = static_cast<MemFdData*>(kmalloc(sizeof(MemFdData)));
            if (!mfd) return -ENOMEM;
            memset(mfd,0,sizeof(*mfd));mfd->inode=NextMemFdInode();if(!mfd->inode) {kfree(mfd);return -75;}mfd->refCount=1;mfd->seals={};
            if (!MemFdGrow(mfd,length)) { MemFdUnref(mfd); return -ENOMEM; }
            mfd->size=length;
            if(int error=MemFdAdmitMapping(mfd->seals,true,static_cast<uint32_t>(prot),&request.range.maximumProtection)) {MemFdUnref(mfd);return error;}
            backing.value=MappingBackingCreate(mfd,MemFdReleaseMapping);
            if (!backing.value) { MemFdReleaseMapping(mfd); return -ENOMEM; }
            request.range.kind=MappingKind::MemFd;request.range.leafOwnership=MappingLeafOwnership::Borrowed;
            request.range.backing=backing.value;
        } else if (prot && (flags & MAP_POPULATE)) {
            // Ordinary private anonymous mappings reserve address space;
            // ResolveMappingFault supplies zeroed pages on first access.
            // Keep explicit population requests eager.
            request.preparedPages=length/4096;request.prepare=PrepareAnonymousMapping;
        }
        return UserMappingCreate(proc,proc->addressSpace,request);
    }
    FdRef descriptor(proc,static_cast<int>(fd));
    if (!descriptor) return -EBADF;
    auto* entry=descriptor.get();
    if(entry->type==FdType::Path)return -EBADF;
    if (entry->type == FdType::MemFd && entry->handle) {
        if ((entry->statusFlags & 3) == 1 ||
            ((flags & MAP_SHARED) && (prot & 2) && (entry->statusFlags & 3) == 0)) return -13;
        if ((flags & MAP_PRIVATE) && (prot & 2)) return -95;
        auto* mfd=static_cast<MemFdData*>(entry->handle);
        uint32_t maximum=5u|(((flags & MAP_SHARED) && (entry->statusFlags & 3) != 0)?2u:0u);
        MfdLock(mfd);
        if((flags&MAP_SHARED) && (prot&2) && (mfd->seals.bits&8)) {MfdUnlock(mfd);return -EPERM;}
        if (!MemFdGrowLocked(mfd,offset+length)) {MfdUnlock(mfd);return -ENOMEM;}
        int admitted=MemFdAdmitMapping(mfd->seals,flags&MAP_SHARED,static_cast<uint32_t>(prot),&maximum);
        if(admitted) {MfdUnlock(mfd);return admitted;}
        MemFdRef(mfd);MfdUnlock(mfd);
        backing.value=MappingBackingCreate(mfd,MemFdReleaseMapping);
        if (!backing.value) { MemFdReleaseMapping(mfd);return -ENOMEM; }
        request.range.kind=MappingKind::MemFd;request.range.leafOwnership=MappingLeafOwnership::Borrowed;
        request.range.maximumProtection=maximum;
        request.range.backing=backing.value;
        return UserMappingCreate(proc,proc->addressSpace,request);
    }
    if (entry->type == FdType::DevDri || entry->type == FdType::DevFramebuf) {
        auto* source=static_cast<DeviceMappingBacking*>(kmalloc(sizeof(DeviceMappingBacking)));
        if (!source) return -ENOMEM;
        *source={};backing.value=MappingBackingCreate(source,ReleaseDeviceMapping);
        if (!backing.value) { kfree(source);return -ENOMEM; }
        uint64_t available=0;
        if (entry->type == FdType::DevDri) {
            auto* context=static_cast<DrmCtx*>(entry->handle);if (!context) return -ENODEV;
            DrmMappingGuard guard(context);
            auto* gem=DrmFindMapToken(context->gem,DRM_MAX_GEM,offset);
            if (!gem) return -EINVAL;
            auto* resource=gem->resource;
            if (resource->hostBlob) {
                const auto& host=resource->hostMapping;
                if (!(flags&MAP_SHARED) || !host.length || !host.physicalBase || request.length>host.length) return -EINVAL;
                switch(host.cache) {
                case 1: break;
                case 2: request.cacheFlags=VMM_CACHE_DISABLE|VMM_WRITE_THRU;break;
                case 3:
                    if (!CpuWriteCombiningReady()) return -95;
                    request.cacheFlags=VMM_LEAF_PAT;break;
                default:return -95;
                }
                source->borrowedPhysical=true;source->physicalBase=host.physicalBase;
            } else if (!resource->backingKva || !resource->backingAttached) return -EINVAL;
            MappingBackingRef(resource->lifetime); // before releasing the GEM lock
            source->resourceLifetime=resource->lifetime;
            source->kernelBase=resource->backingKva;
            available=resource->hostBlob ? resource->hostMapping.length :
                (static_cast<uint64_t>(resource->backingBytes)+4095)&~uint64_t{4095};
        } else {
            uint32_t width,height,stride;uint64_t physical;
            if (!TtyGetFramebufferPhys(&physical,&width,&height,&stride)) return -ENODEV;
            KMutexLock(&proc->framebufferLock);
            MappingBacking* framebuffer=proc->fbBacking;MappingBackingRef(framebuffer);
            source->kernelBase=reinterpret_cast<uint64_t>(proc->fbVirtual);
            available=source->kernelBase ? proc->fbVirtualSize : static_cast<uint64_t>(stride)*height;
            KMutexUnlock(&proc->framebufferLock);
            if (framebuffer) {
                if (request.length>((available+4095)&~uint64_t{4095})) request.length=(available+4095)&~uint64_t{4095};
                MappingBackingUnref(backing.value);backing.value=framebuffer;
                request.range.offset=0;
                if (!UserMappingConfigureBuffer(request,framebuffer)) return -EINVAL;
                return UserMappingCreate(proc,proc->addressSpace,request);
            }
            source->borrowedPhysical=!source->kernelBase;source->physicalBase=physical;
            available=(available+4095)&~uint64_t{4095};
        }
        if (!available) return -EINVAL;
        if (request.length > available) request.length=available;
        request.range.kind=MappingKind::Device;request.range.sharing=MappingSharing::Shared;
        request.range.offset=0;request.range.backing=backing.value;
        request.range.leafOwnership=source->borrowedPhysical?MappingLeafOwnership::Borrowed:MappingLeafOwnership::Managed;
        request.preparedPages=request.length/4096;request.prepare=PrepareDeviceMapping;
        return UserMappingCreate(proc,proc->addressSpace,request);
    }
    if (entry->type != FdType::Vnode || !entry->handle || static_cast<Vnode*>(entry->handle)->type != VnodeType::File) return -ENODEV;
    if ((entry->statusFlags & 3) == 1) return -13;
    if ((flags & MAP_SHARED) && (prot & 2) && (entry->statusFlags & 3) == 0)return -13;
    auto fileFailure=[&](const char* stage,int64_t result) -> int64_t {
        // Only selected errors-only tracing emits this setup-stage evidence.
        // The mapping core reports its own detailed failure after unlocking.
        if (proc->straceEnabled && proc->straceErrorsOnly)
            SerialPrintf("[mmap-file-error:%u] stage=%s error=%ld fd=%lu offset=0x%lx length=%lu prot=0x%lx flags=0x%lx\n",
                         proc->pid,stage,result,fd,offset,length,prot,flags);
        return result;
    };
    auto* vnode=static_cast<Vnode*>(entry->handle);
    if(vnode->ops->deny_mapping)return -95;
    auto* fileObject=vnode->ops->file_object?vnode->ops->file_object(vnode):nullptr;
    if((flags&MAP_SHARED) && (prot&2) && (!fileObject || !vnode->ops->write))return -95;
    int pin=VfsFileMapPin(vnode);if (pin && pin != -95) return fileFailure("mapping-pin",pin);
    VnodeStat stat{};int error=VfsStat(vnode,&stat);
    if (error) { if (!pin) VfsFileMapUnpin(vnode);return fileFailure("stat",error); }
    auto* file=static_cast<FileMappingBacking*>(kmalloc(sizeof(FileMappingBacking)));
    if (!file) { if (!pin) VfsFileMapUnpin(vnode);return fileFailure("file-backing-storage",-ENOMEM); }
    VnodeRef(vnode);*file={vnode,stat.size,pin==0};
    backing.value=MappingBackingCreate(file,ReleaseFileMapping);
    if (!backing.value) { ReleaseFileMapping(file);return fileFailure("backing-descriptor-storage",-ENOMEM); }
    backing.value->file=fileObject;
    backing.value->fileVnode=vnode;
    request.range.kind=MappingKind::File;request.range.backing=backing.value;
    if (flags & MAP_SHARED) request.range.maximumProtection=5u|
        ((fileObject && vnode->ops->write && (entry->statusFlags&3)!=0)?2u:0u);
    if (pin == -95) { request.preparedPages=length/4096;request.prepare=PrepareFileMapping; }
    else if (length<=16*1024*1024 && stat.size>offset)
        PageCachePreload(vnode,offset/4096,stat.size,static_cast<uint32_t>(length/4096));
    return UserMappingCreate(proc,proc->addressSpace,request);
}
static int64_t sys_mprotect(uint64_t addr,uint64_t length,uint64_t prot,uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();if (!proc) return -ESRCH;
    if (prot & ~7ULL) return -EINVAL;
    return UserMappingProtect(proc,proc->addressSpace,addr,length,static_cast<uint32_t>(prot));
}
static int64_t sys_msync(uint64_t addr,uint64_t length,uint64_t flags,uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();if(!proc)return -ESRCH;
    return UserMappingSync(proc,proc->addressSpace,addr,length,flags);
}
static int64_t sys_munmap(uint64_t addr,uint64_t length,uint64_t,uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();if (!proc) return -ESRCH;
    return UserMappingRemove(proc,proc->addressSpace,addr,length);
}

// ---------------------------------------------------------------------------
// sys_mremap (25) -- resize an existing mapping.  Minimal implementation:
//   - shrink: unmap the tail pages in place, return old_addr
//   - same:   return old_addr
//   - grow:   if MREMAP_MAYMOVE, allocate a new region, copy the old contents
//             page-by-page, unmap the old range, return new address.  Otherwise
//             return -ENOMEM (we don't try to extend in place — the next pages
//             are usually already taken by mmapNext bumping).
// Flags values from <sys/mman.h>: MREMAP_MAYMOVE=1, MREMAP_FIXED=2.
// We don't honour MREMAP_FIXED; if userspace passes new_addr we ignore it.
// ---------------------------------------------------------------------------

static int PrepareUserRemap(const MappingRange& range,uint64_t length,void*) {
    if (range.kind==MappingKind::MemFd) {
        if (range.offset>~uint64_t{0}-length) return -EINVAL;
        return MemFdGrow(static_cast<MemFdData*>(range.backing->object),range.offset+length)?0:-ENOMEM;
    }
    if (range.kind==MappingKind::File && !static_cast<FileMappingBacking*>(range.backing->object)->pinned) return -95;
    return 0;
}
static int64_t sys_mremap(uint64_t source,uint64_t oldLength,uint64_t newLength,uint64_t flags,uint64_t destination,uint64_t) {
    Process* proc=ProcessCurrent();if (!proc) return -ESRCH;
    return UserMappingResize(proc,proc->addressSpace,source,oldLength,newLength,flags,destination,PrepareUserRemap,nullptr);
}

// ---------------------------------------------------------------------------
// sys_arch_prctl (158) -- TLS setup
// ---------------------------------------------------------------------------

static constexpr uint64_t ARCH_SET_GS    = 0x1001;
static constexpr uint64_t ARCH_GET_GS    = 0x1004;
static constexpr uint64_t ARCH_SET_FS    = 0x1002;
static constexpr uint64_t ARCH_GET_FS    = 0x1003;
static constexpr uint64_t ARCH_CET_STATUS = 0x3001;

static int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t,
                               uint64_t, uint64_t, uint64_t)
{
    switch (code)
    {
    case ARCH_SET_GS:
    case ARCH_SET_FS:
    {
        // Restrict bases to the canonical user half, even if not mapped yet.
        // High GS would also defeat the interrupt-entry kernel GS discriminator.
        if (addr >= 0x0000800000000000ULL) return -EPERM;
        Process* proc = ProcessCurrent();
        if (!proc) return -EINVAL;
        uint64_t flags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        if (code == ARCH_SET_GS) {
            proc->savedCtx.gsBase = addr;
            // Kernel GS is active here. Change only the parked user base.
            WriteMsr(MSR_KERNEL_GS_BASE, addr);
        } else {
            proc->fsBase = addr;
            proc->savedCtx.fsBase = addr;
            WriteMsr(0xC0000100U, addr);
        }
        __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
        return 0;
    }
    case ARCH_GET_GS:
    {
        if (!UserBufferWritable(addr, sizeof(uint64_t))) return -EFAULT;
        *reinterpret_cast<uint64_t*>(addr) = ReadMsr(MSR_KERNEL_GS_BASE);
        return 0;
    }
    case ARCH_GET_FS:
    {
        if (!UserBufferWritable(addr, sizeof(uint64_t))) return -EFAULT;
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100U));
        uint64_t fsBase = (static_cast<uint64_t>(hi) << 32) | lo;
        *reinterpret_cast<uint64_t*>(addr) = fsBase;
        return 0;
    }
    case ARCH_CET_STATUS:
        return -EINVAL; // CET not supported
    default:
        return -EINVAL;
    }
}

// ---------------------------------------------------------------------------
// sys_exit (60) / sys_exit_group (231)
// ---------------------------------------------------------------------------

static int64_t sys_exit(uint64_t status, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    {
        Process* p = ProcessCurrent();
        (void)p;
        DbgPrintf("sys_exit: '%s' (pid %u, tgid %u) exited with status %lu\n",
                  p ? p->name : "?", p ? p->pid : 0, p ? p->tgid : 0, status);
    }

    // Linux sys_exit terminates only the calling thread, including the
    // leader. SchedulerExitCurrentProcess retains shared FDs/address space
    // while live peers exist; asLiveThreads prevents premature leader reap.
    Process* proc = ProcessCurrent();

    // Wake vfork parent if child exits without exec
    if (proc && proc->vforkParent)
    {
        SchedulerUnblock(proc->vforkParent);
        proc->vforkParent = nullptr;
    }

    SchedulerExitCurrentProcess(static_cast<int>(status));
    // never reached
    return 0;
}

static int64_t sys_exit_group(uint64_t status, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    DbgPrintf("sys_exit_group: '%s' tgid %u exiting with status %lu\n",
              proc ? proc->name : "?", proc ? proc->tgid : 0, status);

    // Kill all other threads in this thread group so they don't linger
    // and cause use-after-free on shared resources after the leader exits.
    if (proc)
        SchedulerKillThreadGroup(proc->tgid, proc, static_cast<int>(status));

    // Wake vfork parent if child exits without exec
    if (proc && proc->vforkParent)
    {
        SchedulerUnblock(proc->vforkParent);
        proc->vforkParent = nullptr;
    }

    SchedulerExitCurrentProcess(static_cast<int>(status));
    // never reached
    return 0;
}

// ---------------------------------------------------------------------------
// sys_readv (19)
// ---------------------------------------------------------------------------

struct iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

static int64_t sys_readv(uint64_t fd, uint64_t iovAddr, uint64_t iovcnt,
                          uint64_t, uint64_t, uint64_t)
{
    if (iovcnt > 1024) return -EINVAL;
    if (!UserBufferReadable(iovAddr, iovcnt * sizeof(iovec))) return -EFAULT;
    const auto* iov = reinterpret_cast<const iovec*>(iovAddr);
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; ++i)
    {
        int64_t ret = sys_read(fd, iov[i].iov_base, iov[i].iov_len, 0, 0, 0);
        if (ret < 0) return (total > 0) ? total : ret;
        total += ret;
        if (static_cast<uint64_t>(ret) < iov[i].iov_len) break; // short read
    }
    return total;
}

// ---------------------------------------------------------------------------
// sys_writev (20)
// ---------------------------------------------------------------------------

static int64_t sys_writev(uint64_t fd, uint64_t iovAddr, uint64_t iovcnt,
                           uint64_t, uint64_t, uint64_t)
{
    if (iovcnt > 1024) return -EINVAL;
    if (!UserBufferReadable(iovAddr, iovcnt * sizeof(iovec))) return -EFAULT;
    const auto* iov = reinterpret_cast<const iovec*>(iovAddr);
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref) return -EBADF;
    auto* entry = ref.get();
    if (entry->type == FdType::Vnode && entry->handle && (entry->statusFlags & 0x400)) {
        if ((entry->statusFlags & 3) == 0) return -EBADF;
        // Snapshot descriptors before validation: users may change the array.
        auto* vectors = static_cast<iovec*>(kmalloc(iovcnt ? iovcnt * sizeof(iovec) : 1));
        if (!vectors) return -ENOMEM;
        memcpy(vectors, iov, iovcnt * sizeof(iovec));
        uint64_t bytes = 0;
        for (uint64_t i = 0; i < iovcnt; ++i) {
            if (vectors[i].iov_len > 0x7fffffffffffffffULL - bytes) {
                kfree(vectors);return -EINVAL;
            }
            bytes += vectors[i].iov_len;
        }
        uint64_t capacity = bytes < AppendStageLimit ? bytes : AppendStageLimit;
        auto* staged = static_cast<uint8_t*>(kmalloc(capacity ? capacity : 1));
        if (!staged) { kfree(vectors);return -ENOMEM; }
        uint64_t copied = 0;
        for (uint64_t i = 0; i < iovcnt && copied < capacity; ++i) {
            uint64_t length = vectors[i].iov_len;
            if (length > capacity - copied) length = capacity - copied;
            if (length && !UserBufferReadable(vectors[i].iov_base, length)) {
                kfree(staged);kfree(vectors);return -EFAULT;
            }
            if (length) memcpy(staged + copied, reinterpret_cast<const void*>(vectors[i].iov_base), length);
            copied += length;
        }
        kfree(vectors);
        auto* vn = static_cast<Vnode*>(entry->handle);
        int64_t result = VfsAppend(vn, staged, capacity, &entry->seekPos);
        kfree(staged);
        if (result > 0 && (entry->statusFlags & 0x1000)) {
            int error = VfsFsync(vn);if (error) return error;
        }
        return result;
    }
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; ++i)
    {
        int64_t ret = sys_write(fd, iov[i].iov_base, iov[i].iov_len, 0, 0, 0);
        if (ret < 0) return (total > 0) ? total : ret;
        total += ret;
        if (static_cast<uint64_t>(ret) < iov[i].iov_len) break;
    }
    return total;
}

// SysV semaphores: user memory is staged before entering the IPC service.
static int64_t sys_semget(uint64_t key,uint64_t count,uint64_t flags,uint64_t,uint64_t,uint64_t)
{
    return SysvSemGet(ProcessCurrent(),int32_t(key),int(count),int(flags));
}
static int64_t SemUserOperation(uint64_t id,uint64_t address,uint64_t count,uint64_t timeoutAddress)
{
    if(!count)return -EINVAL;
    if(count>500)return -E2BIG;
    uint64_t deadline=~uint64_t{0};
    if(timeoutAddress) {
        PosixTimespec timeout{};uint64_t duration=0;
        if(!CopyFromUser(&timeout,timeoutAddress,sizeof(timeout)))return -EFAULT;
        if(!TimeoutNanoseconds(timeout.sec,timeout.nsec,&duration))return -EINVAL;
        deadline=TimeoutAddSafe(ClockNowNs(1),duration);
    }
    KernelOperationGuard guard(ProcessCurrent());
    auto* ops=static_cast<SysvSemOp*>(kmalloc(count*sizeof(SysvSemOp)));
    if(!ops)return -ENOMEM;
    int result=CopyFromUser(ops,address,count*sizeof(SysvSemOp))?
        SysvSemOperate(ProcessCurrent(),int(id),ops,unsigned(count),deadline):-EFAULT;
    kfree(ops);return result;
}
static int64_t sys_semop(uint64_t id,uint64_t address,uint64_t count,uint64_t,uint64_t,uint64_t)
{ return SemUserOperation(id,address,count,0); }
static int64_t sys_semtimedop(uint64_t id,uint64_t address,uint64_t count,uint64_t timeout,uint64_t,uint64_t)
{ return SemUserOperation(id,address,count,timeout); }
static int64_t sys_semctl(uint64_t id,uint64_t number,uint64_t command,uint64_t arg,uint64_t,uint64_t)
{
    KernelOperationGuard guard(ProcessCurrent());
    const int cmd=int(command)&~0x100;
    SysvSemDs ds{};uint16_t* values=nullptr;int count=0;
    if(cmd==1) {
        if(!CopyFromUser(&ds,arg,sizeof(ds)))return -EFAULT;
        ds.permission.uid=ProcessResolveId(ProcessCurrent(),false,ds.permission.uid);
        ds.permission.gid=ProcessResolveId(ProcessCurrent(),true,ds.permission.gid);
        if(ds.permission.uid==UserIdInvalid || ds.permission.gid==UserIdInvalid)return -EINVAL;
    }
    if(cmd==13 || cmd==17) {
        count=SysvSemCount(ProcessCurrent(),int(id));if(count<0)return count;
        values=static_cast<uint16_t*>(kmalloc(unsigned(count)*sizeof(uint16_t)));
        if(!values)return -ENOMEM;
        if(cmd==17 && !CopyFromUser(values,arg,unsigned(count)*sizeof(uint16_t))) {
            kfree(values);return -EFAULT;
        }
    }
    int result=SysvSemControl(ProcessCurrent(),int(id),int(number),int(command),int(arg),&ds,values,unsigned(count));
    if(result>=0 && cmd==2) {
        ds.permission.uid=ProcessReportId(ProcessCurrent(),false,ds.permission.uid);
        ds.permission.cuid=ProcessReportId(ProcessCurrent(),false,ds.permission.cuid);
        ds.permission.gid=ProcessReportId(ProcessCurrent(),true,ds.permission.gid);
        ds.permission.cgid=ProcessReportId(ProcessCurrent(),true,ds.permission.cgid);
    }
    if(result>=0 && ((cmd==2 && !CopyToUser(arg,&ds,sizeof(ds))) ||
        (cmd==13 && !CopyToUser(arg,values,unsigned(count)*sizeof(uint16_t)))))result=-EFAULT;
    kfree(values);return result;
}

// ---------------------------------------------------------------------------
// sys_uname (63)
// ---------------------------------------------------------------------------

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static void StrCopyN(char* dst, const char* src, uint64_t n)
{
    uint64_t i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; ++i; }
    while (i < n) dst[i++] = 0;
}

static int64_t sys_uname(uint64_t bufAddr, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    utsname value{};auto* buf=&value;
    StrCopyN(buf->sysname,    "Brook",     65);
    StrCopyN(buf->nodename,   "brook",     65);
    StrCopyN(buf->release,    "6.0.0",     65);
    StrCopyN(buf->version,    "#1",        65);
    StrCopyN(buf->machine,    "x86_64",    65);
    StrCopyN(buf->domainname, "(none)",    65);
    return CopyToUser(bufAddr,&value,sizeof(value))?0:-EFAULT;
}

// ---------------------------------------------------------------------------
// sys_getpid (39)
// ---------------------------------------------------------------------------

static int64_t sys_getpid(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return 1;
    // For threads, getpid returns the thread group leader's PID (tgid)
    return proc->tgid ? proc->tgid : proc->pid;
}

// ---------------------------------------------------------------------------
// sys_getppid (110)
// ---------------------------------------------------------------------------

static int64_t sys_getppid(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? proc->parentPid : 0;
}

// ---------------------------------------------------------------------------
// sys_fork (57) / sys_clone (56) / sys_vfork (58)
// ---------------------------------------------------------------------------
// The fork syscall needs access to the user-mode return address (RCX) and
// RFLAGS (R11) that were saved on the kernel stack by BrookSyscallDispatcher.
// These are at known offsets from the current kernel stack frame.

static int64_t sys_fork(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    Process* parent = ProcessCurrent();
    if (!parent) return -ENOSYS;

    Process* child = ProcessFork(parent, parent->syscallUserRip,
                                 parent->syscallUserRsp,
                                 parent->syscallUserRflags);
    if (!child) return -ENOMEM;

    // Write captured register values into the child process struct.
    child->forkRbx = parent->syscallUserRbx;
    child->forkRbp = parent->syscallUserRbp;
    child->forkR12 = parent->syscallUserR12;
    child->forkR13 = parent->syscallUserR13;
    child->forkR14 = parent->syscallUserR14;
    child->forkR15 = parent->syscallUserR15;
    child->forkRdi = parent->syscallUserRdi;
    child->forkRsi = parent->syscallUserRsi;
    child->forkRdx = parent->syscallUserRdx;
    child->forkR8  = parent->syscallUserR8;
    child->forkR9  = parent->syscallUserR9;
    child->forkR10 = parent->syscallUserR10;

    SchedulerAddProcess(child);

    DbgPrintf("FORK: parent pid=%u tgid=%u -> child pid=%u tgid=%u\n",
              parent->pid, parent->tgid, child->pid, child->tgid);
    return static_cast<int64_t>(child->pid);
}

static int64_t sys_clone(uint64_t flags, uint64_t newStack, uint64_t parentTidAddr,
                          uint64_t childTidAddr, uint64_t tlsAddr, uint64_t)
{
    // Clone flags used by musl pthread_create:
    //   CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
    //   CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID
    static constexpr uint64_t CLONE_FS             = 0x00000200;
    static constexpr uint64_t CLONE_THREAD         = 0x00010000;
    static constexpr uint64_t CLONE_SETTLS          = 0x00080000;
    static constexpr uint64_t CLONE_PARENT_SETTID   = 0x00100000;
    static constexpr uint64_t CLONE_CHILD_CLEARTID  = 0x00200000;
    static constexpr uint64_t CLONE_VFORK           = 0x00004000;
    static constexpr uint64_t CLONE_CLEAR_SIGHAND   = 0x100000000ULL;
    static constexpr uint64_t CLONE_NEWNS           = 0x00020000ULL;

    // Other isolation domains must not fall through to an ordinary fork.
    constexpr uint64_t unsupportedNamespaces=0x02000000ULL|0x04000000ULL|
                                           0x08000000ULL|0x20000000ULL|0x40000000ULL;
    constexpr uint64_t CLONE_NEWUSER=0x10000000ULL;
    if(flags&unsupportedNamespaces)return -EINVAL;
    Process* parent = ProcessCurrent();
    if (!parent) return -ENOSYS;
    if(flags&(CLONE_NEWNS|CLONE_NEWUSER)) {
        // Bounded fork-style namespace creation. Do not silently ignore shared
        // VM/FS/thread, tid, pidfd, TLS, vfork or unknown requested semantics.
        if((flags&~(CLONE_NEWNS|CLONE_NEWUSER|uint64_t{0xff})) || (flags&0xff)!=17)return -EINVAL;
        if(!(flags&CLONE_NEWUSER)) {
            auto* owner=ProcessUserNamespaceAcquire(parent);
            bool allowed=ProcessCapable(parent,owner,CapSysAdmin);UserNamespaceRelease(owner);
            if(!allowed)return -EPERM;
        }
    }

    if ((flags & CLONE_THREAD) && (flags & CLONE_PARENT_SETTID) &&
        !UserBufferWritable(parentTidAddr, sizeof(int32_t))) return -EFAULT;

    uint64_t userRip = parent->syscallUserRip;
    uint64_t userRsp = parent->syscallUserRsp;
    uint64_t userRflags = parent->syscallUserRflags;

    // If caller provided a new stack, use it for the child.
    if (newStack)
        userRsp = newStack;

    Process* child;

    if (flags & CLONE_THREAD)
    {
        // Thread creation: share address space
        uint64_t tls = (flags & CLONE_SETTLS) ? tlsAddr : parent->fsBase;
        child = ProcessCreateThread(parent, userRip, userRsp, userRflags, tls,(flags & CLONE_FS)!=0);
        if (!child) return -ENOMEM;

        // CLONE_CHILD_CLEARTID: store address for thread exit cleanup
        if (flags & CLONE_CHILD_CLEARTID)
            child->clearChildTid = childTidAddr;
    }
    else
    {
        // Fork: copy address space
        child = ProcessFork(parent, userRip, userRsp, userRflags,(flags & CLONE_FS)!=0);
        if (!child) return -ENOMEM;
    }

    if(flags&CLONE_NEWUSER) {
        if(int error=ProcessUserNamespaceCreate(child)) {ProcessDiscardUnpublished(child);return error;}
    }
    if(flags&CLONE_NEWNS) {
        if(int error=ProcessFsIsolateMounts(child)) {ProcessDiscardUnpublished(child);return error;}
    }
    if (int error=SysvSemClone(parent,child,(flags & 0x00040000)!=0)) {
        SysvSemExit(child);
        ProcessDiscardUnpublished(child);
        return error;
    }
    if ((flags & CLONE_THREAD) && (flags & CLONE_PARENT_SETTID))
        *reinterpret_cast<volatile int32_t*>(parentTidAddr)=static_cast<int32_t>(child->pid);

    child->forkRbx = parent->syscallUserRbx;  child->forkRbp = parent->syscallUserRbp;
    child->forkR12 = parent->syscallUserR12;  child->forkR13 = parent->syscallUserR13;
    child->forkR14 = parent->syscallUserR14;  child->forkR15 = parent->syscallUserR15;
    child->forkRdi = parent->syscallUserRdi;  child->forkRsi = parent->syscallUserRsi;
    child->forkRdx = parent->syscallUserRdx;  child->forkR8  = parent->syscallUserR8;
    child->forkR9  = parent->syscallUserR9;   child->forkR10 = parent->syscallUserR10;

    // CLONE_CLEAR_SIGHAND: reset all signal handlers to SIG_DFL in the child.
    // Used by Ladybird's WebContent spawn so inherited handlers don't fire
    // before exec replaces the address space.
    if ((flags & CLONE_CLEAR_SIGHAND) && !(flags & CLONE_THREAD))
    {
        uint32_t childPid = child->pid;
        if (childPid < MAX_PROCESSES)ResetSignalActions(childPid);
    }

    SchedulerAddProcess(child);

    {
        if (!(flags & CLONE_THREAD))
            SerialPrintf("[PROFILE] fork t=%lums parent_pid=%u child_pid=%u\n",
                         ApicTickCount(), parent->pid, child->pid);
    }

    DbgPrintf("CLONE: parent '%s' pid=%u tgid=%u -> child pid=%u tgid=%u flags=0x%lx %s\n",
              parent->name, parent->pid, parent->tgid, child->pid, child->tgid, flags,
              (flags & CLONE_THREAD) ? "THREAD" : "FORK");

    // CLONE_VFORK: parent blocks until child calls exec or exit.
    if ((flags & CLONE_VFORK) && !(flags & CLONE_THREAD))
    {
        child->vforkParent = parent;
        SchedulerBlock(parent);
        // Parent resumes here after child exec/exit wakes us
    }

    return static_cast<int64_t>(child->pid);
}

static int64_t sys_vfork(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    // vfork: fork + parent blocks until child exec/exit.
    // Implemented via CLONE_VFORK in sys_clone.
    static constexpr uint64_t CLONE_VFORK = 0x00004000;
    return sys_clone(CLONE_VFORK, 0, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_clone3 (435)
// ---------------------------------------------------------------------------
// Extended clone with struct clone_args. We extract fields and delegate
// to sys_clone so all thread/fork logic stays in one place.
// glibc and musl try clone3 first and fall back to clone on ENOSYS;
// implementing it avoids that fallback and silences the UNIMPL noise.

struct CloneArgs {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

static int64_t sys_clone3(uint64_t argsPtr, uint64_t size, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    if (!argsPtr || size < offsetof(CloneArgs, tls) + sizeof(uint64_t))
        return -EINVAL;
    if (!UserBufferReadable(argsPtr, size))
        return -EFAULT;

    CloneArgs copied{};
    __builtin_memcpy(&copied,reinterpret_cast<const void*>(argsPtr),size<sizeof(copied)?size:sizeof(copied));
    const CloneArgs* ca = &copied;
    const uint64_t cloneFlags=ca->flags;
    // CLONE_NEWTIME occupies an exit-signal bit in legacy clone, but it is an
    // independent unsupported namespace request in clone3.
    if(cloneFlags&0x80)return -EINVAL;

    // Stack: clone3 passes base+size; clone expects the top (stack grows down)
    uint64_t stackTop = ca->stack;
    if (stackTop && ca->stack_size)
        stackTop = ca->stack + ca->stack_size;

    // Delegate to sys_clone with the same argument layout:
    //   flags, newStack, parentTidAddr, childTidAddr, tlsAddr
    if(cloneFlags&(0x00020000|0x10000000)) {
        for(uint64_t i=sizeof(CloneArgs);i<size;++i)
            if(reinterpret_cast<const uint8_t*>(argsPtr)[i])return -E2BIG;
        if((cloneFlags&0xff) || ca->exit_signal!=17 || ca->pidfd || ca->set_tid || ca->set_tid_size || ca->cgroup)return -EINVAL;
        if(ca->stack && ca->stack_size>~uint64_t{0}-ca->stack)return -EINVAL;
        return sys_clone(cloneFlags|ca->exit_signal,stackTop,ca->parent_tid,ca->child_tid,ca->tls,0);
    }
    return sys_clone(cloneFlags, stackTop, ca->parent_tid, ca->child_tid, ca->tls, 0);
}

// ---------------------------------------------------------------------------
// sys_wait4 (61)
// ---------------------------------------------------------------------------
// Wait for a child process to terminate. Supports pid=-1 (any child)
// or a specific child PID. WNOHANG (options & 1) returns 0 if no child
// has exited yet instead of blocking.

static constexpr uint32_t WNOHANG = 1;
static constexpr uint32_t WUNTRACED = 2;
static constexpr uint32_t WEXITED = 4;
static constexpr uint32_t WCONTINUED = 8;
static constexpr uint32_t WNOWAIT = 0x01000000;

struct WaitSiginfo {
    int32_t signo, error, code, reserved;
    int32_t pid;
    uint32_t uid;
    int32_t status, reserved2;
    int64_t userTime, systemTime;
    uint8_t padding[80];
};
static_assert(sizeof(WaitSiginfo) == 128, "Linux siginfo size");
static_assert(__builtin_offsetof(WaitSiginfo, pid) == 16, "Linux SIGCHLD pid");
static_assert(__builtin_offsetof(WaitSiginfo, status) == 24, "Linux SIGCHLD status");

static void CopyWaitUsage(uint64_t address, const ChildWaitSnapshot& event)
{
    if (!address) return;
    int64_t usage[18]{}; // Linux x86-64 rusage (144 bytes), tracked CPU ticks.
    usage[0] = event.userTicks / 1000;
    usage[1] = (event.userTicks % 1000) * 1000;
    usage[2] = event.sysTicks / 1000;
    usage[3] = (event.sysTicks % 1000) * 1000;
    memcpy(reinterpret_cast<void*>(address), usage, sizeof(usage));
}

static bool ChildWaitInterrupted(Process* caller)
{
    if (__atomic_load_n(&caller->terminalPending, __ATOMIC_ACQUIRE)) return true;
    uint64_t pending = (ProcessPendingSignals(caller) |
                        PosixTimerPending(caller)) & ~caller->sigMask;
    constexpr uint64_t childBit = 1ULL << (17 - 1);
    if (pending & ~childBit) return true;
    if (pending & childBit) {
        // Default/ignored SIGCHLD is a wait wakeup. A caught SIGCHLD must reach
        // its handler through normal syscall signal delivery instead of vanishing.
        if (ReadSignalAction(caller->tgid,16).handler > 1) return true;
        ProcessDequeueSignal(caller, 17, nullptr);
    }
    return false;
}

static int WaitChildEvent(Process* caller, int selector, uint32_t id,
                          uint32_t options, ChildWaitSnapshot* event)
{
    for (;;) {
        int result = SchedulerWaitChild(caller, selector, id, options, event);
        if (result != 0 || (options & WNOHANG)) return result;
        if (ChildWaitInterrupted(caller)) return -EINTR;
        // Exit publication sets pendingWakeup on every parent-group waiter;
        // SchedulerBlock consumes it even if notification preceded the block.
        caller->wakeupTick = ApicTickCount() + 1000;
        SchedulerBlock(caller);
        if (ChildWaitInterrupted(caller)) return -EINTR;
    }
}

static int64_t sys_wait4(uint64_t pidArg, uint64_t statusAddr, uint64_t rawOptions,
                         uint64_t usageAddr, uint64_t, uint64_t)
{
    Process* parent = ProcessCurrent();
    if (!parent) return -ESRCH;
    uint32_t options = static_cast<uint32_t>(rawOptions);
    if (options & ~(WNOHANG | WUNTRACED | WCONTINUED)) return -EINVAL;
    if (statusAddr && !UserBufferWritable(statusAddr, sizeof(int32_t))) return -EFAULT;
    if (usageAddr && !UserBufferWritable(usageAddr, 144)) return -EFAULT;
    int32_t pid = static_cast<int32_t>(pidArg);
    if (pid == static_cast<int32_t>(0x80000000U)) return -ESRCH;
    int selector = pid == -1 ? 0 : pid > 0 ? 1 : 2;
    uint32_t id = pid == 0 ? parent->pgid : pid < -1 ? static_cast<uint32_t>(-pid) : static_cast<uint32_t>(pid);
    ChildWaitSnapshot event{};
    int result = WaitChildEvent(parent, selector, id, options | WEXITED, &event);
    if (result <= 0) return result;
    if (statusAddr) *reinterpret_cast<int32_t*>(statusAddr) = event.status;
    CopyWaitUsage(usageAddr, event);
    return event.pid;
}

static int64_t sys_waitid(uint64_t rawSelector, uint64_t rawId, uint64_t infoAddr,
                          uint64_t rawOptions, uint64_t usageAddr, uint64_t)
{
    Process* parent = ProcessCurrent();
    if (!parent) return -ESRCH;
    uint32_t selector = static_cast<uint32_t>(rawSelector);
    uint32_t id = static_cast<uint32_t>(rawId);
    uint32_t options = static_cast<uint32_t>(rawOptions);
    if (selector > 2 || (selector == 1 && static_cast<int32_t>(id) <= 0) ||
        (selector == 2 && static_cast<int32_t>(id) < 0) ||
        (options & ~(WNOHANG | WUNTRACED | WEXITED | WCONTINUED | WNOWAIT)) ||
        !(options & (WUNTRACED | WEXITED | WCONTINUED))) return -EINVAL;
    if (infoAddr && !UserBufferWritable(infoAddr, sizeof(WaitSiginfo))) return -EFAULT;
    if (usageAddr && !UserBufferWritable(usageAddr, 144)) return -EFAULT;
    if (selector == 2 && !id) id = parent->pgid;
    ChildWaitSnapshot event{};
    int result = WaitChildEvent(parent, static_cast<int>(selector), id, options, &event);
    if (result < 0) return result;
    WaitSiginfo info{};
    if (result) {
        info.signo = 17;
        info.code = event.code;
        info.pid = event.pid;
        info.uid = ProcessReportId(parent,false,event.uid);
        info.status = event.code == 1 ? (event.status >> 8) & 0xff :
                      event.code == 5 ? (event.status >> 8) & 0xff :
                      event.code == 6 ? 18 : event.status & 0x7f;
        info.userTime = event.userTicks / 10; // Linux USER_HZ = 100.
        info.systemTime = event.sysTicks / 10;
        CopyWaitUsage(usageAddr, event);
    }
    if (infoAddr) memcpy(reinterpret_cast<void*>(infoAddr), &info, sizeof(info));
    return 0;
}

// ---------------------------------------------------------------------------
// sys_execve (59)
// ---------------------------------------------------------------------------
// Replace the current process image with a new ELF binary.
// rdi = pathname, rsi = argv[], rdx = envp[]
// This function does NOT return on success — it enters the new program.

// Copy a NULL-terminated user vector without truncation. Validate pointer
// slots and each source page before dereferencing; do not scan unchecked user
// strings or require memory after their terminating NUL to be mapped.
static int64_t CopyExecVector(uint64_t vectorAddr, const char** values,
                              int maxCount, char* storage, uint32_t capacity,
                              int& count, uint32_t& used)
{
    count = 0;
    used = 0;
    if (!vectorAddr) return 0;
    for (;;) {
        uint64_t slot;
        if (__builtin_add_overflow(vectorAddr, static_cast<uint64_t>(count) * 8, &slot) ||
            !UserBufferReadable(slot, sizeof(uint64_t))) return -EFAULT;
        uint64_t source = *reinterpret_cast<const uint64_t*>(slot);
        if (!source) return 0;
        if (count == maxCount) return -E2BIG;
        uint32_t start = used;
        bool terminated = false;
        while (!terminated) {
            if (used == capacity) return -E2BIG;
            uint32_t chunk = 4096 - (source & 4095);
            if (chunk > capacity - used) chunk = capacity - used;
            if (!UserBufferReadable(source, chunk)) return -EFAULT;
            const char* bytes = reinterpret_cast<const char*>(source);
            for (uint32_t i = 0; i < chunk; ++i) {
                char c = bytes[i];
                storage[used++] = c;
                if (!c) { terminated = true; break; }
            }
            source += chunk;
        }
        values[count++] = storage + start;
    }
}

static int64_t sys_execve(uint64_t pathAddr, uint64_t argvAddr, uint64_t envpAddr,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    const char* userPath = reinterpret_cast<const char*>(pathAddr);
    if (!userPath) return -EFAULT;

    // --- Copy filename from user memory into kernel buffer ---
    char kPath[256];
    if (!CopyUserCString(pathAddr, kPath, sizeof(kPath))) return -EFAULT;

    DbgPrintf("sys_execve: pid=%u path='%s'\n", proc->pid, kPath);
    if (!g_hotLogQuiet)
    {
        SerialPrintf("[PROFILE] execve t=%lums pid=%u '%s'\n",
                     ApicTickCount(), proc->pid, kPath);
    }

    // Resolve path: try as-is, then /boot/BIN/<UPPER>, then /boot/<UPPER>
    int shebangDepth = 0;
    static constexpr int MAX_SHEBANG_DEPTH = 4;

    // argv/envp storage and counters. These are declared BEFORE the
    // resolve_path label so that a shebang re-entry (goto resolve_path) does
    // NOT re-run their initializers — the shebang handler rebuilds kArgv/argc
    // in place ([interp, interpArg?, scriptPath, orig argv[1:]]) and that
    // rebuild must survive the goto. Re-reading user argv on re-entry would
    // discard the rebuild and feed the interpreter the original script's args
    // (breaking nested shebang/makeWrapper wrappers).
    static_assert(EXEC_ARG_BYTES + EXEC_ENV_BYTES +
                  (EXEC_MAX_ARGS + EXEC_MAX_ENV) * sizeof(char*) < KERNEL_STACK_SIZE / 2,
                  "exec argument storage must leave room for nested kernel calls");
    const char* kArgv[EXEC_MAX_ARGS];
    const char* kEnvp[EXEC_MAX_ENV];
    char argBuf[EXEC_ARG_BYTES];
    char envBuf[EXEC_ENV_BYTES];
    uint32_t argBufPos = 0;
    uint32_t envBufPos = 0;
    int argc = 0;
    int envc = 0;
    int64_t copyResult = CopyExecVector(argvAddr, kArgv, EXEC_MAX_ARGS,
                                       argBuf, sizeof(argBuf), argc, argBufPos);
    if (copyResult < 0) return copyResult;
    copyResult = CopyExecVector(envpAddr, kEnvp, EXEC_MAX_ENV,
                                envBuf, sizeof(envBuf), envc, envBufPos);
    if (copyResult < 0) return copyResult;

    // Preserve the existing empty-argv convention, using the requested path.
    if (argc == 0) {
        argBufPos = strlen(kPath) + 1;
        memcpy(argBuf, kPath, argBufPos);
        kArgv[argc++] = argBuf;
    }
resolve_path:
    // Procfs exposes executable identity as a virtual path. Resolve self before
    // opening an ELF; reading procfs' path-text entry is not an executable image.
    // Keep argv unchanged, including a caller-provided argv[0]. This path-based
    // lookup does not yet retain an executable inode across unlink/rename.
    if(StrEq(kPath,"/proc/self/exe")) {
        if(!proc->exePath[0])return -ENOENT;
        const uint64_t bytes=strlen(proc->exePath)+1;
        if(bytes>sizeof(kPath))return -36;
        memcpy(kPath,proc->exePath,bytes);
    }
    AtLookup executableLookup;if(int error=executableLookup.Init(-100,kPath))return error;
    char resolvedPath[256];
    char newExePath[sizeof(Process::exePath)];
    Vnode* vn=nullptr;
    const char* lookupPath=kPath;
    int openResult=VfsOpenExecutableAt(executableLookup.fs.root,executableLookup.start(),kPath,
                                       &vn,newExePath,sizeof(newExePath));
    if(openResult && openResult!=-ENOENT)return openResult;
    bool found=vn!=nullptr;
    char aliasName[512];
    bool commandAlias=!strchr(kPath,'/');
    if(!commandAlias) {
        static const char* directories[]={"/bin/","/usr/bin/","/usr/local/bin/","/sbin/","/usr/sbin/","/boot/BIN/","/boot/bin/"};
        for(const char* prefix:directories) {
            size_t length=strlen(prefix);
            if(!strncmp(kPath,prefix,length) && kPath[length] && !strchr(kPath+length,'/'))commandAlias=true;
        }
    }
    const bool allowAliases=commandAlias && !executableLookup.SyntheticName(kPath,aliasName,sizeof(aliasName));

    if (!found && allowAliases)
    {
        // Extract basename for fallback lookups
        const char* baseName = kPath;
        for (const char* p = kPath; *p; ++p)
            if (*p == '/') baseName = p + 1;

        // FAT is case-insensitive, so try the original name first, then uppercased
        const char* prefixes[] = { "/boot/BIN/", "/boot/" };
        for (int pi = 0; pi < 2 && !found; ++pi)
        {
            char tryPath[128];
            uint32_t pLen = 0;
            for (const char* s = prefixes[pi]; *s && pLen < 120; ++s)
                tryPath[pLen++] = *s;
            for (const char* s = baseName; *s && pLen < 126; ++s)
                tryPath[pLen++] = *s;
            tryPath[pLen] = '\0';

            openResult=VfsOpenExecutableAt(executableLookup.fs.root,executableLookup.fs.root,tryPath,
                                           &vn,newExePath,sizeof(newExePath));
            if(openResult && openResult!=-ENOENT)return openResult;
            if(vn)
            {
                for (uint32_t k = 0; k <= pLen; ++k) resolvedPath[k] = tryPath[k];
                lookupPath = resolvedPath;
                found = true;
            }
        }
    }

    // Busybox fallback: if the command is not found, try running busybox
    // with the command name as argv[0]. Busybox reads argv[0] to determine
    // which applet to run.
    //
    // This handles two cases:
    //   1. Simple commands (no '/') — e.g. "ls"
    //   2. Full paths under bin directories — e.g. "/boot/BIN/ls"
    //      (bash resolves these via stat's BusyboxStatFallback, then calls
    //       execve with the full path)
    bool busyboxFallback = false;
    if (!found && allowAliases)
    {
        bool useBusybox = false;
        bool simpleCommand = true;
        for (const char* p = kPath; *p; ++p)
            if (*p == '/') { simpleCommand = false; break; }

        if (simpleCommand)
        {
            useBusybox = true;
        }
        else
        {
            // Check if path is under a bin-like directory (same as BusyboxStatFallback)
            const char* binPrefixes[] = {
                "/boot/BIN/", "/boot/bin/", "/bin/", "/usr/bin/",
                "/usr/local/bin/", "/sbin/", "/usr/sbin/"
            };
            for (int i = 0; i < 7; ++i)
            {
                const char* pfx = binPrefixes[i];
                const char* a = kPath;
                const char* b = pfx;
                while (*b && *a == *b) { a++; b++; }
                if (!*b && *a)
                {
                    bool hasSlash = false;
                    for (const char* c = a; *c; ++c)
                        if (*c == '/') { hasSlash = true; break; }
                    if (!hasSlash) { useBusybox = true; break; }
                }
            }
        }

        const char* applet = kPath;
        for (const char* p = kPath; *p; ++p)
            if (*p == '/') applet = p + 1;
        if (useBusybox && BusyboxHasApplet(applet))
        {
            static const char* busyboxPaths[] = {
                "/boot/BIN/busybox", "/boot/BIN/BUSYBOX",
                "/boot/bin/busybox", "/bin/busybox"
            };
            for (int i = 0; i < 4; i++)
            {
                openResult=VfsOpenExecutableAt(executableLookup.fs.root,executableLookup.fs.root,busyboxPaths[i],
                                               &vn,newExePath,sizeof(newExePath));
                if(openResult && openResult!=-ENOENT)return openResult;
                if(vn)
                {
                    uint32_t k = 0;
                    const char* p = busyboxPaths[i];
                    while (p[k]) { resolvedPath[k] = p[k]; k++; }
                    resolvedPath[k] = '\0';
                    lookupPath = resolvedPath;
                    found = true;
                    busyboxFallback = true;
                    DbgPrintf("sys_execve: busybox fallback for '%s'\n", kPath);
                    break;
                }
            }
        }
    }

    if (!found)
    {
        DbgPrintf("sys_execve: not found: %s\n", kPath);
        SerialPrintf("sys_execve: ENOENT path='%s' pid=%u\n", kPath,
                     ProcessCurrent() ? ProcessCurrent()->pid : 0);
        return -ENOENT;
    }

    SerialPrintf("sys_execve: envpAddr=0x%lx envc=%d argc=%d path='%s'\n",
                 envpAddr, envc, argc, lookupPath);

    // Keep the original lookup spelling for script argv, but record the final
    // ELF's resolved location (including a shebang interpreter's symlinks).
    uint32_t newExeLen = strlen(newExePath);

    ExecutableImage image{};
    int readResult = ReadExecutableImage(vn, &image);
    VfsClose(vn);
    if (readResult != 0) return readResult;
    uint8_t* elfBuf = image.data;
    uint64_t elfSize = image.size;
    DbgPrintf("sys_execve: read '%s' %lu bytes (pid %u)\n",
                 lookupPath, elfSize, proc->pid);

    DbgPrintf("sys_execve: loaded '%s' (%lu bytes) for pid %u\n",
              lookupPath, elfSize, proc->pid);

    // --- Shebang (#!) support ---
    // If the file starts with "#!", extract the interpreter path and re-exec.
    // Uses goto instead of recursive sys_execve to avoid passing kernel-space
    // pointers through user-pointer validation (which returns EFAULT).
    if (elfSize >= 2 && elfBuf[0] == '#' && elfBuf[1] == '!')
    {
        if (++shebangDepth > MAX_SHEBANG_DEPTH) {
            FreeExecutableImage(&image);
            SerialPrintf("sys_execve: shebang depth exceeded for '%s'\n", lookupPath);
            return -ELOOP;
        }

        // Parse interpreter line: "#!<interp> [arg]\n"
        const char* line = reinterpret_cast<const char*>(elfBuf);
        const char* lineEnd = line + (elfSize < 256 ? elfSize : 256);
        const char* p = line + 2;
        const char* terminator = p;
        while (terminator < lineEnd && *terminator != '\n' && *terminator != '\0')
            ++terminator;
        if (terminator == lineEnd && elfSize >= 256) {
            FreeExecutableImage(&image);
            return -ENOEXEC; // Never execute a truncated interpreter line.
        }

        while (p < lineEnd && (*p == ' ' || *p == '\t')) p++;

        // Extract interpreter path into kPath (reuse existing buffer)
        char interpPath[256];
        uint32_t interpLen = 0;
        while (p < lineEnd && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\0')
        {
            if (interpLen == sizeof(interpPath) - 1) {
                FreeExecutableImage(&image);
                return -E2BIG;
            }
            interpPath[interpLen++] = *p;
            p++;
        }
        interpPath[interpLen] = '\0';

        // Skip whitespace to optional arg
        while (p < lineEnd && (*p == ' ' || *p == '\t')) p++;

        char interpArg[128];
        uint32_t interpArgLen = 0;
        while (p < lineEnd && *p != '\n' && *p != '\r' && *p != '\0')
        {
            if (interpArgLen == sizeof(interpArg) - 1) {
                FreeExecutableImage(&image);
                return -E2BIG;
            }
            interpArg[interpArgLen++] = *p;
            p++;
        }
        interpArg[interpArgLen] = '\0';

        FreeExecutableImage(&image);
        if (interpLen == 0) return -ENOEXEC;

        SerialPrintf("sys_execve: shebang interp='%s' arg='%s' script='%s'\n",
                     interpPath, interpArg, lookupPath);

        // Rebuild argv in-place: [interp, interpArg?, scriptPath, orig_argv[1:]]
        // Save the script path (lookupPath may point into resolvedPath which gets
        // overwritten on the next resolve_path iteration).
        char scriptPath[256];
        uint32_t spi = 0;
        while (lookupPath[spi] && spi < 255) { scriptPath[spi] = lookupPath[spi]; spi++; }
        scriptPath[spi] = '\0';

        // All argument strings are packed in argBuf. Move the old argv[1:]
        // tail once, then adjust its pointers from the end before replacing
        // argv[0]. No shared scratch buffer or silent loss of trailing args.
        uint32_t oldHeadBytes = strlen(kArgv[0]) + 1;
        uint32_t tailBytes = argBufPos - oldHeadBytes;
        int prefixCount = interpArgLen ? 3 : 2;
        uint32_t prefixBytes = interpLen + 1 + spi + 1 +
                               (interpArgLen ? interpArgLen + 1 : 0);
        if (argc - 1 + prefixCount > EXEC_MAX_ARGS ||
            prefixBytes + tailBytes > sizeof(argBuf)) return -E2BIG;
        memmove(argBuf + prefixBytes, argBuf + oldHeadBytes, tailBytes);
        for (int i = argc - 1; i >= 1; --i)
            kArgv[i + prefixCount - 1] = argBuf + prefixBytes +
                                          (kArgv[i] - argBuf - oldHeadBytes);
        uint32_t prefixPos = 0;
        int prefixIndex = 0;
        auto pushPrefix = [&](const char* s, uint32_t bytes) {
            kArgv[prefixIndex++] = argBuf + prefixPos;
            memcpy(argBuf + prefixPos, s, bytes);
            prefixPos += bytes;
        };
        pushPrefix(interpPath, interpLen + 1);
        if (interpArgLen) pushPrefix(interpArg, interpArgLen + 1);
        pushPrefix(scriptPath, spi + 1);
        argc += prefixCount - 1;
        argBufPos = prefixBytes + tailBytes;

        // Set kPath to the interpreter for path resolution
        memcpy(kPath, interpPath, interpLen + 1);

        goto resolve_path;
    }

    if (elfSize < 64) // Too small to be a valid ELF
    {
        FreeExecutableImage(&image);
        DbgPrintf("sys_execve: file too small (%lu bytes)\n", elfSize);
        return -ENOEXEC;
    }

    // Reject malformed/truncated headers while the old image and siblings
    // still exist. Allocation failures after commitment remain fatal.
    if (!ElfValidateMainImage(elfBuf, elfSize)) {
        FreeExecutableImage(&image);
        return -ENOEXEC;
    }

    ExecutableImage interpreterImage{};
    const int interpreterResult=PrepareExecInterpreter(image,&interpreterImage);
    if(interpreterResult) {FreeExecutableImage(&image);return interpreterResult;}
    // exec enters user mode without returning through this C++ frame. Drop
    // the lookup snapshot explicitly before all non-returning commit paths.
    executableLookup.Clear();

    // --- Kill sibling threads before replacing the process image ---
    // Linux: execve terminates all other threads in the thread group
    // before the old address space is destroyed.  Without this, sibling
    // threads may still be executing user code backed by the old page
    // table / file VMAs, causing use-after-free crashes (BRO-091).
    if (proc->tgid && proc->pid == proc->tgid)
    {
        SchedulerKillThreadGroup(proc->tgid, proc, 128 + 9, 9 /* SIGKILL */);
        // SchedulerKillThreadGroup latches tgidExiting=true on the whole group
        // (including this surviving leader) to stillbirth any racing clone.
        // But execve is a RE-IMAGE, not a group exit: this thread lives on as
        // the new program. The kill is synchronous (all siblings are reaped on
        // return) and this is now the only live thread, so clear the latch —
        // otherwise the re-imaged program's own future threads (e.g. an SDL
        // audio thread) are wrongly stillborn and it blocks forever waiting on
        // a thread that never runs. (This silently broke launcher/makeWrapper
        // style programs that exec then spawn threads.)
        __atomic_store_n(&proc->tgidExiting, false, __ATOMIC_RELEASE);
    }

    // --- Replace the process image ---
    uint64_t newStackPtr = 0;
    uint64_t newEntry = ProcessExecPreparedExecutable(proc, image, interpreterImage,
                                     argc, kArgv, envc, kEnvp,
                                     &newStackPtr);

    // Both image snapshots remain retained until loading has finished.
    FreeExecutableImage(&interpreterImage);
    FreeExecutableImage(&image);

    if (!newEntry)
    {
        SerialPrintf("sys_execve: ProcessExec failed for pid %u\n", proc->pid);
        // Process is in a broken state — kill it
        SchedulerExitCurrentProcess(-1);
        __builtin_unreachable();
    }

    // --- execve committed: clear the group-exit latch ---
    // SchedulerKillThreadGroup() above latched tgidExiting=true to refuse
    // sibling-thread births while the old thread group was being torn down
    // (BRO-173).  Now that the new image has loaded successfully, the surviving
    // leader is a fresh, single-threaded, very-much-alive process — the latch
    // MUST be cleared.  Otherwise the leader's incarnation still reads
    // tgidExiting=true, and its FIRST pthread_create (e.g. SDL2/3's audio or
    // event thread) is stillborn forever: ProcessCreateThread marks the new
    // thread Terminated instead of running it, glibc's start_thread then runs on
    // a misaligned/parent stack and #GPs inside __vsnprintf_internal (movaps on a
    // 16-byte-unaligned stack).  This is BRO-203 and is why every exec'd threaded
    // app (yquake2, SuperTuxKart, any SDL app) crashed right after window setup.
    __atomic_store_n(&proc->tgidExiting, false, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i <= newExeLen; ++i)
        proc->exePath[i] = newExePath[i];

    // Update process name — use argv[0] for busybox applets, binary name otherwise
    const char* nameSource = lookupPath;
    if (busyboxFallback && argc > 0)
        nameSource = kArgv[0];
    const char* baseName2 = nameSource;
    for (const char* p = nameSource; *p; ++p)
        if (*p == '/') baseName2 = p + 1;
    uint32_t ni = 0;
    while (baseName2[ni] && ni < 30)
    {
        proc->name[ni] = baseName2[ni];
        ni++;
    }
    proc->name[ni] = '\0';

    DbgPrintf("sys_execve: entering user mode for '%s' pid=%u entry=0x%lx sp=0x%lx\n",
              proc->name, proc->pid, newEntry, newStackPtr);

    // Validate that entry point and stack are mapped in the new address space.
    {
        PhysicalAddress entryPhys = VmmVirtToPhys(proc->addressSpace->pageTable, VirtualAddress(newEntry));
        PhysicalAddress stackPhys = VmmVirtToPhys(proc->addressSpace->pageTable, VirtualAddress(newStackPtr & ~0xFFFULL));
        if (!entryPhys || !stackPhys) {
            SerialPrintf("sys_execve: FATAL unmapped pages! entry phys=0x%lx stack phys=0x%lx\n",
                         entryPhys.raw(), stackPhys.raw());
            SchedulerExitCurrentProcess(-1);
            __builtin_unreachable();
        }
    }

    // --- Switch to new address space and enter user mode ---
    // Load the new page table
    __asm__ volatile("mov %0, %%cr3" : : "r"(proc->addressSpace->pageTable.pml4.raw()) : "memory");

    // Reset user segment state atomically with the live MSRs. Earlier exec
    // work can be preempted, so write GS=0 explicitly rather than trusting a
    // saved context which a switch may have refreshed from the old image.
    uint64_t segmentFlags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(segmentFlags) :: "memory");
    proc->savedCtx.fsBase = proc->fsBase;
    proc->savedCtx.gsBase = 0;
    WriteMsr(0xC0000100U, proc->fsBase);
    WriteMsr(MSR_KERNEL_GS_BASE, 0);
    __asm__ volatile("push %0; popfq" :: "r"(segmentFlags) : "memory", "cc");

    // Wake vfork parent — exec is complete, parent can resume
    if (proc->vforkParent)
    {
        SchedulerUnblock(proc->vforkParent);
        proc->vforkParent = nullptr;
    }

    // Enter user mode — this does NOT return
    SwitchToUserMode(newStackPtr, newEntry);
    __builtin_unreachable();
}

// ---------------------------------------------------------------------------
// sys_set_tid_address (218)
// ---------------------------------------------------------------------------

static int64_t sys_set_tid_address(uint64_t tidptr, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (proc)
        proc->clearChildTid = tidptr;
    return proc ? static_cast<int64_t>(proc->pid) : 1;
}

// ---------------------------------------------------------------------------
// sys_clock_gettime (228) / sys_clock_getres (229) / sys_gettimeofday (96)
// ---------------------------------------------------------------------------

// LAPIC fires every 1ms — monotonic tick counter.

struct timespec {
    int64_t  tv_sec;
    int64_t  tv_nsec;
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

// Coarse clocks retain their corresponding epoch. Higher precision is safe;
// returning uptime for REALTIME_COARSE makes absolute realtime waits expire.
// CLOCK_REALTIME=0, REALTIME_COARSE=5; MONOTONIC=1, RAW=4, COARSE=6, BOOTTIME=7.
static uint64_t ClockNowNs(uint64_t clockid)
{
    if (clockid == 0 || clockid == 5 || clockid == 8) return RtcNowNs();
    return KvmClockAvailable() ? KvmClockReadNs()
                               : ApicTickCount() * 1000000ULL;
}

static int64_t sys_clock_gettime(uint64_t clockid, uint64_t tsAddr, uint64_t,
                                  uint64_t, uint64_t, uint64_t)
{
    if (clockid > 9) return -EINVAL; // No TAI/dynamic CPU clock support yet.
    timespec value{};auto* ts=&value;
    uint64_t ns = 0;
    if (clockid == 2 || clockid == 3) {
        if (!SchedulerCpuTime(ProcessCurrent(), clockid == 2, &ns)) return -EINVAL;
    } else ns = ClockNowNs(clockid);
    ts->tv_sec = static_cast<int64_t>(ns / 1000000000ULL);
    ts->tv_nsec = static_cast<int64_t>(ns % 1000000000ULL);
    return CopyToUser(tsAddr,&value,sizeof(value))?0:-EFAULT;
}

static int64_t sys_clock_getres(uint64_t clockid, uint64_t tsAddr, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    if (clockid > 9) return -EINVAL;
    if (!tsAddr) return 0;
    timespec value{};auto* ts=&value;

    ts->tv_sec = 0;
    ts->tv_nsec = clockid == 2 || clockid == 3 ? 1000000 : (KvmClockAvailable() ? 1 : 1000000);
    return CopyToUser(tsAddr,&value,sizeof(value))?0:-EFAULT;
}

static int64_t sys_gettimeofday(uint64_t tvAddr, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    timeval value{};auto* tv=&value;

    const uint64_t ns = RtcNowNs();
    tv->tv_sec = static_cast<int64_t>(ns / 1000000000ULL);
    tv->tv_usec = static_cast<int64_t>((ns / 1000) % 1000000ULL);
    return CopyToUser(tvAddr,&value,sizeof(value))?0:-EFAULT;
}

// ---------------------------------------------------------------------------
// sys_time (201) — returns seconds since epoch
// ---------------------------------------------------------------------------

static int64_t sys_time(uint64_t tloc, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    uint64_t epoch = RtcNow();
    if (tloc) {
        if(!CopyToUser(tloc,&epoch,sizeof(epoch)))return -EFAULT;
    }
    return static_cast<int64_t>(epoch);
}

// ---------------------------------------------------------------------------
// sys_sched_yield (24)
// ---------------------------------------------------------------------------

static int64_t sys_sched_yield(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t)
{
    SchedulerYield();
    return 0;
}

// ---------------------------------------------------------------------------
// sys_nanosleep (35) / sys_clock_nanosleep (230)
// ---------------------------------------------------------------------------

static int64_t sys_nanosleep(uint64_t reqAddr, uint64_t remAddr, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferReadable(reqAddr, sizeof(timespec))) return -EFAULT;
    timespec req = *reinterpret_cast<const timespec*>(reqAddr);
    uint64_t duration;
    if (!TimeoutNanoseconds(req.tv_sec, req.tv_nsec, &duration)) return -EINVAL;
    const uint64_t deadline = TimeoutAddSafe(ClockNowNs(1), duration);

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    uint64_t now = ClockNowNs(1);
    while (now < deadline)
    {
        if (HasPendingSignals())
        {
            if (remAddr)
            {
                if (!UserBufferWritable(remAddr, sizeof(timespec))) return -EFAULT;
                auto* rem = reinterpret_cast<timespec*>(remAddr);
                uint64_t remaining = deadline - now;
                rem->tv_sec = static_cast<int64_t>(remaining / 1000000000ULL);
                rem->tv_nsec = static_cast<int64_t>(remaining % 1000000000ULL);
            }
            return -EINTR;
        }
        // Round positive durations UP to a scheduler tick. Recheck the
        // monotonic deadline after wakeup, since a tick can arrive early or
        // another event can unblock this process before the sleep expires.
        uint64_t remaining = deadline - now;
        uint64_t ticks = remaining / 1000000ULL + (remaining % 1000000ULL != 0);
        proc->wakeupTick = ApicTickCount() + ticks;
        SchedulerBlockInterruptible(proc);
        now = ClockNowNs(1);
    }

    if (remAddr && UserBufferWritable(remAddr, sizeof(timespec)))
    {
        auto* rem = reinterpret_cast<timespec*>(remAddr);
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

static int64_t sys_clock_nanosleep(uint64_t clockid, uint64_t flags, uint64_t reqAddr,
                                    uint64_t remAddr, uint64_t, uint64_t)
{
    if (clockid != 0 && clockid != 1 && clockid != 7) return -EINVAL;
    if (flags & ~1ULL) return -EINVAL;
    if (!UserBufferReadable(reqAddr, sizeof(timespec))) return -EFAULT;
    timespec req = *reinterpret_cast<const timespec*>(reqAddr);
    uint64_t requested;
    if (!TimeoutNanoseconds(req.tv_sec, req.tv_nsec, &requested)) return -EINVAL;
    if (flags & 1) { // TIMER_ABSTIME: convert from the requested clock domain.
        const uint64_t deadline = requested;
        uint64_t now = ClockNowNs(clockid);
        if (deadline <= now) return 0;
        Process* proc = ProcessCurrent();
        if (!proc) return -EINVAL;
        while (now < deadline) {
            if (HasPendingSignals()) return -EINTR;
            const uint64_t remaining = deadline - now;
            const uint64_t ticks = remaining / 1000000ULL + (remaining % 1000000ULL != 0);
            proc->wakeupTick = ApicTickCount() + ticks;
            SchedulerBlockInterruptible(proc);
            // LAPIC ticks and unrelated wakeups are scheduling hints, not
            // evidence that the selected clock's absolute deadline expired.
            now = ClockNowNs(clockid);
        }
        return 0;
    }
    return sys_nanosleep(reqAddr, remAddr, 0, 0, 0, 0);
}

// POSIX timer ABI wrappers. All user memory is validated/copied outside the
// timer lock; the implementation never follows user pointers while locked.
static int64_t sys_timer_create(uint64_t clock, uint64_t eventAddr, uint64_t idAddr,
                                uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(idAddr, sizeof(int32_t))) return -EFAULT;
    PosixSigevent event{};
    if (eventAddr) {
        if (!UserBufferReadable(eventAddr, sizeof(event))) return -EFAULT;
        __builtin_memcpy(&event, reinterpret_cast<void*>(eventAddr), sizeof(event));
    }
    int32_t id;
    int result = PosixTimerCreate(ProcessCurrent(), (int)clock, eventAddr ? &event : nullptr, &id);
    if (!result) __builtin_memcpy(reinterpret_cast<void*>(idAddr), &id, sizeof(id));
    return result;
}
static int64_t sys_timer_settime(uint64_t id, uint64_t flags, uint64_t valueAddr,
                                 uint64_t oldAddr, uint64_t, uint64_t)
{
    PosixItimerspec value, old;
    if (!valueAddr) return -EINVAL;
    if (!UserBufferReadable(valueAddr, sizeof(value))) return -EFAULT;
    __builtin_memcpy(&value, reinterpret_cast<void*>(valueAddr), sizeof(value));
    int result = PosixTimerSet(ProcessCurrent(), (int32_t)id, (unsigned)flags, value, oldAddr ? &old : nullptr);
    if (!result && oldAddr && !CopyToUser(oldAddr, &old, sizeof(old))) return -EFAULT;
    return result;
}
static int64_t sys_timer_gettime(uint64_t id, uint64_t valueAddr, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    PosixItimerspec value;
    if (!UserBufferWritable(valueAddr, sizeof(value))) return -EFAULT;
    int result = PosixTimerGet(ProcessCurrent(), (int32_t)id, &value);
    if (!result) __builtin_memcpy(reinterpret_cast<void*>(valueAddr), &value, sizeof(value));
    return result;
}
static int64_t sys_timer_getoverrun(uint64_t id, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t)
{
    return PosixTimerOverrun(ProcessCurrent(), (int32_t)id);
}
static int64_t sys_timer_delete(uint64_t id, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t)
{
    return PosixTimerDelete(ProcessCurrent(), (int32_t)id);
}

// ---------------------------------------------------------------------------
// sys_alarm (37) — deliver SIGALRM after seconds
// ---------------------------------------------------------------------------

static int64_t sys_alarm(uint64_t seconds, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    uint64_t prevRemaining = 0;
    if (proc->alarmTick != 0)
    {
        // Calculate remaining seconds from previous alarm
        uint64_t now = ApicTickCount();
        if (proc->alarmTick > now)
            prevRemaining = (proc->alarmTick - now + 999) / 1000; // round up
    }

    if (seconds == 0)
        proc->alarmTick = 0; // Cancel alarm
    else
        proc->alarmTick = ApicTickCount() + seconds * 1000;

    return static_cast<int64_t>(prevRemaining);
}

// ---------------------------------------------------------------------------
// sys_pause (34) — wait for a signal
// ---------------------------------------------------------------------------

static int64_t sys_pause(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    // Block until a signal is delivered
    while (!HasPendingSignals())
    {
        proc->wakeupTick = ApicTickCount() + 100; // recheck every 100ms
        SchedulerBlockInterruptible(proc);
    }

    return -EINTR; // pause always returns EINTR
}

// ---------------------------------------------------------------------------
// sys_rt_sigsuspend (130) — temporarily replace signal mask and wait
// ---------------------------------------------------------------------------

static int64_t sys_rt_sigsuspend(uint64_t maskAddr, uint64_t sigsetsize, uint64_t,
                                   uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (sigsetsize != 8) return -EINVAL;
    if (!maskAddr) return -EFAULT;
    int error = InstallWaitSignalMask(maskAddr, sigsetsize);
    if (error) return error;

    // Block until a signal that isn't blocked is pending
    while (!HasPendingSignals())
    {
        proc->wakeupTick = ApicTickCount() + 100;
        SchedulerBlockInterruptible(proc);
    }

    // Select and deliver the signal under the temporary mask. The shared
    // syscall-return path saves the original mask in the handler's ucontext;
    // restoring it here can block SIGCHLD again before its handler runs.
    return -EINTR;
}

// ---------------------------------------------------------------------------
// sys_rt_sigtimedwait (128) — wait synchronously for a selected signal
// ---------------------------------------------------------------------------

static int64_t sys_rt_sigtimedwait(uint64_t setAddr, uint64_t infoAddr,
                                   uint64_t timeoutAddr, uint64_t sigsetsize,
                                   uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (sigsetsize != 8) return -EINVAL;
    if (!UserBufferReadable(setAddr, sizeof(uint64_t))) return -EFAULT;
    // Linux dequeues first and only then copies siginfo to the user buffer.
    // A bad output pointer therefore consumes the selected signal.
    uint64_t waitSet = *reinterpret_cast<const uint64_t*>(setAddr);
    waitSet &= ~((1ULL << 8) | (1ULL << 18));
    uint64_t deadline = 0;
    if (timeoutAddr) {
        if (!UserBufferReadable(timeoutAddr, sizeof(timespec))) return -EFAULT;
        timespec ts = *reinterpret_cast<const timespec*>(timeoutAddr);
        uint64_t duration;
        if (!TimeoutNanoseconds(ts.tv_sec, ts.tv_nsec, &duration)) return -EINVAL;
        deadline = TimeoutAddSafe(ClockNowNs(1), duration);
    }
    for (;;) {
        // A fatal group exit must unwind this synchronous wait even though
        // its signal mask excludes ordinary asynchronous delivery. The syscall
        // return boundary publishes termination before any return to user code.
        if (__atomic_load_n(&proc->terminalPending, __ATOMIC_ACQUIRE))
            return -EINTR;
        uint64_t pending = ProcessPendingSignals(proc) |
                           PosixTimerPending(proc);
        uint64_t matched = pending & waitSet;
        if (matched) {
            int signum = __builtin_ctzll(matched) + 1;
            SignalInfo info{};
            if (!PosixTimerDequeue(proc, 1ULL << (signum - 1), &info)) {
                if (!ProcessDequeueSignal(proc, signum, &info)) continue;
            }
            ProcessSignalInfoToUser(proc,&info);
            if (infoAddr && !CopyToUser(infoAddr, &info, sizeof(info))) return -EFAULT;
            return signum;
        }
        if (pending & ~proc->sigMask & ~waitSet) return -EINTR;
        uint64_t now = ClockNowNs(1);
        if (timeoutAddr && now >= deadline) return -EAGAIN;
        uint64_t sleepMs = timeoutAddr ? (deadline - now) / 1000000ULL +
            ((deadline - now) % 1000000ULL != 0) : 100;
        if (sleepMs > 100) sleepMs = 100;
        proc->wakeupTick = ApicTickCount() + sleepMs;
        SchedulerBlockInterruptible(proc);
    }
}

// ---------------------------------------------------------------------------
// sys_access (21)
// ---------------------------------------------------------------------------

static bool BusyboxStatFallback(const char* path, VnodeStat* vs);

// Inquiry only: do not use access() as authorization for a later operation.
static int64_t AccessAt(int dirfd, uint64_t pathAddr, uint64_t mode, uint64_t flags)
{
    constexpr uint64_t NOFOLLOW = 0x100, EACCESS = 0x200, EMPTY_PATH = 0x1000;
    if ((mode & ~7ULL) || (flags & ~(NOFOLLOW | EACCESS | EMPTY_PATH))) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    char path[256];
    uint32_t length = 0;
    for (; length < sizeof(path); ++length) {
        if (!UserBufferReadable(pathAddr + length, 1)) return -EFAULT;
        path[length] = *reinterpret_cast<const char*>(pathAddr + length);
        if (!path[length]) break;
    }
    if (length == sizeof(path)) return -36; // ENAMETOOLONG; never truncate
    if (!length) {
        if (!(flags & EMPTY_PATH)) return -ENOENT;
        if (dirfd != -100) {
            FdRef ref(proc, dirfd);
            FdEntry* entry=ref.get();
            if (!entry) return -EBADF;
            if (entry->type==FdType::Vnode && entry->handle)
                return VfsAccess(static_cast<Vnode*>(entry->handle),static_cast<uint8_t>(mode),!(flags&EACCESS));
            return mode ? -95 : 0; // EOPNOTSUPP: non-vnode permission metadata is not implemented.
        }
        FsSnapshot fs{};int error=ProcessFsSnapshot(proc,&fs);
        if(!error)error=VfsPathAccess(fs.cwd,static_cast<uint8_t>(mode),!(flags&EACCESS));
        FsSnapshotRelease(&fs);return error;
    }
    AtLookup at;if(int error=at.Init(dirfd,path))return error;
    char resolved[512];bool hasSyntheticName=!at.SyntheticName(path,resolved,sizeof(resolved));
    const char* lookup=hasSyntheticName?resolved:"";
    VnodeStat st{};
    bool realIds=!(flags&EACCESS);
    int result=VfsAccessAt(at.fs.root,at.start(),path,static_cast<uint8_t>(mode),realIds,!(flags&NOFOLLOW));
    if (result == 0) return 0;
    if (result != -ENOENT && result != -1) return result;
    static const char* syntheticPaths[] = {
        "/etc/passwd", "/etc/group", "/etc/shells", "/etc/hostname",
        "/etc/nsswitch.conf", "/etc/hosts", "/etc/resolv.conf",
        "/proc/version", nullptr
    };
    for (auto** sp = syntheticPaths; *sp; ++sp)
        if (StrEq(lookup, *sp))
            return ProcessCheckFileAccess(proc,0,0,0100444,static_cast<uint8_t>(mode),realIds);
    if (!BusyboxStatFallback(lookup,&st)) return -ENOENT;
    return ProcessCheckFileAccess(proc,st.uid,st.gid,st.mode ? st.mode : 0100755,
                                  static_cast<uint8_t>(mode),realIds);
}

static int64_t sys_access(uint64_t pathAddr, uint64_t mode, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    return AccessAt(-100, pathAddr, mode, 0);
}

// ---------------------------------------------------------------------------
// sys_ioctl (16) -- framebuffer and keyboard ioctls
// ---------------------------------------------------------------------------

// Linux framebuffer ioctl commands
static constexpr uint64_t FBIOGET_VSCREENINFO = 0x4600;
static constexpr uint64_t FBIOPUT_VSCREENINFO = 0x4601;
static constexpr uint64_t FBIOGET_FSCREENINFO = 0x4602;

// Linux fb_var_screeninfo (simplified — only fields DOOM uses)
struct FbVarScreeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    // red/green/blue/transp bitfields (4 x 3 uint32_t each = 48 bytes)
    uint32_t red_offset, red_length, red_msb_right;
    uint32_t green_offset, green_length, green_msb_right;
    uint32_t blue_offset, blue_length, blue_msb_right;
    uint32_t transp_offset, transp_length, transp_msb_right;
    // rest
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;    // mm
    uint32_t width;     // mm
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

// Linux fb_fix_screeninfo (simplified)
struct FbFixScreeninfo {
    char     id[16];
    uint64_t smem_start;     // physical start of framebuffer memory
    uint32_t smem_len;       // length of framebuffer memory
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;    // bytes per scanline
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

// ---------------------------------------------------------------------------
// virtio-gpu DRM render node (/dev/dri/renderD128) — GL shim, Milestone 0.
//
// Maps the Linux virtio-gpu DRM uABI onto Brook's native per-process GPU path
// (GpuAppOps) so unmodified Mesa's virgl driver can drive it. M0 implements the
// device-identification / capability ioctls Mesa issues during initialization
// (VERSION, GET_CAP, VIRTGPU_GETPARAM, VIRTGPU_GET_CAPS) — verified against a
// real virtio-gpu node via a host Mesa strace (see
// artifacts/gl-drm-enumeration-spike.md). M1 adds the resource/submit ioctls
// (CONTEXT_INIT, RESOURCE_CREATE, RESOURCE_INFO, TRANSFER_*, EXECBUFFER, WAIT,
// GEM_CLOSE) over GpuAppOps, with a per-fd GEM-handle->virgl-resid table.
//
// ioctl command words are _IOWR('d', nr, struct): the low byte is the request
// number (nr), the next byte is the type ('d' = 0x64); size/dir occupy the high
// bits and are ignored here (Linux drivers dispatch on type+nr).
// ---------------------------------------------------------------------------
namespace {

constexpr uint8_t  DRM_IOCTL_TYPE        = 'd';   // 0x64
constexpr uint8_t  DRM_NR_VERSION        = 0x00;  // DRM_IOCTL_VERSION
constexpr uint8_t  DRM_NR_GEM_CLOSE      = 0x09;  // DRM_IOCTL_GEM_CLOSE
constexpr uint8_t  DRM_NR_PRIME_HANDLE_TO_FD = 0x2d;  // DRM_IOCTL_PRIME_HANDLE_TO_FD
constexpr uint8_t  DRM_NR_PRIME_FD_TO_HANDLE = 0x2e;  // DRM_IOCTL_PRIME_FD_TO_HANDLE
constexpr uint8_t  DRM_NR_GET_CAP        = 0x0c;  // DRM_IOCTL_GET_CAP
constexpr uint8_t  DRM_COMMAND_BASE      = 0x40;  // driver-private ioctl base
constexpr uint8_t  DRM_NR_VIRTGPU_MAP            = DRM_COMMAND_BASE + 0x01; // 0x41
constexpr uint8_t  DRM_NR_VIRTGPU_EXECBUFFER     = DRM_COMMAND_BASE + 0x02; // 0x42
constexpr uint8_t  DRM_NR_VIRTGPU_GETPARAM       = DRM_COMMAND_BASE + 0x03; // 0x43
constexpr uint8_t  DRM_NR_VIRTGPU_RESOURCE_CREATE= DRM_COMMAND_BASE + 0x04; // 0x44
constexpr uint8_t  DRM_NR_VIRTGPU_RESOURCE_INFO  = DRM_COMMAND_BASE + 0x05; // 0x45
constexpr uint8_t  DRM_NR_VIRTGPU_TRANSFER_FROM_HOST = DRM_COMMAND_BASE + 0x06; // 0x46
constexpr uint8_t  DRM_NR_VIRTGPU_TRANSFER_TO_HOST   = DRM_COMMAND_BASE + 0x07; // 0x47
constexpr uint8_t  DRM_NR_VIRTGPU_WAIT           = DRM_COMMAND_BASE + 0x08; // 0x48
constexpr uint8_t  DRM_NR_VIRTGPU_GET_CAPS       = DRM_COMMAND_BASE + 0x09; // 0x49
constexpr uint8_t  DRM_NR_VIRTGPU_RESOURCE_CREATE_BLOB = DRM_COMMAND_BASE + 0x0a;
constexpr uint8_t  DRM_NR_VIRTGPU_CONTEXT_INIT   = DRM_COMMAND_BASE + 0x0b; // 0x4b

// virtio-gpu DRM driver identity (matches the upstream kernel driver).
constexpr int32_t  VIRTGPU_DRIVER_MAJOR  = 0;
constexpr int32_t  VIRTGPU_DRIVER_MINOR  = 1;
constexpr int32_t  VIRTGPU_DRIVER_PATCH  = 0;

// struct drm_version (64-bit layout; size_t/ptr are 8 bytes, with 4 bytes of
// padding after the three int fields to align name_len).
struct DrmVersion {
    int32_t  version_major;
    int32_t  version_minor;
    int32_t  version_patchlevel;
    int32_t  _pad;
    uint64_t name_len;
    uint64_t name;          // char*
    uint64_t date_len;
    uint64_t date;          // char*
    uint64_t desc_len;
    uint64_t desc;          // char*
};

struct DrmGetCap   { uint64_t capability; uint64_t value; };

// DRM_CAP_PRIME (capability id 0x5) bits: import/export support. Mesa queries
// this during init to decide whether dmabuf sharing is available.
constexpr uint64_t DRM_CAP_PRIME            = 0x5;
constexpr uint64_t DRM_PRIME_CAP_IMPORT     = 0x1;
constexpr uint64_t DRM_PRIME_CAP_EXPORT     = 0x2;

// struct drm_prime_handle — the argument to PRIME_HANDLE_TO_FD / FD_TO_HANDLE.
struct DrmPrimeHandle {
    uint32_t handle;   // GEM bo_handle (in for export, out for import)
    uint32_t flags;    // DRM_CLOEXEC | DRM_RDWR (export only)
    int32_t  fd;       // dmabuf fd (out for export, in for import)
};
struct DrmVirtgpuGetparam { uint64_t param; uint64_t value; }; // value = user u64*
struct DrmVirtgpuGetCaps {
    uint32_t cap_set_id; uint32_t cap_set_ver;
    uint64_t addr; uint32_t size; uint32_t pad;
};

// M1 ioctl structs (virtgpu_drm.h, exact layout).
struct DrmGemClose { uint32_t handle; uint32_t pad; };
struct DrmVirtgpuResourceCreate {
    uint32_t target, format, bind, width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags;
    uint32_t bo_handle;   // in: recreate attached to this bo; out: GEM handle
    uint32_t res_handle;  // out: host resource id
    uint32_t size, stride;
};
struct DrmVirtgpuResourceInfo {
    uint32_t bo_handle, res_handle, size, blob_mem;
};
struct DrmVirtgpuCreateBlob {
    uint32_t memory, flags, bo_handle, res_handle;
    uint64_t size;
    uint32_t pad, cmd_size;
    uint64_t cmd, blob_id;
};
struct DrmVirtgpu3dBox { uint32_t x, y, z, w, h, d; };
struct DrmVirtgpu3dTransfer {     // to_host and from_host share this layout
    uint32_t bo_handle;
    DrmVirtgpu3dBox box;
    uint32_t level, offset, stride, layer_stride;
};
struct DrmVirtgpuExecbuffer {
    uint32_t flags, size;
    uint64_t command;        // void* to virgl dwords
    uint64_t bo_handles;     // u32* residency list
    uint32_t num_bo_handles;
    int32_t  fence_fd;
    uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs;
    uint64_t in_syncobjs, out_syncobjs;
};
struct DrmVirtgpu3dWait { uint32_t handle, flags; };
struct DrmVirtgpuMap { uint64_t offset; uint32_t handle, pad; };
struct DrmVirtgpuContextSetParam { uint64_t param, value; };
struct DrmVirtgpuContextInit {
    uint32_t num_params, pad;
    uint64_t ctx_set_params;  // DrmVirtgpuContextSetParam*
};

// VIRTGPU_PARAM_* (virtgpu_drm.h).
constexpr uint64_t VIRTGPU_PARAM_3D_FEATURES        = 1;
constexpr uint64_t VIRTGPU_PARAM_CAPSET_QUERY_FIX   = 2;
constexpr uint64_t VIRTGPU_PARAM_RESOURCE_BLOB      = 3;
constexpr uint64_t VIRTGPU_PARAM_HOST_VISIBLE       = 4;
constexpr uint64_t VIRTGPU_PARAM_CROSS_DEVICE       = 5;
constexpr uint64_t VIRTGPU_PARAM_CONTEXT_INIT       = 6;
constexpr uint64_t VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs = 7;
constexpr uint64_t VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME  = 8;

} // namespace

// Copy a NUL-padded string into a user buffer following the DRM len/ptr
// protocol: always report the true length in *outLen; copy min(provided, true)
// bytes if the user gave a buffer. libdrm calls twice (first to learn the
// length, then with an allocated buffer).
static void DrmFillStr(uint64_t userPtr, uint64_t& userLen, const char* s)
{
    uint64_t actual = 0;
    while (s[actual]) ++actual;
    if (userPtr && userLen)
    {
        uint64_t n = userLen < actual ? userLen : actual;
        CopyToUser(userPtr, s, n);
    }
    userLen = actual;
}

// Ensure a GEM-backed resource has guest backing pages attached to its host
// virgl resource (an iov). virglrenderer requires src_res->iov != NULL for the
// COPY_TRANSFER3D (READ_FROM_HOST) that Mesa emits for glReadPixels — and that
// applies to the *render target* being read, not just the staging buffer, so we
// must attach backing to every resource at creation time, not lazily at MAP
// (Mesa never MAPs a render target). Idempotent: a second call is a no-op once
// attachment is verified, so MAP reuses the same pages. Ambiguous attachment
// retains its allocation for safe retirement/quarantine and returns false.
static bool DrmEnsureBacking(DrmGemObj* gem)
{
    auto* g=gem->resource;
    struct Guard {
        KMutex* lock;
        explicit Guard(KMutex* value):lock(value){KMutexLock(lock);}
        ~Guard(){KMutexUnlock(lock);}
    } guard(&g->backingLock);
    if (g->hostBlob) return g->hostMapping.length != 0;
    if (g->backingKva != 0) return g->backingAttached;
    uint32_t bytes = g->backingBytes ? g->backingBytes
                   : (g->size ? g->size
                              : g->stride * (g->height ? g->height : 1));
    if (bytes == 0) return false;
    uint64_t pages = (static_cast<uint64_t>(bytes) + 4095) / 4096;
    VirtualAddress kva = VmmAllocPages(pages, VMM_WRITABLE,
                                       MemTag::Device, KernelPid);
    if (!kva.raw()) {
        __atomic_fetch_add(&g_drmBackingDiagnostics.failedAllocations,1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&g_drmBackingDiagnostics.failedRequestedBytes,bytes,__ATOMIC_RELAXED);
        return false;
    }
    __atomic_fetch_add(&g_drmBackingDiagnostics.allocatedMappings,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&g_drmBackingDiagnostics.allocatedPageBytes,pages*4096,__ATOMIC_RELAXED);
    auto* zp = reinterpret_cast<volatile uint8_t*>(kva.raw());
    for (uint64_t b = 0; b < pages * 4096; ++b) zp[b] = 0;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    const bool attached=ga && ga->ResourceAttachUser &&
        ga->ResourceAttachUser(g->owner->ctxId,(int32_t)g->gres,kva.raw(),bytes)>=0;
    g->backingKva = kva.raw();
    g->backingBytes = bytes;
    g->backingAttached=attached;
    return attached;
}

static int CreateCompletedSyncFile(Process* process) {
    if(!process)return -ESRCH;
    auto* file=static_cast<SyncFileData*>(kmalloc(sizeof(SyncFileData)));
    if(!file)return -ENOMEM;
    *file={1,ClockNowNs(1),__atomic_add_fetch(&g_eventFdIdentity,1,__ATOMIC_RELAXED)};
    FdEntry owned{};owned.type=FdType::SyncFile;owned.handle=file;owned.fdFlags=1;owned.statusFlags=0;
    const int fd=FdTablePublishOwnedPinned(process->fds,ProcessFdLock(process),owned);
    if(fd<0)SyncFileUnref(file);
    return fd;
}
// Adopts the pin installed atomically by CreateCompletedSyncFile; no second
// lookup/pin is allowed because a sibling may already have logically closed it.
struct SyncFilePublicationPin {
    Process* process;
    int fd;
    SyncFilePublicationPin(Process* p,int f):process(p),fd(f) {}
    ~SyncFilePublicationPin() { FdPut(process,fd); }
    SyncFilePublicationPin(const SyncFilePublicationPin&)=delete;
    SyncFilePublicationPin& operator=(const SyncFilePublicationPin&)=delete;
};

static int64_t DrmRenderIoctl(DrmCtx* dctx, uint8_t nr, uint64_t arg)
{
    DrmMappingGuard guard(dctx);
    switch (nr)
    {
    case DRM_NR_VERSION:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVersion)) ||
            !UserBufferWritable(arg, sizeof(DrmVersion)))
            return -EFAULT;
        DrmVersion v;
        for (uint64_t i = 0; i < sizeof(v); ++i)
            reinterpret_cast<char*>(&v)[i] = reinterpret_cast<char*>(arg)[i];
        v.version_major = VIRTGPU_DRIVER_MAJOR;
        v.version_minor = VIRTGPU_DRIVER_MINOR;
        v.version_patchlevel = VIRTGPU_DRIVER_PATCH;
        DrmFillStr(v.name, v.name_len, "virtio_gpu");
        DrmFillStr(v.date, v.date_len, "0");
        DrmFillStr(v.desc, v.desc_len, "Brook virtio-gpu GL shim");
        if (!CopyToUser(arg, &v, sizeof(v))) return -EFAULT;
        return 0;
    }
    case DRM_NR_GET_CAP:
    {
        if (!UserBufferReadable(arg, sizeof(DrmGetCap)) ||
            !UserBufferWritable(arg, sizeof(DrmGetCap)))
            return -EFAULT;
        DrmGetCap c = *reinterpret_cast<DrmGetCap*>(arg);
        // Advertise PRIME import+export so Mesa enables the dmabuf sharing path
        // (windowed GL presentation). Everything else (dumb buffers, etc.) stays
        // unadvertised — we only support the GPU-resource dmabuf path.
        c.value = (c.capability == DRM_CAP_PRIME)
                ? (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)
                : 0;
        return CopyToUser(arg, &c, sizeof(c)) ? 0 : -EFAULT;
    }
    case DRM_NR_VIRTGPU_GETPARAM:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuGetparam)))
            return -EFAULT;
        DrmVirtgpuGetparam p = *reinterpret_cast<DrmVirtgpuGetparam*>(arg);
        const auto* ga = GpuAppGet();
        if (!ga || !ga->Capabilities) return -ENODEV;
        const auto caps = ga->Capabilities();
        uint64_t val;
        switch (p.param)
        {
        case VIRTGPU_PARAM_3D_FEATURES:       val = caps.capsetMask != 0; break;
        case VIRTGPU_PARAM_CAPSET_QUERY_FIX:  val = 1; break;
        case VIRTGPU_PARAM_CONTEXT_INIT:      val = caps.contextInit; break;
        // capset id bitmask: VIRGL(1) | VIRGL2(2).
        case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs: val = caps.capsetMask; break;
        case VIRTGPU_PARAM_RESOURCE_BLOB:     val = caps.resourceBlob; break;
        case VIRTGPU_PARAM_HOST_VISIBLE:      val = caps.hostVisible; break;
        case VIRTGPU_PARAM_CROSS_DEVICE:      val = 0; break;
        case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME: val = 0; break;
        default: return -EINVAL;
        }
        if (!CopyToUser(p.value, &val, sizeof(val))) return -EFAULT;
        return 0;
    }
    case DRM_NR_VIRTGPU_GET_CAPS:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuGetCaps)))
            return -EFAULT;
        DrmVirtgpuGetCaps gc = *reinterpret_cast<DrmVirtgpuGetCaps*>(arg);
        if (gc.size == 0 || gc.size > 64u * 1024) return -EINVAL;
        if (!UserBufferWritable(gc.addr, gc.size)) return -EFAULT;
        // Fetch the REAL virgl capset blob from the host so Mesa binds the
        // hardware virgl screen (instead of falling back to llvmpipe). The
        // driver issues VIRTIO_GPU_CMD_GET_CAPSET(id, version) and copies the
        // blob into the user buffer. Zero the tail so any bytes the host didn't
        // supply read as 0 (Mesa reads a fixed-size union virgl_caps).
        const brook::GpuAppOps* ga = brook::GpuAppGet();
        if (!ga || !ga->GetCapset || !ga->Capabilities) return -ENODEV;
        if (gc.cap_set_id >= 64 ||
            !(ga->Capabilities().capsetMask & (uint64_t{1} << gc.cap_set_id))) return -EINVAL;
        for (uint32_t i = 0; i < gc.size; ++i)
            reinterpret_cast<char*>(gc.addr)[i] = 0;
        int32_t n = ga->GetCapset(gc.cap_set_id, gc.cap_set_ver,
                                  reinterpret_cast<void*>(gc.addr), gc.size);
        if (n < 0) return -EINVAL;
        return 0;
    }
    // --- M1: resource / submit path (per-fd GEM table -> host virgl ids) ---
    case DRM_NR_VIRTGPU_CONTEXT_INIT:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuContextInit))) return -EFAULT;
        DrmVirtgpuContextInit init{};
        if (!CopyFromUser(&init, arg, sizeof(init))) return -EFAULT;
        if (init.num_params > 4) return -EINVAL;
        GpuContextParam params[4]{};
        if (init.num_params && !CopyFromUser(params, init.ctx_set_params,
                                             init.num_params * sizeof(params[0]))) return -EFAULT;
        const auto* ga = GpuAppGet();
        if (!ga || !ga->CtxCreate || !ga->Capabilities) return -ENODEV;
        if (dctx->owner->ctxId > 0) return -17; // EEXIST
        GpuContextConfig config{};
        if (!GpuContextParse(params, init.num_params, ga->Capabilities(), config)) return -EINVAL;
        Process* proc = ProcessCurrent();
        const int32_t ctx = ga->CtxCreate(proc ? proc->pid : 0, config);
        if (ctx < 0) return -ENOMEM;
        dctx->owner->config = config;
        dctx->owner->ctxId = ctx;
        return 0;
    }
    case DRM_NR_VIRTGPU_RESOURCE_CREATE_BLOB:
    {
        if (!UserBufferReadable(arg,sizeof(DrmVirtgpuCreateBlob)) ||
            !UserBufferWritable(arg,sizeof(DrmVirtgpuCreateBlob))) return -EFAULT;
        DrmVirtgpuCreateBlob create{};
        if (!CopyFromUser(&create,arg,sizeof(create))) return -EFAULT;
        const auto* ga=GpuAppGet();
        if (!ga || !ga->ResourceCreateBlob || !ga->Capabilities || !ga->Capabilities().resourceBlob) return -EINVAL;
        if (create.memory!=2 || (create.flags&~3u) || !create.size || create.size>0xfffff000ULL) return -EINVAL;
        if(dctx->owner->ctxId<=0) {
            if(!ga->CtxCreate) return -ENODEV;
            auto* process=ProcessCurrent();
            const auto context=ga->CtxCreate(process?process->pid:0,{});
            if(context<0)return -ENOMEM;
            dctx->owner->ctxId=context;
        }
        uint32_t virglCreate[12]{};
        if(dctx->owner->config.capsetId==4) {
            if(create.cmd_size) return -EINVAL;
        } else {
            // Mesa's persistent-buffer path bundles one PIPE_RESOURCE_CREATE.
            if(create.cmd_size!=sizeof(virglCreate))return -EINVAL;
            if(!CopyFromUser(virglCreate,create.cmd,sizeof(virglCreate)))return -EFAULT;
            if(virglCreate[0]!=((11u<<16)|48u) || create.blob_id>0xffffffffULL ||
                virglCreate[11]!=create.blob_id || virglCreate[1]!=0 ||
                virglCreate[4]!=((create.size+4095)&~uint64_t{4095}) ||
                virglCreate[5]>1 || virglCreate[6]>1 || virglCreate[7]!=1 ||
                virglCreate[8] || virglCreate[9])return -EINVAL;
        }
        const uint32_t size=static_cast<uint32_t>((create.size+4095)&~uint64_t{4095});
        const uint32_t bo=DrmGemAlloc(dctx,0,size,0,0,0,0,0);
        if (!bo) return -ENOMEM;
        auto* gem=&dctx->gem[bo-1];auto* resource=gem->resource;
        resource->hostBlob=true;resource->blobFlags=create.flags;
        if(dctx->owner->config.capsetId!=4) {
            resource->target=virglCreate[1];resource->format=virglCreate[2];
            resource->width=virglCreate[4];resource->height=virglCreate[5]?virglCreate[5]:1;
            if(!ga->Submit3D || ga->Submit3D(dctx->owner->ctxId,virglCreate,12)<0) {DrmGemDrop(gem);return -EIO;}
        }
        const GpuBlobCreate parameters{create.size,create.blob_id,create.memory,create.flags};
        if (ga->ResourceCreateBlob(dctx->owner->ctxId,parameters,&resource->gres,&resource->hostMapping)<0) {
            DrmGemDrop(gem);return -ENOMEM;
        }
        create.bo_handle=bo;create.res_handle=resource->gres;
        if (!CopyToUser(arg,&create,sizeof(create))) {DrmGemDrop(gem);return -EFAULT;}
        return 0;
    }
    case DRM_NR_VIRTGPU_RESOURCE_CREATE:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuResourceCreate)) ||
            !UserBufferWritable(arg, sizeof(DrmVirtgpuResourceCreate)))
            return -EFAULT;
        DrmVirtgpuResourceCreate rc = *reinterpret_cast<DrmVirtgpuResourceCreate*>(arg);
        const brook::GpuAppOps* ga = brook::GpuAppGet();
        if (!ga || !ga->ResourceCreate3D) return -ENODEV;
        // Context is created lazily if the client skipped CONTEXT_INIT.
        if (dctx->owner->ctxId <= 0) {
            if (!ga->CtxCreate) return -ENODEV;
            Process* proc = ProcessCurrent();
            int32_t ctx = ga->CtxCreate(proc ? proc->pid : 0, {});
            if (ctx < 0) return -ENOMEM;
            dctx->owner->ctxId = ctx;
        }
        // target 0 = PIPE_BUFFER: a 1D linear buffer whose "width" is a byte
        // count (e.g. glReadPixels staging), so it can far exceed the 2D texture
        // dimension cap. Validate buffers by byte size, textures by dimension.
        bool isBuffer = (rc.target == 0);
        if (rc.width == 0) return -EINVAL;
        if (isBuffer) {
            if (rc.width > 256u * 1024 * 1024) return -EINVAL;   // 256 MB cap
        } else if (rc.width > 16384 || rc.height > 16384) {
            return -EINVAL;
        }
        uint32_t createH = rc.height ? rc.height : 1;   // PIPE_BUFFER is 1D (h=0)
        // For a buffer the "stride" is its byte length, not width*texel; for a
        // 2D texture default to width*4 bytes/texel when the client omits stride.
        uint32_t stride = rc.stride ? rc.stride
                        : (isBuffer ? rc.width : rc.width * 4);
        // Mesa allocates no guest storage for host-side textures and passes a
        // bogus rc.size (often 1), so never honour it for a texture — compute
        // the full row-stride*height extent. Buffers carry a real byte count.
        uint32_t size = isBuffer ? (rc.size ? rc.size : rc.width)
                                 : stride * createH;
        // Allocate owning metadata before creating a host resource, so every
        // successful host allocation has a tracked rollback/retirement owner.
        uint32_t bo = DrmGemAlloc(dctx,0,size,stride,rc.width,createH,rc.target,rc.format);
        if (bo == 0) {
            SerialPrintf("drm: GEM metadata allocation failed ctx=%d capacity=%u\n",
                         dctx->owner->ctxId,DRM_MAX_GEM);
            return -ENOMEM;
        }
        int32_t gres = ga->ResourceCreate3D(dctx->owner->ctxId, rc.target, rc.format,
                                            rc.bind, rc.width, createH,
                                            rc.depth, rc.array_size,
                                            rc.last_level, rc.nr_samples,
                                            rc.flags);
        if (gres < 0) {
            SerialPrintf("drm: resource create failed ctx=%d target=%u size=%ux%u\n",
                         dctx->owner->ctxId,rc.target,rc.width,createH);
            DrmGemDrop(&dctx->gem[bo-1]);
            return -ENOMEM;
        }
        dctx->gem[bo-1].resource->gres=static_cast<uint32_t>(gres);
        // Attach guest backing now (not lazily at MAP): a render target read by
        // Mesa's glReadPixels COPY_TRANSFER3D must already have an iov, or
        // virglrenderer rejects the command (check_copy_transfer3d_handles).
        DrmGemObj* gobj = DrmGemGet(dctx, bo);
        if (gobj && !DrmEnsureBacking(gobj)) {
            SerialPrintf("drm: resource backing allocation failed ctx=%d gres=%d bytes=%u\n",
                         dctx->owner->ctxId,gres,gobj->resource->size);
            DrmGemDrop(gobj);
            return -ENOMEM;
        }
        rc.res_handle = (uint32_t)gres;   // host id, used inside virgl streams
        rc.bo_handle  = bo;               // per-fd GEM handle, used in ioctls
        rc.size = size; rc.stride = stride;
        if (!CopyToUser(arg,&rc,sizeof(rc))) { DrmGemDrop(gobj);return -EFAULT; }
        return 0;
    }
    case DRM_NR_VIRTGPU_RESOURCE_INFO:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuResourceInfo)) ||
            !UserBufferWritable(arg, sizeof(DrmVirtgpuResourceInfo)))
            return -EFAULT;
        DrmVirtgpuResourceInfo ri = *reinterpret_cast<DrmVirtgpuResourceInfo*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, ri.bo_handle);
        if (!g) return -ENOENT;
        ri.res_handle = g->resource->gres;
        ri.size = g->resource->size;
        ri.blob_mem = g->resource->hostBlob ? 2 : 0;
        return CopyToUser(arg, &ri, sizeof(ri)) ? 0 : -EFAULT;
    }
    case DRM_NR_VIRTGPU_TRANSFER_TO_HOST:
    case DRM_NR_VIRTGPU_TRANSFER_FROM_HOST:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpu3dTransfer)))
            return -EFAULT;
        DrmVirtgpu3dTransfer t = *reinterpret_cast<DrmVirtgpu3dTransfer*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, t.bo_handle);
        if (!g) return -ENOENT;
        if (g->resource->owner->ctxId <= 0 || g->resource->hostBlob) return -EINVAL;
        const brook::GpuAppOps* ga = brook::GpuAppGet();
        if (!ga || !ga->Transfer3D) return -ENODEV;
        int dir = (nr == DRM_NR_VIRTGPU_TRANSFER_TO_HOST) ? 0 : 1;
        if (!t.box.w || !t.box.h || !t.box.d) return -EINVAL;
        const brook::GpuTransfer3D transfer{
            t.box.x,t.box.y,t.box.z,t.box.w,t.box.h,t.box.d,
            t.offset,t.level,t.stride,t.layer_stride};
        int32_t r = ga->Transfer3D(g->resource->owner->ctxId, (int32_t)g->resource->gres, dir,
                                   transfer);
        return r < 0 ? -EIO : 0;
    }
    case DRM_NR_VIRTGPU_EXECBUFFER:
    {
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuExecbuffer)))
            return -EFAULT;
        DrmVirtgpuExecbuffer eb = *reinterpret_cast<DrmVirtgpuExecbuffer*>(arg);
        if (dctx->owner->ctxId <= 0) return -EINVAL;
        if (dctx->owner->config.capsetId==4) {
            if ((eb.flags&~6u) || !(eb.flags&4u) || eb.ring_idx>=dctx->owner->config.numRings ||
                eb.syncobj_stride || eb.num_in_syncobjs || eb.num_out_syncobjs ||
                (eb.size&3u) || eb.size>60u*1024u-32u || eb.num_bo_handles>DRM_MAX_GEM) return -EINVAL;
            if ((eb.flags&2u) && !UserBufferWritable(arg,sizeof(eb))) return -EFAULT;
            struct BoSnapshot {
                uint32_t* data;
                ~BoSnapshot(){kfree(data);}
            } handles{eb.num_bo_handles?static_cast<uint32_t*>(kmalloc(eb.num_bo_handles*sizeof(uint32_t))):nullptr};
            if(eb.num_bo_handles && !handles.data)return -ENOMEM;
            if(eb.num_bo_handles && !CopyFromUser(handles.data,eb.bo_handles,eb.num_bo_handles*sizeof(uint32_t)))return -EFAULT;
            for(uint32_t i=0;i<eb.num_bo_handles;++i)if(!DrmGemGet(dctx,handles.data[i]))return -ENOENT;
            auto* commands=eb.size ? static_cast<uint32_t*>(kmalloc(eb.size)) : nullptr;
            if (eb.size && !commands) return -ENOMEM;
            if (eb.size && !CopyFromUser(commands,eb.command,eb.size)) {kfree(commands);return -EFAULT;}
            // BO reservations are also dependencies when no fd was requested.
            // Retain a pending marker until this exact selected timeline completes.
            uint32_t begun=0;
            for(;begun<eb.num_bo_handles;++begun) {
                auto* gem=DrmGemGet(dctx,handles.data[begun]);
                KMutexLock(&gem->resource->reservationLock);
                const bool admitted=gem->resource->reservation.Begin();
                KMutexUnlock(&gem->resource->reservationLock);
                if(!admitted)break;
            }
            const auto* ga=GpuAppGet();
            const bool admitted=begun==eb.num_bo_handles;
            const int result=admitted && ga && ga->SubmitVenus ?
                ga->SubmitVenus(dctx->owner->ctxId,commands,eb.size/4,eb.ring_idx,(eb.flags&2u)!=0 || eb.num_bo_handles!=0) : -1;
            for(uint32_t i=0;i<begun;++i) {
                auto* gem=DrmGemGet(dctx,handles.data[i]);
                if(gem) {
                    KMutexLock(&gem->resource->reservationLock);
                    gem->resource->reservation.Finish(!admitted || result==0);
                    KMutexUnlock(&gem->resource->reservationLock);
                }
            }
            kfree(commands);
            if (result<0) return -EIO;
            if (eb.flags&2u) {
                auto* process=ProcessCurrent();
                const int descriptor=CreateCompletedSyncFile(process);
                if(descriptor<0)return descriptor;
                SyncFilePublicationPin publication(process,descriptor);
                eb.fence_fd=descriptor;
                if(!CopyToUser(arg,&eb,sizeof(eb))) {CloseProcessFd(process,descriptor);return -EFAULT;}
            }
            return 0;
        }
        if (eb.size == 0 || (eb.size & 3) || eb.size > 1024u * 1024) return -EINVAL;
        if (!UserBufferReadable(eb.command, eb.size)) return -EFAULT;
        const brook::GpuAppOps* ga = brook::GpuAppGet();
        if (!ga || !ga->Submit3D) return -ENODEV;
        uint32_t nDw = eb.size / 4;
        auto* cmd = static_cast<uint32_t*>(kmalloc(eb.size));
        if (!cmd) return -ENOMEM;
        for (uint32_t i = 0; i < nDw; ++i)
            cmd[i] = reinterpret_cast<const uint32_t*>(eb.command)[i];
        int32_t r = ga->Submit3D(dctx->owner->ctxId, cmd, nDw);
        kfree(cmd);
        if (r < 0) return -EIO;
        // QEMU can acknowledge SUBMIT_3D while rendering is still in flight.
        // Complete a real fence on the app context before handing Mesa a
        // signaled fd: the compositor samples this buffer in another context.
        if (eb.flags & 0x02 /*FENCE_FD_OUT*/) {
            if (!ga->PresentBarrier || ga->PresentBarrier(dctx->owner->ctxId) < 0)
                return -EIO;
            Process* fp = ProcessCurrent();
            const int fenceFd=CreateCompletedSyncFile(fp);
            if(fenceFd<0)return fenceFd;
            SyncFilePublicationPin publication(fp,fenceFd);
            eb.fence_fd = fenceFd;
            if (!CopyToUser(arg, &eb, sizeof(eb))) {
                CloseProcessFd(fp,fenceFd);
                return -EFAULT;
            }
        }
        return 0;
    }
    case DRM_NR_VIRTGPU_WAIT:
    {
        if(!UserBufferReadable(arg,sizeof(DrmVirtgpu3dWait)))return -EFAULT;
        DrmVirtgpu3dWait wait{};
        if(!CopyFromUser(&wait,arg,sizeof(wait)))return -EFAULT;
        auto* gem=DrmGemGet(dctx,wait.handle);if(!gem)return -ENOENT;
        if(gem->resource->hostBlob) {
            if((wait.flags&~1u) || gem->resource->owner->config.capsetId==4)return -95;
            // GL contexts with host blobs complete their submissions before
            // returning, so NOWAIT may safely report idle in this bounded path.
            if(wait.flags&1u)return 0;
            const auto* ga=GpuAppGet();
            return ga && ga->PresentBarrier && ga->PresentBarrier(gem->resource->owner->ctxId)==0 ? 0 : -EIO;
        }
        // Preserve the legacy guest-backed path at this checkpoint.
        return 0;
    }
    case DRM_NR_PRIME_HANDLE_TO_FD:
    {
        // Export a GEM (render target) as a PRIME dmabuf fd: a capability that
        // wraps the GEM's host virgl resource (gres). The client passes this fd
        // over the Wayland socket so the compositor can import + present it.
        if (!UserBufferReadable(arg, sizeof(DrmPrimeHandle)) ||
            !UserBufferWritable(arg, sizeof(DrmPrimeHandle)))
            return -EFAULT;
        DrmPrimeHandle ph = *reinterpret_cast<DrmPrimeHandle*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, ph.handle);
        if (!g) return -ENOENT;
        Process* proc = ProcessCurrent();
        if (!proc) return -ESRCH;
        auto* p = static_cast<DrmPrimeObj*>(kmalloc(sizeof(DrmPrimeObj)));
        if (!p) return -ENOMEM;
        p->resource=g->resource;
        MappingBackingRef(p->resource->lifetime);
        p->refCount=1;
        // Sticky authorization for the numeric WM_PRESENT_GRES ABI. Actual
        // lifetime is owned by GEM/PRIME/VMA/capture references, not this flag.
        __atomic_store_n(&p->resource->everExported,true,__ATOMIC_RELEASE);
        int fd = FdAlloc(proc, FdType::DrmPrime, p);
        if (fd < 0) { DrmPrimeUnref(p);return -EMFILE; }
        ph.fd = fd;
        if (!CopyToUser(arg, &ph, sizeof(ph))) {
            CloseProcessFd(proc, fd);
            return -EFAULT;
        }
        return 0;
    }
    case DRM_NR_PRIME_FD_TO_HANDLE:
    {
        // Import a PRIME dmabuf fd (received over the Wayland socket) into a GEM
        // handle aliasing the SAME gres. Possession of the fd IS the
        // authorization — a process cannot reach a gres it was never handed.
        if (!UserBufferReadable(arg, sizeof(DrmPrimeHandle)) ||
            !UserBufferWritable(arg, sizeof(DrmPrimeHandle)))
            return -EFAULT;
        DrmPrimeHandle ph = *reinterpret_cast<DrmPrimeHandle*>(arg);
        Process* proc = ProcessCurrent();
        if (!proc) return -ESRCH;
        // Keep the capability alive across metadata lookup and GEM creation.
        // FdGet drops the table lock and can race a concurrent close/reuse.
        FdRef primeRef(proc, ph.fd);
        FdEntry* fde = primeRef.get();
        if (!fde || fde->type != FdType::DrmPrime || !fde->handle) return -EINVAL;
        auto* p = static_cast<DrmPrimeObj*>(fde->handle);
        // Repeated PRIME imports into one DRM file describe one GEM handle,
        // including imports through separately exported FDs and self-import.
        // Compare shared object identity, independently of host ID or GEM-slot
        // reuse. The pinned PRIME capability retains that exact object.
        for (uint32_t i = 0; i < DRM_MAX_GEM; ++i) {
            if (dctx->gem[i].used && dctx->gem[i].resource == p->resource) {
                ph.handle = i + 1;
                return CopyToUser(arg, &ph, sizeof(ph)) ? 0 : -EFAULT;
            }
        }
        uint32_t bo = DrmGemAllocImported(dctx,p->resource);
        if (bo == 0) return -ENOMEM;
        ph.handle = bo;
        if (!CopyToUser(arg, &ph, sizeof(ph))) {
            // Drop only the newly inserted alias/reference. Existing shared
            // handles and the PRIME capability remain owned by their callers.
            DrmGemDrop(&dctx->gem[bo-1]);
            return -EFAULT;
        }
        return 0;
    }
    case DRM_NR_GEM_CLOSE:
    {
        if (!UserBufferReadable(arg, sizeof(DrmGemClose))) return -EFAULT;
        DrmGemClose gc = *reinterpret_cast<DrmGemClose*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, gc.handle);
        if (!g) return -ENOENT;
        // Drop this alias; other handles, PRIME capabilities and VMAs retain
        // the shared resource. The last reference queues verified retirement.
        if (g->resource->backingKva) {
            const uint64_t pages=(static_cast<uint64_t>(g->resource->backingBytes)+4095)/4096;
            __atomic_fetch_add(&g_drmBackingDiagnostics.gemCloseBackingAliases,1,__ATOMIC_RELAXED);
            __atomic_fetch_add(&g_drmBackingDiagnostics.gemCloseBackingAliasPageBytes,pages*4096,__ATOMIC_RELAXED);
        }
        DrmGemDrop(g);
        return 0;
    }
    case DRM_NR_VIRTGPU_MAP:
    {
        // Map-for-CPU-access (e.g. glReadPixels staging buffers). Lazily
        // allocate guest backing for the resource and attach it to the host
        // virgl resource, so a later TRANSFER_FROM_HOST DMAs the rendered
        // pixels into pages that sys_mmap then maps into user space. The
        // returned offset identifies this GEM incarnation (see sys_mmap).
        if (!UserBufferReadable(arg, sizeof(DrmVirtgpuMap)) ||
            !UserBufferWritable(arg, sizeof(DrmVirtgpuMap)))
            return -EFAULT;
        DrmVirtgpuMap m = *reinterpret_cast<DrmVirtgpuMap*>(arg);
        DrmGemObj* g = DrmGemGet(dctx, m.handle);
        if (!g) return -ENOENT;
        if (g->resource->hostBlob && !g->resource->hostMapping.length) return -EINVAL;
        if (!DrmEnsureBacking(g)) return -ENOMEM;
        if (!g->mapToken) {
            g->mapToken = g_drmMapTokens.Allocate(g->resource->size ? g->resource->size : 4096);
            if (!g->mapToken) return -ENOMEM;
        }
        m.offset = g->mapToken;
        return CopyToUser(arg, &m, sizeof(m)) ? 0 : -EFAULT;
    }
    default:
        return -25; // ENOTTY
    }
}

static int64_t sys_ioctl(uint64_t fd, uint64_t cmd_raw, uint64_t arg,
                          uint64_t, uint64_t, uint64_t)
{
    // ioctl cmd is a 32-bit value; mask off sign-extension from syscall ABI
    uint64_t cmd = cmd_raw & 0xFFFFFFFFULL;
    Process* proc = ProcessCurrent();
    if (!proc || fd >= MAX_FDS) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if (!fde || fde->type==FdType::Path) return -EBADF;

    if(fde->type==FdType::Vnode && fde->handle && static_cast<Vnode*>(fde->handle)->type==VnodeType::Namespace)
        return cmd==0xb703?0x20000:-25; // NS_GET_NSTYPE; other namespace ioctls unsupported.
    if(fde->type==FdType::SyncFile) {
        if(!fde->handle)return -EBADF;
        if(cmd!=SYNC_FILE_INFO_IOCTL)return -25;
        SyncFileInfo request{};
        if(!CopyFromUser(&request,arg,sizeof(request)))return -EFAULT;
        if(!SyncFileInfoRequestValid(request))return -EINVAL;
        if(!UserBufferWritable(arg,sizeof(request)))return -EFAULT;
        SyncFileInfo info{};SyncFenceInfo detail{};
        SyncFileDescribe(*static_cast<SyncFileData*>(fde->handle),info,detail);
        info.fences=request.fences;
        if(request.numFences && !CopyToUser(request.fences,&detail,sizeof(detail)))return -EFAULT;
        return CopyToUser(arg,&info,sizeof(info))?0:-EFAULT;
    }
    // libsync uses this query to distinguish a sync_file from an arbitrary fd.
    if(cmd==SYNC_FILE_INFO_IOCTL)return -25;

    if(fde->type==FdType::DrmPrime) {
        if(!fde->handle)return -EBADF;
        struct FenceIoctl {uint32_t flags;int32_t fd;};
        static_assert(sizeof(FenceIoctl)==8);
        constexpr uint32_t exportFence=0xc0086202,importFence=0x40086203;
        if(cmd!=exportFence && cmd!=importFence)return -25;
        FenceIoctl request{};
        if(!CopyFromUser(&request,arg,sizeof(request)))return -EFAULT;
        if(!request.flags || (request.flags&~3u))return -EINVAL;
        auto* resource=static_cast<DrmPrimeObj*>(fde->handle)->resource;
        if(cmd==importFence) {
            if(request.fd<0)return -EINVAL;
            FdRef source(proc,request.fd);
            if(!source || source->type!=FdType::SyncFile || !source->handle)return -EINVAL;
            KMutexLock(&resource->reservationLock);
            const bool accepted=resource->reservation.ImportCompleted(request.flags);
            KMutexUnlock(&resource->reservationLock);
            return accepted?0:-EIO;
        }
        if(!UserBufferWritable(arg,sizeof(request)))return -EFAULT;
        if(resource->owner->config.capsetId!=4) {
            const auto* ga=GpuAppGet();
            if(!ga || !ga->PresentBarrier || ga->PresentBarrier(resource->owner->ctxId)<0)return -EIO;
        }
        // This implementation exports completed snapshots. Wait for admitted
        // BO submissions rather than dropping their dependencies from the fd.
        const uint64_t deadline=ClockNowNs(1)+5000000000ull;
        for(;;) {
            KMutexLock(&resource->reservationLock);
            const bool uncertain=resource->reservation.uncertain;
            const bool pending=resource->reservation.pending!=0 || resource->reservation.capturing;
            KMutexUnlock(&resource->reservationLock);
            if(uncertain)return -EIO;
            if(!pending)break;
            if(HasPendingSignals())return -EINTR;
            if(ClockNowNs(1)>=deadline)return -110; // ETIMEDOUT
            proc->wakeupTick=ApicTickCount()+1;
            SchedulerBlockInterruptible(proc);
        }
        const int descriptor=CreateCompletedSyncFile(proc);
        if(descriptor<0)return descriptor;
        SyncFilePublicationPin publication(proc,descriptor);
        request.fd=descriptor;
        if(!CopyToUser(arg,&request,sizeof(request))) {CloseProcessFd(proc,descriptor);return -EFAULT;}
        return 0;
    }

    // FIONBIO / FIONREAD are socket ioctls, but their Linux request numbers
    // live in the same 0x54xx range as terminal ioctls.
    if (cmd == 0x5421) // FIONBIO
    {
        if (!UserBufferReadable(arg, sizeof(int))) return -EFAULT;
        bool nonblock = *reinterpret_cast<int*>(arg) != 0;
        if (fde->type == FdType::Socket || fde->type == FdType::UnixSocket ||
            FdPipe(fde) || fde->type == FdType::DevTty) {
            if (nonblock)
                fde->statusFlags |= 0x800; // O_NONBLOCK
            else
                fde->statusFlags &= ~0x800u;
            if (fde->type == FdType::UnixSocket && fde->handle)
                static_cast<UnixSocketData*>(fde->handle)->nonblock = nonblock;
            return 0;
        }
        return -25; // ENOTTY
    }

    if (cmd == 0x541B) // FIONREAD
    {
        if (!UserBufferWritable(arg, sizeof(int))) return -EFAULT;
        int available = 0;
        if (auto* pipe=FdPipe(fde)) {
            available = static_cast<int>(pipe->count());
        } else if (fde->type == FdType::UnixSocket && fde->handle) {
            auto* usd = static_cast<UnixSocketData*>(fde->handle);
            if (usd->state != UnixSocketData::State::Connected || !usd->rxPipe)
                return -ENOTCONN;
            available = static_cast<int>(usd->rxPipe->count());
        } else if (fde->type == FdType::Socket && fde->handle) {
            available = 0;
        } else {
            return -25; // ENOTTY
        }
        *reinterpret_cast<int*>(arg) = available;
        return 0;
    }

    if (fde->type == FdType::DevDri)
    {
        uint8_t type = static_cast<uint8_t>((cmd >> 8) & 0xFF);
        uint8_t nr   = static_cast<uint8_t>(cmd & 0xFF);
        if (type != DRM_IOCTL_TYPE) return -25; // ENOTTY
        if (!fde->handle) return -EBADF;
        return DrmRenderIoctl(static_cast<DrmCtx*>(fde->handle), nr, arg);
    }

    if (fde->type == FdType::DevFramebuf)
    {
        uint64_t physBase;
        uint32_t fbW, fbH, fbStride;
        if (!TtyGetFramebufferPhys(&physBase, &fbW, &fbH, &fbStride))
            return -ENODEV;

        // Auto-create a VFB + WM window when a process without one queries
        // the framebuffer in WM mode. This handles programs launched from
        // bash that open /dev/fb0 (e.g. DOOM, Quake 2) — they need a composited VFB.
        // Use a default window size of 640×480 rather than the full physical FB,
        // so the window fits on screen with WM decorations.
        if (!proc->fbVirtual && WmIsActive() &&
            (cmd == FBIOGET_VSCREENINFO || cmd == FBIOGET_FSCREENINFO))
        {
            static uint32_t s_autoWinCount = 0;
            int16_t winX = static_cast<int16_t>(60 + (s_autoWinCount % 6) * 40);
            int16_t winY = static_cast<int16_t>(60 + (s_autoWinCount % 6) * 40);

            uint32_t autoW = 640;
            uint32_t autoH = 480;
            if (autoW > fbW) autoW = fbW;
            if (autoH > fbH) autoH = fbH;

            CompositorSetupProcess(proc,
                                   winX + static_cast<int16_t>(WmMetric(WM_BORDER_WIDTH)),
                                   winY + static_cast<int16_t>(WmMetric(WM_TITLE_BAR_HEIGHT) + WmMetric(WM_BORDER_WIDTH)),
                                   autoW, autoH, 1);

            WmCreateWindow(proc, winX, winY,
                          static_cast<uint16_t>(autoW),
                          static_cast<uint16_t>(autoH),
                          proc->name);

            s_autoWinCount++;
            SerialPrintf("sys_ioctl: auto-created WM window for pid %u (%ux%u)\n",
                         proc->pid, autoW, autoH);
        }
        uint32_t repW = (proc->fbVfbWidth  > 0) ? proc->fbVfbWidth  : fbW;
        uint32_t repH = (proc->fbVfbHeight > 0) ? proc->fbVfbHeight : fbH;
        uint32_t repStride = repW * 4; // bytes per line

        if (cmd == FBIOGET_VSCREENINFO)
        {
            if (!UserBufferWritable(arg, sizeof(FbVarScreeninfo))) return -EFAULT;
            auto* info = reinterpret_cast<FbVarScreeninfo*>(arg);
            auto* raw = reinterpret_cast<uint8_t*>(info);
            memset(raw, 0, sizeof(FbVarScreeninfo));

            info->xres = repW;
            info->yres = repH;
            info->xres_virtual = repW;
            info->yres_virtual = repH;
            info->bits_per_pixel = 32;
            // BGRA pixel format (common UEFI framebuffer)
            info->blue_offset  = 0;  info->blue_length  = 8;
            info->green_offset = 8;  info->green_length = 8;
            info->red_offset   = 16; info->red_length   = 8;
            info->transp_offset = 24; info->transp_length = 8;
            return 0;
        }

        if (cmd == FBIOGET_FSCREENINFO)
        {
            if (!UserBufferWritable(arg, sizeof(FbFixScreeninfo))) return -EFAULT;
            auto* info = reinterpret_cast<FbFixScreeninfo*>(arg);
            auto* raw = reinterpret_cast<uint8_t*>(info);
            memset(raw, 0, sizeof(FbFixScreeninfo));

            const char* name = "brook_fb";
            for (int i = 0; name[i] && i < 15; ++i) info->id[i] = name[i];

            info->smem_start  = physBase;
            info->smem_len    = repStride * repH;
            info->type        = 0; // FB_TYPE_PACKED_PIXELS
            info->visual      = 2; // FB_VISUAL_TRUECOLOR
            info->line_length = repStride;
            info->mmio_start  = physBase;
            info->mmio_len    = repStride * repH;
            return 0;
        }

        if (cmd == FBIOPUT_VSCREENINFO)
        {
            // Honor requested xres/yres: resize the process VFB and its
            // associated WM window so the renderer blits 1:1.
            const auto* info = reinterpret_cast<const FbVarScreeninfo*>(arg);
            if (!UserBufferReadable(arg, sizeof(FbVarScreeninfo))) return -EFAULT;
            uint32_t newW = info->xres;
            uint32_t newH = info->yres;
            if (newW == 0 || newH == 0) return -EINVAL;
            if (!proc->fbVirtual) return 0; // no VFB yet — nothing to resize

            if (newW > fbW) newW = fbW;
            if (newH > fbH) newH = fbH;

            if (!CompositorResizeVfb(proc, newW, newH))
                return -ENOMEM;

            if (WmIsActive())
            {
                int widx = WmFindWindowForProcess(proc);
                if (widx >= 0)
                    WmResizeWindow(widx,
                                   static_cast<uint16_t>(newW),
                                   static_cast<uint16_t>(newH));
            }

            SerialPrintf("sys_ioctl: FBIOPUT_VSCREENINFO pid %u → %ux%u\n",
                         proc->pid, newW, newH);
            return 0;
        }

        SerialPrintf("sys_ioctl: fb unknown cmd 0x%lx\n", cmd);
        return -EINVAL;
    }

    if (fde->type == FdType::DevKeyboard)
    {
        // Custom ioctl 1 = enter non-blocking mode (Enkel compat)
        if (cmd == 1)
        {
            fde->flags |= 1; // mark as non-blocking
            return 0;
        }

        // TCGETS — return current termios state
        if (cmd == 0x5401)
        {
            // termios struct: 4 × uint32_t + 1 byte c_line + 19 bytes c_cc = 36 bytes
            if (!UserBufferWritable(arg, 36)) return -EFAULT;
            auto* t = reinterpret_cast<uint32_t*>(arg);
            t[0] = 0x0500;   // c_iflag: ICRNL | IXON
            t[1] = 0x0005;   // c_oflag: OPOST | ONLCR
            t[2] = 0x00BF;   // c_cflag: CS8 | CREAD | HUPCL | B38400
            // Build c_lflag from actual state
            uint32_t lflag = 0x8A31; // base: ECHOE|ECHOK|ISIG|IEXTEN|ECHOCTL|ECHOKE
            if (proc->ttyCanonical) lflag |= 0x0002; // ICANON
            if (proc->ttyEcho)      lflag |= 0x0008; // ECHO
            t[3] = lflag;
            // c_line at offset 16
            auto* raw = reinterpret_cast<uint8_t*>(&t[4]);
            raw[0] = 0;  // c_line = N_TTY
            // c_cc starts at offset 17 (raw[1])
            auto* cc = &raw[1];
            __builtin_memset(cc, 0, 19); // NCCS = 19 for old termios
            cc[0]  = 0x03;  // VINTR  = Ctrl+C
            cc[1]  = 0x1C;  // VQUIT  = Ctrl+backslash
            cc[2]  = 0x7F;  // VERASE = DEL
            cc[3]  = 0x15;  // VKILL  = Ctrl+U
            cc[4]  = 0x04;  // VEOF   = Ctrl+D
            cc[5]  = 0;     // VTIME  = 0
            cc[6]  = 1;     // VMIN   = 1
            cc[7]  = 0;     // VSWTC
            cc[8]  = 0x11;  // VSTART = Ctrl+Q (XON)
            cc[9]  = 0x13;  // VSTOP  = Ctrl+S (XOFF)
            cc[10] = 0x1A;  // VSUSP  = Ctrl+Z
            cc[11] = 0;     // VEOL
            cc[12] = 0x12;  // VREPRINT = Ctrl+R
            cc[13] = 0x0F;  // VDISCARD = Ctrl+O
            cc[14] = 0x17;  // VWERASE = Ctrl+W
            cc[15] = 0x16;  // VLNEXT  = Ctrl+V
            cc[16] = 0;     // VEOL2
            return 0;
        }

        // TCSETS/TCSETSW/TCSETSF — track ICANON and ECHO flags
        if (cmd >= 0x5402 && cmd <= 0x5404)
        {
            if (!UserBufferReadable(arg, 16)) return -EFAULT;
            auto* t = reinterpret_cast<const uint32_t*>(arg);
            Process* cur = ProcessCurrent();
            if (cur)
            {
                uint32_t lflag = t[3];
                cur->ttyCanonical = (lflag & 0x0002) != 0; // ICANON
                cur->ttyEcho      = (lflag & 0x0008) != 0; // ECHO
            }
            return 0;
        }

        // TIOCGPGRP
        if (cmd == 0x540F)
        {
            if (!UserBufferWritable(arg, sizeof(int))) return -EFAULT;
            auto* pgrp = reinterpret_cast<int*>(arg);
            Process* cur = ProcessCurrent();
            if (cur)
            {
                Terminal* term = TerminalFindByProcess(cur);
                if (term)
                    *pgrp = static_cast<int>(term->foregroundPgid);
                else
                    *pgrp = static_cast<int>(cur->pgid);
            }
            else
            {
                *pgrp = 1;
            }
            return 0;
        }

        // TIOCSPGRP
        if (cmd == 0x5410)
        {
            if (!UserBufferReadable(arg, sizeof(int))) return -EFAULT;
            auto* pgrpPtr = reinterpret_cast<const int*>(arg);
            int newPgid = *pgrpPtr;
            Process* cur = ProcessCurrent();
            if (cur)
            {
                Terminal* term = TerminalFindByProcess(cur);
                if (term)
                {
                    term->foregroundPgid = static_cast<uint16_t>(newPgid);
                    DbgPrintf("TIOCSPGRP: pid %u set fg pgid to %d\n", cur->pid, newPgid);
                }
            }
            return 0;
        }

        // TIOCSCTTY
        if (cmd == 0x540E)
            return 0;

        // TIOCGWINSZ
        if (cmd == 0x5413)
        {
            if (!UserBufferWritable(arg, 4 * sizeof(uint16_t))) return -EFAULT;
            auto* ws = reinterpret_cast<uint16_t*>(arg);
            TerminalGeometry geometry{25,80,0,0};
            TerminalSnapshotGeometry(proc,&geometry);
            ws[0] = static_cast<uint16_t>(geometry.rows);
            ws[1] = static_cast<uint16_t>(geometry.cols);
            ws[2] = static_cast<uint16_t>(geometry.width);
            ws[3] = static_cast<uint16_t>(geometry.height);
            return 0;
        }

        return 0;
    }

    // tcgetattr/tcsetattr arrive as ioctl on stdin (fd 0)
    // TCGETS = 0x5401, TCSETS/TCSETSW/TCSETSF = 0x5402-0x5404
    // A pipe is a TTY only if its FdEntry has flags bit 0x04 set (marked by
    // the terminal when setting up bash stdio). That way the terminal's
    // shell sees its stdio as a TTY (for readline, job control, etc.) but
    // anonymous pipes created by pipe() — e.g. the curl | xz | nar-unpack
    // pipeline — are not TTYs and isatty() returns false on them.
    bool isTtyFd = IsTerminalFd(fde);
    if (isTtyFd && cmd == 0x5401)
    {
        if (!UserBufferWritable(arg, 36)) return -EFAULT;
        auto* t = reinterpret_cast<uint32_t*>(arg);
        t[0] = 0x0500;   // c_iflag: ICRNL | IXON
        t[1] = 0x0005;   // c_oflag: OPOST | ONLCR
        t[2] = 0x00BF;   // c_cflag: CS8 | CREAD | HUPCL | B38400
        uint32_t lflag = 0x8A31; // base: ECHOE|ECHOK|ISIG|IEXTEN|ECHOCTL|ECHOKE
        if (proc->ttyCanonical) lflag |= 0x0002; // ICANON
        if (proc->ttyEcho)      lflag |= 0x0008; // ECHO
        t[3] = lflag;
        auto* raw = reinterpret_cast<uint8_t*>(&t[4]);
        raw[0] = 0;  // c_line = N_TTY
        auto* cc = &raw[1];
        __builtin_memset(cc, 0, 19);
        cc[0]  = 0x03;  // VINTR  = Ctrl+C
        cc[1]  = 0x1C;  // VQUIT  = Ctrl+backslash
        cc[2]  = 0x7F;  // VERASE = DEL
        cc[3]  = 0x15;  // VKILL  = Ctrl+U
        cc[4]  = 0x04;  // VEOF   = Ctrl+D
        cc[5]  = 0;     // VTIME  = 0
        cc[6]  = 1;     // VMIN   = 1
        cc[7]  = 0;     // VSWTC
        cc[8]  = 0x11;  // VSTART = Ctrl+Q
        cc[9]  = 0x13;  // VSTOP  = Ctrl+S
        cc[10] = 0x1A;  // VSUSP  = Ctrl+Z
        cc[11] = 0;     // VEOL
        cc[12] = 0x12;  // VREPRINT = Ctrl+R
        cc[13] = 0x0F;  // VDISCARD = Ctrl+O
        cc[14] = 0x17;  // VWERASE = Ctrl+W
        cc[15] = 0x16;  // VLNEXT  = Ctrl+V
        cc[16] = 0;     // VEOL2
        return 0;
    }
    if (isTtyFd && cmd >= 0x5402 && cmd <= 0x5404)
    {
        if (!UserBufferReadable(arg, 16)) return -EFAULT;
        auto* t = reinterpret_cast<const uint32_t*>(arg);
        Process* cur = ProcessCurrent();
        if (cur)
        {
            uint32_t lflag = t[3];
            cur->ttyCanonical = (lflag & 0x0002) != 0; // ICANON
            cur->ttyEcho      = (lflag & 0x0008) != 0; // ECHO
        }
        return 0;
    }

    // TIOCGPGRP = 0x540F — get foreground process group
    if (isTtyFd && cmd == 0x540F)
    {
        if (!UserBufferWritable(arg, sizeof(int))) return -EFAULT;
        auto* pgrp = reinterpret_cast<int*>(arg);
        Process* cur = ProcessCurrent();
        if (cur)
        {
            Terminal* term = TerminalFindByProcess(cur);
            if (term)
                *pgrp = static_cast<int>(term->foregroundPgid);
            else
                *pgrp = static_cast<int>(cur->pgid);
        }
        else
        {
            *pgrp = 1;
        }
        return 0;
    }

    // TIOCSPGRP = 0x5410 — set foreground process group
    if (isTtyFd && cmd == 0x5410)
    {
        if (!UserBufferReadable(arg, sizeof(int))) return -EFAULT;
        auto* pgrpPtr = reinterpret_cast<const int*>(arg);
        int newPgid = *pgrpPtr;
        Process* cur = ProcessCurrent();
        if (cur)
        {
            Terminal* term = TerminalFindByProcess(cur);
            if (term)
            {
                term->foregroundPgid = static_cast<uint16_t>(newPgid);
                DbgPrintf("TIOCSPGRP: pid %u set fg pgid to %d\n", cur->pid, newPgid);
            }
        }
        return 0;
    }

    // TIOCSCTTY = 0x540E — set controlling terminal
    if (isTtyFd && cmd == 0x540E)
        return 0;

    // TIOCGWINSZ = 0x5413 — terminal window size
    if (isTtyFd && cmd == 0x5413)
    {
        struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };
        if (!UserBufferWritable(arg, sizeof(winsize))) return -EFAULT;
        auto* ws = reinterpret_cast<winsize*>(arg);
        TerminalGeometry geometry{25,80,0,0};
        TerminalSnapshotGeometry(proc,&geometry);
        ws->ws_row = static_cast<uint16_t>(geometry.rows);
        ws->ws_col = static_cast<uint16_t>(geometry.cols);
        ws->ws_xpixel = static_cast<uint16_t>(geometry.width);
        ws->ws_ypixel = static_cast<uint16_t>(geometry.height);
        return 0;
    }

    // TIOCSWINSZ = 0x5414 — set terminal window size (accept but ignore)
    if (isTtyFd && cmd == 0x5414)
        return 0;

    // TCXONC = 0x540A — flow control (XON/XOFF), accept but ignore
    if (cmd == 0x540A)
        return 0;

    // TCFLSH = 0x540B — flush input/output, accept but ignore
    if (cmd == 0x540B)
        return 0;

    // ---------------------------------------------------------------------------
    // OSS /dev/dsp ioctls
    // ---------------------------------------------------------------------------
    if (fde->type == FdType::DevDsp && fde->handle)
    {
        auto* dsp = static_cast<DspState*>(fde->handle);

        AudioMixerRefresh(); // Driver query before taking the per-open control mutex.
        if (cmd == SNDCTL_DSP_SYNC) {
            uint64_t epoch, target, controlEpoch;
            {
                DspIoGuard held(dsp);
                AudioStreamProgress progress{};
                if (!AudioMixerGetProgress(dsp->mixerStreamId, &progress)) return -EBADF;
                if (!progress.clocked) return -95; // No fictitious legacy completion.
                if (progress.discontinuity) return -32;
                epoch = progress.epoch; target = progress.submittedFrames;
                controlEpoch = dsp->controlEpoch;
                // A finite paused queue must be started even without another write.
                AudioMixerSetEnabled(dsp->mixerStreamId, true);
            }
            uint64_t lastCompleted = 0, lastProgressTime = AudioClockNowNs();
            for (;;) {
                AudioMixerRefresh(); // Never hold the open-description mutex here.
                uint64_t completed;
                {
                    DspIoGuard held(dsp);
                    AudioStreamProgress progress{};
                    if (!AudioMixerGetProgress(dsp->mixerStreamId, &progress)) return -EBADF;
                    if (progress.epoch != epoch || dsp->controlEpoch != controlEpoch)
                        return -125; // ECANCELED: reset, pause or reconfiguration.
                    if (progress.discontinuity) return -32;
                    if (progress.completedFrames >= target) return 0;
                    completed = progress.completedFrames;
                }
                if (HasReadInterruptSignal()) return -EINTR;
                const uint64_t now = AudioClockNowNs();
                if (!now || !lastProgressTime || now < lastProgressTime) return -EIO;
                if (completed != lastCompleted) { lastCompleted = completed; lastProgressTime = now; }
                else if (now - lastProgressTime >= 2000000000ULL) return -EIO;
                Process* self = SchedulerCurrentProcess();
                if (self) {
                    self->wakeupTick = ApicTickCount() + 5;
                    SchedulerBlockInterruptible(self);
                }
            }
        }
        AudioStreamProgress beforeControl{};
        if (!AudioMixerGetProgress(dsp->mixerStreamId, &beforeControl)) return -EBADF;
        const uint32_t legacyDelayFrames = cmd == SNDCTL_DSP_GETODELAY && !beforeControl.clocked
            ? AudioGetPosition() / MIXER_FRAME_BYTES : 0;
        DspIoGuard held(dsp);
        AudioStreamProgress progress{};
        if (!AudioMixerGetProgress(dsp->mixerStreamId, &progress)) return -EBADF;
        if (cmd == SNDCTL_DSP_RESET) {
            AudioMixerReset(dsp->mixerStreamId);
            dsp->bufferOffset = 0;
            dsp->pointerEpoch = dsp->pointerBaseFrames = 0;
            ++dsp->controlEpoch;
            return 0;
        }
        if (cmd == SNDCTL_DSP_GETOPTR) {
            if (progress.discontinuity) return -32;
            // The current write-local resampler has no exact cumulative mapping
            // back to input frames. Expose a pointer only for its native rate.
            if (!progress.clocked || dsp->sampleRate != MIXER_HW_RATE) return -95;
            const bool sameEpoch = dsp->pointerEpoch == progress.epoch;
            const uint64_t base = sameEpoch ? dsp->pointerBaseFrames : 0;
            if (progress.completedFrames < base) return -32;
            const uint64_t frameBytes = dsp->channels * (dsp->bitsPerSample / 8);
            const uint64_t frames = progress.completedFrames - base;
            if (frames > ~uint64_t(0) / frameBytes) return -75; // EOVERFLOW
            const uint64_t bytes = frames * frameBytes;
            // Linux non-mmap OSS reports queued whole fragments in blocks;
            // the query-delta variant belongs to mmap, which is unsupported.
            const uint64_t blocks = uint64_t(progress.queuedFrames) * frameBytes / dsp->fragmentSize;
            struct CountInfo { int bytes, blocks, ptr; } info{};
            info.bytes = static_cast<int>(bytes & 0x7fffffffULL);
            info.blocks = static_cast<int>(blocks);
            info.ptr = static_cast<int>(bytes % (uint64_t(progress.capacityFrames) * frameBytes));
            if (!CopyToUser(arg, &info, sizeof(info))) return -EFAULT;
            dsp->pointerEpoch = progress.epoch;
            dsp->pointerBaseFrames = base;
            return 0;
        }
        if (cmd == BROOK_DSP_GETQUEUE) {
            // Exact native units only: converted OSS frame rounding cannot
            // represent reusable storage and physical completion coherently.
            if (!progress.clocked || dsp->sampleRate != MIXER_HW_RATE ||
                dsp->channels != 2 || dsp->bitsPerSample != 16) return -95;
            const auto state=AudioStreamQueueSnapshot(progress);
            return CopyToUser(arg,&state,sizeof(state))?0:-EFAULT;
        }
        if (cmd == SNDCTL_DSP_GETOSPACE) {
            struct AudioBufInfo { int fragments, fragstotal, fragsize, bytes; } info{};
            // Software plus in-flight capacity in negotiated input bytes.
            const uint64_t frameBytes = dsp->channels * (dsp->bitsPerSample / 8);
            const uint64_t capacity = static_cast<uint64_t>(progress.capacityFrames) * dsp->sampleRate / MIXER_HW_RATE * frameBytes;
            const uint64_t available = static_cast<uint64_t>(AudioMixerAvailableFrames(dsp->mixerStreamId)) * dsp->sampleRate / MIXER_HW_RATE * frameBytes;
            info.fragsize = static_cast<int>(dsp->fragmentSize);
            info.fragstotal = static_cast<int>(capacity / dsp->fragmentSize);
            info.fragments = static_cast<int>(available / dsp->fragmentSize);
            info.bytes = static_cast<int>(available);
            return CopyToUser(arg, &info, sizeof(info)) ? 0 : -EFAULT;
        }
        int value = 0;
        const bool input = cmd == SNDCTL_DSP_SPEED || cmd == SNDCTL_DSP_SETFMT ||
            cmd == SNDCTL_DSP_CHANNELS || cmd == SNDCTL_DSP_STEREO ||
            cmd == SNDCTL_DSP_SETFRAGMENT || cmd == SNDCTL_DSP_SETTRIGGER ||
            cmd == SNDCTL_DSP_LOW_WATER;
        if (input && !CopyFromUser(&value, arg, sizeof(value))) return -EFAULT;
        if (cmd == SNDCTL_DSP_SPEED) {
            if (value > 0) {
                // Report a realizable conversion rate and bounded OSS geometry.
                if (value < 4000) value = 4000;
                if (value > 192000) value = 192000;
                if (static_cast<uint32_t>(value) != dsp->sampleRate && progress.queuedFrames) return -16;
                if (static_cast<uint32_t>(value) != dsp->sampleRate) {
                    const int error = DspChangeFormat(dsp, progress, value, dsp->channels, dsp->bitsPerSample);
                    if (error) return error;
                }
            }
            value = static_cast<int>(dsp->sampleRate);
        } else if (cmd == SNDCTL_DSP_SETFMT) {
            uint8_t bits = value == AFMT_S16_LE ? 16 : value == AFMT_U8 ? 8 : dsp->bitsPerSample;
            if (bits != dsp->bitsPerSample && progress.queuedFrames) return -16;
            if (bits != dsp->bitsPerSample) {
                const int error = DspChangeFormat(dsp, progress, dsp->sampleRate, dsp->channels, bits);
                if (error) return error;
            }
            value = dsp->bitsPerSample == 16 ? AFMT_S16_LE : AFMT_U8;
        } else if (cmd == SNDCTL_DSP_CHANNELS) {
            if (value >= 1 && value <= 2) {
                if (value != dsp->channels && progress.queuedFrames) return -16;
                if (value != dsp->channels) {
                    const int error = DspChangeFormat(dsp, progress, dsp->sampleRate, value, dsp->bitsPerSample);
                    if (error) return error;
                }
            }
            value = dsp->channels;
        } else if (cmd == SNDCTL_DSP_STEREO) {
            const uint8_t channels = value ? 2 : 1;
            if (channels != dsp->channels && progress.queuedFrames) return -16;
            if (channels != dsp->channels) {
                const int error = DspChangeFormat(dsp, progress, dsp->sampleRate, channels, dsp->bitsPerSample);
                if (error) return error;
            }
            value = dsp->channels == 2;
        } else if (cmd == SNDCTL_DSP_GETODELAY) {
            if (progress.discontinuity) return -32;
            uint64_t frames = progress.queuedFrames;
            // Unclocked drivers still expose the legacy estimate; a clocked
            // endpoint charges only this description's incomplete contribution.
            if (!progress.clocked) frames += legacyDelayFrames;
            uint64_t bytes = (frames * dsp->sampleRate + MIXER_HW_RATE - 1) / MIXER_HW_RATE * dsp->channels * (dsp->bitsPerSample / 8);
            value = static_cast<int>(bytes > 0x7fffffffULL ? 0x7fffffffULL : bytes);
        } else if (cmd == SNDCTL_DSP_GETCAPS) value = DSP_CAP_TRIGGER;
        else if (cmd == SNDCTL_DSP_GETFMTS) value = AFMT_U8 | AFMT_S16_LE;
        else if (cmd == SNDCTL_DSP_SETFRAGMENT) {
            if (progress.submittedFrames) return -16; // RESET before changing pointer geometry.
            if (dsp->sampleRate != MIXER_HW_RATE) return -95;
            const uint32_t frameBytes = dsp->channels * (dsp->bitsPerSample / 8);
            DspGeometry geometry;
            if(!DspPlanGeometry(static_cast<uint32_t>(value),frameBytes,MIXER_RING_FRAMES,
                                AudioMixerHorizonEnabled(),&geometry))return -EINVAL;
            if (!AudioMixerSetCapacity(dsp->mixerStreamId, geometry.capacityFrames, geometry.writableFrames)) return -16;
            dsp->fragmentSize = geometry.fragmentBytes; dsp->ringBytes = geometry.totalBytes;
            ++dsp->controlEpoch;
            return 0; // Actual negotiated size/count are reported by GETBLKSIZE/GETOSPACE.
        } else if (cmd == SNDCTL_DSP_LOW_WATER) {
            // This endpoint's readiness accounting is in native mixer frames.
            // Do not silently reinterpret converted-rate byte counts.
            if (!progress.clocked || dsp->sampleRate != MIXER_HW_RATE) return -95;
            const uint32_t frameBytes = dsp->channels * (dsp->bitsPerSample / 8);
            if (value <= 0 || static_cast<uint32_t>(value) % frameBytes ||
                static_cast<uint32_t>(value) / frameBytes > progress.capacityFrames) return -EINVAL;
            if (!AudioMixerSetWritableFrames(dsp->mixerStreamId, static_cast<uint32_t>(value) / frameBytes)) return -EBADF;
            return 0; // _IOW: no output copy and no queue/pointer reset.
        } else if (cmd == SNDCTL_DSP_GETBLKSIZE) value = static_cast<int>(dsp->fragmentSize);
        else if (cmd == SNDCTL_DSP_SETTRIGGER) {
            if (value & ~2) return -95; // EOPNOTSUPP: output only; no capture endpoint.
            if (!(value & 2)) ++dsp->controlEpoch;
            AudioMixerSetEnabled(dsp->mixerStreamId, (value & 2) != 0);
            return 0;
        } else {
            SerialPrintf("sys_ioctl: dsp unknown cmd 0x%lx\n", cmd);
            return -EINVAL;
        }
        return CopyToUser(arg, &value, sizeof(value)) ? 0 : -EFAULT;
    }

    // TCGETS2 = 0x802c542a — termios2 (like TCGETS plus ispeed/ospeed uint32s)
    if (isTtyFd && cmd == 0x802c542aUL)
    {
        // termios2: 4 × uint32 flags + 1 c_line + 19 c_cc + 2 × uint32 speeds = 44 bytes
        if (!UserBufferWritable(arg, 44)) return -EFAULT;
        auto* t = reinterpret_cast<uint32_t*>(arg);
        t[0] = 0x0500;
        t[1] = 0x0005;
        t[2] = 0x00BF;
        uint32_t lflag = 0x8A31;
        if (proc->ttyCanonical) lflag |= 0x0002;
        if (proc->ttyEcho)      lflag |= 0x0008;
        t[3] = lflag;
        auto* raw = reinterpret_cast<uint8_t*>(&t[4]);
        raw[0] = 0;
        auto* cc = &raw[1];
        __builtin_memset(cc, 0, 19);
        cc[0]=0x03; cc[1]=0x1C; cc[2]=0x7F; cc[3]=0x15; cc[4]=0x04;
        cc[5]=0;    cc[6]=1;    cc[7]=0;    cc[8]=0x11; cc[9]=0x13;
        cc[10]=0x1A;cc[11]=0;   cc[12]=0x12;cc[13]=0x0F;cc[14]=0x17;
        cc[15]=0x16;cc[16]=0;
        // termios2 appends c_ispeed, c_ospeed (uint32 each) at offset 20 in cc-block
        uint32_t* speeds = reinterpret_cast<uint32_t*>(&raw[20]);
        speeds[0] = 38400;
        speeds[1] = 38400;
        return 0;
    }

    // Any unhandled 'T'-type ioctl (termios family) on a non-tty or for
    // commands we don't implement -> ENOTTY is the correct POSIX reply.
    // Go runtime uses this to detect isatty; returning ENOSYS makes it loop.
    if (((cmd >> 8) & 0xFF) == 'T')
    {
        return -25; // ENOTTY
    }

    SerialPrintf("sys_ioctl: unhandled fd=%lu cmd=0x%lx\n", fd, cmd);
    return -ENOSYS;
}

// ---------------------------------------------------------------------------
// sys_stat (4) / sys_lstat (6) / sys_fstat (5) / sys_newfstatat (262)
// ---------------------------------------------------------------------------

// Linux x86-64 struct stat layout (musl)
struct LinuxStat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime_sec;
    uint64_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};

static void FillStat(LinuxStat* st, const VnodeStat& vs, const Vnode* vn = nullptr)
{
    auto* raw = reinterpret_cast<uint8_t*>(st);
    memset(raw, 0, sizeof(LinuxStat));

    // Use the stable inode number when available (from ext2 or vnode cacheId).
    // This is critical: the dynamic linker uses dev+ino to detect
    // already-loaded libraries.  The same file must return the same ino
    // across multiple stat calls; otherwise libraries get loaded twice.
    uint64_t ino = vs.ino;
    if (!ino && vn && vn->cacheId)
        ino = vn->cacheId;
    if (ino) {
        st->st_ino = ino;
    } else {
        // Fallback for filesystems without stable inode numbers (FATFS, procfs)
        static volatile uint64_t s_nextIno = 100;
        st->st_ino = __atomic_fetch_add(&s_nextIno, 1, __ATOMIC_RELAXED);
    }
    st->st_dev = vs.dev;
    st->st_nlink = vs.nlink;
    st->st_blksize = 4096;
    st->st_size = static_cast<int64_t>(vs.size);
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_uid = ProcessReportId(ProcessCurrent(),false,vs.uid);
    st->st_gid = ProcessReportId(ProcessCurrent(),true,vs.gid);
    st->st_atime_sec = vs.atime.sec;
    st->st_atime_nsec = vs.atime.nsec;
    st->st_mtime_sec = vs.mtime.sec;
    st->st_mtime_nsec = vs.mtime.nsec;
    st->st_ctime_sec = vs.ctime.sec;
    st->st_ctime_nsec = vs.ctime.nsec;

    // Use real mode from VnodeStat if available, otherwise generate from flags
    if (vs.mode)
        st->st_mode = vs.mode;
    else if (vs.isSymlink)
        st->st_mode = 0120777; // S_IFLNK | rwxrwxrwx
    else if (vs.isDir)
        st->st_mode = 0040755; // S_IFDIR | rwxr-xr-x
    else
        st->st_mode = 0100755; // S_IFREG | rwxr-xr-x
}

// Busybox-aware stat: if a file doesn't exist in /boot/BIN/ but busybox does,
// return busybox's stat info. This lets bash's PATH search find busybox applets.
static bool BusyboxStatFallback(const char* path, VnodeStat* vs)
{
    // Check if path is under a bin-like directory
    const char* binPrefixes[] = {
        "/boot/BIN/", "/boot/bin/", "/bin/", "/usr/bin/",
        "/usr/local/bin/", "/sbin/", "/usr/sbin/"
    };
    bool isBinPath = false;
    for (int i = 0; i < 7; ++i)
    {
        const char* pfx = binPrefixes[i];
        const char* a = path;
        const char* b = pfx;
        while (*b && *a == *b) { a++; b++; }
        // Check prefix matches and remainder has no slashes (is a simple filename)
        if (!*b && *a)
        {
            bool hasSlash = false;
            for (const char* c = a; *c; ++c) { if (*c == '/') { hasSlash = true; break; } }
            if (!hasSlash)
            {
                isBinPath = true;
                break;
            }
        }
    }
    if (!isBinPath) return false;
    const char* applet = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/') applet = p + 1;
    if (!BusyboxHasApplet(applet)) return false;

    static const char* busyboxPaths[] = {
        "/boot/BIN/busybox", "/boot/BIN/BUSYBOX",
        "/boot/bin/busybox", "/bin/busybox"
    };
    for (int i = 0; i < 4; i++)
    {
        if (VfsStatPath(busyboxPaths[i], vs) == 0)
            return true;
    }
    return false;
}

// GL shim (M0): fill a LinuxStat for the synthetic DRM devfs/sysfs entries
// (/dev/dri[/renderD128], /sys/dev/char/226:128/device/...). Returns true if
// `path` is a DRM-shim entry (and fills *st), false otherwise. Gated on a 3D
// driver being present so we never advertise an unusable node.
static bool DrmStatFill(const char* path, LinuxStat* st)
{
    if (!brook::GpuAppGet()) return false;
    if (StrEq(path, "/dev/dri/renderD128"))
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0020666; // S_IFCHR | rw-rw-rw-
        st->st_rdev = DRM_RENDER_RDEV;
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return true;
    }
    if (StrEq(path, "/dev/dri") || DrmSysIsDir(path))
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0040755; // S_IFDIR | rwxr-xr-x
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return true;
    }
    const char* drmAttr = DrmSysAttrContent(path);
    if (drmAttr)
    {
        uint64_t len = 0; while (drmAttr[len]) ++len;
        memset(st, 0, sizeof(*st));
        st->st_mode = 0100444; // S_IFREG | r--r--r--
        st->st_nlink = 1;
        st->st_size = static_cast<int64_t>(len);
        st->st_blksize = 4096;
        st->st_blocks = 1;
        return true;
    }
    return false;
}

// Internal stat helper — takes a kernel-space path directly (no user copy).
// statAddr is the user-space address of the stat buffer.
static int64_t do_stat_internal(const char* path, uint64_t statAddr,int dirfd=-100)
{
    if (!UserBufferWritable(statAddr, sizeof(LinuxStat))) return -EFAULT;
    auto* st = reinterpret_cast<LinuxStat*>(statAddr);

    // Resolve relative paths (including ".") against CWD
    AtLookup at;if(int error=at.Init(dirfd,path))return error;
    char resolved[512];bool hasSyntheticName=!at.SyntheticName(path,resolved,sizeof(resolved));
    const char* lookup=hasSyntheticName?resolved:"";

    // GL shim (M0): stat the synthetic DRM devfs/sysfs entries so libdrm can
    // enumerate the render node. Only when a 3D driver is present.
    if (DrmStatFill(lookup, st))
        return 0;

    VnodeStat vs{};
    int pathError=VfsStatAt(at.fs.root,at.start(),path,&vs,true);
    if (pathError < 0)
    {
        if (pathError != -ENOENT && pathError != -1) return pathError;
        // Synthetic proc/etc files — stat should succeed for these
        static const char* syntheticPaths[] = {
            "/etc/passwd", "/etc/group", "/etc/shells", "/etc/hostname",
            "/etc/nsswitch.conf", "/etc/hosts", "/etc/resolv.conf",
            "/proc/version", nullptr
        };
        bool isSynthetic = false;
        for (auto** sp = syntheticPaths; *sp; ++sp) {
            if (StrEq(lookup, *sp)) { isSynthetic = true; break; }
        }
        if (isSynthetic) {
            // Return a plausible stat for synthetic files
            __builtin_memset(st, 0, sizeof(*st));
            st->st_mode = 0100444; // regular file, read-only
            st->st_nlink = 1;
            st->st_size = StrEq(lookup, "/etc/passwd") ? sizeof(DesktopPasswd) - 1 :
                          StrEq(lookup, "/etc/group") ? sizeof(DesktopGroup) - 1 : 64;
            st->st_blksize = 4096;
            st->st_blocks = 1;
            return 0;
        }
        if (!BusyboxStatFallback(lookup, &vs))
            return -ENOENT;
    }

    FillStat(st, vs);
    return 0;
}

// Internal lstat helper — takes a kernel-space path directly.
static int64_t do_lstat_internal(const char* path, uint64_t statAddr,int dirfd=-100)
{
    if (!UserBufferWritable(statAddr, sizeof(LinuxStat))) return -EFAULT;
    auto* st = reinterpret_cast<LinuxStat*>(statAddr);

    AtLookup at;if(int error=at.Init(dirfd,path))return error;
    char resolved[512];bool hasSyntheticName=!at.SyntheticName(path,resolved,sizeof(resolved));
    const char* lookup=hasSyntheticName?resolved:"";

    // GL shim (M0): the synthetic DRM entries are not symlinks, so lstat
    // returns the same as stat (lets glibc realpath() resolve the device dir).
    if (DrmStatFill(lookup, st))
        return 0;

    VnodeStat vs{};
    int pathError=VfsStatAt(at.fs.root,at.start(),path,&vs,false);
    if (pathError < 0)
    {
        if (pathError != -ENOENT && pathError != -1) return pathError;
        // Synthetic proc/etc files
        static const char* syntheticPaths[] = {
            "/etc/passwd", "/etc/group", "/etc/shells", "/etc/hostname",
            "/etc/nsswitch.conf", "/etc/hosts", "/etc/resolv.conf",
            "/proc/version", nullptr
        };
        bool isSynthetic = false;
        for (auto** sp = syntheticPaths; *sp; ++sp) {
            if (StrEq(lookup, *sp)) { isSynthetic = true; break; }
        }
        if (isSynthetic) {
            __builtin_memset(st, 0, sizeof(*st));
            st->st_mode = 0100444;
            st->st_nlink = 1;
            st->st_size = StrEq(lookup, "/etc/passwd") ? sizeof(DesktopPasswd) - 1 :
                          StrEq(lookup, "/etc/group") ? sizeof(DesktopGroup) - 1 : 64;
            st->st_blksize = 4096;
            st->st_blocks = 1;
            return 0;
        }
        if (!BusyboxStatFallback(lookup, &vs))
            return -ENOENT;
    }

    FillStat(st, vs);
    return 0;
}

static int64_t sys_stat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    char pathBuf[256];
    if (!CopyUserCString(pathAddr, pathBuf, sizeof(pathBuf))) return -EFAULT;
    return do_stat_internal(pathBuf, statAddr);
}

static int64_t sys_lstat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    char pathBuf[256];
    if (!CopyUserCString(pathAddr, pathBuf, sizeof(pathBuf))) return -EFAULT;
    return do_lstat_internal(pathBuf, statAddr);
}

static int64_t sys_fstat(uint64_t fd, uint64_t statAddr, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    auto* st = reinterpret_cast<LinuxStat*>(statAddr);

    Process* proc = ProcessCurrent();
    if (!proc || fd >= MAX_FDS) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if (!fde) return -EBADF;

    if (IsTerminalFd(fde)) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0020666; // S_IFCHR | rw-rw-rw-
        st->st_rdev = (fde->type == FdType::Pipe || fde->type == FdType::DevTty)
            ? 0x8800 : 0x0400;
        st->st_blksize = 4096;
        return 0;
    }

    if ((fde->type == FdType::Vnode || fde->type == FdType::Path) && fde->handle) {
        auto* vn = static_cast<Vnode*>(fde->handle);
        VnodeStat vs{};
        if (VfsStat(vn, &vs) < 0) return -EBADF;
        FillStat(st, vs, vn);
        return 0;
    }

    if (fde->type == FdType::DevFramebuf) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0020666; // S_IFCHR
        st->st_rdev = 0x1D00;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::DevKeyboard || fde->type == FdType::DevNull ||
        fde->type == FdType::DevUrandom || fde->type == FdType::DevDsp ||
        fde->type == FdType::DebugSerial) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0020666; // S_IFCHR
        st->st_rdev = (fde->type == FdType::DevNull) ? 0x0103 :
                      (fde->type == FdType::DevUrandom) ? 0x0109 :
                      (fde->type == FdType::DevDsp) ? 0x0E03 :
                      (fde->type == FdType::DebugSerial) ? 0x0440 : 0x0400; // 4,64 = UART
        st->st_blksize = 4096;
        return 0;
    }

    // GL shim: /dev/dri/renderD128 is a char device (libdrm fstats the open fd
    // to recover major:minor for its sysfs lookups).
    if (fde->type == FdType::DevDri) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0020666; // S_IFCHR | rw-rw-rw-
        st->st_rdev = DRM_RENDER_RDEV;
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::Pipe) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0010666; // S_IFIFO | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::SyntheticMem) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        // GL shim: the /dev/dri synthetic directory fd reports as a directory
        // (opendir() fstats the fd and requires S_ISDIR).
        if (fde->handle == g_drmDriDirTag) {
            st->st_mode = 0040755; // S_IFDIR | rwxr-xr-x
            st->st_blksize = 4096;
            return 0;
        }
        st->st_mode = 0100444; // S_IFREG | r--r--r--
        if (fde->handle) {
            auto* content = static_cast<const char*>(fde->handle);
            uint64_t len = 0;
            while (content[len]) len++;
            st->st_size = len;
        }
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::Socket) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0140666; // S_IFSOCK | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::EventFd || fde->type==FdType::SyncFile || fde->type==FdType::SignalFd) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0100666; // S_IFREG | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::EpollFd) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0100666;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::TimerFd) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0100666;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::MemFd && fde->handle) {
        auto* mfd = static_cast<MemFdData*>(fde->handle);
        MfdLock(mfd);
        uint64_t logicalSize = mfd->size;
        uint64_t allocatedBlocks = 0;
        for (uint64_t index = 0; index < mfd->pageMapCount; ++index)
            if (mfd->pageMap[index]) allocatedBlocks += 4096 / 512;
        MfdUnlock(mfd);
        // User-buffer writes stay outside the backing lock: they may fault.
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_dev = 0x01000000; // anonymous shmem namespace, distinct from disk mounts
        st->st_ino = mfd->inode;
        st->st_nlink = 0;
        st->st_mode = 0100666; // S_IFREG | rw-rw-rw-
        st->st_size = static_cast<int64_t>(logicalSize);
        st->st_blksize = 4096;
        st->st_blocks  = static_cast<int64_t>(allocatedBlocks);
        return 0;
    }

    if (fde->type == FdType::UnixSocket) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        memset(raw, 0, sizeof(LinuxStat));
        st->st_mode = 0140666; // S_IFSOCK | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    return -EBADF;
}

static int64_t sys_newfstatat(uint64_t dirfd, uint64_t pathAddr, uint64_t statAddr,
                               uint64_t flags, uint64_t, uint64_t)
{
    char pathBuf[256];
    if (!CopyUserCString(pathAddr, pathBuf, sizeof(pathBuf)))
    {
        // Empty path or copy failure — try fstat on dirfd
        if (!pathAddr) return sys_fstat(dirfd, statAddr, 0, 0, 0, 0);
        return -EFAULT;
    }
    const char* path = pathBuf;
    if (path[0] == '\0')
        return sys_fstat(dirfd, statAddr, 0, 0, 0, 0);

    static constexpr uint64_t AT_SYMLINK_NOFOLLOW = 0x100;
    bool noFollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;

    if(noFollow)return do_lstat_internal(path,statAddr,static_cast<int>(dirfd));
    return do_stat_internal(path,statAddr,static_cast<int>(dirfd));
}

// ---------------------------------------------------------------------------
// sys_getdents64 (217) -- directory listing
// ---------------------------------------------------------------------------

struct LinuxDirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

static int64_t sys_getdents64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                               uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef pinned(proc,static_cast<int>(fd));
    if(!pinned)return -EBADF;
    FdEntry* fde=pinned.get();

    // Synthetic /dev/dri directory (GL shim M0): list ".", "..", "renderD128"
    // so libdrm's drmGetDevices2 can discover the render node.
    if (fde->type == FdType::SyntheticMem && fde->handle == g_drmDriDirTag)
    {
        static const char* driEntries[] = { ".", "..", "renderD128" };
        auto* obuf = reinterpret_cast<uint8_t*>(bufAddr);
        uint64_t opos = 0;
        uint32_t ck = static_cast<uint32_t>(fde->seekPos);
        while (ck < 3)
        {
            const char* nm = driEntries[ck];
            uint64_t nameLen = 0;
            while (nm[nameLen]) ++nameLen;
            uint64_t reclen = (19 + nameLen + 1 + 7) & ~7ULL;
            if (opos + reclen > count) break;
            auto* ent = reinterpret_cast<LinuxDirent64*>(obuf + opos);
            ent->d_ino = ck + 1;
            ent->d_off = static_cast<int64_t>(ck + 1);
            ent->d_reclen = static_cast<uint16_t>(reclen);
            ent->d_type = (ck < 2) ? 4 : 2; // DT_DIR for . / .. ; DT_CHR for the node
            for (uint64_t i = 0; i < nameLen; ++i) ent->d_name[i] = nm[i];
            for (uint64_t i = nameLen; i < reclen - 19; ++i) ent->d_name[i] = '\0';
            opos += reclen;
            ++ck;
        }
        fde->seekPos = ck;
        return static_cast<int64_t>(opos);
    }

    if (fde->type != FdType::Vnode || !fde->handle) return -EBADF;
    auto* vn = static_cast<Vnode*>(fde->handle);
    auto* buf = reinterpret_cast<uint8_t*>(bufAddr);
    uint64_t pos = 0;
    uint32_t cookie = static_cast<uint32_t>(fde->seekPos);

    DirEntry de; de.name[0] = 0; de.size = 0; de.isDir = false;
    while (pos < count) {
        uint32_t nextCookie = cookie;
        int ret=VfsReaddir(vn,&de,&nextCookie);
        if(ret<0) {if(!pos)return ret;break;}
        if(!ret)break;

        uint64_t nameLen = 0;
        while (de.name[nameLen] && nameLen < 255) ++nameLen;

        // d_name starts at offset 19 in LinuxDirent64
        uint64_t reclen = (19 + nameLen + 1 + 7) & ~7ULL;
        if (pos + reclen > count) {
            if (pos == 0) return -EINVAL;
            break;
        }
        cookie = nextCookie;

        auto* ent = reinterpret_cast<LinuxDirent64*>(buf + pos);
        ent->d_ino = cookie + 1;
        ent->d_off = static_cast<int64_t>(cookie);
        ent->d_reclen = static_cast<uint16_t>(reclen);
        // DirEntry only distinguishes directories. Other entries can be
        // symlinks or special files: DT_UNKNOWN asks callers to use lstat,
        // whereas falsely claiming DT_REG breaks ldconfig's inode cache.
        ent->d_type = de.isDir ? 4 : 0; // DT_DIR : DT_UNKNOWN

        for (uint64_t i = 0; i < nameLen; ++i) ent->d_name[i] = de.name[i];
        ent->d_name[nameLen] = '\0';
        for (uint64_t i = nameLen + 1; i < reclen - 19; ++i)
            ent->d_name[i] = '\0';

        pos += reclen;
    }

    fde->seekPos = cookie;
    return static_cast<int64_t>(pos);
}

static int64_t sys_sync(uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t)
{
    return VfsSync(nullptr);
}
#include "syscall_mount_namespace.inc"

static int64_t sys_fsync(uint64_t fd, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if (!fde || fde->type == FdType::None || fde->type==FdType::Path) return -EBADF;

    if (fde->type == FdType::Vnode && fde->handle)
        return VfsFsync(static_cast<Vnode*>(fde->handle));

    // Devices, sockets, pipes, memfds, and synthetic files have no persistent
    // backing store in Brook yet. Media stacks often call fdatasync() as a
    // harmless flush hint; treating it as complete avoids false fatal errors.
    return 0;
}

// ---------------------------------------------------------------------------
// Identity syscalls: getuid/getgid/geteuid/getegid/setuid/setgid
// ---------------------------------------------------------------------------

static int64_t sys_getuid(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? ProcessReportId(proc,false,proc->uid) : 0;
}
static int64_t sys_getgid(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? ProcessReportId(proc,true,proc->gid) : 0;
}
static int64_t sys_geteuid(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? ProcessReportId(proc,false,proc->euid) : 0;
}
static int64_t sys_getegid(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? ProcessReportId(proc,true,proc->egid) : 0;
}
static int64_t ChangeIds(bool group, CredentialChange operation,
                         uint64_t real, uint64_t effective = CredentialUnchanged,
                         uint64_t saved = CredentialUnchanged) {
    auto* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    return ProcessChangeIds(proc, group, operation, static_cast<uint32_t>(real),
                            static_cast<uint32_t>(effective), static_cast<uint32_t>(saved));
}
static int64_t sys_setuid(uint64_t id, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(false, CredentialChange::Single, id);
}
static int64_t sys_setgid(uint64_t id, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(true, CredentialChange::Single, id);
}
static int64_t sys_setreuid(uint64_t real, uint64_t effective, uint64_t, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(false, CredentialChange::Re, real, effective);
}
static int64_t sys_setregid(uint64_t real, uint64_t effective, uint64_t, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(true, CredentialChange::Re, real, effective);
}
static int64_t sys_setresuid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(false, CredentialChange::Res, real, effective, saved);
}
static int64_t sys_setresgid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(true, CredentialChange::Res, real, effective, saved);
}

// Empty-capability API for tasks without retained root IDs. The current UID
// rules cannot introduce zero into an all-nonzero tuple, and exec grants no
// set-ID/file capabilities. Revisit this invariant before adding either feature.
// Root-capable tasks remain unsupported: do not pretend an empty capset drops
// their UID-based privileges. Bounding/ambient sets and securebits are separate.
static bool EmptyCapabilitiesSupported(Process* proc) {
    CredentialIds ids = ProcessReadIds(proc, false);
    return ids.real != 0 && ids.effective != 0 && ids.saved != 0;
}
static int CapabilityWords(uint64_t header, bool queryOnly) {
    if (!UserBufferReadable(header, sizeof(uint32_t))) return -EFAULT;
    uint32_t version;
    memcpy(&version, reinterpret_cast<const void*>(header), sizeof(version));
    if (version == 0x19980330U) return 3; // V1: one 3-word structure.
    if (version == 0x20071026U || version == 0x20080522U) return 6; // V2/V3.
    if (!UserBufferWritable(header, sizeof(uint32_t))) return -EFAULT;
    version = 0x20080522U;
    memcpy(reinterpret_cast<void*>(header), &version, sizeof(version));
    // Linux permits version discovery using capget(header, NULL).
    return queryOnly ? 0 : -EINVAL;
}
static int64_t sys_capget(uint64_t header, uint64_t data, uint64_t,
                          uint64_t, uint64_t, uint64_t) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    UserCapabilityState capabilities{};bool privateCaps=ProcessReadCapabilities(proc,&capabilities)==0;
    if (!privateCaps && !EmptyCapabilitiesSupported(proc)) return -ENOSYS;
    int words = CapabilityWords(header, !data);
    if (words <= 0) return words;
    if (!data) return 0; // Valid-version null data does not require the PID field.
    if (!UserBufferReadable(header + sizeof(uint32_t), sizeof(int32_t))) return -EFAULT;
    int32_t pid;
    memcpy(&pid, reinterpret_cast<const void*>(header + sizeof(uint32_t)), sizeof(pid));
    if (pid < 0) return -EINVAL;
    if (pid && pid != proc->pid) return -ENOSYS; // Foreign queries not implemented.
    if (!UserBufferWritable(data, words * sizeof(uint32_t))) return -EFAULT;
    uint32_t values[6]={};
    if(privateCaps)for(unsigned i=0;i<unsigned(words/3);++i) {
        values[i*3]=capabilities.effective>>(i*32);values[i*3+1]=capabilities.permitted>>(i*32);values[i*3+2]=capabilities.inheritable>>(i*32);
    }
    memcpy(reinterpret_cast<void*>(data),values,words*sizeof(uint32_t));return 0;
}
static int64_t sys_capset(uint64_t header, uint64_t data, uint64_t,
                          uint64_t, uint64_t, uint64_t) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    UserCapabilityState capabilities{};bool privateCaps=ProcessReadCapabilities(proc,&capabilities)==0;
    if (!privateCaps && !EmptyCapabilitiesSupported(proc)) return -ENOSYS;
    int words = CapabilityWords(header, false);
    if (words < 0) return words;
    if (!UserBufferReadable(header + sizeof(uint32_t), sizeof(int32_t))) return -EFAULT;
    int32_t pid;
    memcpy(&pid, reinterpret_cast<const void*>(header + sizeof(uint32_t)), sizeof(pid));
    if (pid && pid != proc->pid) return -EPERM;
    uint32_t values[6] = {};
    if (!UserBufferReadable(data, words * sizeof(uint32_t))) return -EFAULT;
    memcpy(values, reinterpret_cast<const void*>(data), words * sizeof(uint32_t));
    if(privateCaps) {
        const uint64_t effective=values[0]|(uint64_t{values[3]}<<32),permitted=values[1]|(uint64_t{values[4]}<<32),inheritable=values[2]|(uint64_t{values[5]}<<32);
        return ProcessSetCapabilities(proc,effective,permitted,inheritable);
    }
    for (int i = 0; i < words; ++i) if (values[i]) return -EPERM;
    return 0; // An already-empty state remains empty.
}

static int64_t sys_getgroups(uint64_t size, uint64_t listAddr, uint64_t,
                              uint64_t, uint64_t, uint64_t) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    struct Snapshot {
        SupplementaryGroups* groups;
        ~Snapshot() { ProcessGroupsRelease(groups); }
    } snapshot{ProcessGroupsAcquire(proc)};
    const uint32_t count = snapshot.groups ? snapshot.groups->count : 0;
    const int capacity = static_cast<int32_t>(size);
    if (capacity < 0 || (capacity && static_cast<uint32_t>(capacity) < count)) return -EINVAL;
    if (!capacity) return count;
    if (!UserBufferWritable(listAddr, uint64_t(count) * sizeof(uint32_t))) return -EFAULT;
    for(uint32_t i=0;i<count;++i)reinterpret_cast<uint32_t*>(listAddr)[i]=ProcessReportId(proc,true,snapshot.groups->ids[i]);
    return count;
}
static void SortGroupIds(uint32_t* ids, uint32_t count) {
    auto sift = [&](uint32_t root, uint32_t end) {
        while (root < end / 2) {
            uint32_t child = root * 2 + 1;
            if (child + 1 < end && ids[child] < ids[child + 1]) ++child;
            if (ids[root] >= ids[child]) break;
            uint32_t tmp = ids[root]; ids[root] = ids[child]; ids[child] = tmp;
            root = child;
        }
    };
    for (uint32_t start = count / 2; start; --start) sift(start - 1, count);
    for (uint32_t end = count; end > 1; --end) {
        uint32_t tmp = ids[0]; ids[0] = ids[end - 1]; ids[end - 1] = tmp;
        sift(0, end - 1);
    }
}
static int64_t sys_setgroups(uint64_t size, uint64_t listAddr, uint64_t,
                              uint64_t, uint64_t, uint64_t) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (!ProcessSetgroupsAllowed(proc)) return -EPERM;
    const uint32_t count = static_cast<uint32_t>(size);
    if (count > MAX_SUPPLEMENTARY_GROUPS) return -EINVAL;
    if (!UserBufferReadable(listAddr, uint64_t(count) * sizeof(uint32_t))) return -EFAULT;
    SupplementaryGroups* groups = nullptr;
    if (count) {
        groups = static_cast<SupplementaryGroups*>(kmalloc(sizeof(SupplementaryGroups) + (count - 1) * sizeof(uint32_t)));
        if (!groups) return -ENOMEM;
        groups->references = 1; groups->count = count;
        memcpy(groups->ids, reinterpret_cast<const void*>(listAddr), count * sizeof(uint32_t));
        for (uint32_t i = 0; i < count; ++i) {
            groups->ids[i]=ProcessResolveId(proc,true,groups->ids[i]);
            if (groups->ids[i] == CredentialUnchanged) { kfree(groups); return -EINVAL; }
        }
        SortGroupIds(groups->ids, count); // keep duplicates, like Linux
    }
    ProcessGroupsReplace(proc, groups);
    return 0;
}

// ---------------------------------------------------------------------------
// Signal: rt_sigaction (13), rt_sigprocmask (14)
// ---------------------------------------------------------------------------

static int64_t sys_rt_sigaction(uint64_t signum, uint64_t actAddr, uint64_t oldactAddr,
                                 uint64_t sigsetsize, uint64_t, uint64_t)
{
    if (signum < 1 || signum > 64) return -EINVAL;
    if (sigsetsize != 8) return -EINVAL;

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    uint32_t idx = static_cast<uint32_t>(signum) - 1;
    uint16_t pid = proc->tgid;
    if (pid >= MAX_PROCESSES) return -EINVAL;

    KernelSigaction action{};
    if(actAddr && !CopyFromUser(&action,actAddr,sizeof(action)))return -EFAULT;
    if(actAddr && (signum==9 || signum==19))return -EINVAL;
    action.mask &= ~((1ULL<<8)|(1ULL<<18));
    const KernelSigaction previous=ExchangeSignalAction(pid,idx,actAddr?&action:nullptr);
    return !oldactAddr || CopyToUser(oldactAddr,&previous,sizeof(previous))?0:-EFAULT;
}

static int64_t sys_rt_sigpending(uint64_t setAddr, uint64_t sigsetsize,
                                uint64_t, uint64_t, uint64_t, uint64_t)
{
    // Unlike rt_sigprocmask, Linux permits a partial (or zero-length) set.
    if (sigsetsize > sizeof(uint64_t)) return -EINVAL;
    if (sigsetsize == 0) return 0;
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    // Observe without consuming. Ordinary signals currently have per-task
    // storage; unioning sibling queues would leak thread-directed signals.
    uint64_t pending = (ProcessPendingSignals(proc) |
                        PosixTimerPending(proc)) & proc->sigMask;
    return CopyToUser(setAddr, &pending, sigsetsize) ? 0 : -EFAULT;
}

static int64_t sys_rt_sigprocmask(uint64_t how, uint64_t setAddr, uint64_t oldAddr,
                                   uint64_t sigsetsize, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (sigsetsize != 8) return -EINVAL;

    // Return old mask
    if (oldAddr)
    {
        if (!UserBufferWritable(oldAddr, sizeof(uint64_t))) return -EFAULT;
        *reinterpret_cast<uint64_t*>(oldAddr) = proc->sigMask;
    }

    // Modify mask
    if (setAddr)
    {
        if (!UserBufferReadable(setAddr, sizeof(uint64_t))) return -EFAULT;
        uint64_t newSet = *reinterpret_cast<const uint64_t*>(setAddr);
        // Can't block SIGKILL (9) or SIGSTOP (19)
        uint64_t unblockable = (1ULL << 8) | (1ULL << 18);
        newSet &= ~unblockable;

        switch (how)
        {
        case 0: // SIG_BLOCK
            proc->sigMask |= newSet;
            break;
        case 1: // SIG_UNBLOCK
            proc->sigMask &= ~newSet;
            break;
        case 2: // SIG_SETMASK
            proc->sigMask = newSet;
            break;
        default:
            return -EINVAL;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sys_prlimit64 (302)
// ---------------------------------------------------------------------------

struct rlimit64 { uint64_t rlim_cur; uint64_t rlim_max; };
static constexpr uint64_t RLIM_INFINITY = ~0ULL;

static int64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t newlimitAddr,
                               uint64_t oldlimitAddr, uint64_t, uint64_t)
{
    (void)pid; (void)newlimitAddr;

    if (oldlimitAddr)
    {
        if (!UserBufferWritable(oldlimitAddr, sizeof(rlimit64))) return -EFAULT;
        auto* old = reinterpret_cast<rlimit64*>(oldlimitAddr);
        switch (resource)
        {
        case 7: // RLIMIT_NOFILE
            old->rlim_cur = MAX_FDS;
            old->rlim_max = MAX_FDS;
            break;
        case 0: // RLIMIT_CPU
        case 1: // RLIMIT_FSIZE
        case 2: // RLIMIT_DATA
        case 3: // RLIMIT_STACK
            old->rlim_cur = 8 * 1024 * 1024; // 8MB stack
            old->rlim_max = RLIM_INFINITY;
            break;
        default:
            old->rlim_cur = RLIM_INFINITY;
            old->rlim_max = RLIM_INFINITY;
            break;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sys_getrandom (318) — RDRAND-backed random number generation
// ---------------------------------------------------------------------------

static int64_t sys_getrandom(uint64_t bufAddr, uint64_t count, uint64_t,
                               uint64_t, uint64_t, uint64_t)
{
    if (count > 256) count = 256; // cap per-call to avoid long stalls
    uint8_t buf[256];

    uint64_t filled = 0;
    while (filled < count)
    {
        uint64_t rnd;
        if (!RdrandU64(&rnd))
            rnd = SoftRandU64();

        uint64_t remaining = count - filled;
        uint64_t chunk = (remaining < 8) ? remaining : 8;
        for (uint64_t b = 0; b < chunk; b++)
        {
            buf[filled++] = static_cast<uint8_t>(rnd >> (b * 8));
        }
    }
    const uint64_t copied=CopyToUserBytes(bufAddr,buf,count);
    return copied || !count ? static_cast<int64_t>(copied) : -EFAULT;
}

// ---------------------------------------------------------------------------
// sys_openat (257) -- delegate to sys_open
// ---------------------------------------------------------------------------

static int64_t sys_openat(uint64_t dirfd,uint64_t path,uint64_t flags,uint64_t mode,uint64_t,uint64_t)
{
    return OpenAtInternal(static_cast<int>(dirfd),path,flags,mode);
}

// ---------------------------------------------------------------------------
// sys_getcwd (79)
// ---------------------------------------------------------------------------

static int64_t sys_getcwd(uint64_t bufAddr, uint64_t size, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    char cwd[512];int error=ProcessFsGetcwd(ProcessCurrent(),cwd,sizeof(cwd));
    if(error)return error;
    size_t bytes=strlen(cwd)+1;
    if(size<bytes)return -ERANGE;
    return CopyToUser(bufAddr,cwd,bytes)?static_cast<int64_t>(bytes):-EFAULT;
}

// ---------------------------------------------------------------------------
// sys_fcntl (72)
// ---------------------------------------------------------------------------

static constexpr int F_DUPFD         = 0;
static constexpr int F_GETFD         = 1;
static constexpr int F_SETFD         = 2;
static constexpr int F_GETFL         = 3;
static constexpr int F_SETFL         = 4;
static constexpr int F_DUPFD_CLOEXEC = 1030;

static constexpr int FD_CLOEXEC = 1;

// Linux file flags (used in fcntl F_GETFL/F_SETFL and dup3)
[[maybe_unused]] static constexpr int O_NONBLOCK  = 0x800;
[[maybe_unused]] static constexpr int O_CLOEXEC   = 0x80000;

struct LinuxFlock {
    int16_t type,whence;
    uint32_t padding;
    int64_t start,length;
    int32_t pid;
    uint32_t tailPadding;
};
static_assert(sizeof(LinuxFlock)==32 && __builtin_offsetof(LinuxFlock,start)==8);
// Caller pins the descriptor and owns a kernel-operation guard throughout.
static int64_t FileRecordLock(Process* proc,FdEntry* entry,uint64_t cmd,uint64_t address) {
    if(entry->type!=FdType::Vnode || !entry->handle)return -EBADF;
    auto* vnode=static_cast<Vnode*>(entry->handle);
    if(vnode->type!=VnodeType::File || !vnode->ops->file_object)return -95;
    auto* object=vnode->ops->file_object(vnode);
    if(!object)return -95;
    if(!UserBufferReadable(address,sizeof(LinuxFlock)) ||
       (cmd==5 && !UserBufferWritable(address,sizeof(LinuxFlock))))return -EFAULT;
    LinuxFlock input;memcpy(&input,reinterpret_cast<void*>(address),sizeof(input));
    if(input.type<0 || input.type>2 || (cmd==5 && input.type==2))return -EINVAL;
    uint64_t size=0;
    if(input.whence==2) {
        VnodeStat stat{};int result=VfsStat(vnode,&stat);if(result)return result;size=stat.size;
    }
    RecordLockPlan plan;
    if(cmd!=5 && !plan.Prepare(input.type==2?1:2))return -37; // ENOLCK
    const auto owner=FileLockOwner(proc);
    RecordLockRange range{};RecordLockNode conflict{};
    // Lock acquisition is ordered against every logical close and dup2
    // replacement, including close of a different FD for this inode.
    SpinLockAcquire(ProcessFdLock(proc));
    int result=entry->closing?-EBADF:RecordLockNormalize(input.whence,input.start,input.length,
                                                       entry->seekPos,size,&range);
    if(!result && cmd!=5 && input.type!=2) {
        const auto mode=entry->statusFlags&3;
        if((input.type==0 && mode!=0 && mode!=2) || (input.type==1 && mode!=1 && mode!=2))result=-EBADF;
    }
    bool found=false;
    if(!result) {
        if(cmd==5)found=RecordLockQuery(object->recordLocks,owner,range,input.type,&conflict);
        else {
            result=plan.Set(object->recordLocks,owner,range,input.type);
            // Explicit interim limit: do not pretend contended SETLKW waited.
            if(result==-11 && cmd==7)result=-95;
        }
    }
    SpinLockRelease(ProcessFdLock(proc));
    if(result)return result;
    if(cmd==5) {
        if(found) {
            input.type=conflict.type;input.whence=0;input.start=conflict.range.begin;
            input.length=conflict.range.end==0x7fffffffffffffffULL?0:
                         conflict.range.end-conflict.range.begin+1;
            input.pid=conflict.owner.pid;
        } else input.type=2;
        memcpy(reinterpret_cast<void*>(address),&input,sizeof(input));
    }
    return 0;
}

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                          uint64_t, uint64_t, uint64_t)
{
    fd=static_cast<uint32_t>(fd);cmd=static_cast<uint32_t>(cmd);
    if(fd>=MAX_FDS)return -EBADF;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    KernelOperationGuard operation(proc);
    // BRO-156: pin the slot so a sibling close() can't free it while we read or
    // mutate the FD's scalar flags (F_GETFD/F_SETFD/F_GETFL/F_SETFL) or read the
    // source slot for F_DUPFD.
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref) return -EBADF;
    FdEntry* fde = ref.get();

    if(fde->type==FdType::Path && cmd!=F_DUPFD && cmd!=F_DUPFD_CLOEXEC && cmd!=F_GETFD && cmd!=F_SETFD && cmd!=F_GETFL)return -EBADF;
    switch (static_cast<int>(cmd))
    {
    case 5: // F_GETLK
    case 6: // F_SETLK
    case 7: // F_SETLKW: immediate acquisition only until cancellable waits exist.
        return FileRecordLock(proc,fde,cmd,arg);
    case 36: // F_OFD_GETLK: requires shared open-file-description ownership.
    case 37: // F_OFD_SETLK
    case 38: // F_OFD_SETLKW
        return -EINVAL;
    case 1033: // F_ADD_SEALS
    case 1034: { // F_GET_SEALS
        if(fde->type!=FdType::MemFd || !fde->handle)return -EINVAL;
        auto* mfd=static_cast<MemFdData*>(fde->handle);MfdLock(mfd);
        int result=cmd==1034?static_cast<int>(mfd->seals.bits):MemFdAddSeals(mfd->seals,static_cast<uint32_t>(arg),(fde->statusFlags&3)!=0);
        MfdUnlock(mfd);return result;
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
    {
        int minFd = static_cast<int>(arg);
        if (minFd < 0 || minFd >= static_cast<int>(MAX_FDS)) return -EINVAL;
        return DuplicateFd(proc, static_cast<int>(fd), -1,
                           static_cast<uint32_t>(minFd),
                           (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0);
    }
    case F_GETFD:
        return fde->fdFlags;
    case F_SETFD:
        fde->fdFlags = static_cast<uint8_t>(arg & FD_CLOEXEC);
        return 0;
    case F_GETFL:
    {
        SpinLockAcquire(ProcessFdLock(proc));
        const uint32_t status = fde->statusFlags;
        SpinLockRelease(ProcessFdLock(proc));
        return status;
    }
    case F_SETFL:
    {
        // Only O_NONBLOCK (0x800) and O_APPEND (0x400) are settable via F_SETFL.
        // Access mode bits (O_RDONLY/O_WRONLY/O_RDWR) are preserved from open.
        SpinLockAcquire(ProcessFdLock(proc));
        fde->statusFlags = (fde->statusFlags & 0x3) | (static_cast<uint32_t>(arg) & ~0x3u);
        SpinLockRelease(ProcessFdLock(proc));
        return 0;
    }
    default:
        return -EINVAL;
    }
}

// ---------------------------------------------------------------------------
// sys_poll (7) — basic poll implementation
// ---------------------------------------------------------------------------

struct pollfd {
    int   fd;
    short events;
    short revents;
};

static constexpr short POLLIN  = 0x0001;
static constexpr short POLLPRI = 0x0002;
static constexpr short POLLOUT = 0x0004;
static constexpr short POLLERR = 0x0008;
static constexpr short POLLHUP = 0x0010;
static constexpr short POLLNVAL = 0x0020;

// LAPIC ticks schedule wakeups, but can drift relative to CLOCK_MONOTONIC.
// As with nanosleep, only the monotonic deadline can expire a user timeout.
static uint64_t WaitDeadlineNs(uint64_t timeoutMs)
{
    uint64_t now = ClockNowNs(1);
    if (timeoutMs == ~0ULL || timeoutMs > (~0ULL - now) / 1000000ULL)
        return ~0ULL;
    return now + timeoutMs * 1000000ULL;
}

static uint64_t WaitWakeupTick(uint64_t deadlineNs, uint64_t safetyMs)
{
    uint64_t now = ClockNowNs(1);
    uint64_t remaining = deadlineNs > now ? deadlineNs - now : 0;
    uint64_t ticks = remaining / 1000000ULL + (remaining % 1000000ULL != 0);
    if (ticks > safetyMs) ticks = safetyMs;
    return ApicTickCount() + ticks;
}

// Trusted kernel array; syscall wrappers validate/copy user memory separately.
static int EpollPollReady(Process* proc, EpollInstance* ep);

static int64_t PollWait(pollfd* fds, uint64_t nfds, uint64_t timeout_ms, bool selectMode = false)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EFAULT;
    const bool infinite = timeout_ms == ~0ULL;
    const uint64_t deadline = WaitDeadlineNs(timeout_ms);

    // Poll diagnostics are opt-in, like the syscall trace itself. Printing
    // each RequestServer poll directly to the UART stalls browser IPC and
    // holds interrupts off, even when ordinary strace is disabled.
    bool logPoll = false;
    // SC_IO is declared with the strace dispatcher below.
    constexpr uint32_t pollTraceCategory = 1u << 5;
    logPoll = proc->straceEnabled && !proc->straceErrorsOnly &&
        (proc->straceFilter == 0 || (proc->straceFilter & pollTraceCategory));
    if (logPoll) {
        for (uint64_t i = 0; i < nfds && i < 8; i++) {
            FdEntry* fde = fds[i].fd >= 0 ? FdGet(proc, fds[i].fd) : nullptr;
            const char* tname = "?";
            if (fde) {
                switch (fde->type) {
                    case FdType::Socket: tname = "inet"; break;
                    case FdType::UnixSocket: tname = "unix"; break;
                    case FdType::Pipe: tname = "pipe"; break;
                    case FdType::EventFd: tname = "evfd"; break;
                    case FdType::SyncFile: tname = "sync_file"; break;
                    case FdType::TimerFd: tname = "tmfd"; break;
                    default: tname = "oth"; break;
                }
            }
            SerialPrintf("%s%d(%s,ev=0x%x)", i ? "," : "",
                         fds[i].fd, tname, fds[i].events);
        }
        if (nfds > 8) SerialPrintf(",... +%lu more", nfds - 8);
        SerialPrintf("]\n");
    }

retry_poll:
    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++)
    {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;

        FdRef fde_ref(proc, fds[i].fd);  // BRO-156: pin across handle deref
        FdEntry* fde = fde_ref.get();

        if (!fde || fde->type == FdType::None)
        {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }

        if(FdNamedFifo(fde)) {
            fds[i].revents=static_cast<int16_t>(NamedFifoReady(fde,fds[i].events));
            if(fds[i].revents)++ready;
            continue;
        }
        // Pipes: check readability/writability
        if (fde->type == FdType::Pipe && fde->handle)
        {
            auto* pb = static_cast<PipeBuffer*>(fde->handle);
            bool isWrite = (fde->flags & 1);
            if (!isWrite && (fds[i].events & POLLIN))
            {
                if (pb->count() > 0 || pb->writers == 0)
                {
                    fds[i].revents |= (pb->count() > 0) ? POLLIN : POLLHUP;
                    ready++;
                }
            }
            if (isWrite && (fds[i].events & POLLOUT))
            {
                if (pb->space() > 0 || pb->readers == 0)
                {
                    fds[i].revents |= (pb->readers > 0) ? POLLOUT : POLLERR;
                    ready++;
                }
            }
            continue;
        }

        // An epoll fd is readable when a watched event is available, and is
        // never writable. Do not fall through to synthetic device readiness.
        if (fde->type == FdType::EpollFd && fde->handle) {
            if (fds[i].events & POLLIN) {
                int result = EpollPollReady(proc, static_cast<EpollInstance*>(fde->handle));
                if (result < 0) return result;
                if (result) { fds[i].revents = POLLIN; ready++; }
            }
            continue;
        }

        // Regular files are always ready
        if(fde->type==FdType::Path) {fds[i].revents=POLLNVAL;++ready;continue;}
        if (fde->type == FdType::Vnode)
        {
            if (fds[i].events & POLLIN)  fds[i].revents |= POLLIN;
            if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
            if (fds[i].revents) ready++;
            continue;
        }

        // Keyboard device: check if input available
        if (fde->type == FdType::DevKeyboard)
        {
            if (fds[i].events & POLLIN)
            {
                if (InputHasEvents())
                {
                    fds[i].revents |= POLLIN;
                    ready++;
                }
            }
            continue;
        }

        // Socket: check TCP/UDP readiness
        if (fde->type == FdType::Socket && fde->handle)
        {
            int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
            bool rdReady = (fds[i].events & POLLIN) && brook::SockPollReady(sockIdx, true, false);
            bool wrReady = (fds[i].events & POLLOUT) && brook::SockPollReady(sockIdx, false, true);
            bool hupReady = brook::SockPollHangup(sockIdx);

            if (rdReady) fds[i].revents |= POLLIN;
            if (wrReady) fds[i].revents |= POLLOUT;
            if (hupReady) fds[i].revents |= POLLHUP;
            if (fds[i].revents) ready++;
            continue;
        }

        // Unix domain socket
        if (fde->type == FdType::UnixSocket && fde->handle)
        {
            auto* usd = static_cast<UnixSocketData*>(fde->handle);
            fds[i].revents=static_cast<short>(UnixSocketReady(usd,static_cast<uint16_t>(fds[i].events)));
            if (fds[i].revents) ready++;
            continue;
        }

        // /dev/tty: check read pipe for POLLIN, write pipe always ready for POLLOUT
        if (fde->type == FdType::DevTty && fde->handle)
        {
            auto* pair = static_cast<TtyDevicePair*>(fde->handle);
            if (fds[i].events & POLLIN)
            {
                auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                if (rp->count() > 0 || rp->writers == 0)
                {
                    fds[i].revents |= POLLIN;
                }
            }
            if (fds[i].events & POLLOUT)
                fds[i].revents |= POLLOUT;
            if (fds[i].revents) ready++;
            continue;
        }

        if(fde->type==FdType::SyncFile && fde->handle) {
            fds[i].revents|=SyncFileReady(fds[i].events);
            if(fds[i].revents)++ready;
            continue;
        }

        if(fde->type==FdType::SignalFd && fde->handle) {
            fds[i].revents=SignalFdReady(proc,static_cast<SignalFd*>(fde->handle),fds[i].events);
            if(fds[i].revents)++ready;
            continue;
        }

        // EventFd: readable when nonempty, writable below counter capacity.
        if (fde->type == FdType::EventFd && fde->handle)
        {
            auto* efd = static_cast<EventFdData*>(fde->handle);
            fds[i].revents |= EventFdReady(efd, fds[i].events).ready;
            if (fds[i].revents) ready++;
            continue;
        }

        if (fde->type == FdType::TimerFd && fde->handle)
        {
            auto* tfd = static_cast<TimerFdData*>(fde->handle);
            if (fds[i].events & POLLIN)
            {
                if (TimerFdReady(tfd)) fds[i].revents |= POLLIN;
            }
            if (fds[i].revents) ready++;
            continue;
        }

        if (fde->type == FdType::MemFd)
        {
            if (fds[i].events & (POLLIN | POLLOUT))
                fds[i].revents |= (fds[i].events & (POLLIN | POLLOUT));
            if (fds[i].revents) ready++;
            continue;
        }

        // Cached per-stream capacity: descriptor scans must not call the sleeping driver.
        if (fde->type == FdType::DevDsp)
        {
            AudioStreamProgress progress{};
            auto* dsp = static_cast<DspState*>(fde->handle);
            if (dsp && AudioMixerGetProgress(dsp->mixerStreamId, &progress)) {
                if (progress.discontinuity) fds[i].revents |= POLLERR;
                else if ((fds[i].events & POLLOUT) &&
                         AudioStreamWritable(progress))
                    fds[i].revents |= POLLOUT;
            } else fds[i].revents |= POLLERR;
            if (fds[i].revents) ready++;
            continue;
        }

        // Ordinary synthetic descriptors are readable/writable; exceptional
        // conditions must never be invented for select's exceptfds.
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        if (fds[i].revents) ready++;
    }

    // select maps hangup/error into requested read/write sets, never exceptfds.
    if (selectMode) {
        ready = 0;
        for (uint64_t i = 0; i < nfds; ++i) {
            short allowed = POLLNVAL;
            if (fds[i].events & POLLIN) allowed |= POLLIN | POLLHUP | POLLERR;
            if (fds[i].events & POLLOUT) allowed |= POLLOUT | POLLERR;
            if (fds[i].events & POLLPRI) allowed |= POLLPRI;
            fds[i].revents &= allowed;
            if (fds[i].revents) ++ready;
        }
    }

    // If nothing ready and timeout != 0, block and retry
    if (ready == 0 && timeout_ms != 0 && (infinite || ClockNowNs(1) < deadline))
    {
        // Register as waiter BEFORE re-checking data availability.
        // This closes the race where data arrives between the initial
        // scan and the block call.
        Process* self = ProcessCurrent();

        // For pipe FDs, register as waiter on the pipe
        uint64_t waiterCap = nfds;
        uint64_t safetyMs = 50;
        TimerWaitGroup timerWaiters;
        PipePollGroup pipeWaiters;
        // Allocate pipe subscriptions before installing legacy wait hooks, so
        // ENOMEM unwinds only RAII-owned exact registrations, never stale hooks.
        for (uint64_t i = 0; i < waiterCap; ++i) {
            if (fds[i].fd < 0) continue;
            FdRef fde_ref(proc, fds[i].fd);
            FdEntry* fde = fde_ref.get();
            if (!fde) continue;
            if(FdNamedFifo(fde)) {
                if(!pipeWaiters.add(FdPipe(fde),self,fds[i].events|POLLHUP|POLLERR))return -ENOMEM;
            }
            if (fde->type == FdType::Pipe && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                if (!(fde->flags & 1)) // read end
                    if (!pipeWaiters.add(pb, self, POLLIN)) return -ENOMEM;
            }
            if (fde->type == FdType::DevTty && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pair = static_cast<TtyDevicePair*>(fde->handle);
                auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                if (!pipeWaiters.add(rp, self, POLLIN)) return -ENOMEM;
            }
            if (fde->type == FdType::UnixSocket && fde->handle)
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if ((fds[i].events & POLLIN) &&
                    usd->state == UnixSocketData::State::Connected && usd->rxPipe)
                {
                    if (!pipeWaiters.add(usd->rxPipe, self, POLLIN)) return -ENOMEM;
                }
                if ((fds[i].events & POLLOUT) &&
                    usd->state == UnixSocketData::State::Connected && usd->txPipe)
                {
                    if (!pipeWaiters.add(usd->txPipe, self, POLLOUT)) return -ENOMEM;
                }
            }
        }
        for (uint64_t i = 0; i < waiterCap; i++)
        {
            if (fds[i].fd < 0) continue;
            FdRef fde_ref(proc, fds[i].fd);  // BRO-156: pin across handle deref
            FdEntry* fde = fde_ref.get();
            if (!fde) continue;
            if (fde->type == FdType::TimerFd && fde->handle && (fds[i].events & POLLIN))
                timerWaiters.add(static_cast<TimerFdData*>(fde->handle), self);
            if (fde->type == FdType::DevDsp) safetyMs = 5; // Cached DMA readiness has no IRQ waiter yet.
            if (fde->type == FdType::DevKeyboard && (fds[i].events & POLLIN))
                InputAddWaiter(self);
            if (fde->type == FdType::Socket && fde->handle)
            {
                int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                brook::SockSetPollWaiter(sockIdx, self);
            }
            if (fde->type == FdType::UnixSocket && fde->handle) {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if ((fds[i].events & POLLIN) && usd->state == UnixSocketData::State::Listening)
                    usd->epollWaiter = self;
            }
            if (fde->type == FdType::EventFd && fde->handle)
            {
                auto* efd = static_cast<EventFdData*>(fde->handle);
                efd->readerWaiter = self;
            }
        }

        // Re-check data availability after registration.
        // If data arrived between the initial scan and registration,
        // we catch it here instead of blocking forever.
        for (uint64_t i = 0; i < nfds; i++)
        {
            if (fds[i].fd < 0) continue;
            FdRef fde_ref(proc, fds[i].fd);  // BRO-156: pin across handle deref
            FdEntry* fde = fde_ref.get();
            if (!fde) continue;
            if (fde->type == FdType::TimerFd && fde->handle && (fds[i].events & POLLIN) &&
                TimerFdReady(static_cast<TimerFdData*>(fde->handle)))
            { ready++; break; }
            if (fde->type == FdType::DevKeyboard && (fds[i].events & POLLIN) && InputHasEvents())
            { ready++; break; }
            if(FdNamedFifo(fde) && NamedFifoReady(fde,fds[i].events)) {ready++;break;}
            if (fde->type == FdType::Pipe && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                if (!(fde->flags & 1) && (pb->count() > 0 || pb->writers == 0))
                { ready++; break; }
            }
            if (fde->type == FdType::Socket && fde->handle)
            {
                int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                if (brook::SockPollReady(sockIdx, (fds[i].events & POLLIN) != 0,
                                                  (fds[i].events & POLLOUT) != 0) ||
                    ((!selectMode || (fds[i].events & (POLLIN | POLLOUT))) &&
                     brook::SockPollHangup(sockIdx)))
                { ready++; break; }
            }
            if (fde->type == FdType::UnixSocket && fde->handle)
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if ((fds[i].events & POLLIN) && usd->state == UnixSocketData::State::Listening) {
                    if (usd->pendingCount > 0) { ready++; break; }
                } else if ((fds[i].events & POLLIN) &&
                           usd->state == UnixSocketData::State::Connected && usd->rxPipe) {
                    if (usd->rxPipe->count() > 0 ||
                        __atomic_load_n(&usd->rxPipe->writers, __ATOMIC_ACQUIRE) == 0)
                    { ready++; break; }
                }
                if ((fds[i].events & POLLOUT) &&
                    usd->state == UnixSocketData::State::Connected && usd->txPipe &&
                    (usd->txPipe->space() > 0 ||
                     __atomic_load_n(&usd->txPipe->readers, __ATOMIC_ACQUIRE) == 0))
                { ready++; break; }
            }
            if (fde->type == FdType::EventFd && fde->handle && (fds[i].events & POLLIN))
            {
                auto* efd = static_cast<EventFdData*>(fde->handle);
                if (EventFdReady(efd, fds[i].events).ready)
                { ready++; break; }
            }
        }

        if (ready == 0)
        {
            // Keep one absolute deadline across spurious wakes. Periodic
            // checks also service timerfds and descriptors without wait hooks.
            const uint64_t timerDeadline = timerWaiters.deadline(deadline);
            if (timerWaiters.allocationFailed) safetyMs = 5;
            self->wakeupTick = WaitWakeupTick(timerDeadline, safetyMs);
            if (!HasPendingSignals() && (infinite || ClockNowNs(1) < deadline))
                SchedulerBlockInterruptible(self);


        }

        // Clean up waiters
        InputRemoveWaiter(self);
        for (uint64_t i = 0; i < waiterCap; i++)
        {
            if (fds[i].fd < 0) continue;
            FdRef fde_ref(proc, fds[i].fd);  // BRO-156: pin across handle deref
            FdEntry* fde = fde_ref.get();
            if (!fde) continue;
            if (fde->type == FdType::UnixSocket && fde->handle)
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if ((fds[i].events & POLLIN) &&
                    usd->state == UnixSocketData::State::Listening &&
                    usd->epollWaiter == self)
                    usd->epollWaiter = nullptr;
            }
            if (fde->type == FdType::EventFd && fde->handle)
            {
                auto* efd = static_cast<EventFdData*>(fde->handle);
                if (efd->readerWaiter == self)
                    efd->readerWaiter = nullptr;
            }
            if (fde->type == FdType::Socket && fde->handle)
            {
                int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                brook::SockSetPollWaiter(si, nullptr);
            }
        }

        // Both ordinary signal interruption and terminal cancellation use the
        // complete common unregister path (including DevTty) before unwinding.
        if (HasPendingSignals()) return -EINTR;

        // Always use the same readiness scan before/after sleeping, including
        // timerfds, sockets and eventfds. Expiry is checked after that scan.
        goto retry_poll;
    }

    if (logPoll && ready > 0) {
        int logged = 0;
        for (uint64_t i = 0; i < nfds && logged < 8; i++) {
            if (fds[i].revents) {
                SerialPrintf("%s%d(0x%x)", logged ? "," : "",
                             fds[i].fd, fds[i].revents);
                logged++;
            }
        }
        SerialPrintf("]\n");
    }

    return ready;
}

// Parse the ABI explicitly: timeval uses microseconds, timespec nanoseconds.
// Round positive fractions up to the scheduler's millisecond resolution.
static int ParseWaitTimeout(uint64_t address, bool nanoseconds, uint64_t* timeoutMs)
{
    *timeoutMs = ~0ULL;
    if (!address) return 0;
    if (!UserBufferReadable(address, 2 * sizeof(int64_t))) return -EFAULT;
    auto* value = reinterpret_cast<const int64_t*>(address);
    int64_t sec = value[0], fraction = value[1];
    int64_t scale = nanoseconds ? 1000000000LL : 1000000LL;
    if (sec < 0 || fraction < 0 || fraction >= scale) return -EINVAL;
    uint64_t unitsPerMs = static_cast<uint64_t>(scale / 1000);
    uint64_t partial = (static_cast<uint64_t>(fraction) + unitsPerMs - 1) / unitsPerMs;
    *timeoutMs = static_cast<uint64_t>(sec) > (~0ULL - 1 - partial) / 1000
        ? ~0ULL - 1 : static_cast<uint64_t>(sec) * 1000 + partial;
    return 0;
}

static int64_t PollUser(uint64_t fdsAddr, uint64_t nfds, uint64_t timeoutMs)
{
    if (nfds > MAX_FDS) return -EINVAL;
    uint64_t bytes = nfds * sizeof(pollfd);
    if (bytes && (!UserBufferReadable(fdsAddr, bytes) || !UserBufferWritable(fdsAddr, bytes)))
        return -EFAULT;
    pollfd fds[MAX_FDS];
    if (bytes) memcpy(fds, reinterpret_cast<const void*>(fdsAddr), bytes);
    int64_t result = PollWait(fds, nfds, timeoutMs);
    if (result >= 0 && bytes) {
        if (!UserBufferWritable(fdsAddr, bytes)) return -EFAULT;
        auto* user = reinterpret_cast<pollfd*>(fdsAddr);
        for (uint64_t i = 0; i < nfds; ++i) user[i].revents = fds[i].revents;
    }
    return result;
}

static int64_t sys_poll(uint64_t fdsAddr, uint64_t nfds, uint64_t timeout,
                       uint64_t, uint64_t, uint64_t)
{
    int32_t milliseconds = static_cast<int32_t>(timeout);
    return PollUser(fdsAddr, nfds, milliseconds < 0 ? ~0ULL : static_cast<uint64_t>(milliseconds));
}

static int64_t sys_ppoll(uint64_t fdsAddr, uint64_t nfds, uint64_t tspecAddr,
                        uint64_t sigmaskAddr, uint64_t sigsetsize, uint64_t)
{
    uint64_t timeoutMs;
    int error = ParseWaitTimeout(tspecAddr, true, &timeoutMs);
    if (!error) error = InstallWaitSignalMask(sigmaskAddr, sigsetsize);
    return error ? error : PollUser(fdsAddr, nfds, timeoutMs);
}

// ---------------------------------------------------------------------------
// sys_readlink (89) / sys_readlinkat (267)
// ---------------------------------------------------------------------------

static int64_t DoReadlink(const char* original,uint64_t bufAddr,uint64_t bufsiz,int dirfd=-100)
{
    AtLookup at;if(int error=at.Init(dirfd,original))return error;
    char synthetic[512];
    const char* path=!at.SyntheticName(original,synthetic,sizeof(synthetic))?synthetic:"";
    if (bufsiz == 0) return -EINVAL;

    // GL shim (M0): the DRM device's 'subsystem' symlink. libdrm's
    // get_subsystem_type() readlinks this and matches the basename to classify
    // the bus; returning ".../pci" yields DRM_BUS_PCI for the virtio-gpu node.
    if (StrEq(path, "/sys/dev/char/226:128/device/subsystem") && brook::GpuAppGet())
    {
        const char* tgt = "/sys/bus/pci";
        uint64_t slen = 0; while (tgt[slen]) ++slen;
        if (slen > bufsiz) slen = bufsiz;
        if (!CopyToUser(bufAddr, tgt, slen)) return -EFAULT;
        return static_cast<int64_t>(slen);
    }

    // /proc/self/exe → return the process's executable path
    auto streq = [](const char* a, const char* b) {
        while (*a && *a == *b) { a++; b++; }
        return *a == *b;
    };
    if (streq(path, "/proc/self/exe"))
    {
        const char* exe = "/boot/BIN/UNKNOWN";
        Process* proc = ProcessCurrent();
        if (proc && proc->exePath[0])
        {
            exe = proc->exePath;
        }
        else if (proc && proc->name[0])
        {
            // Build "/boot/BIN/<NAME>" from process name
            char exePath[128];
            int len = 0;
            const char* prefix = "/boot/BIN/";
            for (int i = 0; prefix[i]; i++) exePath[len++] = prefix[i];
            for (int i = 0; proc->name[i] && len < 120; i++) exePath[len++] = proc->name[i];
            exePath[len] = '\0';
            uint64_t slen = 0;
            while (exePath[slen]) slen++;
            if (slen > bufsiz) slen = bufsiz;
            if (!CopyToUser(bufAddr, exePath, slen)) return -EFAULT;
            return static_cast<int64_t>(slen);
        }
        uint64_t slen = 0;
        while (exe[slen]) slen++;
        if (slen > bufsiz) slen = bufsiz;
        if (!CopyToUser(bufAddr, exe, slen)) return -EFAULT;
        return static_cast<int64_t>(slen);
    }

    // FD links use retained procfs objects below. The old synthetic shortcut
    // returned stale opening labels (or "?" for O_PATH) and bypassed FD identity.

    // /proc/PID/exe → return executable path for any process
    {
        const char* p = path;
        if (p[0] == '/' && p[1] == 'p' && p[2] == 'r' && p[3] == 'o' &&
            p[4] == 'c' && p[5] == '/')
        {
            p += 6;
            uint16_t pid = 0;
            while (*p >= '0' && *p <= '9') { pid = pid * 10 + (*p - '0'); p++; }
            if (*p == '/' && p[1] == 'e' && p[2] == 'x' && p[3] == 'e' && p[4] == '\0' && pid > 0) {
                ProcessSnapshot snap = {};
                if (SchedulerSnapshotProcess(pid, &snap) && snap.exePath[0]) {
                    uint64_t slen = 0;
                    while (snap.exePath[slen]) slen++;
                    if (slen > bufsiz) slen = bufsiz;
                    if (!CopyToUser(bufAddr, snap.exePath, slen)) return -EFAULT;
                    return static_cast<int64_t>(slen);
                }
            }
        }
    }

    // Try VFS readlink for real symlinks
    char linkTarget[1024];
    uint64_t cap = (bufsiz < sizeof(linkTarget)) ? bufsiz : sizeof(linkTarget);
    int r=VfsReadlinkAt(at.fs.root,at.start(),original,linkTarget,static_cast<uint32_t>(cap));
    if (r >= 0 && !CopyToUser(bufAddr, linkTarget, static_cast<uint64_t>(r)))
        return -EFAULT;
    if (r >= 0) return static_cast<int64_t>(r);
    return r;
}

static int64_t sys_readlink(uint64_t pathAddr, uint64_t bufAddr, uint64_t bufsiz,
                             uint64_t, uint64_t, uint64_t)
{
    char path[256];
    if (!CopyUserCString(pathAddr, path, sizeof(path)))
        return -EFAULT;
    return DoReadlink(path,bufAddr,bufsiz);
}

static int64_t sys_readlinkat(uint64_t dirfd, uint64_t pathAddr, uint64_t bufAddr,
                               uint64_t bufsiz, uint64_t, uint64_t)
{
    char path[256];
    if (!CopyUserCString(pathAddr, path, sizeof(path)))
        return -EFAULT;

    if(!path[0]) {
        if(!bufsiz)return -EINVAL;
        if(static_cast<int>(dirfd)==-100)return -ENOENT;
        FdRef ref(ProcessCurrent(),static_cast<int>(dirfd));if(!ref)return -EBADF;
        if((ref->type!=FdType::Path && ref->type!=FdType::Vnode) || !ref->handle)return -EINVAL;
        VfsPath retained{};int error=VfsPathFromVnode(static_cast<Vnode*>(ref->handle),&retained);
        char target[1024];uint32_t capacity=bufsiz<sizeof(target)?static_cast<uint32_t>(bufsiz):sizeof(target);
        if(!error)error=VfsPathReadlink(retained,target,capacity);
        VfsPathRelease(retained);
        if(error>=0 && !CopyToUser(bufAddr,target,static_cast<uint64_t>(error)))return -EFAULT;
        return error;
    }
    return DoReadlink(path,bufAddr,bufsiz,static_cast<int>(dirfd));
}

// ---------------------------------------------------------------------------
// sys_symlink (88) / sys_symlinkat (266)
// ---------------------------------------------------------------------------

static int64_t sys_symlink(uint64_t targetAddr, uint64_t linkpathAddr, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    char target[512],path[512];
    if(!CopyUserCString(targetAddr,target,sizeof(target)) || !CopyUserCString(linkpathAddr,path,sizeof(path)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    return VfsSymlinkAt(lookup.fs.root,lookup.start(),target,path);
}

static int64_t sys_symlinkat(uint64_t targetAddr, uint64_t newdirfd, uint64_t linkpathAddr,
                              uint64_t, uint64_t, uint64_t)
{
    char target[512],path[512];
    if(!CopyUserCString(targetAddr,target,sizeof(target)) || !CopyUserCString(linkpathAddr,path,sizeof(path)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(newdirfd),path))return error;
    return VfsSymlinkAt(lookup.fs.root,lookup.start(),target,path);
}

// ---------------------------------------------------------------------------
// sys_pipe2 (293) — like pipe() but with flags
// ---------------------------------------------------------------------------

static int64_t sys_pipe2(uint64_t pipefdAddr, uint64_t flags, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    constexpr uint64_t PIPE2_ALLOWED = O_CLOEXEC | O_NONBLOCK;
    if (flags & ~PIPE2_ALLOWED) return -EINVAL;

    int64_t ret = sys_pipe(pipefdAddr, 0, 0, 0, 0, 0);
    if (ret < 0) return ret;

    Process* proc = ProcessCurrent();
    auto* pipefd = reinterpret_cast<int32_t*>(pipefdAddr);
    if (!proc || !pipefd) return ret;

    if (flags & O_CLOEXEC)
    {
        proc->fds[pipefd[0]].fdFlags = FD_CLOEXEC;
        proc->fds[pipefd[1]].fdFlags = FD_CLOEXEC;
    }
    if (flags & O_NONBLOCK)
    {
        proc->fds[pipefd[0]].statusFlags |= O_NONBLOCK;
        proc->fds[pipefd[1]].statusFlags |= O_NONBLOCK;
    }
    return ret;
}

static int64_t sys_signalfd4(uint64_t descriptor,uint64_t maskAddress,uint64_t size,uint64_t flags,uint64_t,uint64_t)
{
    if(size!=8 || (flags&~uint64_t(0x800|0x80000)))return -EINVAL;
    uint64_t mask=0;if(!CopyFromUser(&mask,maskAddress,sizeof(mask)))return -EFAULT;
    Process* p=ProcessCurrent();
    if(int(descriptor)!=-1) {
        FdRef ref(p,int(descriptor));if(!ref)return -EBADF;
        if(ref->type!=FdType::SignalFd || !ref->handle)return -EINVAL;
        SignalFdSetMask(static_cast<SignalFd*>(ref->handle),mask);return int(descriptor);
    }
    KernelOperationGuard guard(p);
    auto* object=SignalFdCreate(mask);if(!object)return -ENOMEM;
    FdEntry entry{};entry.type=FdType::SignalFd;entry.handle=object;
    entry.statusFlags=flags&0x800;entry.fdFlags=(flags&0x80000)?1:0;
    int fd=FdTablePublishOwned(p->fds,ProcessFdLock(p),entry,0,-1,nullptr,nullptr);
    if(fd<0){SignalFdUnref(object);return -EMFILE;}return fd;
}
static int64_t sys_signalfd(uint64_t descriptor,uint64_t mask,uint64_t size,uint64_t,uint64_t,uint64_t)
{ return sys_signalfd4(descriptor,mask,size,0,0,0); }

// ---------------------------------------------------------------------------
// sys_eventfd2 (290) — event notification file descriptor
// ---------------------------------------------------------------------------

static int64_t sys_eventfd2(uint64_t initval, uint64_t flagsVal, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    const uint32_t flags=static_cast<uint32_t>(flagsVal);
    if(flags & ~(EFD_SEMAPHORE|EFD_CLOEXEC|EFD_NONBLOCK))return -EINVAL;
    KernelOperationGuard operation(proc);

    auto* efd = static_cast<EventFdData*>(kmalloc(sizeof(EventFdData)));
    if (!efd) return -ENOMEM;

    EventFdInit(efd, static_cast<uint32_t>(initval), flags);

    FdEntry entry{};entry.type=FdType::EventFd;entry.handle=efd;
    entry.statusFlags=2u|((flags&EFD_NONBLOCK)?0x800u:0u);
    entry.fdFlags=(flags&EFD_CLOEXEC)?1:0;
    int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),entry,0,-1,nullptr);
    if (fd < 0) { kfree(efd); return -EMFILE; }

    DbgPrintf("sys_eventfd2: fd=%d initval=%lu flags=0x%lx\n", fd, initval, flagsVal);
    return fd;
}

// ---------------------------------------------------------------------------
// epoll — Linux epoll_create / epoll_ctl / epoll_wait
// ---------------------------------------------------------------------------
//
// Wayland compositors (weston, etc.) use epoll as their primary event loop.
// We implement a simple but correct epoll supporting EPOLLIN/EPOLLOUT on
// pipes, sockets, eventfds, timerfds, and other fd types.
//
// Data structures:
//   EpollInstance  — the epoll fd's state (interest list + ready list)
//   EpollEntry     — one registered fd (interest list entry)

static constexpr uint32_t EPOLLIN      = 0x001;
static constexpr uint32_t EPOLLOUT     = 0x004;
[[maybe_unused]] static constexpr uint32_t EPOLLRDHUP   = 0x2000;
[[maybe_unused]] static constexpr uint32_t EPOLLPRI     = 0x002;
static constexpr uint32_t EPOLLERR     = 0x008;
static constexpr uint32_t EPOLLHUP     = 0x010;
static constexpr uint32_t EPOLLET      = (1u << 31); // eventfd notifications; other sources remain LT
[[maybe_unused]] static constexpr uint32_t EPOLLONESHOT = (1u << 30);

[[maybe_unused]] static constexpr int EPOLL_CTL_ADD = 1;
static constexpr int EPOLL_CTL_DEL = 2;
static constexpr int EPOLL_CTL_MOD = 3;

// Serialize interest-list mutation and traversals, including graph validation.
// Never acquired by fd finalization, and never held across a userspace wait.
static KMutex g_epollGraphLock{};
struct EpollGraphGuard {
    EpollGraphGuard() { KMutexLock(&g_epollGraphLock); }
    ~EpollGraphGuard() { KMutexUnlock(&g_epollGraphLock); }
};
static constexpr int EPOLL_MAX_DEPTH = 5; // four epoll-to-epoll edges

// Check if a single fd is ready given the requested events mask.
// Returns the set of events that are actually ready.
static uint32_t EpollLeafReady(FdEntry* fde, uint32_t events)
{
    // A closed watch is not an fd producing a hangup event. In particular,
    // do not feed libuv orphan ERR|HUP notifications forever after close.
    if (!fde) return 0;

    if(FdNamedFifo(fde))return NamedFifoReady(fde,events);
    if(fde->type==FdType::SyncFile && fde->handle)return SyncFileReady(events);
    if(fde->type==FdType::SignalFd && fde->handle)
        return SignalFdReady(ProcessCurrent(),static_cast<SignalFd*>(fde->handle),events);

    if (fde->type == FdType::EventFd && fde->handle)
        return EventFdReady(static_cast<EventFdData*>(fde->handle), events).ready;

    uint32_t ready = 0;
    if (fde->type == FdType::Socket && fde->handle) {
        int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        if (brook::SockPollHangup(si))
            ready |= EPOLLHUP | EPOLLRDHUP;
    }
    if (fde->type == FdType::UnixSocket && fde->handle)
        return UnixSocketReady(static_cast<UnixSocketData*>(fde->handle),events);
    if (fde->type == FdType::DevDsp) {
        // Walks pin the open description; scan only cached mixer state here.
        auto* dsp = static_cast<DspState*>(fde->handle);
        AudioStreamProgress progress{};
        if (!dsp || !AudioMixerGetProgress(dsp->mixerStreamId, &progress) || progress.discontinuity)
            return EPOLLERR;
        return (events & EPOLLOUT) && AudioStreamWritable(progress)
            ? EPOLLOUT : 0;
    }

    if (events & EPOLLIN) {
        bool readable = false;
        if (fde->type == FdType::Pipe && fde->handle) {
            auto* pb = static_cast<PipeBuffer*>(fde->handle);
            readable = (pb->count() > 0 || pb->writers == 0);
        } else if (fde->type == FdType::Socket && fde->handle) {
            int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
            readable = brook::SockPollReady(si, true, false);
        } else if (fde->type == FdType::TimerFd && fde->handle) {
            readable = TimerFdReady(static_cast<TimerFdData*>(fde->handle));
        } else if (fde->type == FdType::Vnode) {
            readable = true; // files always readable
        } else if (fde->type == FdType::DevTty && fde->handle) {
            auto* pair = static_cast<TtyDevicePair*>(fde->handle);
            auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
            readable = (rp->count() > 0 || rp->writers == 0);
        }
        if (readable) ready |= EPOLLIN;
    }

    if (events & EPOLLOUT) {
        bool writable = false;
        if (fde->type == FdType::Pipe && fde->handle) {
            // writable if write end and there's space
            if (fde->flags & 1) { // write end
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                writable = (pb->space() > 0);
            }
        } else if (fde->type == FdType::Socket && fde->handle) {
            int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
            writable = brook::SockPollReady(si, false, true);
        } else if (fde->type == FdType::Vnode) {
            writable = true;
        } else if (fde->type == FdType::DevTty) {
            writable = true;
        } else if (fde->type == FdType::MemFd) {
            writable = true;
        }
        if (writable) ready |= EPOLLOUT;
    }

    return ready;
}

// Heap-backed walk state: recursive kernel stacks must not contain MAX_FDS
// arrays. Pins survive registration/sleep/cleanup, even if ctl DEL or close
// changes the graph. Entries still use Brook's existing numeric-fd identity.
struct EpollWalk {
    Process* proc;
    FdEntry* pins[MAX_FDS];
    bool attempted[MAX_FDS];
    uint32_t masks[MAX_FDS];
    // Only selected events have delivery snapshots. Retained until copyout
    // succeeds under the graph mutex; peeking never changes a watch.
    struct Delivery { int index; EventFdSnapshot eventFd; } deliveries[MAX_FDS];
    struct Node {
        EpollInstance* ep;
        uint8_t ready[EPOLL_MAX_DEPTH]; // 0 unknown, 1 false, 2 true
        bool active;
        uint8_t collectedDepth;
        int height;
    } nodes[EPOLL_MAX_FDS + 1];
    int count;
    FdEntry* pin(int fd) {
        if (fd < 0 || fd >= EPOLL_MAX_FDS) return nullptr;
        if (!attempted[fd]) {
            attempted[fd] = true;
            pins[fd] = FdGetRef(proc, fd);
        }
        return pins[fd];
    }
    Node* node(EpollInstance* ep) {
        for (int i = 0; i < count; i++) if (nodes[i].ep == ep) return &nodes[i];
        if (count >= EPOLL_MAX_FDS + 1) return nullptr;
        auto* n = &nodes[count++]; n->ep = ep; return n;
    }
    bool any(EpollInstance* ep, int depth = 0) {
        if (depth >= EPOLL_MAX_DEPTH) return false;
        auto* n = node(ep);
        if (!n || n->active) return false; // guard fd-reuse-created cycles too
        if (n->ready[depth]) return n->ready[depth] == 2;
        n->active = true;
        bool result = false;
        for (int i = 0; i < ep->count && !result; i++) {
            auto e = ep->entries[i];
            if (e.fd >= 0) {
                if (!pin(e.fd)) { ep->entries[i].fd = -1; continue; }
                result = watchReady(e, depth);
            }
        }
        n->active = false; n->ready[depth] = result ? 2 : 1;
        return result;
    }
    uint32_t ready(int fd, uint32_t events, int depth) {
        auto* fde = pin(fd);
        if (fde && fde->type == FdType::EpollFd && fde->handle)
            return (events & EPOLLIN) && any(static_cast<EpollInstance*>(fde->handle), depth + 1) ? EPOLLIN : 0;
        return EpollLeafReady(fde, events);
    }
    uint32_t watchReady(const EpollEntry& entry, int depth, EventFdSnapshot* captured = nullptr) {
        auto* fde = pin(entry.fd);
        if(fde && fde->type==FdType::SyncFile && fde->handle && (entry.events&(EPOLLET|EPOLLONESHOT))) {
            auto* fence=static_cast<SyncFileData*>(fde->handle);
            EventFdSnapshot snapshot{fence->identity,1,0,SyncFileReady(entry.events)};
            if(captured)*captured=snapshot;
            return entry.eventFdIdentity!=fence->identity ? snapshot.ready : 0;
        }
        if (!(entry.events & EPOLLET) || !fde ||
            fde->type != FdType::EventFd || !fde->handle)
            return ready(entry.fd, entry.events, depth);
        auto snapshot = EventFdReady(static_cast<EventFdData*>(fde->handle), entry.events);
        if (captured) *captured = snapshot;
        const bool notified = entry.eventFdIdentity != snapshot.identity ||
            ((entry.events & EPOLLIN) && entry.inGeneration != snapshot.inGeneration) ||
            ((entry.events & EPOLLOUT) && entry.outGeneration != snapshot.outGeneration);
        return notified ? snapshot.ready : 0;
    }
    void collect(EpollInstance* ep, int depth = 0) {
        if (depth >= EPOLL_MAX_DEPTH) return;
        auto* n = node(ep);
        if (!n || n->collectedDepth >= EPOLL_MAX_DEPTH - depth) return;
        n->collectedDepth = EPOLL_MAX_DEPTH - depth;
        for (int i = 0; i < ep->count; i++) {
            auto e = ep->entries[i];
            auto* fde = pin(e.fd);
            if (!fde || !fde->handle) continue;
            if (fde->type == FdType::EpollFd) {
                if (e.events & EPOLLIN) collect(static_cast<EpollInstance*>(fde->handle), depth + 1);
            } else masks[e.fd] |= e.events | EPOLLERR | EPOLLHUP;
        }
    }
    // All edges participate in cycle/depth validation, regardless of masks.
    int height(EpollInstance* ep, int depth = 0) {
        if (depth >= EPOLL_MAX_DEPTH) return EPOLL_MAX_DEPTH + 1;
        auto* n = node(ep);
        if (!n || n->active) return EPOLL_MAX_DEPTH + 1;
        if (n->height) return n->height;
        n->active = true;
        int h = 1;
        for (int i = 0; i < ep->count; i++) {
            auto* fde = pin(ep->entries[i].fd);
            if (!fde || fde->type != FdType::EpollFd || !fde->handle) continue;
            int child = height(static_cast<EpollInstance*>(fde->handle), depth + 1) + 1;
            if (child > h) h = child;
            if (h > EPOLL_MAX_DEPTH) break;
        }
        n->active = false; n->height = h; return h;
    }
    void clearMemo() {
        __builtin_memset(nodes, 0, sizeof(nodes)); count = 0;
    }
};
// Reuse free scratch objects on hot epoll paths. Sleepers retain their own
// objects but do not occupy cache slots; any completed fallback can refill it.
// At most 32 free objects remain cached; concurrent waits retain heap fallback.
static ObjectCache<EpollWalk, 32> g_epollWalkCache{};
struct EpollWalkOwner {
    KernelOperationGuard operation; // unwind pins/storage before fatal cancellation
    EpollWalk* walk;
    explicit EpollWalkOwner(Process* proc) : operation(proc), walk(g_epollWalkCache.TryAcquire()) {
        if (!walk) walk = static_cast<EpollWalk*>(kmalloc(sizeof(EpollWalk)));
        if (walk) { __builtin_memset(walk, 0, sizeof(*walk)); walk->proc = proc; }
    }
    ~EpollWalkOwner() {
        if (!walk) return;
        for (int fd = 0; fd < EPOLL_MAX_FDS; fd++) if (walk->pins[fd]) FdPut(walk->proc, fd);
        if (!g_epollWalkCache.Release(walk)) kfree(walk);
    }
};
static bool g_epollDiagEnabled = false;
static EpollDiagSnapshot g_epollDiag[MAX_PROCESSES]{};
void EpollDiagEnable(bool enabled) { __atomic_store_n(&g_epollDiagEnabled, enabled, __ATOMIC_RELEASE); }
bool EpollDiagEnabled() { return __atomic_load_n(&g_epollDiagEnabled, __ATOMIC_ACQUIRE); }
bool EpollDiagRead(unsigned pid, EpollDiagSnapshot* output) {
    if (pid >= MAX_PROCESSES || !output) return false;
    const auto& row = g_epollDiag[pid];
    const uint64_t incarnation = __atomic_load_n(&row.incarnation, __ATOMIC_ACQUIRE);
    if (!incarnation) return false;
    for (unsigned i = 0; i < EpDiagCount; ++i)
        output->counters[i] = __atomic_load_n(&row.counters[i], __ATOMIC_RELAXED);
    output->incarnation = incarnation;
    return incarnation == __atomic_load_n(&row.incarnation, __ATOMIC_ACQUIRE);
}
static EpollDiagSnapshot* EpollDiagFor(Process* proc) {
    if (!EpollDiagEnabled() || proc->pid >= MAX_PROCESSES) return nullptr;
    auto* row = &g_epollDiag[proc->pid];
    // One live task owns a PID. Invalidate before reusing its diagnostic row.
    if (__atomic_load_n(&row->incarnation, __ATOMIC_RELAXED) != proc->incarnation) {
        __atomic_store_n(&row->incarnation, 0, __ATOMIC_RELEASE);
        for (auto& counter : row->counters) __atomic_store_n(&counter, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&row->incarnation, proc->incarnation, __ATOMIC_RELEASE);
    }
    return row;
}
static void EpollDiagAdd(EpollDiagSnapshot* row, EpollDiagCounter metric) {
    if (row) __atomic_fetch_add(&row->counters[metric], 1, __ATOMIC_RELAXED);
}
static int EpollScanReady(EpollWalk* walk, EpollInstance* ep,
                          EpollEvent* events, int maxevents, EpollDiagSnapshot* diag,
                          uint64_t eventsAddr) {
    EpollGraphGuard guard;
    walk->clearMemo();
    int n = 0;
    for (int i = 0; i < ep->count && n < maxevents; i++) {
        auto e = ep->entries[i];
        if (e.fd < 0) continue;
        if (!walk->pin(e.fd)) { ep->entries[i].fd = -1; continue; }
        EventFdSnapshot captured{};
        uint32_t ready = walk->watchReady(e, 0, &captured);
        if (ready) {
            walk->deliveries[n] = {i, captured};
            events[n].events = ready; events[n++].data.u64 = e.data;
            if (diag) {
                if (ready & EPOLLIN) EpollDiagAdd(diag, EpScanRead);
                if (ready & EPOLLOUT) EpollDiagAdd(diag, EpScanWrite);
                if (ready & EPOLLHUP) EpollDiagAdd(diag, EpScanHup);
                if (ready & EPOLLERR) EpollDiagAdd(diag, EpScanError);
                if (e.events & EPOLLET) EpollDiagAdd(diag, EpScanEdge);
                if (e.events & EPOLLONESHOT) EpollDiagAdd(diag, EpScanOneShot);
                __atomic_store_n(&diag->counters[EpLastFd], e.fd, __ATOMIC_RELAXED);
                __atomic_store_n(&diag->counters[EpLastType], static_cast<uint64_t>(walk->pins[e.fd]->type), __ATOMIC_RELAXED);
                __atomic_store_n(&diag->counters[EpLastRequested], e.events, __ATOMIC_RELAXED);
                __atomic_store_n(&diag->counters[EpLastReturned], ready, __ATOMIC_RELAXED);
            }
        }
    }
    if (n && !CopyToUser(eventsAddr, events, sizeof(EpollEvent) * n)) return -EFAULT;
    for (int i = 0; i < n; ++i) {
        const auto& delivery = walk->deliveries[i];
        if (!delivery.eventFd.identity) continue;
        auto& entry = ep->entries[delivery.index];
        // Commit exactly the snapshot returned to userspace, not newer source
        // generations that may have arrived during copyout.
        entry.eventFdIdentity = delivery.eventFd.identity;
        entry.inGeneration = delivery.eventFd.inGeneration;
        entry.outGeneration = delivery.eventFd.outGeneration;
    }
    return n;
}
static int EpollPollReady(Process* proc, EpollInstance* ep) {
    EpollWalkOwner state(proc);
    if (!state.walk) return -ENOMEM;
    EpollGraphGuard guard;
    return state.walk->any(ep) ? 1 : 0;
}

static int64_t epoll_create_impl(uint64_t flagsVal)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    auto* ep = static_cast<EpollInstance*>(kmalloc(sizeof(EpollInstance)));
    if (!ep) return -ENOMEM;
    for (int i = 0; i < EPOLL_MAX_FDS; i++) ep->entries[i] = {-1, 0, 0};
    ep->count = 0;
    ep->refCount = 1;
    ep->waiter = nullptr;

    int fd = FdAlloc(proc, FdType::EpollFd, ep);
    if (fd < 0) { kfree(ep); return -EMFILE; }

    if (flagsVal & 0x80000) // EPOLL_CLOEXEC
        proc->fds[fd].fdFlags |= 1;

    DbgPrintf("EPOLL: create fd=%d pid=%u tgid=%u flags=0x%lx ep=0x%lx\n",
              fd, proc->pid, proc->tgid, flagsVal,
              reinterpret_cast<uint64_t>(ep));
    return fd;
}

static int64_t sys_epoll_create(uint64_t size, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t)
{
    (void)size; // size hint ignored since Linux 2.6.8
    return epoll_create_impl(0);
}

static int64_t sys_epoll_create1(uint64_t flags, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t)
{
    return epoll_create_impl(flags);
}

static int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t watchfd,
                               uint64_t eventAddr, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef epref(proc, static_cast<int>(epfd));
    FdRef watchref(proc, static_cast<int>(watchfd));
    if (!epref || !watchref || epref->type==FdType::Path || watchref->type==FdType::Path) return -EBADF;
    if (epref->type != FdType::EpollFd || !epref->handle) return -EINVAL;
    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL) return -EINVAL;
    auto* ep = static_cast<EpollInstance*>(epref->handle);
    if (watchref->type == FdType::EpollFd && watchref->handle == ep) return -EINVAL;
    EpollEvent ev{};
    if (op != EPOLL_CTL_DEL) {
        if (!UserBufferReadable(eventAddr, sizeof(ev))) return -EFAULT;
        __builtin_memcpy(&ev, reinterpret_cast<const void*>(eventAddr), sizeof(ev));
    }
    EpollWalkOwner state(proc);
    if (!state.walk) return -ENOMEM;
    EpollGraphGuard guard;
    int existing = -1, free = -1;
    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        if (ep->entries[i].fd == static_cast<int>(watchfd)) existing = i;
        if (ep->entries[i].fd < 0 && free < 0) free = i;
    }
    if (op == EPOLL_CTL_DEL) {
        if (existing < 0) return -ENOENT;
        ep->entries[existing].fd = -1;
        return 0;
    }
    if (op == EPOLL_CTL_MOD && existing < 0) return -ENOENT;
    if (op == EPOLL_CTL_ADD && existing >= 0) return -EEXIST;
    int slot = op == EPOLL_CTL_MOD ? existing : free;
    if (slot < 0) return -ENOMEM;
    const int oldCount = ep->count;
    const auto old = ep->entries[slot];
    ep->entries[slot] = {static_cast<int>(watchfd), ev.events, ev.data.u64};
    if (slot >= ep->count) ep->count = slot + 1;
    if (op == EPOLL_CTL_ADD && watchref->type == FdType::EpollFd) {
        // Include ancestors: adding at the bottom can also exceed the limit.
        bool valid = state.walk->height(ep) <= EPOLL_MAX_DEPTH;
        for (int fd = 0; fd < EPOLL_MAX_FDS && valid; fd++) {
            auto* fde = state.walk->pin(fd);
            if (fde && fde->type == FdType::EpollFd && fde->handle)
                valid = state.walk->height(static_cast<EpollInstance*>(fde->handle)) <= EPOLL_MAX_DEPTH;
        }
        if (!valid) { ep->entries[slot] = old; ep->count = oldCount; return -ELOOP; }
    }
    return 0;
}

static int64_t epoll_wait_impl(Process* proc, EpollInstance* ep,
                                EpollEvent* kEvents, int maxevents,
                                int timeout_ms, EpollDiagSnapshot* diag, uint64_t eventsAddr)
{
    const uint64_t deadline = WaitDeadlineNs(timeout_ms < 0 ? ~0ULL : static_cast<uint64_t>(timeout_ms));
    bool firstScan = true;
    for (;;) {
        EpollDiagAdd(diag, EpIterations);
        EpollWalkOwner state(proc);
        auto* walk = state.walk;
        if (!walk) return -ENOMEM;
        int n = EpollScanReady(walk, ep, kEvents, maxevents, diag, eventsAddr);
        if (n < 0) return n;
        if (n > 0) { EpollDiagAdd(diag, firstScan ? EpInitialReady : EpLaterReady); return n; }
        if (timeout_ms == 0) { EpollDiagAdd(diag, EpTimeouts); return 0; }
        firstScan = false;
        {
            EpollGraphGuard guard;
            walk->clearMemo();
            walk->collect(ep);
        }
        TimerWaitGroup timerWaiters;
        PipePollGroup namedWaiters;bool namedWaitFailed=false;
        auto registerWaiters = [&]() {
            for (int wfd = 0; wfd < EPOLL_MAX_FDS; wfd++) {
                FdEntry* fde = walk->pins[wfd];
                if (!walk->masks[wfd]) continue;
                if (!fde || !fde->handle) continue;
                if(FdNamedFifo(fde)) {
                    if(!namedWaiters.add(FdPipe(fde),proc,walk->masks[wfd]|16|8))namedWaitFailed=true;
                } else if (fde->type == FdType::Pipe) {
                    static_cast<PipeBuffer*>(fde->handle)->epollWaiter = proc;
                } else if (fde->type == FdType::EventFd) {
                    static_cast<EventFdData*>(fde->handle)->epollWaiter = proc;
                } else if (fde->type == FdType::TimerFd) {
                    if (walk->masks[wfd] & EPOLLIN)
                        timerWaiters.add(static_cast<TimerFdData*>(fde->handle), proc);
                } else if (fde->type == FdType::Socket) {
                    int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                    brook::SockSetPollWaiter(si, proc);
                } else if (fde->type == FdType::UnixSocket) {
                    auto* usd = static_cast<UnixSocketData*>(fde->handle);
                    uint32_t events = walk->masks[wfd];
                    if ((events & EPOLLIN) && usd->state == UnixSocketData::State::Listening) {
                        usd->epollWaiter = proc;
                    } else if ((events & (EPOLLIN | EPOLLRDHUP)) &&
                               usd->state == UnixSocketData::State::Connected && usd->rxPipe) {
                        usd->rxPipe->epollWaiter = proc;
                    }
                    if ((events & EPOLLOUT) &&
                        usd->state == UnixSocketData::State::Connected && usd->txPipe)
                        usd->txPipe->epollWaiter = proc;
                }
            }
        };
        auto unregisterWaiters = [&]() {
            for (int wfd = 0; wfd < EPOLL_MAX_FDS; wfd++) {
                FdEntry* fde = walk->pins[wfd];
                if (!walk->masks[wfd]) continue;
                if (!fde || !fde->handle) continue;
                if (fde->type == FdType::Pipe) {
                    auto* pb = static_cast<PipeBuffer*>(fde->handle);
                    if (pb->epollWaiter == proc) pb->epollWaiter = nullptr;
                } else if (fde->type == FdType::EventFd) {
                    auto* efd = static_cast<EventFdData*>(fde->handle);
                    if (efd->epollWaiter == proc) efd->epollWaiter = nullptr;
                } else if (fde->type == FdType::Socket) {
                    int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                    brook::SockSetPollWaiter(si, nullptr);
                } else if (fde->type == FdType::UnixSocket) {
                    auto* usd = static_cast<UnixSocketData*>(fde->handle);
                    if (usd->epollWaiter == proc) usd->epollWaiter = nullptr;
                    if (usd->rxPipe && usd->rxPipe->epollWaiter == proc)
                        usd->rxPipe->epollWaiter = nullptr;
                    if (usd->txPipe && usd->txPipe->epollWaiter == proc)
                        usd->txPipe->epollWaiter = nullptr;
                }
            }
        };

        registerWaiters();
        if(namedWaitFailed) {unregisterWaiters();return -ENOMEM;}
        // The graph mutex can sleep. Arm the timeout only after this scan:
        // SchedulerUnblock clears wakeupTick when granting a contended mutex.
        n = EpollScanReady(walk, ep, kEvents, maxevents, diag, eventsAddr);
        if (!n && !HasPendingSignals() && (timeout_ms < 0 || ClockNowNs(1) < deadline)) {
            uint64_t safetyMs = timerWaiters.allocationFailed ? 5 : 100;
            for (int fd = 0; fd < EPOLL_MAX_FDS; fd++) {
                auto* fde = walk->pins[fd];
                if (!walk->masks[fd] || !fde) continue;
                if (fde->type == FdType::DevDsp) {
                    safetyMs = 5;
                    continue;
                }
            }
            const uint64_t timerDeadline = timerWaiters.deadline(deadline);
            proc->wakeupTick = WaitWakeupTick(timerDeadline, safetyMs);
            EpollDiagAdd(diag, EpBlocks);
            SchedulerBlockInterruptible(proc);
        }
        unregisterWaiters();
        if (n < 0) return n;
        if (n > 0) { EpollDiagAdd(diag, EpLaterReady); return n; }
        if (HasPendingSignals()) { EpollDiagAdd(diag, EpInterrupts); return -EINTR; }
        n = EpollScanReady(walk, ep, kEvents, maxevents, diag, eventsAddr);
        if (n < 0) return n;
        if (n > 0) { EpollDiagAdd(diag, EpLaterReady); return n; }
        if (timeout_ms >= 0 && ClockNowNs(1) >= deadline) {
            EpollDiagAdd(diag, EpTimeouts); return 0;
        }
        // Release pins only after unregistering the exact flattened leaves.
    }
}

static int64_t sys_epoll_wait(uint64_t epfd, uint64_t eventsAddr,
                               uint64_t maxevents, uint64_t timeout_ms,
                               uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    // BRO-156: pin epfd for the whole wait — epoll_wait_impl dereferences `ep`
    // (= epfde->handle) across SchedulerBlock, so a sibling close() of epfd must
    // not free the EpollInstance mid-wait.
    FdRef epfde_ref(proc, static_cast<int>(epfd));
    FdEntry* epfde = epfde_ref.get();
    if (!epfde || epfde->type != FdType::EpollFd || !epfde->handle) {
        // BRO-005 diagnostic: cross-check ProcessCurrent() against the pid
        // the scheduler stamped into cpuEnv->currentPid (gs:40) at the most
        // recent context switch. If these disagree, ProcessCurrent() is
        // returning a stale per-CPU pointer and the bug is in the scheduler,
        // not the fd table.
        uint32_t curPidGs = 0;
        __asm__ volatile("movl %%gs:40, %0" : "=r"(curPidGs));
        Process* proc2 = ProcessCurrent();
        SerialPrintf("sys_epoll_wait: EBADF epfd=%lu pid=%u tgid=%u "
                     "fde=%p type=%d handle=%p | cpu=%u gs:currentPid=%u "
                     "proc=%p proc2=%p proc2pid=%u\n",
                     epfd, proc->pid, proc->tgid, epfde,
                     epfde ? static_cast<int>(epfde->type) : -1,
                     epfde ? epfde->handle : nullptr,
                     SmpCurrentCpuIndex(), curPidGs,
                     proc, proc2, proc2 ? proc2->pid : 0);
        return -EBADF;
    }
    auto* ep = static_cast<EpollInstance*>(epfde->handle);

    if (maxevents <= 0 || maxevents > 1024) return -EINVAL;

    // Small event batches do not need to visit the global heap. Keep the stack
    // budget fixed (192 bytes), with the existing heap path for larger batches.
    EpollEvent localEvents[16];
    auto* kEvents = maxevents <= 16 ? localEvents : static_cast<EpollEvent*>(
        kmalloc(sizeof(EpollEvent) * static_cast<uint32_t>(maxevents)));
    if (!kEvents) return -ENOMEM;

    auto* diag = EpollDiagFor(proc);
    EpollDiagAdd(diag, EpCalls);
    if (static_cast<int>(static_cast<int64_t>(timeout_ms)) == 0) EpollDiagAdd(diag, EpZeroTimeout);
    int n = epoll_wait_impl(proc, ep, kEvents,
                             static_cast<int>(maxevents),
                             static_cast<int>(static_cast<int64_t>(timeout_ms)), diag, eventsAddr);

    if (kEvents != localEvents) kfree(kEvents);
    return n;
}

static int64_t sys_epoll_pwait(uint64_t epfd, uint64_t eventsAddr,
                                uint64_t maxevents, uint64_t timeout_ms,
                                uint64_t sigmaskAddr, uint64_t sigsetsize)
{
    int error = InstallWaitSignalMask(sigmaskAddr, sigsetsize);
    if (error) return error;
    return sys_epoll_wait(epfd, eventsAddr, maxevents, timeout_ms, 0, 0);
}

// ---------------------------------------------------------------------------
// timerfd — timerfd_create / timerfd_settime / timerfd_gettime
// ---------------------------------------------------------------------------
//
// Used by Wayland clients for frame pacing and animation timers.
// We implement a simple periodic/one-shot timer using LAPIC tick count.

static constexpr int CLOCK_REALTIME  = 0;
static constexpr int CLOCK_MONOTONIC = 1;

struct ITimerSpec {
    struct {
        int64_t tv_sec;
        int64_t tv_nsec;
    } it_interval; // period (0 = one-shot)
    struct {
        int64_t tv_sec;
        int64_t tv_nsec;
    } it_value;    // initial expiry (0 = disarmed)
};

static int64_t sys_timerfd_create(uint64_t clockid, uint64_t flags,
                                   uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC)
        return -EINVAL;

    if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC)) return -EINVAL;
    auto* tfd = static_cast<TimerFdData*>(kmalloc(sizeof(TimerFdData)));
    if (!tfd) return -ENOMEM;
    __builtin_memset(tfd, 0, sizeof(*tfd));
    tfd->clockId = static_cast<int>(clockid);
    tfd->refCount = 1;

    int fd = FdAlloc(proc, FdType::TimerFd, tfd);
    if (fd < 0) { kfree(tfd); return -EMFILE; }

    if (flags & TFD_NONBLOCK)
        proc->fds[fd].statusFlags = 0x800;
    if (flags & TFD_CLOEXEC)
        proc->fds[fd].fdFlags |= 1;

    return fd;
}

static ITimerSpec TimerFdSnapshotLocked(TimerFdData* timer) {
    const uint64_t now = ClockNowNs(timer->clockId);
    timer->state.refresh(now);
    const uint64_t remaining = timer->state.armed ? timer->state.deadline - now : 0;
    ITimerSpec value{};
    value.it_value.tv_sec = remaining / 1000000000ULL;
    value.it_value.tv_nsec = remaining % 1000000000ULL;
    value.it_interval.tv_sec = timer->state.interval / 1000000000ULL;
    value.it_interval.tv_nsec = timer->state.interval % 1000000000ULL;
    return value;
}
static int64_t sys_timerfd_settime(uint64_t fd, uint64_t flagsVal,
                                    uint64_t newValAddr, uint64_t oldValAddr,
                                    uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef pin(proc, static_cast<int>(fd));
    auto* fde = pin.get();
    if (!fde) return -EBADF;
    if (fde->type != FdType::TimerFd || !fde->handle) return -EINVAL;
    // Wall-clock setting/cancellation is not implemented. Reject unsupported
    // flags rather than claiming CANCEL_ON_SET behavior.
    if (flagsVal & ~static_cast<uint64_t>(TFD_TIMER_ABSTIME)) return -EINVAL;
    ITimerSpec value{};
    if (!CopyFromUser(&value, newValAddr, sizeof(value))) return -EFAULT;
    uint64_t initial, interval;
    if (!brook::TimerFdNanoseconds(value.it_value.tv_sec, value.it_value.tv_nsec, &initial) ||
        !brook::TimerFdNanoseconds(value.it_interval.tv_sec, value.it_interval.tv_nsec, &interval))
        return -EINVAL;
    auto* timer = static_cast<TimerFdData*>(fde->handle);
    TimerFdGuard guard(timer);
    const uint64_t now = ClockNowNs(timer->clockId);
    const ITimerSpec old = TimerFdSnapshotLocked(timer);
    if (oldValAddr && !CopyToUser(oldValAddr, &old, sizeof(old))) return -EFAULT;
    timer->state.interval = interval;
    timer->state.pending = 0;
    timer->state.armed = initial != 0;
    timer->state.deadline = !initial ? 0 : (flagsVal & TFD_TIMER_ABSTIME) ? initial : TimeoutAddSafe(now, initial);
    __atomic_fetch_add(&timer->rearmGeneration, 1, __ATOMIC_RELEASE);
    TimerFdWakeLocked(timer);
    return 0;
}

static int64_t sys_timerfd_gettime(uint64_t fd, uint64_t currValAddr,
                                    uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef pin(proc, static_cast<int>(fd));
    auto* fde = pin.get();
    if (!fde) return -EBADF;
    if (fde->type != FdType::TimerFd || !fde->handle) return -EINVAL;
    auto* timer = static_cast<TimerFdData*>(fde->handle);
    TimerFdGuard guard(timer);
    const ITimerSpec value = TimerFdSnapshotLocked(timer);
    return CopyToUser(currValAddr, &value, sizeof(value)) ? 0 : -EFAULT;
}

// ---------------------------------------------------------------------------
// sys_memfd_create (319) — anonymous in-memory file
// ---------------------------------------------------------------------------
//
// Wayland uses memfd for shared memory buffers (wl_shm).
// We implement it as a SyntheticMem fd backed by a heap allocation.

static int64_t sys_memfd_create(uint64_t nameAddr, uint64_t flags,
                                  uint64_t, uint64_t, uint64_t, uint64_t)
{
    flags = static_cast<uint32_t>(flags);
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    KernelOperationGuard operation(proc);

    // Name is just for debugging; we don't enforce it
    char name[64] = "<memfd>";
    if (nameAddr >= 0x1000) {
        const char* src = reinterpret_cast<const char*>(nameAddr);
        for (int i = 0; i < (int)(sizeof(name) - 1) && src[i]; i++) name[i] = src[i], name[i+1] = 0;
    }

    auto* mfd = static_cast<MemFdData*>(kmalloc(sizeof(MemFdData)));
    if (!mfd) return -ENOMEM;
    mfd->inode        = NextMemFdInode();
    if(!mfd->inode) {kfree(mfd);return -75;}
    mfd->pageMap      = nullptr;
    mfd->pageMapCount = 0;
    mfd->size         = 0;
    mfd->capacity     = 0;
    mfd->lock         = 0;
    mfd->refCount     = 0; // bumped by FdAlloc below
    mfd->seals        = {flags&MFD_ALLOW_SEALING?0u:1u,0};

    int fd = MemFdInstall(proc, mfd, 2u | ((flags & MFD_CLOEXEC) ? 0x80000u : 0));
    if (fd < 0) kfree(mfd);

    return fd;
}

// ---------------------------------------------------------------------------
// sys_sendfile (40) — stub: copies between fds in kernel
// ---------------------------------------------------------------------------

static int64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd, uint64_t offsetAddr,
                             uint64_t count, uint64_t, uint64_t)
{
    out_fd=static_cast<uint32_t>(out_fd);in_fd=static_cast<uint32_t>(in_fd);
    Process* proc=ProcessCurrent();if(!proc)return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef input(proc,static_cast<int>(in_fd)),output(proc,static_cast<int>(out_fd));
    if(!input || !output)return -EBADF;
    auto* out_fde=output.get();
    if(out_fde->type==FdType::Path)return -EBADF;
    if(out_fde->statusFlags&0x400u)return -EINVAL; // Linux forbids append output.
    if((out_fde->type==FdType::MemFd || (out_fde->type==FdType::Vnode && out_fde->handle)) &&
       (out_fde->statusFlags&3)==0)return -EBADF;
    auto* in_fde=input.get();
    if(in_fde->type==FdType::Path || (in_fde->statusFlags&3)==1)return -EBADF;
    if((in_fde->type!=FdType::Vnode && in_fde->type!=FdType::MemFd) || !in_fde->handle)return -EINVAL;
    uint64_t offset=in_fde->seekPos;
    if(offsetAddr) {
        if(!UserBufferWritable(offsetAddr,8) || !CopyFromUser(&offset,offsetAddr,8))return -EFAULT;
    }
    if(offset>0x7fffffffffffffffULL)return -EINVAL;
    if(!count)return 0;
    if(count>256*1024)count=256*1024;
    uint32_t chunk=count>4096?4096:static_cast<uint32_t>(count);
    auto* bounce=static_cast<uint8_t*>(kmalloc(chunk));if(!bounce)return -ENOMEM;
    int64_t total=0,error=0;
    while(static_cast<uint64_t>(total)<count) {
        uint64_t remaining=count-static_cast<uint64_t>(total),amount=remaining<chunk?remaining:chunk;
        uint64_t readOffset=offset;
        int64_t read=in_fde->type==FdType::MemFd?
            MemFdReadAt(static_cast<MemFdData*>(in_fde->handle),bounce,amount,&readOffset):
            VfsRead(static_cast<Vnode*>(in_fde->handle),bounce,amount,&readOffset);
        if(read<=0) {error=read;break;}
        int64_t written=WriteToPinnedFdByType(proc,output.get(),out_fd,reinterpret_cast<uint64_t>(bounce),static_cast<uint64_t>(read),false);
        if(written<=0 || written>read) {error=written<0?written:-EIO;break;}
        offset+=static_cast<uint64_t>(written);total+=written;
        if(written<read)break;
    }
    kfree(bounce);
    if(offsetAddr) {if(!CopyToUser(offsetAddr,&offset,8))return -EFAULT;}
    else in_fde->seekPos=offset;
    return total?total:error;
}

// ---------------------------------------------------------------------------
// sys_getrusage (98) — stub
// ---------------------------------------------------------------------------

static int64_t sys_getrusage(uint64_t who, uint64_t usageAddr, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(usageAddr, 144)) return -EFAULT;
    __builtin_memset(reinterpret_cast<void*>(usageAddr), 0, 144);

    Process* proc = ProcessCurrent();
    if (!proc) return 0;

    // struct rusage: first two fields are struct timeval (utime, stime)
    // timeval = { int64_t tv_sec; int64_t tv_usec; }
    auto* ru = reinterpret_cast<int64_t*>(usageAddr);

    uint64_t uticks = proc->userTicks;
    uint64_t sticks = proc->sysTicks;

    if (who == 0) // RUSAGE_SELF
    {
        ru[0] = static_cast<int64_t>(uticks / 1000);        // utime.tv_sec
        ru[1] = static_cast<int64_t>((uticks % 1000) * 1000); // utime.tv_usec
        ru[2] = static_cast<int64_t>(sticks / 1000);        // stime.tv_sec
        ru[3] = static_cast<int64_t>((sticks % 1000) * 1000); // stime.tv_usec
    }
    // RUSAGE_CHILDREN (-1) — we include reaped ticks
    else if (who == static_cast<uint64_t>(-1ULL))
    {
        uint64_t reapedUser = 0, reapedSys = 0;
        SchedulerGetReapedTicks(reapedUser, reapedSys);
        ru[0] = static_cast<int64_t>(reapedUser / 1000);
        ru[1] = static_cast<int64_t>((reapedUser % 1000) * 1000);
        ru[2] = static_cast<int64_t>(reapedSys / 1000);
        ru[3] = static_cast<int64_t>((reapedSys % 1000) * 1000);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sys_sysinfo (99) — basic system info
// ---------------------------------------------------------------------------

static int64_t sys_sysinfo(uint64_t infoAddr, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    struct sysinfo_s {
        int64_t  uptime;
        uint64_t loads[3];
        uint64_t totalram;
        uint64_t freeram;
        uint64_t sharedram;
        uint64_t bufferram;
        uint64_t totalswap;
        uint64_t freeswap;
        uint16_t procs;
        uint16_t pad;
        uint32_t pad2;
        uint64_t totalhigh;
        uint64_t freehigh;
        uint32_t mem_unit;
    };
    if (!UserBufferWritable(infoAddr, sizeof(sysinfo_s))) return -EFAULT;
    auto* info = reinterpret_cast<sysinfo_s*>(infoAddr);
    __builtin_memset(info, 0, sizeof(sysinfo_s));

    info->uptime = static_cast<int64_t>(ApicTickCount() / 1000);

    uint64_t totalPages = PmmGetRamPageCount();
    uint64_t freePages  = PmmGetFreePageCount();
    info->totalram = totalPages * 4096;
    info->freeram  = freePages  * 4096;

    // Count running processes and set load averages
    uint32_t total = 0, running = 0;
    SchedulerGetProcessCounts(total, running);
    info->procs = static_cast<uint16_t>(total);

    // Load averages: Linux uses SI_LOAD_SHIFT=16, values are fixed-point << 16
    uint32_t avg1, avg5, avg15;
    SchedulerGetLoadAvg(avg1, avg5, avg15);
    // Our values are * 1000; Linux expects * 65536. Convert: (val * 65536) / 1000
    info->loads[0] = static_cast<uint64_t>(avg1) * 65536 / 1000;
    info->loads[1] = static_cast<uint64_t>(avg5) * 65536 / 1000;
    info->loads[2] = static_cast<uint64_t>(avg15) * 65536 / 1000;

    info->mem_unit = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// sys_times (100) — process times
// ---------------------------------------------------------------------------

static int64_t sys_times(uint64_t bufAddr, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    // struct tms { clock_t tms_utime, tms_stime, tms_cutime, tms_cstime; }
    // clock_t is int64_t on x86_64, 4 fields = 32 bytes
    if (bufAddr)
    {
        if (!UserBufferWritable(bufAddr, 32)) return -EFAULT;
        auto* tms = reinterpret_cast<int64_t*>(bufAddr);

        Process* proc = ProcessCurrent();
        if (proc)
        {
            tms[0] = static_cast<int64_t>(proc->userTicks); // tms_utime
            tms[1] = static_cast<int64_t>(proc->sysTicks);  // tms_stime
        }
        else
        {
            tms[0] = 0;
            tms[1] = 0;
        }
        // Children times (accumulated from reaped processes)
        uint64_t reapedUser = 0, reapedSys = 0;
        SchedulerGetReapedTicks(reapedUser, reapedSys);
        tms[2] = static_cast<int64_t>(reapedUser); // tms_cutime
        tms[3] = static_cast<int64_t>(reapedSys);  // tms_cstime
    }

    // Return value: clock ticks since boot
    return static_cast<int64_t>(ApicTickCount());
}

// ---------------------------------------------------------------------------
// sys_umask (95) — set process umask, return old value
// ---------------------------------------------------------------------------

static int64_t sys_umask(uint64_t newMask, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return ProcessFsExchangeMask(proc,static_cast<uint16_t>(newMask));
}

// ---------------------------------------------------------------------------
// sys_chdir (80) / sys_fchdir (81) — canonical path and final search check
// ---------------------------------------------------------------------------

static int64_t sys_chdir(uint64_t pathAddr,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)
{
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    for(unsigned attempt=0;attempt<8;++attempt) {
        AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
        VfsPath cwd{};int error=VfsPathResolve(lookup.fs.root,lookup.start(),path,&cwd);
        if(!error)error=ProcessFsSetResolvedCwd(ProcessCurrent(),cwd,lookup.fs);
        VfsPathRelease(cwd);if(error!=-EAGAIN)return error;
    }
    return -EAGAIN;
}

static int64_t sys_fchdir(uint64_t fd,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)
{
    Process* proc=ProcessCurrent();if(!proc)return -EBADF;
    VfsPath cwd{};int error=0;
    { FdRef ref(proc,static_cast<int>(fd));
      if(!ref)return -EBADF;
      if((ref->type!=FdType::Vnode && ref->type!=FdType::Path) || !ref->handle)return -ENOTDIR;
      error=VfsPathFromVnode(static_cast<Vnode*>(ref->handle),&cwd); }
    if(!error)error=ProcessFsSetCwd(proc,cwd);
    VfsPathRelease(cwd);return error;
}

// ---------------------------------------------------------------------------
// sys_rmdir (84) — remove an empty directory
// ---------------------------------------------------------------------------

static int64_t DoUnlink(const char* path, bool directory = false);

static int64_t sys_rmdir(uint64_t pathAddr, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    char pathBuf[256];
    if (!CopyUserCString(pathAddr, pathBuf, sizeof(pathBuf))) return -EFAULT;
    return DoUnlink(pathBuf, true);
}

// ---------------------------------------------------------------------------
// sys_unlink (87) — delete a file
// ---------------------------------------------------------------------------

static int64_t DoUnlink(const char* path, bool directory)
{
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    char synthetic[512];
    if(!directory && !lookup.SyntheticName(path,synthetic,sizeof(synthetic)) &&
       !strncmp(synthetic,"/dev/shm/",9))return NamedShmUnlink(synthetic+9);
    return VfsRemoveAt(lookup.fs.root,lookup.start(),path,directory);
}

static int64_t sys_unlink(uint64_t pathAddr, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    char pathBuf[256];
    if (!CopyUserCString(pathAddr, pathBuf, sizeof(pathBuf))) return -EFAULT;
    return DoUnlink(pathBuf);
}

// ---------------------------------------------------------------------------
// sys_rename (82) — rename a file or directory
// ---------------------------------------------------------------------------

static int64_t DoRename(const char* oldBuf, const char* newBuf)
{
    AtLookup oldLookup,newLookup;
    if(int error=oldLookup.Init(-100,oldBuf))return error;
    if(int error=newLookup.Init(-100,newBuf))return error;
    return VfsRenameAt(oldLookup.fs.root,oldLookup.start(),oldBuf,newLookup.start(),newBuf);
}

static int64_t sys_rename(uint64_t oldAddr, uint64_t newAddr, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    char oldBuf[256], newBuf[256];
    if (!CopyUserCString(oldAddr, oldBuf, sizeof(oldBuf))) return -EFAULT;
    if (!CopyUserCString(newAddr, newBuf, sizeof(newBuf))) return -EFAULT;
    return DoRename(oldBuf, newBuf);
}

static int64_t DoMkdir(const char* path, uint16_t mode)
{
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    return VfsMkdirAt(lookup.fs.root,lookup.start(),path,mode,lookup.fs.mask);
}

static int64_t sys_mkdir(uint64_t pathAddr, uint64_t mode, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    char pathBuf[256];
    if (!CopyUserCString(pathAddr, pathBuf, sizeof(pathBuf))) return -EFAULT;
    return DoMkdir(pathBuf,static_cast<uint16_t>(mode));
}

// ---------------------------------------------------------------------------
// sys_mkdirat (258) — create a directory relative to dirfd
// ---------------------------------------------------------------------------

static int64_t sys_mknodat(uint64_t dirfd,uint64_t pathAddr,uint64_t mode,uint64_t device,uint64_t,uint64_t)
{
    if((mode&0170000)!=0010000)return -95;
    if(device)return -EINVAL;
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    int error=0;Vnode* node=VfsCreateFifoAt(lookup.fs.root,lookup.start(),path,static_cast<uint16_t>(mode),lookup.fs.mask,&error);
    if(!node)return error?error:-EIO;
    VfsClose(node);return 0;
}
static int64_t sys_mknod(uint64_t path,uint64_t mode,uint64_t device,uint64_t,uint64_t,uint64_t)
{ return sys_mknodat(static_cast<uint64_t>(-100),path,mode,device,0,0); }

static int64_t sys_mkdirat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                            uint64_t, uint64_t, uint64_t)
{
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    return VfsMkdirAt(lookup.fs.root,lookup.start(),path,static_cast<uint16_t>(mode),lookup.fs.mask);
}

// ---------------------------------------------------------------------------
// sys_unlinkat (263) — unlink file or rmdir relative to dirfd
// ---------------------------------------------------------------------------

static int64_t sys_unlinkat(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                             uint64_t, uint64_t, uint64_t)
{
    if(flags & ~0x200ULL)return -EINVAL;
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    char synthetic[512];
    if(!(flags&0x200) && !lookup.SyntheticName(path,synthetic,sizeof(synthetic)) &&
       !strncmp(synthetic,"/dev/shm/",9))return NamedShmUnlink(synthetic+9);
    return VfsRemoveAt(lookup.fs.root,lookup.start(),path,(flags&0x200)!=0);
}

// ---------------------------------------------------------------------------
// sys_renameat / sys_renameat2 (264 / 316)
// ---------------------------------------------------------------------------

static int64_t sys_renameat(uint64_t olddirfd, uint64_t oldAddr,
                             uint64_t newdirfd, uint64_t newAddr,
                             uint64_t, uint64_t)
{
    char oldP[512],newP[512];
    if(!CopyUserCString(oldAddr,oldP,sizeof(oldP)) || !CopyUserCString(newAddr,newP,sizeof(newP)))return -EFAULT;
    AtLookup oldLookup,newLookup;
    if(int error=oldLookup.Init(static_cast<int>(olddirfd),oldP))return error;
    if(int error=newLookup.Init(static_cast<int>(newdirfd),newP))return error;
    return VfsRenameAt(oldLookup.fs.root,oldLookup.start(),oldP,newLookup.start(),newP);
}

static int64_t sys_renameat2(uint64_t olddirfd, uint64_t oldAddr,
                              uint64_t newdirfd, uint64_t newAddr,
                              uint64_t flags, uint64_t)
{
    // Unsupported atomic rename policies must not silently replace a target.
    if (flags) return -EINVAL;
    return sys_renameat(olddirfd, oldAddr, newdirfd, newAddr, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_linkat (265) — create hard link
// ---------------------------------------------------------------------------

static int64_t sys_linkat(uint64_t olddirfd, uint64_t oldAddr,
                           uint64_t newdirfd, uint64_t newAddr,
                           uint64_t flags, uint64_t)
{
    (void)olddirfd; (void)newdirfd; (void)flags;
    // Hard links not supported yet — return EPERM
    (void)oldAddr; (void)newAddr;
    return -EPERM;
}

// ---------------------------------------------------------------------------
// sys_ftruncate (77) / sys_truncate (76) — change file size
// ---------------------------------------------------------------------------

static int64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    if (length > 0x7fffffffffffffffULL) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref || !ref.get()->handle) return -EBADF;
    FdEntry* fde = ref.get();
    if(fde->type==FdType::Path)return -EBADF;
    if (fde->type == FdType::Vnode) {
        if ((fde->statusFlags & 3) == 0) return -EINVAL;
        return VfsTruncate(static_cast<Vnode*>(fde->handle), length);
    }
    if (fde->type == FdType::MemFd) {
            if ((fde->statusFlags & 3) == 0) return -EINVAL;
            auto* mfd = static_cast<MemFdData*>(fde->handle);
            MfdLock(mfd);
            if(int error=MemFdSealedResize(mfd->seals,mfd->size,length)) {MfdUnlock(mfd);return error;}
            if (!MemFdGrowLocked(mfd, length)) { MfdUnlock(mfd); return -ENOMEM; }
            // Shrinking ftruncate: free pages past the new tail. (Growth is
            // free — pages are unbacked / read-as-zero until first access.)
            if (length < mfd->size) {
                const uint64_t tail=length&4095,index=length/4096;
                if(tail && index<mfd->pageMapCount && mfd->pageMap[index])
                    memset(reinterpret_cast<void*>(PhysToVirt(PhysicalAddress(mfd->pageMap[index])).raw()+tail),0,4096-tail);
                uint64_t firstFreeIdx = (length + 4095) / 4096;
                for (uint64_t i = firstFreeIdx; i < mfd->pageMapCount; i++) {
                    if (mfd->pageMap[i]) {
                        PmmFreePage(PhysicalAddress(mfd->pageMap[i]));
                        mfd->pageMap[i] = 0;
                    }
                }
            }
            mfd->size = length;
            MfdUnlock(mfd);
            return 0;
    }
    return -EINVAL;
}

static int64_t sys_truncate(uint64_t pathAddr, uint64_t length, uint64_t,
                             uint64_t, uint64_t, uint64_t)
{
    if (length > 0x7fffffffffffffffULL) return -EINVAL;
    char path[256];
    if (!CopyUserCString(pathAddr, path, sizeof(path))) return -EFAULT;
    if (!path[0]) return -ENOENT;
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    int result=0;
    Vnode* vn=VfsOpenAt(lookup.fs.root,lookup.start(),path,VFS_O_WRITE,0,lookup.fs.mask,&result);
    if(!vn)return result?result:-ENOENT;
    result = VfsTruncate(vn, length);
    VfsClose(vn);
    return result;
}

// ---------------------------------------------------------------------------
// sys_flock (73) — advisory file locking (stub: always succeeds)
// ---------------------------------------------------------------------------

static int64_t sys_flock(uint64_t fd, uint64_t operation, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    (void)fd; (void)operation;
    // Advisory locking — with a single-user OS, always succeed
    return 0;
}

// ---------------------------------------------------------------------------
// sys_chmod (90) / sys_fchmod (91) / sys_fchmodat (268)
// — dispatch mode changes through the file's actual mount
// ---------------------------------------------------------------------------

static int64_t sys_chmod(uint64_t pathAddr, uint64_t mode, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    return VfsChmodAt(lookup.fs.root,lookup.start(),path,static_cast<uint16_t>(mode&07777));
}

static int64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    FdRef ref(proc,static_cast<int>(fd));FdEntry* entry=ref.get();
    if (!entry || !entry->handle || entry->type==FdType::Path) return -EBADF;
    if (entry->type != FdType::Vnode) return -95; // EOPNOTSUPP
    return VfsChmod(static_cast<Vnode*>(entry->handle), static_cast<uint16_t>(mode & 07777));
}

static int64_t sys_fchmodat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                             uint64_t, uint64_t, uint64_t)
{
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    return VfsChmodAt(lookup.fs.root,lookup.start(),path,static_cast<uint16_t>(mode&07777));
}

// ---------------------------------------------------------------------------
// sys_chown (92) / sys_fchown (93) / sys_lchown (94) / sys_fchownat (260)
// — resolve caller IDs, then authorize against the retained backend owner
// ---------------------------------------------------------------------------

static int64_t ChownAt(int dirfd, uint64_t pathAddr, uint32_t uid, uint32_t gid, uint64_t flags)
{
    constexpr uint64_t NOFOLLOW=0x100,EMPTY_PATH=0x1000;
    if(flags & ~(NOFOLLOW|EMPTY_PATH))return -EINVAL;
    Process* proc=ProcessCurrent();if(!proc)return -EPERM;
    if(uid!=0xffffffffu){uid=ProcessResolveId(proc,false,uid);if(uid==UserIdInvalid)return -EINVAL;}
    if(gid!=0xffffffffu){gid=ProcessResolveId(proc,true,gid);if(gid==UserIdInvalid)return -EINVAL;}
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    if(!path[0]) {
        if(!(flags&EMPTY_PATH))return -ENOENT;
        if(dirfd==-100) {
            FsSnapshot fs{};int error=ProcessFsSnapshot(proc,&fs);
            if(!error)error=VfsChownAt(fs.root,fs.cwd,".",uid,gid);
            FsSnapshotRelease(&fs);return error;
        }
        FdRef ref(proc,dirfd);if(!ref)return -EBADF;
        if(ref->type!=FdType::Vnode || !ref->handle)return -95;
        return VfsChown(static_cast<Vnode*>(ref->handle),uid,gid);
    }
    AtLookup lookup;if(int error=lookup.Init(dirfd,path))return error;
    return VfsChownAt(lookup.fs.root,lookup.start(),path,uid,gid,!(flags&NOFOLLOW));
}

static int64_t sys_chown(uint64_t pathAddr, uint64_t uid, uint64_t gid,
                          uint64_t, uint64_t, uint64_t)
{
    return ChownAt(-100, pathAddr, static_cast<uint32_t>(uid), static_cast<uint32_t>(gid), 0);
}

static int64_t sys_fchown(uint64_t fd, uint64_t uid, uint64_t gid,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc)return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref || ref->type==FdType::Path) return -EBADF;
    if (ref->type != FdType::Vnode) return -95;
    uint32_t owner=static_cast<uint32_t>(uid),group=static_cast<uint32_t>(gid);
    if(owner!=0xffffffffu){owner=ProcessResolveId(proc,false,owner);if(owner==UserIdInvalid)return -EINVAL;}
    if(group!=0xffffffffu){group=ProcessResolveId(proc,true,group);if(group==UserIdInvalid)return -EINVAL;}
    return VfsChown(static_cast<Vnode*>(ref->handle),owner,group);
}

static int64_t sys_lchown(uint64_t pathAddr, uint64_t uid, uint64_t gid,
                           uint64_t, uint64_t, uint64_t)
{
    return ChownAt(-100, pathAddr, static_cast<uint32_t>(uid), static_cast<uint32_t>(gid), 0x100);
}

static int64_t sys_fchownat(uint64_t dirfd, uint64_t pathAddr, uint64_t uid,
                             uint64_t gid, uint64_t flags, uint64_t)
{
    return ChownAt(static_cast<int>(dirfd), pathAddr, static_cast<uint32_t>(uid), static_cast<uint32_t>(gid), flags);
}

// ---------------------------------------------------------------------------
// sys_utimensat (280) — change file timestamps
// ---------------------------------------------------------------------------

static int64_t sys_utimensat(uint64_t dirfd, uint64_t pathAddr, uint64_t timesAddr,
                              uint64_t flags, uint64_t, uint64_t)
{
    constexpr uint64_t NOFOLLOW = 0x100;
    constexpr uint64_t EMPTY_PATH = 0x1000;
    if (flags & ~(NOFOLLOW | EMPTY_PATH)) return -EINVAL;
    VnodeTime times[2] = {{0, VFS_UTIME_NOW}, {0, VFS_UTIME_NOW}};
    if (timesAddr) {
        struct UserTime { int64_t sec, nsec; };
        if (!UserBufferReadable(timesAddr, 2 * sizeof(UserTime))) return -EFAULT;
        const auto* userTimes = reinterpret_cast<const UserTime*>(timesAddr);
        for (int i = 0; i < 2; ++i) {
            UserTime value = userTimes[i];
            if (value.nsec != VFS_UTIME_NOW && value.nsec != VFS_UTIME_OMIT &&
                (value.nsec < 0 || value.nsec >= 1000000000)) return -EINVAL;
            times[i] = {value.sec, static_cast<int32_t>(value.nsec)};
        }
    }
    if (times[0].nsec == VFS_UTIME_OMIT && times[1].nsec == VFS_UTIME_OMIT) return 0;
    char path[256] = {};
    if (pathAddr && !CopyUserCString(pathAddr, path, sizeof(path))) return -EFAULT;
    if (!pathAddr || (!path[0] && (flags & EMPTY_PATH))) {
        if (flags & NOFOLLOW) return -EINVAL;
        Process* proc = ProcessCurrent();
        FdRef descriptor(proc,static_cast<int>(dirfd));
        FdEntry* entry = descriptor.get();
        if (!entry || !entry->handle || entry->type != FdType::Vnode) return -EBADF;
        return VfsSetTimes(static_cast<Vnode*>(entry->handle), times);
    }
    if (!path[0]) return -ENOENT;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    return VfsSetTimesAt(lookup.fs.root,lookup.start(),path,times,!(flags&NOFOLLOW));
}

// ---------------------------------------------------------------------------
// sys_sched_getaffinity (204) — get CPU affinity mask
// ---------------------------------------------------------------------------

static int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
                                      uint64_t maskAddr, uint64_t, uint64_t, uint64_t)
{
    (void)pid;
    if (cpusetsize < 8) return -EINVAL;
    if (!UserBufferWritable(maskAddr, 8)) return -EFAULT;

    // Report all 8 CPUs available
    auto* mask = reinterpret_cast<uint64_t*>(maskAddr);
    *mask = 0xFF; // 8 CPUs
    return 8; // size of mask in bytes
}

// ---------------------------------------------------------------------------
// sys_sched_setaffinity (203) — set CPU affinity mask (stub: succeed)
// ---------------------------------------------------------------------------

static int64_t sys_sched_setaffinity(uint64_t, uint64_t, uint64_t,
                                      uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
}

// ---------------------------------------------------------------------------
// sys_sched_setparam (142) / sys_sched_getparam (143)
// sys_sched_setscheduler (144) / sys_sched_getscheduler (145)
// sys_sched_get_priority_max (146) / sys_sched_get_priority_min (147)
// ---------------------------------------------------------------------------

static int64_t sys_sched_setparam(uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t)
{
    return 0; // pretend success
}

static int64_t sys_sched_getparam(uint64_t, uint64_t paramAddr, uint64_t,
                                   uint64_t, uint64_t, uint64_t)
{
    // Return sched_priority = 0 (default for SCHED_OTHER)
    if (paramAddr && UserBufferReadable(paramAddr, 4))
        *reinterpret_cast<int32_t*>(paramAddr) = 0;
    return 0;
}

static int64_t sys_sched_setscheduler(uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t, uint64_t)
{
    return 0; // pretend success
}

static int64_t sys_sched_getscheduler(uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t, uint64_t)
{
    return 0; // SCHED_OTHER
}

static int64_t sys_sched_get_priority_max(uint64_t policy, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t)
{
    // Linux returns: SCHED_OTHER=0, SCHED_FIFO/RR=99
    if (policy == 1 || policy == 2) return 99; // SCHED_FIFO / SCHED_RR
    return 0; // SCHED_OTHER / SCHED_BATCH / SCHED_IDLE
}

static int64_t sys_sched_get_priority_min(uint64_t policy, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t)
{
    if (policy == 1 || policy == 2) return 1; // SCHED_FIFO / SCHED_RR
    return 0;
}

// ---------------------------------------------------------------------------
// sys_statx (332) — extended stat
// ---------------------------------------------------------------------------

struct linux_statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  pad;
};

struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t pad1;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    linux_statx_timestamp stx_atime;
    linux_statx_timestamp stx_btime;
    linux_statx_timestamp stx_ctime;
    linux_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t pad2[13];
};

static int64_t sys_statx(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                          uint64_t mask, uint64_t bufAddr, uint64_t)
{
    (void)mask;
    char pathBuf[256];
    if (!CopyUserCString(pathAddr, pathBuf, sizeof(pathBuf))) return -EFAULT;
    if (!UserBufferWritable(bufAddr, sizeof(linux_statx))) return -EFAULT;
    const char* path = pathBuf;
    auto* stx = reinterpret_cast<linux_statx*>(bufAddr);

    auto fillStatxFromLinuxStat = [&](const LinuxStat& st) {
        auto* raw = reinterpret_cast<uint8_t*>(stx);
        memset(raw, 0, sizeof(linux_statx));
        stx->stx_mask = 0x7FF; // STATX_BASIC_STATS
        stx->stx_blksize = static_cast<uint32_t>(st.st_blksize ? st.st_blksize : 4096);
        stx->stx_nlink = static_cast<uint32_t>(st.st_nlink);
        stx->stx_uid = st.st_uid;
        stx->stx_gid = st.st_gid;
        stx->stx_mode = static_cast<uint16_t>(st.st_mode);
        stx->stx_ino = st.st_ino;
        stx->stx_dev_major = static_cast<uint32_t>(((st.st_dev >> 8) & 0xfff) | ((st.st_dev >> 32) & 0xfffff000));
        stx->stx_dev_minor = static_cast<uint32_t>((st.st_dev & 0xff) | ((st.st_dev >> 12) & 0xffffff00));
        stx->stx_size = static_cast<uint64_t>(st.st_size);
        stx->stx_blocks = static_cast<uint64_t>(st.st_blocks);
        stx->stx_atime.tv_sec = static_cast<int64_t>(st.st_atime_sec);
        stx->stx_atime.tv_nsec = static_cast<uint32_t>(st.st_atime_nsec);
        stx->stx_ctime.tv_sec = static_cast<int64_t>(st.st_ctime_sec);
        stx->stx_ctime.tv_nsec = static_cast<uint32_t>(st.st_ctime_nsec);
        stx->stx_mtime.tv_sec = static_cast<int64_t>(st.st_mtime_sec);
        stx->stx_mtime.tv_nsec = static_cast<uint32_t>(st.st_mtime_nsec);
    };

    static constexpr uint64_t AT_EMPTY_PATH = 0x1000;
    if (path[0] == '\0')
    {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        LinuxStat st;
        int64_t ret = sys_fstat(dirfd, reinterpret_cast<uint64_t>(&st), 0, 0, 0, 0);
        if (ret < 0) return ret;
        fillStatxFromLinuxStat(st);
        return 0;
    }

    VnodeStat st{};

    AtLookup at;if(int error=at.Init(static_cast<int>(dirfd),path))return error;
    char resolved[512];
    if(!at.SyntheticName(path,resolved,sizeof(resolved))) {
        LinuxStat dst;
        if(DrmStatFill(resolved,&dst)) {fillStatxFromLinuxStat(dst);return 0;}
    }
    int ret=VfsStatAt(at.fs.root,at.start(),path,&st,!(flags&0x100));
    if(ret)return ret==-1?-ENOENT:ret;

    // Use the same filesystem metadata as stat/fstatat. Inventing mode0644
    // made Git reject installed HTTPS helpers; inode1 hid file identities.
    LinuxStat dst;
    FillStat(&dst, st);
    fillStatxFromLinuxStat(dst);

    return 0;
}

// ---------------------------------------------------------------------------
// sys_fallocate (285) — real memfd reservation, not sparse logical growth.
// ---------------------------------------------------------------------------

static int64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset,
                              uint64_t length, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, static_cast<int>(fd)); // Linux fd/mode are low-32-bit ints.
    if (!descriptor) return -EBADF;
    FdEntry* entry = descriptor.get();
    if ((entry->statusFlags & 3u) == 0) return -EBADF;
    if (static_cast<int64_t>(offset) < 0 || static_cast<int64_t>(length) <= 0)
        return -EINVAL;
    if (offset > 0x7fffffffffffffffULL - length) return -27; // EFBIG
    uint32_t options = static_cast<uint32_t>(mode);
    if (options & ~1u) return -95; // EOPNOTSUPP: only KEEP_SIZE is supported.
    if (FdPipe(entry)) return -29; // ESPIPE
    if (entry->type == FdType::Vnode) return -95; // Backends have no reservation operation.
    if (entry->type != FdType::MemFd || !entry->handle) return -ENODEV;

    uint64_t end = offset + length;
    if (end > 16ULL * 1024 * 1024 * 1024) return -27; // Existing memfd index limit.
    auto* mfd = static_cast<MemFdData*>(entry->handle);
    MfdLock(mfd);
    // Linux forbids beyond-EOF reservation under GROW even with KEEP_SIZE.
    if(end>mfd->size && (mfd->seals.bits&4)) {MfdUnlock(mfd);return -EPERM;}
    if (!MemFdGrowLocked(mfd, end)) { MfdUnlock(mfd); return -ENOMEM; }

    // Stage only new physical pages in a private chain. Its two temporary
    // words are never published to the backing map or users. On OOM every
    // staged page is freed; existing bytes, size and seek position stay intact.
    uint64_t staged = 0;
    uint64_t first = offset / 4096;
    uint64_t last = (end - 1) / 4096;
    for (uint64_t index = first; index <= last; ++index) {
        if (mfd->pageMap[index]) continue;
        PhysicalAddress page = PmmAllocPage(MemTag::User, 0);
        if (!page) {
            while (staged) {
                auto* words = reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(staged)).raw());
                uint64_t next = words[0];
                PmmFreePage(PhysicalAddress(staged));
                staged = next;
            }
            MfdUnlock(mfd);
            return -28; // ENOSPC: reservation could not be satisfied.
        }
        auto* words = reinterpret_cast<uint64_t*>(PhysToVirt(page).raw());
        words[0] = staged;
        words[1] = index;
        staged = page.raw();
    }
    while (staged) {
        auto* words = reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(staged)).raw());
        uint64_t next = words[0], index = words[1];
        memset(words, 0, 4096);
        mfd->pageMap[index] = staged;
        staged = next;
    }
    if (!(options & 1u) && end > mfd->size) mfd->size = end;
    MfdUnlock(mfd);
    return 0;
}

// ---------------------------------------------------------------------------
// sys_mincore (27) — report per-page residency for a mapped range.
//
// POSIX/Linux semantics that callers depend on:
//   * addr must be page-aligned            -> else -EINVAL
//   * the whole [addr, addr+length) range must be mapped, otherwise the call
//     fails with -ENOMEM (it does NOT partially fill vec).
//   * vec[i] bit0 = 1 if the page is resident in memory, 0 otherwise.
//
// The old stub ignored addr and reported every page resident with a success
// return. That broke pointer-validity probes that rely on mincore failing for
// unmapped addresses — notably Mesa's _eglPointerIsDereferenceable(), which
// uses mincore() to decide whether a wl_egl_window begins with a wl_surface*
// (old ABI) or an intptr_t version (new ABI). A bogus "resident" answer for the
// null page made Mesa treat the version field as the surface pointer and crash
// in wl_proxy_create_wrapper (BRO-204).
static int64_t sys_mincore(uint64_t addr, uint64_t length, uint64_t vecAddr,
                            uint64_t, uint64_t, uint64_t)
{
    if (addr & 0xFFFULL) return -EINVAL;      // addr must be page-aligned

    brook::Process* proc = brook::ProcessCurrent();
    if (!proc) return -ENOMEM;

    uint64_t pages = (length + 4095) / 4096;
    if (pages == 0) return 0;
    if (!UserBufferWritable(vecAddr, pages)) return -EFAULT;

    uint64_t end;
    if (__builtin_add_overflow(addr, pages * 4096ULL, &end)) return -ENOMEM;
    if (end > 0x0000800000000000ULL) return -ENOMEM;

    // First pass: every page in the range must be mapped, or the whole call
    // fails with -ENOMEM (matching Linux — vec is left untouched on failure).
    for (uint64_t i = 0; i < pages; ++i)
    {
        uint64_t pg = addr + i * 4096ULL;
        uint64_t* pte = brook::VmmGetPte(proc->addressSpace->pageTable, brook::VirtualAddress(pg));
        bool present = pte && (*pte & brook::VMM_PRESENT);
        // Lazily-backed regions (demand paging / file maps) count as mapped:
        // fault them in (read intent) so residency reflects the real VMA, as
        // Linux would. 0x4 = user-mode read fault.
        if (!present && (MemFdHandleUserFault(pg, 0x4) || FileMapHandleUserFault(pg, 0x4)))
        {
            pte = brook::VmmGetPte(proc->addressSpace->pageTable, brook::VirtualAddress(pg));
            present = pte && (*pte & brook::VMM_PRESENT);
        }
        if (!present) return -ENOMEM;
    }

    auto* vec = reinterpret_cast<uint8_t*>(vecAddr);
    for (uint64_t i = 0; i < pages; ++i)
        vec[i] = 1; // all mapped pages are treated as resident (no swap)
    return 0;
}

// ---------------------------------------------------------------------------
// sys_madvise (28) — private anonymous discard plus optional locality hints
// ---------------------------------------------------------------------------

static int64_t sys_madvise(uint64_t address, uint64_t length, uint64_t advice,
                            uint64_t, uint64_t, uint64_t)
{
    auto* proc=ProcessCurrent();
    if (!proc) return -EINVAL;
    if (advice==4) return UserMappingDiscardAnonymous(proc,proc->addressSpace,address,length);
    constexpr uint64_t limit=0x0000800000000000ULL;
    if ((address&4095) || address>=limit || length>limit-address) return -EINVAL;
    // Optional locality / hugepage hints do not promise changed contents.
    if (advice<=3 || advice==14 || advice==15) return 0;
    return -EINVAL; // Never claim FREE/REMOVE/fork-inheritance semantics.
}

static int64_t sys_fadvise64(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    return 0; // advisory only
}

static int64_t sys_getpriority(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t)
{
    // Linux's successful syscall result is 20 - nice, not libc's signed nice.
    // All tasks currently expose fixed nice 0; selectors and nice changes
    // remain compatibility stubs, separate from the scheduler priority hint.
    return 20;
}

static int64_t sys_setpriority(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t)
{
    return 0; // priority changes are currently ignored
}

// ---------------------------------------------------------------------------
// sys_mbind (237) — NUMA memory binding. We are single-node, so this is
// always a successful no-op. GIMP via libmetis calls this.
// ---------------------------------------------------------------------------

static int64_t sys_mbind(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return 0; // single-node — nothing to bind
}

// ---------------------------------------------------------------------------
// sys_pwrite64 (18) — write at offset without changing file position
// ---------------------------------------------------------------------------

static int64_t sys_pwrite64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                             uint64_t offset, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if(FdPipe(fde))return -29;
    if (!fde || fde->type==FdType::Path) return -EBADF;
    if (offset > 0x7fffffffffffffffULL) return -EINVAL;
    if (fde->type == FdType::MemFd && fde->handle) {
        if ((fde->statusFlags & 3) == 0) return -EBADF;
        if (count && !UserBufferReadable(bufAddr, count)) return -EFAULT;
        return MemFdWriteAt(static_cast<MemFdData*>(fde->handle),
            reinterpret_cast<const void*>(bufAddr), count, &offset,
            (fde->statusFlags & 0x400) != 0);
    }
    if (fde->type != FdType::Vnode || !fde->handle) return -EINVAL;
    if ((fde->statusFlags & 3) == 0) return -EBADF;
    if (count && !UserBufferReadable(bufAddr, count)) return -EFAULT;

    auto* vn = static_cast<Vnode*>(fde->handle);
    const void* buf = reinterpret_cast<const void*>(bufAddr);
    uint64_t off = offset;
    // Linux pwrite on O_APPEND chooses EOF but leaves the FD offset unchanged.
    int64_t result = (fde->statusFlags & 0x400)
        ? AppendUserBuffer(vn, bufAddr, count, &off)
        : VfsWrite(vn, buf, count < VnodeWriteLimit ? count : VnodeWriteLimit, &off);
    if (result > 0 && (fde->statusFlags & 0x1000)) {
        int error = VfsFsync(vn);
        if (error) return error;
    }
    return result;
}

// ---------------------------------------------------------------------------
// sys_link (86) — create hard link
// ---------------------------------------------------------------------------

static int64_t sys_link(uint64_t oldAddr, uint64_t newAddr, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    (void)oldAddr; (void)newAddr;
    return -EPERM; // hard links not supported
}

// Linux sigaltstack constants
static constexpr int SS_ONSTACK  = 1;
static constexpr int SS_DISABLE  = 2;

struct linux_stack_t {
    uint64_t ss_sp;
    int32_t  ss_flags;
    uint32_t _pad;
    uint64_t ss_size;
};

static int64_t sys_sigaltstack(uint64_t ssAddr, uint64_t oldSsAddr, uint64_t,
                                uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    // Return current state
    if (oldSsAddr)
    {
        auto* oss = reinterpret_cast<linux_stack_t*>(oldSsAddr);
        oss->ss_sp    = proc->sigAltstackSp;
        oss->ss_size  = proc->sigAltstackSize;
        oss->ss_flags = proc->sigAltstackFlags;
        if (proc->inSignalHandler && proc->sigAltstackSp != 0)
            oss->ss_flags |= SS_ONSTACK;
    }

    // Set new state
    if (ssAddr)
    {
        // Cannot change altstack while executing on it
        if (proc->inSignalHandler && proc->sigAltstackSp != 0)
            return -EPERM;

        auto* ss = reinterpret_cast<const linux_stack_t*>(ssAddr);
        if (ss->ss_flags & SS_DISABLE)
        {
            proc->sigAltstackSp    = 0;
            proc->sigAltstackSize  = 0;
            proc->sigAltstackFlags = SS_DISABLE;
        }
        else
        {
            if (ss->ss_size < 2048) // MINSIGSTKSZ
                return -ENOMEM;
            proc->sigAltstackSp    = ss->ss_sp;
            proc->sigAltstackSize  = static_cast<uint32_t>(ss->ss_size);
            proc->sigAltstackFlags = 0;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// sys_rt_sigreturn (15) — restore context saved by signal delivery
// ---------------------------------------------------------------------------
// Sets a flag so SyscallCheckSignals (called on asm return path) restores
// the full register context from the SignalFrame ucontext on the user stack.

static int64_t sys_rt_sigreturn(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc || !proc->inSignalHandler) return -EINVAL;

    proc->sigReturnPending = true;
    DbgPrintf("SIGRETURN: pid %u requesting context restore (cur user RIP=0x%lx)\n",
              proc->pid,
              []{ uint64_t r; __asm__ volatile("movq %%gs:48, %0" : "=r"(r)); return r; }());

    return 0; // Return value doesn't matter — SyscallCheckSignals overwrites RAX
}

// ---------------------------------------------------------------------------
// sys_getpgrp (111) / sys_getpgid (121) / sys_setpgid (109) / sys_getsid (124)
// ---------------------------------------------------------------------------

static int64_t sys_getpgrp(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? proc->pgid : 1;
}

static int64_t sys_getpgid(uint64_t pid, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    if (pid == 0)
    {
        Process* proc = ProcessCurrent();
        return proc ? proc->pgid : 1;
    }
    // Look up by pid
    Process* target = ProcessFindByPid(static_cast<uint16_t>(pid));
    if (!target) return -ESRCH;
    return target->pgid;
}

static int64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    Process* target = (pid == 0) ? proc : ProcessFindByPid(static_cast<uint16_t>(pid));
    if (!target) return -ESRCH;

    // pgid=0 means set pgid to the target's pid
    uint16_t newPgid = (pgid == 0) ? target->pid : static_cast<uint16_t>(pgid);
    target->pgid = newPgid;
    return 0;
}

static int64_t sys_getsid(uint64_t pid, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    if (pid == 0)
    {
        Process* proc = ProcessCurrent();
        return proc ? proc->sid : 1;
    }
    Process* target = ProcessFindByPid(static_cast<uint16_t>(pid));
    if (!target) return -ESRCH;
    return target->sid;
}

// sys_setsid (112) — create a new session
static int64_t sys_setsid(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EPERM;

    // Process becomes session leader and process group leader
    proc->sid = proc->pid;
    proc->pgid = proc->pid;
    return proc->pid;
}

// ---------------------------------------------------------------------------
// sys_gettid (186) / sys_tgkill (234) / sys_tkill (200)
// ---------------------------------------------------------------------------

static int64_t sys_gettid(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? proc->pid : 1;
}

// Thread-directed signals retain their Linux sender metadata. In particular,
// NPTL validates SI_TKILL and the sender TGID before applying setxid requests.
static int64_t SendThreadSignal(uint64_t tgid, uint64_t tid, uint64_t sig, bool checkGroup)
{
    int32_t group = static_cast<int32_t>(tgid), thread = static_cast<int32_t>(tid);
    int32_t signal = static_cast<int32_t>(sig);
    if (thread <= 0 || (checkGroup && group <= 0) || signal < 0 || signal > 64)
        return -EINVAL;
    if (thread > 0xffff || (checkGroup && group > 0xffff)) return -ESRCH;
    Process* self = ProcessCurrent();
    SchedulerKernelRegionEnter(self);
    Process* target = SchedulerFindProcessReference(static_cast<uint16_t>(thread));
    int result = -ESRCH;
    if (target && (!checkGroup || target->tgid == group)) {
        result = ProcessCanSignal(self,target) || (signal==18 && self && self->sid==target->sid)?0:-EPERM;
        if (!result && signal) {
            SignalInfo info{};
            info.si_signo = signal;
            info.si_code = -6; // SI_TKILL
            int32_t sender = self ? self->tgid : 0;
            uint32_t uid = self ? ProcessReadIds(self, false).real : 0;
            memcpy(info._data, &sender, sizeof(sender));
            memcpy(info._data + 4, &uid, sizeof(uid));
            // SA_ONSTACK without an enabled alternate stack uses the normal
            // stack. Silently discarding it deadlocks libc's setxid protocol.
            result = ProcessSendSignal(target, signal, &info);
        }
    }
    if (target) ProcessUnref(target);
    SchedulerKernelRegionLeave(self);
    return result;
}

static int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig,
                           uint64_t, uint64_t, uint64_t)
{
    return SendThreadSignal(tgid, tid, sig, true);
}

static int64_t sys_tkill(uint64_t tid, uint64_t sig, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return SendThreadSignal(0, tid, sig, false);
}

// ---------------------------------------------------------------------------
// sys_kill (62)
// ---------------------------------------------------------------------------

static int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    // Linux pid_t is a signed 32-bit syscall argument. Do not alias a large
    // positive PID onto an unrelated 16-bit Brook process identity.
    int32_t requested = static_cast<int32_t>(pid);
    if (requested <= 0) return -ENOSYS; // process-group selectors not implemented
    if (requested > 0xffff) return -ESRCH;
    int32_t signal = static_cast<int32_t>(sig);
    if (signal < 0 || signal > 64) return -EINVAL;
    Process* self = ProcessCurrent();
    SchedulerKernelRegionEnter(self);
    Process* target = SchedulerFindProcessReference(static_cast<uint16_t>(requested));
    if (!target) {
        SchedulerKernelRegionLeave(self);
        return -ESRCH;
    }
    if(!ProcessCanSignal(self,target) && !(signal==18 && self && self->sid==target->sid)) {ProcessUnref(target);SchedulerKernelRegionLeave(self);return -EPERM;}
    SignalInfo info{};info.si_signo=signal;info.si_code=0; // SI_USER
    const int32_t sender=self?self->tgid:0;
    const uint32_t senderUid=self?ProcessReadIds(self,false).real:0;
    memcpy(info._data,&sender,4);memcpy(info._data+4,&senderUid,4);
    int result = signal ? ProcessSendSignal(target, signal, &info) : 0;
    ProcessUnref(target);
    SchedulerKernelRegionLeave(self);
    return result;
}

// ---------------------------------------------------------------------------
// sys_getrlimit (97) — return resource limits consistent with prlimit64
// ---------------------------------------------------------------------------

static int64_t sys_getrlimit(uint64_t resource, uint64_t rlimAddr, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(rlimAddr, 16)) return -EFAULT;
    auto* rlim = reinterpret_cast<uint64_t*>(rlimAddr);
    switch (resource)
    {
    case 7: // RLIMIT_NOFILE
        rlim[0] = MAX_FDS;
        rlim[1] = MAX_FDS;
        break;
    case 3: // RLIMIT_STACK
        rlim[0] = 8 * 1024 * 1024;
        rlim[1] = RLIM_INFINITY;
        break;
    case 0: // RLIMIT_CPU
    case 1: // RLIMIT_FSIZE
    case 2: // RLIMIT_DATA
    default:
        rlim[0] = RLIM_INFINITY;
        rlim[1] = RLIM_INFINITY;
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sys_statfs (137) / sys_fstatfs (138) — x86-64 Linux ABI.
// ---------------------------------------------------------------------------

struct LinuxStatFs {
    int64_t type, blockSize;
    uint64_t blocks, freeBlocks, availableBlocks, files, freeFiles;
    int32_t identity[2];
    int64_t maxName, fragmentSize, flags, spare[4];
};
static_assert(sizeof(LinuxStatFs) == 120, "x86-64 statfs ABI");

static int64_t CopySpaceResult(uint64_t address, const VfsSpace& source)
{
    LinuxStatFs result{};
    result.type = source.type;
    result.blockSize = source.blockSize;
    result.blocks = source.blocks;
    result.freeBlocks = source.freeBlocks;
    result.availableBlocks = source.availableBlocks;
    result.files = source.files;
    result.freeFiles = source.freeFiles;
    result.identity[0] = static_cast<int32_t>(source.identity);
    result.identity[1] = static_cast<int32_t>(source.identity >> 32);
    result.maxName = source.maxName;
    result.fragmentSize = source.fragmentSize;
    result.flags = source.flags | 0x20; // Linux ST_VALID: mount flag fields are present.
    // No filesystem lock is held while resolving/copying user memory.
    return CopyToUser(address, &result, sizeof(result)) ? 0 : -EFAULT;
}

static int64_t sys_statfs(uint64_t pathAddr, uint64_t bufAddr, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    char path[512];
    int error=CopyUserCStringResult(pathAddr,path,sizeof(path));if(error)return error;
    AtLookup at;if((error=at.Init(-100,path)))return error;
    Vnode* vn=VfsOpenAt(at.fs.root,at.start(),path,VFS_O_METADATA,0,at.fs.mask,&error);
    if (!vn) return error ? error : -ENOENT;
    VfsSpace space{};
    error = VfsSpaceInfo(vn, &space);
    VfsClose(vn);
    return error ? error : CopySpaceResult(bufAddr, space);
}

static int64_t sys_fstatfs(uint64_t fd, uint64_t bufAddr, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    Process* process = ProcessCurrent();
    if (!process || fd >= MAX_FDS) return -EBADF;
    FdRef ref(process, static_cast<int>(fd));
    FdEntry* entry = ref.get();
    if (!entry) return -EBADF;
    if ((entry->type != FdType::Vnode && entry->type != FdType::Path) || !entry->handle) return -ENOSYS;
    VfsSpace space{};
    int error = VfsSpaceInfo(static_cast<Vnode*>(entry->handle), &space);
    return error ? error : CopySpaceResult(bufAddr, space);
}

// ---------------------------------------------------------------------------
// select/pselect share poll readiness and wait registration, including the
// eventfd notifications used by threaded DNS resolvers.
// ---------------------------------------------------------------------------

static int64_t SelectWait(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                          uint64_t exceptfdsAddr, uint64_t timeoutAddr, bool nanoseconds)
{
    if (nfds > MAX_FDS) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    constexpr uint32_t WORDS = (MAX_FDS + 63) / 64;
    uint64_t inputs[3][WORDS] = {}, outputs[3][WORDS] = {};
    uint64_t addresses[3] = {readfdsAddr, writefdsAddr, exceptfdsAddr};
    uint64_t bytes = ((nfds + 63) / 64) * sizeof(uint64_t);
    for (uint32_t set = 0; set < 3; ++set) {
        if (!addresses[set] || !bytes) continue;
        if (!UserBufferReadable(addresses[set], bytes) ||
            !UserBufferWritable(addresses[set], bytes)) return -EFAULT;
        memcpy(inputs[set], reinterpret_cast<const void*>(addresses[set]), bytes);
    }
    uint64_t timeoutMs;
    int error = ParseWaitTimeout(timeoutAddr, nanoseconds, &timeoutMs);
    if (error) return error;
    if (timeoutAddr && !nanoseconds && !UserBufferWritable(timeoutAddr, 16)) return -EFAULT;
    pollfd fds[MAX_FDS];
    uint32_t count = 0;
    for (uint32_t fd = 0; fd < nfds; ++fd) {
        uint64_t mask = 1ULL << (fd % 64);
        uint32_t word = fd / 64;
        short events = 0;
        if (inputs[0][word] & mask) events |= POLLIN;
        if (inputs[1][word] & mask) events |= POLLOUT;
        if (inputs[2][word] & mask) events |= POLLPRI;
        if (!events) continue;
        FdRef ref(proc, fd);
        if (!ref.get() || ref.get()->type == FdType::None) return -EBADF;
        fds[count++] = {static_cast<int>(fd), events, 0};
    }
    uint64_t started = ClockNowNs(1);
    int64_t result = PollWait(fds, count, timeoutMs, true);
    if (timeoutAddr && !nanoseconds) {
        if (!UserBufferWritable(timeoutAddr, 16)) return -EFAULT;
        uint64_t elapsed = (ClockNowNs(1) - started) / 1000000ULL;
        uint64_t remaining = elapsed < timeoutMs ? timeoutMs - elapsed : 0;
        auto* value = reinterpret_cast<int64_t*>(timeoutAddr);
        value[0] = static_cast<int64_t>(remaining / 1000);
        value[1] = static_cast<int64_t>((remaining % 1000) * 1000);
    }
    if (result < 0) return result;
    int readyBits = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (fds[i].revents & POLLNVAL) return -EBADF;
        uint32_t fd = static_cast<uint32_t>(fds[i].fd), word = fd / 64;
        uint64_t mask = 1ULL << (fd % 64);
        const short conditions[3] = {POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI};
        for (uint32_t set = 0; set < 3; ++set) {
            if ((inputs[set][word] & mask) && (fds[i].revents & conditions[set])) {
                outputs[set][word] |= mask;
                ++readyBits;
            }
        }
    }
    for (uint32_t set = 0; set < 3; ++set) {
        if (!addresses[set] || !bytes) continue;
        if (!UserBufferWritable(addresses[set], bytes)) return -EFAULT;
        memcpy(reinterpret_cast<void*>(addresses[set]), outputs[set], bytes);
    }
    return readyBits;
}

static int64_t sys_select(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                         uint64_t exceptfdsAddr, uint64_t timeoutAddr, uint64_t)
{
    return SelectWait(nfds, readfdsAddr, writefdsAddr, exceptfdsAddr, timeoutAddr, false);
}

static int64_t sys_pselect6(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                          uint64_t exceptfdsAddr, uint64_t timeoutAddr, uint64_t sigmaskAddr)
{
    if (sigmaskAddr) {
        // The sixth Linux argument points to {sigset pointer, sigset size},
        // rather than directly to a sigset as in ppoll/epoll_pwait.
        struct MaskArgument { uint64_t address; uint64_t size; } argument;
        if (!UserBufferReadable(sigmaskAddr, sizeof(argument))) return -EFAULT;
        memcpy(&argument, reinterpret_cast<const void*>(sigmaskAddr), sizeof(argument));
        int error = InstallWaitSignalMask(argument.address, argument.size);
        if (error) return error;
    }
    return SelectWait(nfds, readfdsAddr, writefdsAddr, exceptfdsAddr, timeoutAddr, true);
}

// ---------------------------------------------------------------------------
// sys_brook_profile (500) — Brook-specific: start/stop the sampling profiler
// ---------------------------------------------------------------------------
// arg0: op — 0 = start (arg1 = durationMs, 0 = indefinite), 1 = stop,
//            2 = isRunning (returns 1/0)
// Returns 0 on success, -EINVAL on bad op.

static int64_t sys_brook_profile(uint64_t op, uint64_t a1, uint64_t, uint64_t,
                                   uint64_t, uint64_t)
{
    switch (op) {
    case 0: brook::ProfilerStart(static_cast<uint32_t>(a1)); return 0;
    case 1: brook::ProfilerStop(); return 0;
    case 2: return brook::ProfilerIsRunning() ? 1 : 0;
    default: return -EINVAL;
    }
}

// sys_brook_set_crash_entry (502) — register the user-mode crash-dump writer
// entry point. One-shot: cannot be re-registered on the same leader.  The
// fault handler (commit d) will CreateRemoteThread(proc, crashEntry, ...)
// with a CrashCtx snapshot when a synchronous fatal exception hits this
// process.  See files/crash-dump-plan.md.
// ---------------------------------------------------------------------------
// arg0: entry (user VA of __brook_crash_entry)
// Returns 0 on success, -errno otherwise.

static int64_t sys_brook_set_crash_entry(uint64_t entry,
                                          uint64_t, uint64_t, uint64_t,
                                          uint64_t, uint64_t)
{
    if (!entry) return -EINVAL;

    brook::Process* caller = brook::ProcessCurrent();
    if (!caller) return -ESRCH;

    brook::Process* leader = caller->threadLeader ? caller->threadLeader : caller;

    if (leader->crashEntry != 0) return -EINVAL; // already registered
    leader->crashEntry = entry;
    return 0;
}

// ---------------------------------------------------------------------------
// sys_brook_crash_complete (503) — called by the user-mode writer once the
// dump file has been flushed.  Terminates the whole thread group with the
// supplied exit code (conventionally 128 + signum).  This is the "don't
// come back" path — if the writer couldn't finish, the fault handler's
// hardKill fallback would have been taken instead.
// ---------------------------------------------------------------------------
// arg0: exitCode (int, stored in lower 8 bits of wait-status)
// Does not return on success.

static int64_t sys_brook_crash_complete(uint64_t exitCode,
                                         uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t)
{
    brook::Process* caller = brook::ProcessCurrent();
    if (!caller) return -ESRCH;

    brook::Process* leader = caller->threadLeader ? caller->threadLeader : caller;

    SerialPrintf("crash_complete: tgid %u exiting with code %lu\n",
                 leader->tgid, exitCode);

    // Kill sibling threads (and the leader) with the crash exit code so
    // wait4 in the parent sees "exited with 128+signum" rather than
    // "killed by SIGKILL".
    SchedulerKillThreadGroup(leader->tgid, caller, static_cast<int>(exitCode));

    leader->crashInProgress = false;

    SchedulerExitCurrentProcess(static_cast<int>(exitCode));
    return 0; // unreachable
}

// ---------------------------------------------------------------------------
// sys_brook_input_pop (504) — drain the calling process's per-process input
// for the focused window into proc->inputQueue.  This syscall lets a
// userspace display server (waylandd) drain the queue without going via the
// /dev/keyboard fd path (which is scancode-only and lossy for mouse).
//
// Each event is delivered as 16 bytes:
//   [0]  type (uint8)   — InputEventType
//   [1]  scanCode (uint8)
//   [2]  ascii (int8)
//   [3]  modifiers (uint8)
//   [4..7] reserved
//   [8..11] mouse_x (int32) — only valid for MouseMove; absolute screen X
//   [12..15] mouse_y (int32) — only valid for MouseMove; absolute screen Y
// arg0: user buffer pointer
// arg1: max event count
// Returns: number of events copied, or -EFAULT on bad pointer.
// ---------------------------------------------------------------------------

extern "C" void MouseGetPosition(int32_t*, int32_t*);

static int64_t sys_brook_input_pop(uint64_t bufAddr, uint64_t maxCount,
                                     uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (!bufAddr) return -EFAULT;
    if (maxCount == 0) return 0;
    if (maxCount > 256) maxCount = 256;

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    auto* out = reinterpret_cast<uint8_t*>(bufAddr);
    uint64_t n = 0;
    while (n < maxCount) {
        InputEvent ev;
        if (!ProcessInputPop(proc, &ev)) break;
        uint8_t* slot = out + (n * 16);
        slot[0] = static_cast<uint8_t>(ev.type);
        slot[1] = ev.scanCode;
        slot[2] = static_cast<uint8_t>(ev.ascii);
        slot[3] = ev.modifiers;
        slot[4] = slot[5] = slot[6] = slot[7] = 0;
        int32_t mx = 0, my = 0;
        if (ev.type == InputEventType::MouseMove ||
            ev.type == InputEventType::MouseButtonDown ||
            ev.type == InputEventType::MouseButtonUp ||
            ev.type == InputEventType::MouseScroll) {
            int32_t sx = 0, sy = 0;
            MouseGetPosition(&sx, &sy);
            // Translate screen coords to caller's VFB coords. The compositor
            // blits the VFB to (fbDestX, fbDestY) at fbScale×; the inverse is
            // (sx - fbDestX) / fbScale.  Userspace then converts VFB→surface.
            uint8_t scale = proc->fbScale ? proc->fbScale : 1;
            mx = (sx - proc->fbDestX) / scale;
            my = (sy - proc->fbDestY) / scale;
        }
        *reinterpret_cast<int32_t*>(slot + 8)  = mx;
        *reinterpret_cast<int32_t*>(slot + 12) = my;
        ++n;
    }
    return static_cast<int64_t>(n);
}

// ---------------------------------------------------------------------------
// sys_brook_input_grab (505) — register / unregister the calling process as
// the global input grabber.  When set, the compositor copies every keyboard
// and mouse event into the grabber's per-PID input queue (in addition to the
// kernel WM's per-window routing), so a userspace Wayland compositor can
// fan input out to its own clients via wl_pointer / wl_keyboard.
//   arg0 = enable (1 to grab, 0 to release)
//   returns 0 on success, -EPERM if releasing while not the current grabber.
// ---------------------------------------------------------------------------
static int64_t sys_brook_input_grab(uint64_t enable, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    bool ok = brook::CompositorSetInputGrabber(proc, enable != 0);
    return ok ? 0 : -EPERM;
}

// ---------------------------------------------------------------------------
// Brook WM syscalls 506-509 — multi-window-per-process WM API.
// Phase A of the wayland↔WM unification.  Lets one process (e.g. waylandd)
// own multiple top-level windows, each backed by its own kernel-allocated
// VFB mapped read/write into the caller's address space.
// ---------------------------------------------------------------------------

// 506: WM_CREATE_WINDOW
//   arg0 = clientW (uint16)
//   arg1 = clientH (uint16)
//   arg2 = title pointer (user, 0-terminated)
//   arg3 = out struct pointer (user) — receives:
//          uint32_t wmId, uint32_t vfbStride, uint64_t vfbUser
//   arg4 = flags:
//          bit 0 requests initial client-side decoration/noChrome
//          bit 1 creates a raised but non-keyboard-focusable popup/tool window
// Returns 0 on success, -errno on failure.
struct BrookWmCreateOut {
    uint32_t wmId;
    uint32_t vfbStride;
    uint64_t vfbUser;
};

static int64_t sys_brook_wm_create_window(uint64_t cW, uint64_t cH,
                                           uint64_t titlePtr, uint64_t outPtr,
                                           uint64_t flags, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (cW == 0 || cH == 0 || cW > 0xFFFF || cH > 0xFFFF) return -EINVAL;
    if (!outPtr) return -EFAULT;

    char titleBuf[64] = {};
    if (titlePtr) {
        const char* utp = reinterpret_cast<const char*>(titlePtr);
        for (uint32_t i = 0; i < sizeof(titleBuf) - 1; ++i) {
            char c = utp[i];
            titleBuf[i] = c;
            if (!c) break;
        }
        titleBuf[sizeof(titleBuf) - 1] = '\0';
    }

    bool focusable = (flags & 2u) == 0;
    auto res = brook::WmCreateWindowForProcess(proc,
                                                 static_cast<uint16_t>(cW),
                                                 static_cast<uint16_t>(cH),
                                                 titlePtr ? titleBuf : nullptr,
                                                 focusable);
    if (res.wmId == 0) return -ENOMEM;
    if (flags & 1u) {
        brook::WmSetClientSideDecoration(static_cast<int>(res.wmId) - 1, true);
    }

    auto* uo = reinterpret_cast<BrookWmCreateOut*>(outPtr);
    uo->wmId      = res.wmId;
    uo->vfbStride = res.vfbStride;
    uo->vfbUser   = reinterpret_cast<uint64_t>(res.vfbUser);
    return 0;
}

// 507: WM_DESTROY_WINDOW(wmId)
static int64_t sys_brook_wm_destroy_window(uint64_t wmId, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFFFFFu) return -EINVAL;
    brook::WmDestroyWindowById(proc, static_cast<uint32_t>(wmId));
    return 0;
}

// 508: WM_SIGNAL_DIRTY(wmId) — caller has finished writing to the VFB.
static int64_t sys_brook_wm_signal_dirty(uint64_t wmId, uint64_t, uint64_t,
                                          uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0) return -EINVAL;
    brook::WmSignalDirtyById(proc, static_cast<uint32_t>(wmId));
    return 0;
}

// 525: WM_PRESENT_GRES(wmId, gres, w, h) — present a hardware-GL (dmabuf) frame.
// waylandd resolves a client's dmabuf to a host virgl resource id (gres) and
// calls this to wait for a compositor-owned capture of the source frame.
// The gres must be one the caller legitimately holds (it came from the
// caller importing a PRIME fd the client sent it over the Wayland socket).
static int64_t sys_brook_wm_present_gres(uint64_t wmId, uint64_t gres,
                                         uint64_t w, uint64_t h,
                                         uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    constexpr uint64_t wireMax=0xffffffffULL;
    if (!wmId || wmId>wireMax || !gres || gres>wireMax || !w || !h ||
        w>wireMax || h>wireMax) return -EINVAL;
    // This legacy ABI names a resource numerically, but the caller must own
    // its GEM capability. In particular, never permit an unexported resource
    // to become an untracked compositor source before enabling reclamation.
    MappingBackingGuard pin;
    DrmResource* resource=nullptr;
    for (uint32_t fd=0;fd<MAX_FDS && !resource;++fd) {
        FdRef descriptor(proc,static_cast<int>(fd));
        if (!descriptor || descriptor->type!=FdType::DevDri || !descriptor->handle) continue;
        auto* context=static_cast<DrmCtx*>(descriptor->handle);
        DrmMappingGuard guard(context);
        for (auto& gem:context->gem) {
            if (!gem.used || gem.resource->gres!=gres) continue;
            if (!__atomic_load_n(&gem.resource->everExported,__ATOMIC_ACQUIRE)) return -EINVAL;
            resource=gem.resource;MappingBackingRef(resource->lifetime);
            pin.value=resource->lifetime;break;
        }
    }
    if (!resource || resource->owner->config.capsetId==4 || resource->target!=2 || !brook::GpuCaptureFormatSupported(resource->format) ||
        w>resource->width || h>resource->height) return -EINVAL;
    brook::WmCaptureToken token{};
    if (!brook::WmReserveCapture(proc,static_cast<uint32_t>(wmId),
                               static_cast<uint32_t>(w),static_cast<uint32_t>(h),&token)) return -ENOENT;
    return brook::CompositorCaptureExternal(proc,token,resource->lifetime,
                                           resource->owner->ctxId,resource->gres,resource->format);
}

// 0xB06/0xB07: capability-based retained scenes. See gpu_scene_abi.h.
static int64_t sys_brook_scene_create(uint64_t window,uint64_t abi,uint64_t flags,
                                      uint64_t a,uint64_t b,uint64_t c)
{
    auto* proc=ProcessCurrent();if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (!window || window>WM_MAX_WINDOWS || abi!=SCENE_ABI_VERSION || flags || a || b || c) return -EINVAL;
    int error=0;auto* handle=SceneCreate(proc,static_cast<uint32_t>(window),&error);
    if (!handle) return error;
    FdEntry owned{};owned.type=FdType::CompositorScene;owned.handle=handle;owned.fdFlags=1;
    SceneHandleRef(handle); // FD publication owns a reference independently of this call.
    const int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
    if (fd<0) { SceneHandleUnref(handle);SceneHandleUnref(handle);return fd; }
    const bool activated=SceneActivate(handle);
    if (!activated) {
        // The published slot may already have been closed/reused by a sibling.
        // Retain the constructor handle while comparing, then pin the exact
        // slot so rollback cannot close an unrelated replacement descriptor.
        FdRef published(proc,fd);
        if (published && published->type==FdType::CompositorScene && published->handle==handle)
            CloseProcessFd(proc,fd);
        SceneHandleUnref(handle);return -116;
    }
    SceneHandleUnref(handle);
    return fd;
}
static int64_t sys_brook_scene_ctl(uint64_t fd,uint64_t op,uint64_t arg,uint64_t size,
                                   uint64_t flags,uint64_t reserved)
{
    auto* proc=ProcessCurrent();if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (fd>=MAX_FDS) return -EBADF;
    if (flags || reserved) return -EINVAL;
    FdRef ref(proc,static_cast<int>(fd));
    if (!ref || ref->type!=FdType::CompositorScene || !ref->handle) return -EBADF;
    auto* handle=static_cast<SceneHandle*>(ref->handle);
    if (op==SCENE_CTL_STATS) {
        if (size!=sizeof(SceneStatsDesc)) return -EINVAL;
        SceneStatsDesc stats{};SceneGetStats(handle,stats);
        return CopyToUser(arg,&stats,sizeof(stats))?0:-EFAULT;
    }
    if (op==SCENE_CTL_RELEASE) {
        if (size) return -EINVAL;
        return SceneRelease(handle,arg);
    }
    if (op==SCENE_CTL_UPLOAD) {
        if (size!=sizeof(SceneUploadDesc)) return -EINVAL;
        SceneUploadDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        return SceneUpload(proc,handle,desc);
    }
    if (op==SCENE_CTL_CAPTURE) {
        if (size!=sizeof(SceneCaptureDesc)) return -EINVAL;
        SceneCaptureDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        if (desc.reserved) return -EINVAL;
        FdRef drm(proc,desc.drmFd);
        if (!drm || drm->type!=FdType::DevDri || !drm->handle) return -EBADF;
        MappingBackingGuard lease;
        int32_t producer;uint32_t resource,format;
        {
            auto* context=static_cast<DrmCtx*>(drm->handle);DrmMappingGuard guard(context);
            auto* gem=DrmGemGet(context,desc.gem);
            if (!gem || gem->resource->owner->config.capsetId==4 || gem->resource->target!=2 || !GpuCaptureFormatSupported(gem->resource->format) ||
                desc.width>gem->resource->width || desc.height>gem->resource->height) return -EINVAL;
            auto* image=gem->resource;MappingBackingRef(image->lifetime);lease.value=image->lifetime;
            producer=image->owner->ctxId;resource=image->gres;format=image->format;
        }
        return SceneCapture(proc,handle,desc,lease.value,producer,resource,format);
    }
    if (op==SCENE_CTL_CAPTURE_DMABUF) {
        if(size!=sizeof(SceneDmabufCaptureDesc))return -EINVAL;
        SceneDmabufCaptureDesc desc{};if(!CopyFromUser(&desc,arg,sizeof(desc)))return -EFAULT;
        const BrookDmabufLayout layout{desc.width,desc.height,desc.fourcc,desc.stride,desc.offset,desc.flags,desc.modifier};
        FdRef drm(proc,desc.drmFd);
        if(!drm || drm->type!=FdType::DevDri || !drm->handle)return -EBADF;
        MappingBackingGuard lease;DrmResource* image=nullptr;
        {
            auto* context=static_cast<DrmCtx*>(drm->handle);DrmMappingGuard guard(context);
            auto* gem=DrmGemGet(context,desc.gem);if(!gem)return -EINVAL;
            image=gem->resource;
            if(!__atomic_load_n(&image->everExported,__ATOMIC_ACQUIRE))return -EINVAL;
            const bool venus=image->owner->config.capsetId==4;
            if(BrookDmabufValidate(&layout,image->size,!venus)!=BROOK_DMABUF_LAYOUT_OK)return -EINVAL;
            if(!venus && (image->target!=2 || desc.width!=image->width || desc.height!=image->height ||
               desc.stride!=image->stride || desc.offset ||
               !GpuCaptureViewSupported(image->format,desc.fourcc==BROOK_DMABUF_ARGB8888?1u:2u))) {
                SerialPrintf("drm: rejected image view: target=%u backing=%ux%u stride=%u format=%u view=%ux%u stride=%u fourcc=%x offset=%u\n",
                             image->target,image->width,image->height,image->stride,image->format,
                             desc.width,desc.height,desc.stride,desc.fourcc,desc.offset);
                return -EINVAL;
            }
            if(venus && (!image->hostBlob || !(image->blobFlags&2u)))return -EINVAL;
            MappingBackingRef(image->lifetime);lease.value=image->lifetime;
        }
        const bool venus=image->owner->config.capsetId==4;
        if(venus) {
            // Only the current frame's imported WRITE fence is evidence of
            // Venus queue completion. A prior capture or ring0 ack is not.
            KMutexLock(&image->reservationLock);
            const bool admitted=image->reservation.BeginCapture();
            KMutexUnlock(&image->reservationLock);
            if(!admitted)return -11; // EAGAIN, producer synchronization absent
        }
        const SceneCaptureDesc capture{desc.drmFd,desc.gem,desc.width,desc.height,
                                      desc.fourcc==BROOK_DMABUF_ARGB8888?1u:2u,0};
        // VirGL BGRA/BGRX values, distinct from the scene's alpha policy.
        const uint32_t format=venus?(desc.fourcc==BROOK_DMABUF_ARGB8888?1u:2u):image->format;
        return SceneCapture(proc,handle,capture,lease.value,image->owner->ctxId,image->gres,format,
                            venus?&layout:nullptr,venus?SceneCaptureSync{image,DrmCaptureComplete}:SceneCaptureSync{});
    }
    if (op==SCENE_CTL_CLONE) {
        if (size!=sizeof(SceneCloneDesc)) return -EINVAL;
        SceneCloneDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        if (desc.reserved) return -EINVAL;
        if (desc.sourceFd<0 || static_cast<uint32_t>(desc.sourceFd)>=MAX_FDS) return -EBADF;
        FdRef source(proc,desc.sourceFd);
        if (!source || source->type!=FdType::CompositorScene || !source->handle) return -EBADF;
        return SceneClone(proc,handle,static_cast<SceneHandle*>(source->handle),desc.version);
    }
    if (op==SCENE_CTL_PRESENT) {
        if (size!=sizeof(ScenePresentDesc)) return -EINVAL;
        ScenePresentDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        if (desc.reserved || desc.count>GPU_SCENE_MAX_LAYERS) return -EINVAL;
        auto* layers=static_cast<SceneLayerDesc*>(kmalloc(sizeof(SceneLayerDesc)*GPU_SCENE_MAX_LAYERS));
        if (!layers) return -ENOMEM;
        if (desc.count && !CopyFromUser(layers,desc.layers,sizeof(SceneLayerDesc)*desc.count)) {
            kfree(layers);return -EFAULT;
        }
        const int64_t result=ScenePresent(proc,handle,desc,layers);kfree(layers);return result;
    }
    return -EINVAL;
}

// 0xB03: notify the desktop that installed application metadata changed.
static int64_t sys_brook_wm_refresh_apps(uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t, uint64_t)
{
    if (!ProcessCurrent()) return -ESRCH;
    brook::WmRequestLauncherRefresh();
    return 0;
}

// 0xB02: desktop theme query (op 0) or select immutable named palette (op 1).
static int64_t sys_brook_wm_theme(uint64_t op, uint64_t theme,
                                 uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (!ProcessCurrent()) return -ESRCH;
    if (op == 0) return brook::WmGetTheme();
    if (op != 1 || theme > 0xffffffffULL) return -EINVAL;
    return brook::WmSetTheme(static_cast<uint32_t>(theme)) ? 0 : -EINVAL;
}

// 0xB04: native desktop scale. Query requested (0), request (1), query applied (2).
static int64_t sys_brook_wm_scale(uint64_t op,uint64_t scale,uint64_t,uint64_t,uint64_t,uint64_t)
{
    if (!ProcessCurrent()) return -ESRCH;
    if (op==0) return WmGetRequestedScale();
    if (op==2) return WmGetAppliedScale();
    if (op!=1 || scale>0xffffffffULL) return -EINVAL;
    return WmSetScale(static_cast<uint32_t>(scale)) ? 0 : -EINVAL;
}

// 0xB01: WM_SET_APP_ID(wmId, appId*) — metadata for an owned window.
// 0xB05: WM_SET_CLIENT_SOCKET(wmId, accepted AF_UNIX stream fd). The kernel
// authenticates connection origin; the provider supplies the surface association.
// This metadata grants no authority to signal another process.
static int64_t sys_brook_wm_set_client_socket(uint64_t wmId,uint64_t fdValue,
    uint64_t,uint64_t,uint64_t,uint64_t)
{
    Process* proc=ProcessCurrent();
    if (!proc) return -ESRCH;
    if (!wmId || wmId>0xffffffffULL || fdValue>0x7fffffffULL) return -EINVAL;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc,static_cast<int>(fdValue));
    auto* entry=descriptor.get();
    if (!entry) return -EBADF;
    if (entry->type!=FdType::UnixSocket || !entry->handle) return -88; // ENOTSOCK
    auto* socket=static_cast<UnixSocketData*>(entry->handle);
    UnixCredentials credentials{};uint64_t incarnation=0,launchId=0;
    SpinLockAcquire(&g_unixRegistryLock);
    const bool accepted=socket->state==UnixSocketData::State::Connected &&
                        socket->acceptedEndpoint && socket->socketType==1;
    if (accepted) {
        credentials=socket->peerCredentials;incarnation=socket->peerIncarnation;launchId=socket->peerLaunchId;
    }
    SpinLockRelease(&g_unixRegistryLock);
    if (!accepted) return -95; // EOPNOTSUPP
    if (credentials.pid<=0 || credentials.pid>65535 || !incarnation) return -ESRCH;
    return WmRegisterClientOrigin(proc,static_cast<uint32_t>(wmId),
        static_cast<uint16_t>(credentials.pid),incarnation,launchId)?0:-EPERM;
}

static int64_t sys_brook_wm_set_app_id(uint64_t wmId, uint64_t appIdPtr,
                                      uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (!wmId || wmId > 0xffffffffULL || !appIdPtr) return -EINVAL;
    char appId[128] = {};
    if (!CopyUserCString(appIdPtr, appId, sizeof(appId))) return -EFAULT;
    return brook::WmSetAppIdById(proc, static_cast<uint32_t>(wmId), appId) ? 0 : -ENOENT;
}

// 509: WM_SET_TITLE(wmId, title*)
static int64_t sys_brook_wm_set_title(uint64_t wmId, uint64_t titlePtr,
                                       uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || !titlePtr) return -EINVAL;
    char buf[64] = {};
    const char* utp = reinterpret_cast<const char*>(titlePtr);
    for (uint32_t i = 0; i < sizeof(buf) - 1; ++i) {
        char c = utp[i];
        buf[i] = c;
        if (!c) break;
    }
    buf[sizeof(buf) - 1] = '\0';
    {
        extern volatile uint64_t g_blockCacheHits;
        extern volatile uint64_t g_blockCacheMisses;
        SerialPrintf("[PROFILE] set_title t=%lums pid=%u '%s'"
                     " opens=%lu mmaps=%lu faults=%lu"
                     " cache_hit=%lu cache_miss=%lu preloaded=%lu"
                     " blkcache_hit=%lu blkcache_miss=%lu\n",
                     ApicTickCount(), proc->pid, buf,
                     g_profOpenCount, g_profMmapCount, g_profFaultCount,
                     g_profCacheHit, g_profCacheMiss, g_profPreloadPages,
                     g_blockCacheHits, g_blockCacheMisses);
    }
    brook::WmSetTitleById(proc, static_cast<uint32_t>(wmId), buf);
    return 0;
}

// 510: WM_POP_INPUT(wmId, buf*, max) — drain per-window input queue.
// Each event written is a brook::Window::WmInputEvent (12 bytes).
// Returns number of events written, or -errno.
static int64_t sys_brook_wm_pop_input(uint64_t wmId, uint64_t bufPtr,
                                       uint64_t max, uint64_t,
                                       uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || !bufPtr || !max) return -EINVAL;
    if (max > 256) max = 256;
    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -ENOENT;
    auto* uo = reinterpret_cast<brook::Window::WmInputEvent*>(bufPtr);
    return static_cast<int64_t>(brook::WmInputPop(w, uo, static_cast<uint32_t>(max)));
}

// 511: WM_RESIZE_VFB(wmId, newW, newH, outPtr)
//   Reallocates the kernel VFB for an existing waylandd-hosted toplevel
//   to (newW × newH).  Old user mapping is unmapped and a fresh user
//   mapping is returned via outPtr (BrookWmCreateOut layout).  Should be
//   called by waylandd after the client commits a buffer at the size we
//   asked for via xdg_toplevel.configure.
static int64_t sys_brook_wm_resize_vfb(uint64_t wmId, uint64_t newW,
                                        uint64_t newH, uint64_t outPtr,
                                        uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFFFFFu) return -EINVAL;
    if (newW == 0 || newH == 0 || newW > 0xFFFF || newH > 0xFFFF) return -EINVAL;
    if (!outPtr) return -EFAULT;

    brook::WmCreateWindowResult res = {};
    int rc = brook::WmResizeVfbForProcess(proc, static_cast<uint32_t>(wmId),
                                           static_cast<uint16_t>(newW),
                                           static_cast<uint16_t>(newH),
                                           &res);
    if (rc != 0) return rc;

    auto* uo = reinterpret_cast<BrookWmCreateOut*>(outPtr);
    uo->wmId      = res.wmId;
    uo->vfbStride = res.vfbStride;
    uo->vfbUser   = reinterpret_cast<uint64_t>(res.vfbUser);
    return 0;
}

// 512: WM_SET_DECORATION_MODE(wmId, csdEnabled)
//   Toggle CSD (client-side decoration) mode for a waylandd-hosted window.
//   Used to honour zxdg_toplevel_decoration_v1.set_mode.  When enabled,
//   the kernel WM stops drawing chrome and the whole outer area is client.
static int64_t sys_brook_wm_set_decoration_mode(uint64_t wmId, uint64_t csd,
                                                 uint64_t, uint64_t,
                                                 uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    int idx = static_cast<int>(wmId) - 1;
    brook::WmSetClientSideDecoration(idx, csd != 0);
    return 0;
}

// 0xB00 (2816): WM_SET_WINDOW_PROPERTIES(wmId, mask, opacity, blurRadius)
//   Set per-window display properties. `mask` selects which to apply:
//     bit 0 (WM_PROP_OPACITY) -> opacity  (0..255, 255 = opaque)
//     bit 1 (WM_PROP_BLUR)    -> blurRadius (0 = off)
//   A high (0xB00) number deliberately clear of the Linux and Windows syscall
//   ranges. Replaces the opt/gpuopacity demo hook with a real per-window API;
//   the GPU DRAW path composites each window at its own opacity (and, for frosted
//   glass, its blur radius). Only the owning process may set its window's props.
static int64_t sys_brook_wm_set_window_properties(uint64_t wmId, uint64_t mask,
                                                   uint64_t opacity, uint64_t blurRadius,
                                                   uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    int idx = static_cast<int>(wmId) - 1;
    brook::WmSetWindowProperties(idx, static_cast<uint32_t>(mask),
                                 static_cast<uint8_t>(opacity & 0xFF),
                                 static_cast<uint8_t>(blurRadius & 0xFF));
    return 0;
}

// ===========================================================================
// App-GPU syscalls (0xB10..): per-process virgl context for userspace GL-style
// rendering. The app gets its own context + resource ids, submits virgl command
// streams, and reads results back into its window VFB. Backed by GpuAppOps,
// which a 3D display driver registers. All gate on the caller owning the ctx
// (proc->gpuAppCtx). Numbers sit in the high 0xB00 block (clear of Linux/Windows).
// ===========================================================================

// 0xB10: GPU_CTX_CREATE() -> ctxId (>0), or <0. One context per process.
static int64_t sys_brook_gpu_ctx_create(uint64_t, uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->CtxCreate) return -ENODEV;
    if (proc->gpuAppCtx > 0) return proc->gpuAppCtx;   // already have one
    int32_t ctx = ga->CtxCreate(proc->pid, {});
    if (ctx < 0) return -ENOMEM;
    proc->gpuAppCtx = ctx;
    return ctx;
}

// 0xB11: GPU_RESOURCE_CREATE(ctx, format, bind, w, h) -> host-global resId (>0)
// or <0. The returned id is used directly in the app's virgl streams.
static int64_t sys_brook_gpu_resource_create(uint64_t ctx, uint64_t format,
                                             uint64_t bind, uint64_t w, uint64_t h,
                                             uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->ResourceCreate3D) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return -EINVAL;
    // This native ABI only creates 2-D textures; pass PIPE_TEXTURE_2D (2) so the
    // driver picks the texture path (target 0 is reserved for PIPE_BUFFER).
    int32_t res = ga->ResourceCreate3D((int32_t)ctx, /*PIPE_TEXTURE_2D=*/2u,
                                       (uint32_t)format, (uint32_t)bind,
                                       (uint32_t)w, (uint32_t)h,
                                       /*depth=*/1u, /*arraySize=*/1u,
                                       /*lastLevel=*/0u, /*nrSamples=*/0u,
                                       /*flags=*/0u);
    return res < 0 ? -ENOMEM : res;
}

// 0xB12: GPU_ATTACH_WINDOW(ctx, resId, wmId) — back a resource with the caller's
// window VFB (kernel-mapped), so host-rendered pixels land straight in the
// window. Convenience for the common "render into my window" case.
static int64_t sys_brook_gpu_attach_window(uint64_t ctx, uint64_t resId,
                                           uint64_t wmId, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->ResourceAttachUser) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    brook::Window* win = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!win || !win->vfb || !win->vfbBytes) return -EINVAL;
    int32_t r = ga->ResourceAttachUser((int32_t)ctx, (int32_t)resId,
                                       reinterpret_cast<uint64_t>(win->vfb),
                                       static_cast<uint32_t>(win->vfbBytes));
    return r < 0 ? -EINVAL : 0;
}

// 0xB16: GPU_UPLOAD_BUFFER(ctx, srcPtr, bytes) -> host-global resId (>0) or <0.
// Creates a vertex-buffer resource holding a copy of the app's data (e.g.
// vertices) and pushes it to the host; the returned id is used in the app's
// virgl SET_VERTEX_BUFFERS command.
static int64_t sys_brook_gpu_upload_buffer(uint64_t ctx, uint64_t srcPtr,
                                           uint64_t bytes, uint64_t,
                                           uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->BufferUpload) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (bytes == 0 || bytes > 16u * 1024 * 1024) return -EINVAL;
    if (!UserBufferReadable(srcPtr, bytes)) return -EFAULT;
    int32_t r = ga->BufferUpload((int32_t)ctx,
                                 reinterpret_cast<const void*>(srcPtr), (uint32_t)bytes);
    return r < 0 ? -EINVAL : r;
}

// 0xB13: GPU_SUBMIT(ctx, cmdPtr, nDwords) — submit a virgl command stream.
static int64_t sys_brook_gpu_submit(uint64_t ctx, uint64_t cmdPtr, uint64_t nDwords,
                                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->Submit3D) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (nDwords == 0 || nDwords > 8192) return -EINVAL;
    if (!UserBufferReadable(cmdPtr, nDwords * 4)) return -EFAULT;
    int32_t r = ga->Submit3D((int32_t)ctx, reinterpret_cast<const uint32_t*>(cmdPtr),
                             (uint32_t)nDwords);
    return r < 0 ? -EIO : 0;
}

// 0xB14: GPU_TRANSFER(ctx, resId, dir, w, h) — dir 0=to host, 1=from host.
static int64_t sys_brook_gpu_transfer(uint64_t ctx, uint64_t resId, uint64_t dir,
                                      uint64_t w, uint64_t h, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->Transfer3D) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (w == 0 || h == 0 || w > 8192 || h > 8192 || dir > 1) return -EINVAL;
    const brook::GpuTransfer3D transfer{
        0,0,0,(uint32_t)w,(uint32_t)h,1,0,0,(uint32_t)w*4,(uint32_t)(w*h*4)};
    int32_t r = ga->Transfer3D((int32_t)ctx, (int32_t)resId, (int)dir,
                               transfer);
    return r < 0 ? -EIO : 0;
}

// 0xB15: GPU_CTX_DESTROY(ctx) — release the process's GPU context.
static int64_t sys_brook_gpu_ctx_destroy(uint64_t ctx, uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->CtxDestroy) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    ga->CtxDestroy((int32_t)ctx);
    proc->gpuAppCtx = 0;
    return 0;
}

// 513: WM_BEGIN_MOVE(wmId)
//   Begin moving a waylandd-hosted window. Used to honour xdg_toplevel.move
//   for client-side-decorated windows, where the titlebar click lands in
//   client space and the client asks the compositor to start an interactive move.
static int64_t sys_brook_wm_begin_move(uint64_t wmId, uint64_t,
                                        uint64_t, uint64_t,
                                        uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    int idx = static_cast<int>(wmId) - 1;
    return brook::CompositorBeginWindowMove(idx) ? 0 : -EINVAL;
}

// 514: WM_MOVE_RELATIVE(wmId, parentWmId, relX, relY)
//   Position a waylandd-hosted popup relative to the parent window's client
//   area. xdg_popup positioners use parent-surface coordinates, while the
//   kernel WM stores outer window coordinates including optional chrome.
static int64_t sys_brook_wm_move_relative(uint64_t wmId, uint64_t parentWmId,
                                           uint64_t relX, uint64_t relY,
                                           uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;
    if (parentWmId == 0 || parentWmId > 0xFFFFu) return -EINVAL;

    bool ok = brook::WmMoveWindowRelativeToParent(proc,
        static_cast<uint32_t>(wmId),
        static_cast<uint32_t>(parentWmId),
        static_cast<int32_t>(relX),
        static_cast<int32_t>(relY));
    return ok ? 0 : -EINVAL;
}

// 515: WM_SET_MAXIMIZED(wmId, enable)
static int64_t sys_brook_wm_set_maximized(uint64_t wmId, uint64_t enable,
                                           uint64_t, uint64_t,
                                           uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    brook::WmSetMaximized(static_cast<int>(wmId) - 1, enable != 0);
    return 0;
}

// 516: WM_SET_MINIMIZED(wmId)
static int64_t sys_brook_wm_set_minimized(uint64_t wmId, uint64_t,
                                           uint64_t, uint64_t,
                                           uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    brook::WmMinimizeWindow(static_cast<int>(wmId) - 1);
    return 0;
}

// 517: WM_BEGIN_RESIZE(wmId, edges)
// Edge bits are Brook-local: 1=top, 2=bottom, 4=left, 8=right.
static int64_t sys_brook_wm_begin_resize(uint64_t wmId, uint64_t edges,
                                          uint64_t, uint64_t,
                                          uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;
    if ((edges & ~0xFULL) != 0 || edges == 0) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    bool top = (edges & 1u) != 0;
    bool bottom = (edges & 2u) != 0;
    bool left = (edges & 4u) != 0;
    bool right = (edges & 8u) != 0;
    if ((top && bottom) || (left && right)) return -EINVAL;

    return brook::CompositorBeginWindowResize(static_cast<int>(wmId) - 1,
                                               left, right, top, bottom)
        ? 0 : -EINVAL;
}

// 518: WM_SET_CURSOR_VISIBLE(visible)
// Mechanism for userspace compositors to hide/show Brook's built-in cursor.
// This does not implement Wayland cursor surfaces or hotspot rendering.
static int64_t sys_brook_wm_set_cursor_visible(uint64_t visible, uint64_t,
                                                uint64_t, uint64_t,
                                                uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    return brook::CompositorSetCursorVisible(proc, visible != 0) ? 0 : -EPERM;
}

// 519: WM_SET_CURSOR_IMAGE(pixels_ptr, w, h, hotX, hotY)
// Upload a custom ARGB8888 cursor image (max 64×64). Pass w=0 to reset.
static int64_t sys_brook_wm_set_cursor_image(uint64_t pixelsAddr, uint64_t w,
                                              uint64_t h, uint64_t hotX,
                                              uint64_t hotY, uint64_t)
{
    if (w == 0) {
        brook::CompositorSetCursorImage(nullptr, 0, 0, 0, 0);
        return 0;
    }
    if (w > 64 || h > 64) return -EINVAL;
    if (h == 0) {
        brook::CompositorSetCursorImage(nullptr, 0, 0, 0, 0);
        return 0;
    }
    Process* proc = ProcessCurrent();
    if (!proc || !proc->addressSpace) return -ESRCH;
    KernelOperationGuard operation(proc);
    const uint64_t byteSize = w * h * 4;
    // Populate lazy pages before acquiring the AS lock. Validation alone is
    // not a lifetime pin: a sibling may unmap/protect the buffer immediately.
    if (!UserBufferReadable(pixelsAddr, byteSize)) return -EFAULT;
    auto* staged = static_cast<uint8_t*>(kmalloc(byteSize));
    if (!staged) return -ENOMEM;
    int64_t result = 0;
    {
        MappingTransaction lifetime(proc, proc->addressSpace);
        // Read-only resident copy: mutation serializes leaf retirement,
        // protection changes and file invalidation. No backing escapes this lock.
        KMutexLock(&proc->addressSpace->mutation);
        uint64_t offset = 0;
        while (!result && offset < byteSize) {
            const uint64_t address = pixelsAddr + offset;
            const auto* range = MappingRangeFind(proc->addressSpace->ranges, address);
            if (!range || !(range->protection & 1)) { result = -EFAULT; break; }
            const uint64_t leaf = VmmReadLeaf(proc->addressSpace->pageTable, VirtualAddress(address));
            if ((leaf & (VMM_PRESENT | VMM_USER)) != (VMM_PRESENT | VMM_USER)) { result = -EFAULT; break; }
            uint64_t chunk = 4096 - (address & 4095);
            if (chunk > byteSize - offset) chunk = byteSize - offset;
            if (chunk > range->end - address) chunk = range->end - address;
            memcpy(staged + offset, reinterpret_cast<const void*>(address), chunk);
            offset += chunk;
        }
        KMutexUnlock(&proc->addressSpace->mutation);
    }
    // Cursor publication reads kernel memory only, after releasing the AS lock.
    if (!result && !brook::CompositorSetCursorImage(reinterpret_cast<const uint32_t*>(staged),
            static_cast<uint32_t>(w), static_cast<uint32_t>(h),
            static_cast<int32_t>(hotX), static_cast<int32_t>(hotY))) result = -EINVAL;
    kfree(staged);
    return result;
}

// 520: WM_GET_SCREEN_INFO(out_ptr) — return screen width and height
// Writes two uint32_t values [width, height] to the user-supplied buffer.
static int64_t sys_brook_wm_get_screen_info(uint64_t outAddr, uint64_t, uint64_t,
                                             uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(outAddr, 8)) return -EFAULT;
    uint32_t w = 0, h = 0;
    brook::CompositorGetPhysDims(&w, &h);
    uint32_t* out = reinterpret_cast<uint32_t*>(outAddr);
    out[0] = w;
    out[1] = h;
    return 0;
}

// ---------------------------------------------------------------------------
// 521: WM_SCANOUT_MAP(out_ptr) — map compositor backbuffer into caller's space.
// Writes a ScanoutMapResult struct to out_ptr.
// ---------------------------------------------------------------------------
static int64_t sys_brook_wm_scanout_map(uint64_t outAddr, uint64_t, uint64_t,
                                         uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(outAddr, sizeof(brook::ScanoutMapResult)))
        return -EFAULT;

    Process* proc = ProcessCurrent();
    auto* out = reinterpret_cast<brook::ScanoutMapResult*>(outAddr);
    uint64_t addr = brook::CompositorScanoutMap(proc, out);
    return addr ? 0 : -ENOMEM;
}

// ---------------------------------------------------------------------------
// 522: WM_SCANOUT_FLIP(minY, maxY) — copy dirty rows to MMIO framebuffer.
// ---------------------------------------------------------------------------
static int64_t sys_brook_wm_scanout_flip(uint64_t minY, uint64_t maxY, uint64_t,
                                          uint64_t, uint64_t, uint64_t)
{
    return brook::CompositorScanoutFlip(static_cast<uint32_t>(minY),
                                        static_cast<uint32_t>(maxY));
}

// ---------------------------------------------------------------------------
// 523: WM_GET_WINDOWS(out_ptr, max_count) — query window state for usermode compositor
// ---------------------------------------------------------------------------
static int64_t sys_brook_wm_get_windows(uint64_t outAddr, uint64_t maxCount, uint64_t,
                                         uint64_t, uint64_t, uint64_t)
{
    if (maxCount == 0) return 0;
    uint64_t bufSize = maxCount * sizeof(brook::WmWindowDesc);
    if (!UserBufferWritable(outAddr, bufSize)) return -EFAULT;
    Process* proc = ProcessCurrent();
    auto* out = reinterpret_cast<brook::WmWindowDesc*>(outAddr);
    return static_cast<int64_t>(brook::CompositorGetWindows(proc, out, static_cast<uint32_t>(maxCount)));
}

// ---------------------------------------------------------------------------
// 524: WM_MAP_WINDOW_VFB(wmId) — map a window's VFB into compositor process
// ---------------------------------------------------------------------------
static int64_t sys_brook_wm_map_window_vfb(uint64_t wmId, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    uint64_t addr = brook::CompositorMapWindowVfb(proc, static_cast<uint32_t>(wmId));
    return addr ? static_cast<int64_t>(addr) : -ENOENT;
}

// ---------------------------------------------------------------------------
// sys_not_implemented
// ---------------------------------------------------------------------------

static int64_t sys_not_implemented(uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t)
{
    // Rate-limit: log first 8 per-syscall, then every 256th, to avoid
    // serial lock contention when many processes hit unimplemented syscalls.
    static volatile uint32_t s_unimplCount = 0;
    uint32_t n = __atomic_fetch_add(&s_unimplCount, 1, __ATOMIC_RELAXED);

    Process* proc = ProcessCurrent();
    uint64_t syscallNum = 0;
    __asm__ volatile("mov %%gs:120, %0" : "=r"(syscallNum));

    if (n < 8 || (n & 0xFF) == 0)
        SerialPrintf("UNIMPL: syscall %lu from pid %u ('%s') [#%u]\n",
                     syscallNum, proc ? proc->pid : 0, proc ? proc->name : "?", n);
    return -ENOSYS;
}

// Silent ENOSYS: for syscalls we deliberately don't implement but where
// the userland-side fallback is well-trodden (e.g. inotify -> polling).
// Avoids noisy UNIMPL spam during normal operation.
static int64_t sys_enosys_quiet(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    return -ENOSYS;
}

// ---------------------------------------------------------------------------
// sys_getresuid (118) / sys_getresgid (120)
// ---------------------------------------------------------------------------

static int64_t GetResIds(bool group, uint64_t real, uint64_t effective, uint64_t saved) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    if (!UserBufferWritable(real, 4) || !UserBufferWritable(effective, 4) || !UserBufferWritable(saved, 4))
        return -EFAULT;
    const auto ids = ProcessReadIds(proc, group);
    *reinterpret_cast<uint32_t*>(real) = ProcessReportId(proc,group,ids.real);
    *reinterpret_cast<uint32_t*>(effective) = ProcessReportId(proc,group,ids.effective);
    *reinterpret_cast<uint32_t*>(saved) = ProcessReportId(proc,group,ids.saved);
    return 0;
}
static int64_t sys_getresuid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t) {
    return GetResIds(false, real, effective, saved);
}
static int64_t sys_getresgid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t) {
    return GetResIds(true, real, effective, saved);
}

// ---------------------------------------------------------------------------
// sys_pread64 (17) — read at offset without changing file position
// ---------------------------------------------------------------------------

static int64_t sys_pread64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                            uint64_t offset, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if(FdPipe(fde))return -29;
    if (!fde || fde->type==FdType::Path) return -EBADF;

    if (offset > 0x7fffffffffffffffULL) return -EINVAL;
    if (count && !UserBufferWritable(bufAddr, count)) return -EFAULT;
    if (fde->type == FdType::MemFd && fde->handle) {
        if ((fde->statusFlags & 3) == 1) return -EBADF;
        return MemFdReadAt(static_cast<MemFdData*>(fde->handle),
            reinterpret_cast<void*>(bufAddr), count, &offset);
    }
    if (fde->type == FdType::Vnode && fde->handle)
    {
        auto* vn = static_cast<Vnode*>(fde->handle);
        uint64_t pos = offset;
        return VfsRead(vn, reinterpret_cast<void*>(bufAddr),
                       count, &pos);
    }

    return -EBADF;
}

// ---------------------------------------------------------------------------
// sys_prctl (157) — process control
// ---------------------------------------------------------------------------

static int64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3,
                          uint64_t arg4, uint64_t arg5, uint64_t)
{
    if (option == 38) { // PR_SET_NO_NEW_PRIVS
        if (arg2 != 1 || arg3 || arg4 || arg5) return -EINVAL;
        auto* proc = ProcessCurrent();
        if (!proc) return -ESRCH;
        ProcessSetNoNewPrivs(proc);
        return 0;
    }
    if (option == 39) { // PR_GET_NO_NEW_PRIVS
        if (arg2 || arg3 || arg4 || arg5) return -EINVAL;
        auto* proc = ProcessCurrent();
        if (!proc) return -ESRCH;
        return ProcessGetNoNewPrivs(proc) ? 1 : 0;
    }
    if(option==23 || option==24) {
        if(arg3 || arg4 || arg5)return -EINVAL;
        auto* process=ProcessCurrent();return process?ProcessCapabilityControl(process,unsigned(option),arg2):-ESRCH;
    }
    // Remaining capability/secure-exec operations are explicit errors.
    switch (option) {
        case 7: case 8:   // GET/SET_KEEPCAPS
        case 21: case 22: // GET/SET_SECCOMP
        case 23: case 24: // CAPBSET_READ/DROP
        case 27: case 28: // GET/SET_SECUREBITS
        case 47:         // CAP_AMBIENT
            return -EINVAL;
        default: break;
    }
    // Most prctl options are not relevant for Brook.
    // Return success for harmless ones, EINVAL for unknown.
    return 0;
}

// ---------------------------------------------------------------------------
// sys_faccessat (269) / sys_faccessat2 (439)
// ---------------------------------------------------------------------------

static int64_t sys_faccessat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                            uint64_t, uint64_t, uint64_t)
{
    return AccessAt(static_cast<int>(dirfd), pathAddr, mode, 0);
}

static int64_t sys_faccessat2(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                             uint64_t flags, uint64_t, uint64_t)
{
    return AccessAt(static_cast<int>(dirfd), pathAddr, mode, flags);
}

// ---------------------------------------------------------------------------
// sys_set/get_robust_list — per-thread x86-64 robust-list registration.
// ---------------------------------------------------------------------------
static int64_t sys_set_robust_list(uint64_t head,uint64_t length,uint64_t,uint64_t,uint64_t,uint64_t)
{
    if(length!=24)return -EINVAL;
    auto* proc=ProcessCurrent();if(!proc || proc->isKernelThread)return -ESRCH;
    __atomic_store_n(&proc->robustListHead,head,__ATOMIC_RELEASE);
    return 0; // Like Linux, registration does not dereference user memory.
}
static int64_t sys_get_robust_list(uint64_t pid,uint64_t headOut,uint64_t lengthOut,uint64_t,uint64_t,uint64_t)
{
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    if(pid>65535)return -ESRCH;
    auto* target=pid?SchedulerFindProcessReference(static_cast<uint16_t>(pid)):proc;
    if(!target)return -ESRCH;
    const bool held=pid!=0;
    // Conservative bounded access policy: only the calling thread group.
    // Cross-process ptrace credential/dumpability policy is not implemented.
    if(target->tgid!=proc->tgid) {if(held)SchedulerReleaseWaitReference(target);return -EPERM;}
    const uint64_t head=__atomic_load_n(&target->robustListHead,__ATOMIC_ACQUIRE),length=24;
    if(held)SchedulerReleaseWaitReference(target);
    if(!UserBufferWritable(headOut,8) || !UserBufferWritable(lengthOut,8))return -EFAULT;
    return CopyToUser(lengthOut,&length,8) && CopyToUser(headOut,&head,8)?0:-EFAULT;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// sys_rseq (334) — restartable sequences (stub)
// ---------------------------------------------------------------------------

static int64_t sys_rseq(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    return -ENOSYS; // Not supported — musl handles this gracefully
}

// ---------------------------------------------------------------------------
// sys_futex (202) — fast userspace mutex with real wait queues
// ---------------------------------------------------------------------------

static constexpr int FUTEX_WAIT         = 0;
static constexpr int FUTEX_WAKE         = 1;
static constexpr int FUTEX_WAKE_OP      = 5;
static constexpr int FUTEX_LOCK_PI      = 6;
static constexpr int FUTEX_UNLOCK_PI    = 7;
static constexpr int FUTEX_WAIT_BITSET  = 9;
static constexpr int FUTEX_WAKE_BITSET  = 10;
static constexpr int FUTEX_PRIVATE_FLAG    = 128;
static constexpr int FUTEX_CLOCK_REALTIME  = 256;

// Futex wait queues use private TGID/VA keys or tagged shared-object/offset
// keys. Waiters retain the chosen value key through wake, timeout and cancellation.
static constexpr uint32_t FUTEX_HASH_SIZE = 64;

// FutexWaiter and the slot pool live in sync/futex_waiter_pool.h so the pool's
// atomic alloc/free can be unit-tested against the real code (BRO-160).

static constexpr uint32_t FUTEX_BITSET_MATCH_ANY = 0xFFFFFFFFu;

static FutexWaiter* g_futexBuckets[FUTEX_HASH_SIZE];
// BRO-176: this lock MUST disable interrupts while held. It is a plain spinlock
// guarding the futex hash table; the critical sections are short and never
// block. Previously it was a bare test_and_set (interrupts stayed enabled), so a
// thread could be preempted by the timer while holding it and then be marked
// Terminated by a sibling's exit_group before reaching the release — leaking the
// lock permanently and deadlocking every future futex op (observed: 3 CPUs
// spinning here forever under schedstress). An IrqSpinLock keeps IF=0 while held,
// so the holder runs to the release uninterrupted and can never be descheduled
// or killed mid-section.
static IrqSpinLock g_futexLock;

// Pool of waiter nodes (avoid kmalloc from IRQ context). Sized to MAX_PROCESSES:
// a thread blocked in a futex wait cannot start another, so one slot per thread
// is the true upper bound and the pool can never be legitimately exhausted
// (BRO-160 — the old 128 cap returned -ENOMEM under heavy threading). Slot
// claim/release is atomic, so it is safe across FutexWake's lock drop window.
static constexpr uint32_t FUTEX_MAX_WAITERS = MAX_PROCESSES;
static FutexWaiterPool<FUTEX_MAX_WAITERS> g_futexWaiterPool;

static FutexWaiter* FutexAllocWaiter()
{
    return g_futexWaiterPool.Alloc();
}

static void FutexFreeWaiter(FutexWaiter* w)
{
    g_futexWaiterPool.Free(w);
}

// BRO-196 diagnostic: a small ring buffer of recent futex ops, so the hang dump
// can show the WAIT/WAKE timeline right before a deadlock (lost-wakeup hunt).
struct FutexLogEntry { uint64_t uaddr; uint64_t tick; uint32_t pid; int32_t result; uint8_t op; };
static constexpr uint32_t FUTEX_LOG_SIZE = 512;
static FutexLogEntry g_futexLog[FUTEX_LOG_SIZE];
static volatile uint32_t g_futexLogIdx = 0;  // monotonic; & (SIZE-1) for slot
// op: 0=WAIT-enqueue 1=WAIT-return 2=WAKE 3=WAKE_OP 4=REQUEUE
static inline void FutexLog(uint8_t op, uint64_t uaddr, uint32_t pid, int32_t result)
{
    uint32_t i = __atomic_fetch_add(&g_futexLogIdx, 1, __ATOMIC_RELAXED) & (FUTEX_LOG_SIZE - 1);
    g_futexLog[i].op = op;
    g_futexLog[i].uaddr = uaddr;
    g_futexLog[i].pid = pid;
    g_futexLog[i].result = result;
    g_futexLog[i].tick = ApicTickCount();
}

// BRO-196 diagnostic: dump every blocked futex waiter (uaddr + owner + pid).
// Called from SchedulerDumpHang (Ctrl+Shift+ScrollLock). Lock-free read of the
// hash table — safe in the hang path where we can't guarantee lock state; we are
// only reading immutable-while-blocked fields for diagnosis.
extern "C" void FutexDumpWaiters()
{
    SerialPrintf("--- futex waiters ---\n");
    uint32_t total = 0;
    for (uint32_t b = 0; b < FUTEX_HASH_SIZE; ++b) {
        for (FutexWaiter* w = g_futexBuckets[b]; w; w = w->next) {
            Process* wp = w->proc;
            uint32_t pid = (wp && wp->magic == PROCESS_MAGIC) ? wp->pid : 0xFFFFFFFFu;
            SerialPrintf("  futex uaddr=0x%lx owner=0x%lx bitset=0x%x pid=%u\n",
                         w->uaddr, w->owner, w->bitset, pid);
            ++total;
            if (total > 256) { SerialPrintf("  ...(truncated)\n"); return; }
        }
    }
    SerialPrintf("  total futex waiters: %u\n", total);

    // Recent futex op timeline (oldest→newest).
    static const char* opName[] = {"WAITq", "WAITr", "WAKE ", "WAKEOP", "REQ  "};
    SerialPrintf("--- recent futex ops (last %u) ---\n", FUTEX_LOG_SIZE);
    uint32_t head = __atomic_load_n(&g_futexLogIdx, __ATOMIC_RELAXED);
    for (uint32_t k = 0; k < FUTEX_LOG_SIZE; ++k) {
        uint32_t i = (head + k) & (FUTEX_LOG_SIZE - 1);
        const FutexLogEntry& e = g_futexLog[i];
        if (e.uaddr == 0 && e.tick == 0) continue;
        const char* nm = (e.op < 5) ? opName[e.op] : "?";
        SerialPrintf("  t=%lu %s uaddr=0x%lx pid=%u res=%d\n",
                     e.tick, nm, e.uaddr, e.pid, e.result);
    }
}

static uint32_t FutexHash(uint64_t owner, uint64_t addr)
{
    return static_cast<uint32_t>(((addr >> 2) ^ owner) % FUTEX_HASH_SIZE);
}

// Callable from outside syscall dispatch (e.g., scheduler thread exit).
// Only wakes waiters whose stored bitset overlaps wake_bitset.
// Pass FUTEX_BITSET_MATCH_ANY for plain FUTEX_WAKE.
extern "C" int64_t FutexWake(uint64_t owner, uint64_t uaddr, uint32_t maxWake,
                              uint32_t wake_bitset = FUTEX_BITSET_MATCH_ANY)
{
    uint32_t bucket = FutexHash(owner, uaddr);
    uint32_t woken = 0;

    uint64_t fxFlags = IrqSpinLockAcquire(&g_futexLock);

    FutexWaiter** pp = &g_futexBuckets[bucket];
    while (*pp && woken < maxWake) {
        FutexWaiter* w = *pp;
        if (w->uaddr == uaddr && w->owner == owner && (w->bitset & wake_bitset) != 0) {
            Process* waiter = w->proc;
            *pp = w->next;
            FutexFreeWaiter(w);
            // Unlink transfers the node's hold to this stack. Keep IF clear
            // until that hold is dropped: a killed caller never resumes.
            IrqSpinLockRelease(&g_futexLock, fxFlags & ~0x200ULL);

            WakeProcess(waiter);
            // The dequeued node hands its reference to this wake operation,
            // keeping the process alive across the unlocked wakeup window.
            SchedulerReleaseWaitReference(waiter);
            if (fxFlags & 0x200) __asm__ volatile("sti" ::: "memory");
            woken++;

            fxFlags = IrqSpinLockAcquire(&g_futexLock);
            pp = &g_futexBuckets[bucket];
        } else {
            pp = &(*pp)->next;
        }
    }

    IrqSpinLockRelease(&g_futexLock, fxFlags);
    { Process* cp = SchedulerCurrentProcess();
      FutexLog(2, uaddr, cp ? cp->pid : 0, (int32_t)woken); }
    return static_cast<int64_t>(woken);
}

static bool FutexRemoveWaiter(uint64_t owner, uint64_t uaddr, Process* proc)
{
    if (!proc) return false;
    uint32_t bucket = FutexHash(owner, uaddr);

    uint64_t fxFlags = IrqSpinLockAcquire(&g_futexLock);

    FutexWaiter** pp = &g_futexBuckets[bucket];
    while (*pp) {
        FutexWaiter* w = *pp;
        if (w->uaddr == uaddr && w->owner == owner && w->proc == proc) {
            *pp = w->next;
            FutexFreeWaiter(w);
            IrqSpinLockRelease(&g_futexLock, fxFlags & ~0x200ULL);
            SchedulerReleaseWaitReference(proc);
            if (fxFlags & 0x200) __asm__ volatile("sti" ::: "memory");
            return true;
        }
        pp = &(*pp)->next;
    }

    IrqSpinLockRelease(&g_futexLock, fxFlags);
    return false;
}

// A coarse tick can wake us before the selected-clock deadline. Keep the
// existing queue entry/reference while rearming: removing and re-enqueuing
// would create a gap in which FUTEX_WAKE could be lost.
static bool FutexRearmWaiter(uint64_t owner, uint64_t uaddr, Process* proc,
                             const FutexWaitDeadline& deadline)
{
    const uint64_t flags = IrqSpinLockAcquire(&g_futexLock);
    for (FutexWaiter* w = g_futexBuckets[FutexHash(owner, uaddr)]; w; w = w->next) {
        if (w->owner != owner || w->uaddr != uaddr || w->proc != proc) continue;
        const uint64_t nowNs = ClockNowNs(deadline.clockId);
        const uint64_t nowTick = ApicTickCount();
        proc->wakeupTick = deadline.wakeupTick(nowNs, nowTick);
        // Do not clear pendingWakeup here. A real wake after unlocking must
        // still prevent the next block through SchedulerBlock's wake latch.
        IrqSpinLockRelease(&g_futexLock, flags);
        return true;
    }
    IrqSpinLockRelease(&g_futexLock, flags);
    return false; // A real wake already removed the waiter.
}

// Caller owns a process reference and has observed it Terminated/off-CPU.
// Such a syscall cannot resume to remove its own waiter. Release only nodes
// still linked here; a concurrent wake owns any node it already dequeued.
extern "C" void FutexCancelProcess(Process* proc)
{
    uint64_t flags = IrqSpinLockAcquire(&g_futexLock);
    for (uint32_t b = 0; b < FUTEX_HASH_SIZE; ++b) {
        FutexWaiter** pp = &g_futexBuckets[b];
        while (*pp) {
            FutexWaiter* w = *pp;
            if (w->proc != proc) { pp = &w->next; continue; }
            *pp = w->next;
            FutexFreeWaiter(w);
            ProcessUnref(proc); // caller's reference prevents final teardown
        }
    }
    IrqSpinLockRelease(&g_futexLock, flags);
}

static int64_t FutexTimeoutDeadline(int op, uint64_t opVal, uint64_t timeoutAddr,
                                    FutexWaitDeadline& deadline)
{
    deadline = {};
    if (timeoutAddr == 0) return 0;
    timespec request{};
    if (!CopyFromUser(&request, timeoutAddr, sizeof(request))) return -EFAULT;
    uint64_t valueNs = 0;
    if (!FutexTimeoutNanoseconds(request.tv_sec, request.tv_nsec, &valueNs)) return -EINVAL;
    const bool relative = op == FUTEX_WAIT;
    const unsigned clockId = !relative && (opVal & FUTEX_CLOCK_REALTIME) ? 0 : 1;
    return FutexMakeDeadline(relative, valueNs, ClockNowNs(clockId), clockId, &deadline)
        ? 0 : -EINVAL;
}

// Shared memfd/file monotonic IDs are value identities, not an allocator or
// physical-page address. WAIT keeps its mapping transaction through comparison
// and queue insertion, then releases both locks before sleeping. Existing
// aliases keep the object alive; final unmap cannot reuse this key for a new
// object. Waiter cancellation retains only its existing Process reference.
static int FutexMappedKey(Process* process,uint64_t address,uint64_t operation,
                          const MappingRange* range,uint64_t& owner,uint64_t& keyAddress)
{
    owner=process?process->tgid:0;keyAddress=address;
    if(address&3)return -EINVAL;
    if((operation&FUTEX_PRIVATE_FLAG) || !range ||
       range->sharing!=MappingSharing::Shared || !range->backing)return 0;
    constexpr uint64_t SharedDomain=uint64_t{1}<<63;
    constexpr uint64_t FileDomain=uint64_t{1}<<62;
    uint64_t identity=0,domain=SharedDomain;
    if(range->kind==MappingKind::MemFd) {
        auto* memory=static_cast<MemFdData*>(range->backing->object);
        if(memory)identity=memory->inode;
    } else if(range->kind==MappingKind::File && range->backing->file) {
        identity=range->backing->file->orderId;
        domain|=FileDomain;
    } else return 0;
    // Reject exhaustion before publishing a key in the tagged owner domain.
    if(!identity || identity>=FileDomain)return -75;
    const uint64_t displacement=address-range->begin;
    if(range->offset>~uint64_t{0}-displacement)return -75;
    owner=domain|identity;keyAddress=range->offset+displacement;
    return 0;
}
static int FutexResolveKey(Process* process,uint64_t address,uint64_t operation,
                           uint64_t& owner,uint64_t& keyAddress)
{
    owner=process?process->tgid:0;keyAddress=address;
    if(address&3)return -EINVAL;
    if((operation&FUTEX_PRIVATE_FLAG) || !process || !process->addressSpace)return 0;
    MappingTransaction transaction(process,process->addressSpace);
    if(!transaction.LockFaultRange(address))return -ENOMEM;
    return FutexMappedKey(process,address,operation,
        MappingRangeFind(process->addressSpace->ranges,address),owner,keyAddress);
}

// Target-AS access never switches CR3 or dereferences the target's virtual
// address in the cleanup worker. Fault preparation precedes publication locks.
static bool RobustEnsurePage(Process* actor,AddressSpaceState* space,uint64_t address,uint32_t access)
{
    auto state=UserMappingPageAccess(actor,space,address,access,true);
    if(state==UserPageAccess::Missing) {
        if(ResolveMappingFaultForSpace(actor,space,address,4|((access&2)?2:0),8)!=MappingFaultResult::Resolved)return false;
        state=UserMappingPageAccess(actor,space,address,access,true);
    }
    return state==UserPageAccess::Ready;
}
static bool RobustRead(Process* actor,AddressSpaceState* space,uint64_t address,void* output,uint64_t length)
{
    constexpr uint64_t End=0x0000800000000000ULL;
    if(address<VIRTUAL_NULL_GUARD || address>=End || length>End-address)return false;
    auto* bytes=static_cast<uint8_t*>(output);
    while(length) {
        uint64_t count=4096-(address&4095);if(count>length)count=length;
        if(!RobustEnsurePage(actor,space,address,1))return false;
        MappingTransaction mapping(actor,space);
        if(!mapping.LockFaultRange(address))return false;
        const auto* range=MappingRangeFind(space->ranges,address);
        const uint64_t leaf=VmmReadLeaf(space->pageTable,VirtualAddress(address));
        if(!range || range->kind==MappingKind::Device || !(range->protection&1) || range->end-address<count ||
           !(leaf&VMM_PRESENT) || !(leaf&VMM_USER))return false;
        const auto physical=VmmVirtToPhys(space->pageTable,VirtualAddress(address));
        if(!physical)return false;
        memcpy(bytes,reinterpret_cast<const void*>(PhysToVirt(physical).raw()),count);
        address+=count;bytes+=count;length-=count;
    }
    return true;
}
static bool RobustWord(Process* actor,Process* target,AddressSpaceState* space,uint64_t address,bool pending)
{
    constexpr uint32_t OwnerMask=0x3fffffff,Waiters=0x80000000,Died=0x40000000;
    if(address&3)return false;
    uint32_t observed=0;
    if(!RobustRead(actor,space,address,&observed,4))return false;
    if((observed&OwnerMask)!=target->pid && !(pending && !(observed&OwnerMask)))return true;
    for(unsigned retry=0;retry<16;++retry) {
        const bool released=pending && !(observed&OwnerMask);
        const uint32_t access=released?1u:2u;
        if(!RobustEnsurePage(actor,space,address,access))return false;
        uint64_t owner=0,key=0;bool wake=false,done=false;
        {
            MappingTransaction mapping(actor,space);
            if(!mapping.LockFaultRange(address))return false;
            const auto* range=MappingRangeFind(space->ranges,address);
            const uint64_t leaf=VmmReadLeaf(space->pageTable,VirtualAddress(address));
            if(!range || range->kind==MappingKind::Device || (range->protection&access)!=access || range->end-address<4 ||
               !(leaf&VMM_PRESENT) || !(leaf&VMM_USER) || ((access&2) && !(leaf&VMM_WRITABLE)))continue;
            if(FutexMappedKey(target,address,0,range,owner,key))return false;
            const auto physical=VmmVirtToPhys(space->pageTable,VirtualAddress(address));
            if(!physical)return false;
            auto* word=reinterpret_cast<uint32_t*>(PhysToVirt(physical).raw());
            const uint64_t flags=IrqSpinLockAcquire(&g_futexLock);
            observed=__atomic_load_n(word,__ATOMIC_ACQUIRE);
            if(pending && !(observed&OwnerMask)) {wake=true;done=true;}
            else if((observed&OwnerMask)!=target->pid)done=true;
            else if(access&2) {
                const uint32_t replacement=(observed&Waiters)|Died;
                wake=(observed&Waiters)!=0;
                done=__atomic_compare_exchange_n(word,&observed,replacement,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
            }
            IrqSpinLockRelease(&g_futexLock,flags);
        }
        // No mapping/file lock may survive into scheduler wake operations.
        if(done) {
            if(wake && !FutexWake(owner,key,1) && (owner>>63))
                (void)FutexWake(target->tgid,address,1);
            return true;
        }
    }
    return false;
}
void RobustFutexCleanup(Process* target)
{
    if(!target || target->isKernelThread)return;
    auto* actor=ProcessCurrent();if(!actor)return;
    KernelOperationGuard protectedActor(actor);
    uint32_t state=__atomic_load_n(&target->robustExitState,__ATOMIC_ACQUIRE);
    for(;;) {
        if(state>=2)return;
        if(__atomic_compare_exchange_n(&target->robustExitState,&state,2,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
    }
    auto* space=target->addressSpace;
    AddressSpaceRef(space);
    const uint64_t head=__atomic_exchange_n(&target->robustListHead,0,__ATOMIC_ACQ_REL);
    struct Head {uint64_t next;int64_t offset;uint64_t pending;} list{};
    if(head && space && !space->kernel && RobustRead(actor,space,head,&list,sizeof(list))) {
        auto address=[&](uint64_t entry,uint64_t& result) {
            const uint64_t raw=entry&~uint64_t{1};
            if(list.offset>=0)return !__builtin_add_overflow(raw,static_cast<uint64_t>(list.offset),&result);
            const uint64_t distance=uint64_t{0}-static_cast<uint64_t>(list.offset);
            if(raw<distance)return false;result=raw-distance;return true;
        };
        uint64_t entry=list.next;
        for(unsigned count=0;count<2048 && (entry&~uint64_t{1})!=head;++count) {
            const uint64_t node=entry&~uint64_t{1};uint64_t next=0,word=0;
            const bool gotNext=RobustRead(actor,space,node,&next,8);
            if(node!=(list.pending&~uint64_t{1}) && !(entry&1) &&
               (!address(entry,word) || !RobustWord(actor,target,space,word,false)))break;
            if(!gotNext)break;
            entry=next;
        }
        uint64_t word=0;
        if(list.pending && !(list.pending&1) && address(list.pending,word))
            (void)RobustWord(actor,target,space,word,true);
    }
    AddressSpaceUnref(space);
    __atomic_store_n(&target->robustExitState,3,__ATOMIC_RELEASE);
}

static int64_t sys_futex(uint64_t uaddrVal, uint64_t opVal, uint64_t val,
                          uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    int op = static_cast<int>(opVal) & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    Process* proc = ProcessCurrent();
    // Private addresses keep the existing per-thread-group key. Non-private
    // shared memfd aliases need an object/offset key across different VAs/TGIDs.
    uint64_t owner=0,keyAddress=0;
    owner=proc?proc->tgid:0;keyAddress=uaddrVal;
    if(uaddrVal&3)return -EINVAL;
    if(op!=FUTEX_WAIT && op!=FUTEX_WAIT_BITSET)
        if(int error=FutexResolveKey(proc,uaddrVal,opVal,owner,keyAddress))return error;
    if((owner>>63) && op!=FUTEX_WAKE && op!=FUTEX_WAKE_BITSET && op!=FUTEX_WAKE_OP)return -ENOSYS;

    if (op == FUTEX_WAKE || op == FUTEX_WAKE_BITSET) {
        uint32_t maxWake = static_cast<uint32_t>(val);
        if (maxWake == 0) return 0;
        uint32_t wake_bitset = (op == FUTEX_WAKE_BITSET)
            ? static_cast<uint32_t>(arg6)
            : FUTEX_BITSET_MATCH_ANY;
        if (wake_bitset == 0) return -EINVAL;
        int64_t r = FutexWake(owner, keyAddress, maxWake, wake_bitset);
        return r;
    }

    if (op == FUTEX_WAKE_OP) {
        uint64_t secondOwner=0,secondAddress=0;
        if(int error=FutexResolveKey(proc,arg5,opVal,secondOwner,secondAddress))return error;
        // Shared atomic RMW/requeue/PI operations need separate admission work.
        if((owner>>63) || (secondOwner>>63))return -95;
        if (!UserBufferWritable(arg5, sizeof(uint32_t)))
            return -EFAULT;
        auto* uaddr2 = reinterpret_cast<volatile uint32_t*>(arg5);
        uint32_t encoded = static_cast<uint32_t>(arg6);
        uint32_t opCode = (encoded >> 28) & 0xF;
        uint32_t cmpCode = (encoded >> 24) & 0xF;
        uint32_t opArg = (encoded >> 12) & 0xFFF;
        uint32_t cmpArg = encoded & 0xFFF;
        if (opArg & 0x800) opArg |= 0xFFFFF000u;
        if (cmpArg & 0x800) cmpArg |= 0xFFFFF000u;
        if (opCode & 0x8) {
            opCode &= 0x7;
            opArg = 1u << opArg;
        }

        uint32_t oldVal = __atomic_load_n(uaddr2, __ATOMIC_ACQUIRE);
        uint32_t newVal = oldVal;
        switch (opCode) {
        case 0: newVal = opArg; break;              // FUTEX_OP_SET
        case 1: newVal = oldVal + opArg; break;     // FUTEX_OP_ADD
        case 2: newVal = oldVal | opArg; break;     // FUTEX_OP_OR
        case 3: newVal = oldVal & ~opArg; break;    // FUTEX_OP_ANDN
        case 4: newVal = oldVal ^ opArg; break;     // FUTEX_OP_XOR
        default: return -ENOSYS;
        }
        __atomic_store_n(uaddr2, newVal, __ATOMIC_RELEASE);

        bool cmp = false;
        int32_t lhs = static_cast<int32_t>(oldVal);
        int32_t rhs = static_cast<int32_t>(cmpArg);
        switch (cmpCode) {
        case 0: cmp = (lhs == rhs); break; // FUTEX_OP_CMP_EQ
        case 1: cmp = (lhs != rhs); break; // FUTEX_OP_CMP_NE
        case 2: cmp = (lhs <  rhs); break; // FUTEX_OP_CMP_LT
        case 3: cmp = (lhs <= rhs); break; // FUTEX_OP_CMP_LE
        case 4: cmp = (lhs >  rhs); break; // FUTEX_OP_CMP_GT
        case 5: cmp = (lhs >= rhs); break; // FUTEX_OP_CMP_GE
        default: return -ENOSYS;
        }

        uint32_t maxWake1 = static_cast<uint32_t>(val);
        uint32_t maxWake2 = static_cast<uint32_t>(arg4);
        int64_t woken = maxWake1 ? FutexWake(owner, keyAddress, maxWake1) : 0;
        if (cmp && maxWake2)
            woken += FutexWake(secondOwner, secondAddress, maxWake2);
        return woken;
    }

    if (op == FUTEX_WAIT || op == FUTEX_WAIT_BITSET) {
        if (!UserBufferReadable(uaddrVal, sizeof(uint32_t)))
            return -EFAULT;
        auto* uaddr = reinterpret_cast<volatile uint32_t*>(uaddrVal);

        if (!proc) {
            SerialPrintf("sys_futex: WAIT no current process!\n");
            return -ENOSYS;
        }

        FutexWaitDeadline deadline;
        if (int64_t error = FutexTimeoutDeadline(op, opVal, arg4, deadline)) return error;
        const uint32_t waitBitset = op == FUTEX_WAIT_BITSET
            ? static_cast<uint32_t>(arg6) : FUTEX_BITSET_MATCH_ANY;
        if (!waitBitset) return -EINVAL;

        // Fault safely before taking mapping/futex locks. The mapping
        // transaction then keeps key derivation, value comparison and queue
        // publication in one admission interval against unmap/remap.
        uint32_t prefault=0;
        if(!CopyFromUser(&prefault,uaddrVal,sizeof(prefault)))return -EFAULT;
        {
        MappingTransaction mapping(proc,proc->addressSpace);
        if(!mapping.LockFaultRange(uaddrVal))return -ENOMEM;
        const auto* range=MappingRangeFind(proc->addressSpace->ranges,uaddrVal);
        if(!range || !(range->protection&1) || uaddrVal>range->end || range->end-uaddrVal<4)return -EFAULT;
        // A concurrent remap after prefault may install a new lazy range.
        // Never fault while holding AS mutation or the IRQ futex lock: let
        // userspace retry admission instead of touching an absent leaf.
        const uint64_t leaf=VmmReadLeaf(proc->addressSpace->pageTable,VirtualAddress(uaddrVal));
        if(!(leaf&VMM_PRESENT) || !(leaf&VMM_USER))return -EAGAIN;
        if(int error=FutexMappedKey(proc,uaddrVal,opVal,range,owner,keyAddress))return error;
        uint64_t fxFlags = IrqSpinLockAcquire(&g_futexLock);

        // Check if *uaddr == val while holding the lock
        if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != static_cast<uint32_t>(val)) {
            IrqSpinLockRelease(&g_futexLock, fxFlags);
            return -EAGAIN;
        }

        if (deadline.finite && deadline.expired(ClockNowNs(deadline.clockId))) {
            IrqSpinLockRelease(&g_futexLock, fxFlags);
            return -ETIMEDOUT;
        }

        // Allocate and enqueue waiter
        FutexWaiter* w = FutexAllocWaiter();
        if (!w) {
            IrqSpinLockRelease(&g_futexLock, fxFlags);
            SerialPrintf("sys_futex: ENOMEM (pool exhausted) pid=%u\n", proc->pid);
            return -ENOMEM;
        }
        w->uaddr = keyAddress;
        w->owner = owner;
        w->proc = proc;
        w->bitset = waitBitset;
        uint32_t bucket = FutexHash(owner, keyAddress);
        ProcessRef(proc); // owned by linked waiter, then wake/remove/cancel
        w->next = g_futexBuckets[bucket];
        g_futexBuckets[bucket] = w;

        // Clear pending wakeup flag before blocking
        __atomic_store_n(&proc->pendingWakeup, 0, __ATOMIC_RELEASE);
        if (deadline.finite) {
            // Sample the ns clock first, then its coarse scheduling hint.
            const uint64_t nowNs = ClockNowNs(deadline.clockId);
            const uint64_t nowTick = ApicTickCount();
            proc->wakeupTick = deadline.wakeupTick(nowNs, nowTick);
        } else {
            proc->wakeupTick = ~0ULL;
        }

        // Release the futex lock (restoring interrupts) BEFORE blocking — a
        // process must never be descheduled with interrupts disabled.
        IrqSpinLockRelease(&g_futexLock, fxFlags);
        } // release mapping transaction, backing pins and AS gates before wait
        FutexLog(0, uaddrVal, proc->pid, (int32_t)val);  // WAIT-enqueue

        for (;;) {
            SchedulerBlockInterruptible(proc);
            const bool interrupted = HasPendingSignals();
            const bool expired = deadline.finite && deadline.expired(ClockNowNs(deadline.clockId));
            if (interrupted || expired || !deadline.finite) {
                if (!FutexRemoveWaiter(owner, keyAddress, proc)) break;
                if (interrupted) return -EINTR;
                if (expired) return -ETIMEDOUT;
                // BRO-205: an infinite wait may return spuriously, but must
                // never report ETIMEDOUT (glibc mutex waits abort on it).
                return 0;
            }
            // Finite early wake: retain the entry so concurrent FUTEX_WAKE
            // cannot disappear between a dequeue and a new enqueue.
            if (!FutexRearmWaiter(owner, keyAddress, proc, deadline)) break;
        }

        FutexLog(1, uaddrVal, proc->pid, 0);  // WAIT-return (woken)
        return 0;
    }

    // PI futexes (priority inheritance): stub as simple wait/wake.
    if (op == FUTEX_LOCK_PI) {
        auto* uaddr = reinterpret_cast<volatile uint32_t*>(uaddrVal);
        if (!UserBufferWritable(uaddrVal, 4)) return -EFAULT;
        uint32_t tid = proc ? proc->pid : 1;
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(uaddr, &expected, tid, false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return 0; // acquired uncontended
        // Contended — simplified: just pretend success to unblock init.
        return 0;
    }

    if (op == FUTEX_UNLOCK_PI) {
        auto* uaddr = reinterpret_cast<volatile uint32_t*>(uaddrVal);
        if (!UserBufferWritable(uaddrVal, 4)) return -EFAULT;
        __atomic_store_n(uaddr, 0u, __ATOMIC_SEQ_CST);
        FutexWake(owner, keyAddress, 1, FUTEX_BITSET_MATCH_ANY);
        return 0;
    }

    {
        uint64_t userRip = 0, userRax = 0;
        __asm__ volatile("movq %%gs:48, %0" : "=r"(userRip));
        __asm__ volatile("movq %%gs:120, %0" : "=r"(userRax));
        SerialPrintf("sys_futex: unsupported op=%d (raw=0x%lx) pid=%u rax=%lu rip=0x%lx "
                     "uaddr=0x%lx val=0x%lx a4=0x%lx a5=0x%lx a6=0x%lx\n",
                     op, opVal,
                     ProcessCurrent() ? ProcessCurrent()->pid : 0,
                     userRax, userRip, uaddrVal, val, arg4, arg5, arg6);
    }
    return -ENOSYS;
}

// ---------------------------------------------------------------------------
// Socket syscalls
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Per-process socket-to-fd mapping.
// Socket index is stored in FdEntry::handle as (void*)(uintptr_t)(sockIdx + 1).
// +1 so that socket 0 maps to non-null handle.

static int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;
    KernelOperationGuard operation(proc);

    DbgPrintf("sys_socket: domain=%lu type=%lu proto=%lu\n", domain, type, protocol);

    if (domain == AF_UNIX) {
        auto* usd = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
        if (!usd) return -ENOMEM;
        UnixLife(UnixLifeEvent::Allocate, usd, 0, 1, -1,
                 reinterpret_cast<uint64_t>(__builtin_return_address(0)), sizeof(UnixSocketData));
        memset(usd, 0, sizeof(UnixSocketData));
        usd->state    = UnixSocketData::State::Unbound;
        usd->nonblock = (type & UNIX_SOCK_NONBLOCK) != 0;
        usd->refCount = 1;
        usd->socketType = static_cast<uint32_t>(type) & 0xfu;
        usd->peerCredentials = {0, ~uint32_t{0}, ~uint32_t{0}};

        int fd = UnixInstallSocket(proc, usd, static_cast<uint32_t>(type), "socket");
        if (fd < 0) { UnixSocketHandleUnref(usd); return fd; }
        DbgPrintf("sys_socket: AF_UNIX fd=%d\n", fd);
        return fd;
    }

    // AF_INET6 (10) — not supported
    if (domain != AF_INET) {
        // Only log interesting rejections (AF_INET6), not AF_NETLINK spam
        if (domain == 10) { // AF_INET6
        }
        return -EAFNOSUPPORT;
    }

    int sockIdx = SockCreate(static_cast<int>(domain),
                              static_cast<int>(type & 0xFF), // mask SOCK_NONBLOCK etc
                              static_cast<int>(protocol));
    if (sockIdx < 0) return -ENOMEM;

    // Record nonblocking flag
    if (type & 0x800) // SOCK_NONBLOCK
        brook::SockSetNonblock(sockIdx, true);

    // Allocate an fd for this socket
    int fd = FdAlloc(proc, FdType::Socket, reinterpret_cast<void*>(static_cast<uintptr_t>(sockIdx + 1)));
    if (fd < 0) {
        SockClose(sockIdx);
        return -EMFILE;
    }

    DbgPrintf("sys_socket: fd=%d sockIdx=%d\n", fd, sockIdx);
    return fd;
}

static int GetSockIdx(Process* proc, int fd)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return -1;
    FdEntry* e = &proc->fds[fd];
    if (e->type != FdType::Socket) return -1;
    return static_cast<int>(reinterpret_cast<uintptr_t>(e->handle)) - 1;
}

static int64_t sys_bind(uint64_t fdVal, uint64_t addrVal, uint64_t addrLen,
                         uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // Check for AF_UNIX socket first
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if (addrLen<2 || addrLen>sizeof(SockAddrUn)) return -EINVAL;
        SockAddrUn address{};
        if (!CopyFromUser(&address,addrVal,addrLen)) return -EFAULT;
        if (address.sun_family != AF_UNIX) return -EINVAL;
        UnixNameGuard nameGuard;
        SpinLockAcquire(&g_unixRegistryLock);
        if (usd->localAddressLength || usd->state!=UnixSocketData::State::Unbound) {
            SpinLockRelease(&g_unixRegistryLock);return -EINVAL;
        }
        SpinLockRelease(&g_unixRegistryLock);
        char path[109]{};
        memcpy(path, address.sun_path, addrLen - 2);
        Vnode* node = nullptr;
        VnodeStat identity{};
        if (path[0]) {
            AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
            int error=0;
            node=VfsCreateSocketAt(lookup.fs.root,lookup.start(),path,0777,lookup.fs.mask,&error);
            if (!node) return error;
            if ((error = VfsStat(node, &identity))) { VfsClose(node); return error; }
        }
        SpinLockAcquire(&g_unixRegistryLock);
        usd->boundNode = node;
        usd->boundDev = identity.dev;
        usd->boundIno = identity.ino;
        memcpy(usd->localAddress,&address,addrLen);usd->localAddressLength=static_cast<uint32_t>(addrLen);
        for (int i = 0; i < 107 && path[i]; ++i) usd->path[i] = path[i];
        SpinLockRelease(&g_unixRegistryLock);
        UNIX_LIFE(Bind, usd, 0, 0, fd);
        SerialPrintf("sys_bind: AF_UNIX fd=%d path='%s'\n", fd, usd->path);
        return 0;
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    auto* addr = reinterpret_cast<const SockAddrIn*>(addrVal);
    if (!addr || addrLen < sizeof(SockAddrIn)) return -EINVAL;

    return SockBind(sockIdx, addr);
}

static int64_t sys_connect(uint64_t fdVal, uint64_t addrVal, uint64_t addrLen,
                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // AF_UNIX connect: find server, create pipe pair, enqueue, block until accepted
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        UnixNameGuard nameGuard;
        if (usd->state == UnixSocketData::State::Connected) return -106; // EISCONN
        if (usd->state == UnixSocketData::State::Listening) return -EINVAL;
        if (addrLen <= 2 || addrLen > sizeof(SockAddrUn)) return -EINVAL;
        SockAddrUn address{};
        if (!CopyFromUser(&address, addrVal, addrLen)) return -EFAULT;
        if (address.sun_family != AF_UNIX) return -EINVAL;

        // Copy destination path
        char path[109] = {};
        memcpy(path, address.sun_path, addrLen - 2);

        DbgPrintf("sys_connect: AF_UNIX fd=%d path='%s'\n", fd, path);

        // Resolve and pin the named inode, including parent search and socket
        // write permission. The inode pin makes unlink/rebinding unambiguous.
        struct NodePin {
            Vnode* node = nullptr;
            ~NodePin() { if (node) VfsClose(node); }
        } lookup;
        VnodeStat identity{};
        if (path[0]) {
            AtLookup at;if(int error=at.Init(-100,path))return error;
            int error=0;
            lookup.node=VfsOpenAt(at.fs.root,at.start(),path,VFS_O_METADATA,0,at.fs.mask,&error);
            if (!lookup.node) return error ? error : -ENOENT;
            if ((error = VfsAccess(lookup.node, 2, false))) return error;
            if ((error = VfsStat(lookup.node, &identity))) return error;
            if ((identity.mode & 0170000) != 0140000) return -ECONNREFUSED;
        }
        UnixSocketData* server = UnixFindServer(path, lookup.node ? &identity : nullptr);
        if (!server) return -ECONNREFUSED;
        struct ServerReference {
            UnixSocketData* value;
            ~ServerReference() { UnixSocketHandleUnref(value); }
        } serverReference{server};
        // The pending slot owns the server endpoints before connect returns.
        // A successful local stream connect queues a connection; it does not
        // wait for accept, and must survive the client closing immediately.
        auto* pipeCS = PipeBufferCreate(PIPE_BUF_UNIX_SIZE);
        auto* pipeSC = PipeBufferCreate(PIPE_BUF_UNIX_SIZE);
        auto* fdqCS = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue)));
        auto* fdqSC = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue)));
        if (!pipeCS || !pipeSC || !fdqCS || !fdqSC) {
            if (pipeCS) PipeBufferDestroy(pipeCS);
            if (pipeSC) PipeBufferDestroy(pipeSC);
            if (fdqCS) kfree(fdqCS);
            if (fdqSC) kfree(fdqSC);
            return -ENOMEM;
        }
        memset(fdqCS, 0, sizeof(UnixFdQueue));
        memset(fdqSC, 0, sizeof(UnixFdQueue));
        pipeCS->readers = 1; pipeCS->writers = 1;
        pipeSC->readers = 1; pipeSC->writers = 1;
        fdqCS->refCount = 2; // client and pending server endpoint
        fdqSC->refCount = 2;

        SpinLockAcquire(&g_unixRegistryLock);
        int slot = -1;
        for (int i = 0; i < UNIX_ACCEPT_QUEUE; ++i)
            if (!server->pending[i].used) { slot = i; break; }
        int error = usd->state == UnixSocketData::State::Connected ? -106 :
                    usd->state == UnixSocketData::State::Listening ? -EINVAL :
                    slot < 0 ? -EAGAIN : 0;
        if (error) {
            SpinLockRelease(&g_unixRegistryLock);
            PipeBufferDestroy(pipeCS); PipeBufferDestroy(pipeSC);
            kfree(fdqCS); kfree(fdqSC);
            return error;
        }
        usd->peerCredentials = server->peerCredentials;
        usd->peerIncarnation=server->peerIncarnation;usd->peerLaunchId=server->peerLaunchId;
        usd->rxPipe = pipeSC;
        usd->txPipe = pipeCS;
        usd->incomingFds = fdqSC;
        usd->peerIncomingFds = fdqCS;
        for (int i = 0; i < 107 && path[i]; ++i) usd->path[i] = path[i];
        usd->state = UnixSocketData::State::Connected;

        UnixPendingConn& pending = server->pending[slot];
        pending.serverRx = pipeCS;
        pending.serverTx = pipeSC;
        pending.serverRxFds = fdqCS;
        pending.serverTxFds = fdqSC;
        pending.clientCredentials = UnixCurrentCredentials(proc);
        pending.clientIncarnation=UnixCurrentIncarnation(proc);pending.clientLaunchId=proc->launchId;
        pending.used = true;
        ++server->pendingCount;
        if (server->acceptWaiter) {
            Process* waiter = server->acceptWaiter;
            server->acceptWaiter = nullptr;
            WakeProcess(waiter);
        }
        UnixListenWakeEpoll(server);
        SpinLockRelease(&g_unixRegistryLock);

        (void)addrLen;
        return 0;
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    // Snapshot the supplied sockaddr before logging or entering the network
    // stack. Copy only the advertised length, including storage-size callers.
    if (addrLen < sizeof(brook::SockAddrIn) || addrLen > 128) return -EINVAL;
    alignas(brook::SockAddrIn) uint8_t address[128]{};
    if (!CopyFromUser(address, addrVal, addrLen)) return -EFAULT;
    const auto* uaddr = reinterpret_cast<const brook::SockAddrIn*>(address);

    DbgPrintf("sys_connect: fd=%d sockIdx=%d type=%d addr=%u.%u.%u.%u:%u\n",
                 fd, sockIdx, brook::SockGetType(sockIdx),
                 (brook::ntohl(uaddr->sin_addr) >> 24) & 0xFF,
                 (brook::ntohl(uaddr->sin_addr) >> 16) & 0xFF,
                 (brook::ntohl(uaddr->sin_addr) >> 8) & 0xFF,
                 brook::ntohl(uaddr->sin_addr) & 0xFF,
                 brook::ntohs(uaddr->sin_port));
    // For UDP, "connect" just sets the default destination
    // For TCP, perform the 3-way handshake
    int connectResult = brook::SockConnect(sockIdx, uaddr);
    {
    }
    return connectResult;
}

static int64_t sys_sendto(uint64_t fdVal, uint64_t bufVal, uint64_t lenVal,
                           uint64_t flagsVal, uint64_t destVal, uint64_t addrLenVal)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (fde && fde->type == FdType::UnixSocket) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if(lenVal && !UserBufferReadable(bufVal,lenVal))return -EFAULT;
        return UnixSocketWriteSome(usd,(flagsVal&0x40)||(fde->statusFlags&0x800),
            reinterpret_cast<const char*>(bufVal),lenVal,nullptr,0,(flagsVal&0x4000)!=0);
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    (void)flagsVal;

    // For connected TCP sockets, use SockSend (stream send)
    if (brook::SockIsStream(sockIdx))
    {
        int ret = brook::SockSend(sockIdx,
                                   reinterpret_cast<const void*>(bufVal),
                                   static_cast<uint32_t>(lenVal));
        return static_cast<int64_t>(ret);
    }

    // UDP path
    auto* dest = destVal ? reinterpret_cast<const SockAddrIn*>(destVal) : nullptr;
    int ret = SockSendTo(sockIdx, reinterpret_cast<const void*>(bufVal),
                          static_cast<uint32_t>(lenVal), dest);
    if (ret < 0) return -EIO;
    return static_cast<int64_t>(lenVal);
}

static int64_t sys_recvfrom(uint64_t fdVal, uint64_t bufVal, uint64_t lenVal,
                             uint64_t flagsVal, uint64_t srcVal, uint64_t addrLenVal)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (fde && fde->type == FdType::UnixSocket) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        bool oldNonblock = usd ? usd->nonblock : false;
        if (usd && (flagsVal & 0x40)) // MSG_DONTWAIT
            usd->nonblock = true;
        int64_t ret = sys_read(fdVal, bufVal, lenVal, 0, 0, 0);
        if (usd)
            usd->nonblock = oldNonblock;
        if (srcVal && addrLenVal >= 0x1000)
            *reinterpret_cast<uint32_t*>(addrLenVal) = 0;
        return ret;
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    (void)flagsVal;

    // For TCP sockets, use SockRecv (stream receive)
    if (brook::SockIsStream(sockIdx))
    {
        bool nonblock = (fde && (fde->statusFlags & 0x800) != 0) || brook::SockIsNonblock(sockIdx);
        if (nonblock && brook::SockRxCount(sockIdx) == 0 && !brook::SockPollReady(sockIdx, true, false))
            return -EAGAIN;
        return ReceiveTcpToUser(sockIdx, bufVal, lenVal);
    }

    // UDP path
    SockAddrIn srcTmp {};
    auto* src = srcVal ? reinterpret_cast<SockAddrIn*>(srcVal) : &srcTmp;
    int ret = SockRecvFrom(sockIdx, reinterpret_cast<void*>(bufVal),
                            static_cast<uint32_t>(lenVal), src);
    if (ret > 0 && srcVal && addrLenVal >= 0x1000)
        *reinterpret_cast<uint32_t*>(addrLenVal) = sizeof(SockAddrIn);

    // Log DNS response delivery to userspace
    if (ret > 0) {
        uint16_t sport = brook::ntohs(src->sin_port);
        if (sport == 53) {
        }
    }

    if (ret < 0) return -EAGAIN;
    return static_cast<int64_t>(ret);
}

static int64_t sys_setsockopt(uint64_t fdVal, uint64_t levelVal, uint64_t optionVal,
                               uint64_t valueAddr, uint64_t length, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, static_cast<int>(fdVal));
    if (!descriptor) return -EBADF;
    if (descriptor->type != FdType::UnixSocket && descriptor->type != FdType::Socket) return -88;
    if (descriptor->type == FdType::UnixSocket && levelVal == 1 && optionVal == 16) {
        if (length < sizeof(int32_t)) return -EINVAL;
        if (!UserBufferReadable(valueAddr, sizeof(int32_t))) return -EFAULT;
        auto* socket = static_cast<UnixSocketData*>(descriptor->handle);
        __atomic_store_n(&socket->passCredentials,
            *reinterpret_cast<const int32_t*>(valueAddr) != 0, __ATOMIC_RELEASE);
        return 0;
    }
    return 0; // Existing unsupported socket options still use the legacy stub.
}

static int64_t sys_getsockopt(uint64_t fdVal, uint64_t levelVal, uint64_t optnameVal,
                               uint64_t optvalAddr, uint64_t optlenAddr, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, static_cast<int>(fdVal));
    if (!descriptor) return -EBADF;
    FdEntry* entry = descriptor.get();
    if (entry->type != FdType::UnixSocket && entry->type != FdType::Socket)
        return -88; // ENOTSOCK
    if (!entry->handle) return -EBADF;
    int level = static_cast<int>(levelVal);
    int option = static_cast<int>(optnameVal);
    if (level != 1) return entry->type == FdType::UnixSocket ? -95 : -92;
    // Linux socklen_t is 32-bit, and negative lengths are invalid.
    if (!UserBufferReadable(optlenAddr, sizeof(uint32_t)) ||
        !UserBufferWritable(optlenAddr, sizeof(uint32_t))) return -EFAULT;
    uint32_t requested = *reinterpret_cast<const uint32_t*>(optlenAddr);
    if (requested > 0x7fffffffu) return -EINVAL;
    uint32_t valueSize = entry->type == FdType::UnixSocket && option == 17
        ? sizeof(UnixCredentials) : sizeof(int32_t);
    uint32_t copied = requested < valueSize ? requested : valueSize;
    if (copied && !UserBufferWritable(optvalAddr, copied)) return -EFAULT;

    int32_t value = 0;
    UnixCredentials credentials{};
    if (entry->type == FdType::UnixSocket) {
        auto* socket = static_cast<UnixSocketData*>(entry->handle);
        switch (option) {
        case 16: // SO_PASSCRED
            value = __atomic_load_n(&socket->passCredentials, __ATOMIC_ACQUIRE) ? 1 : 0;
            break;
        case 17: // SO_PEERCRED (struct ucred), immutable connection identity.
            SpinLockAcquire(&g_unixRegistryLock);
            credentials = socket->peerCredentials;
            SpinLockRelease(&g_unixRegistryLock);
            break;
        case 3: // SO_TYPE: type selected at socket/socketpair creation.
            value = static_cast<int32_t>(socket->socketType);
            break;
        case 4: // SO_ERROR: Unix connect currently reports failures synchronously.
            value = 0;
            break;
        case 38: // SO_PROTOCOL
            value = 0;
            break;
        case 39: // SO_DOMAIN
            value = AF_UNIX;
            break;
        case 7: // SO_SNDBUF
        case 8: { // SO_RCVBUF
            PipeBuffer* pipe = option == 7 ? socket->txPipe : socket->rxPipe;
            if (!pipe || !pipe->capacity) return -ENOTCONN;
            // Ring buffers reserve one byte to distinguish full from empty.
            value = static_cast<int32_t>(pipe->capacity - 1);
            break;
        }
        default: return -92; // ENOPROTOOPT, never success with untouched output.
        }
    } else {
        // Preserve the existing INET async-connect error implementation only.
        if (option != 4) return -92;
        int index = static_cast<int>(reinterpret_cast<uintptr_t>(entry->handle)) - 1;
        value = brook::SockGetConnectError(index);
    }
    if(valueSize==sizeof(credentials)) {
        credentials.uid=ProcessReportId(proc,false,credentials.uid);credentials.gid=ProcessReportId(proc,true,credentials.gid);
    }
    if (copied) memcpy(reinterpret_cast<void*>(optvalAddr),
                       valueSize == sizeof(credentials) ? static_cast<const void*>(&credentials)
                                                        : static_cast<const void*>(&value), copied);
    *reinterpret_cast<uint32_t*>(optlenAddr) = copied;
    return 0;
}

static int64_t sys_getsockname(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                                uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc,static_cast<int>(fdVal));
    if (!descriptor) return -EBADF;
    if (descriptor->type==FdType::UnixSocket && descriptor->handle) {
        uint32_t requested=0;
        if (!CopyFromUser(&requested,addrLenVal,sizeof(requested))) return -EFAULT;
        if (requested>0x7fffffffu) return -EINVAL;
        uint8_t address[sizeof(SockAddrUn)]={1,0};uint32_t actual=2;
        auto* socket=static_cast<UnixSocketData*>(descriptor->handle);
        SpinLockAcquire(&g_unixRegistryLock);
        if (socket->localAddressLength) {
            actual=socket->localAddressLength;memcpy(address,socket->localAddress,actual);
        }
        SpinLockRelease(&g_unixRegistryLock);
        const uint32_t copied=requested<actual?requested:actual;
        if ((copied && !CopyToUser(addrVal,address,copied)) ||
            !CopyToUser(addrLenVal,&actual,sizeof(actual))) return -EFAULT;
        return 0;
    }

    int fd = static_cast<int>(fdVal);
    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    auto* uaddr = reinterpret_cast<brook::SockAddrIn*>(addrVal);
    auto* ulen  = reinterpret_cast<uint32_t*>(addrLenVal);
    if (!uaddr || !ulen) return -EFAULT;

    brook::SockAddrIn local{};
    local.sin_family = AF_INET;
    uint32_t tmpIp = 0;
    uint16_t tmpPort = 0;
    brook::SockGetLocal(sockIdx, &tmpIp, &tmpPort);
    local.sin_addr = tmpIp;
    local.sin_port = tmpPort;

    {
    }

    uint32_t copyLen = *ulen;
    if (copyLen > sizeof(local)) copyLen = sizeof(local);
    __builtin_memcpy(uaddr, &local, copyLen);
    *ulen = sizeof(local);
    return 0;
}

static int64_t sys_getpeername(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                                uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();

    if (!proc) return -ENOSYS;
    int fd = static_cast<int>(fdVal);

    // For inet DGRAM sockets that are "connected" via connect(), return the peer address
    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx >= 0) {
        uint32_t remIp = 0;
        uint16_t remPort = 0;
        brook::SockGetRemote(sockIdx, &remIp, &remPort);
        if (remIp != 0 || remPort != 0) {
            auto* uaddr = reinterpret_cast<brook::SockAddrIn*>(addrVal);
            auto* ulen  = reinterpret_cast<uint32_t*>(addrLenVal);
            if (uaddr && ulen) {
                brook::SockAddrIn peer{};
                peer.sin_family = AF_INET;
                peer.sin_addr = remIp;
                peer.sin_port = remPort;
                uint32_t copyLen = *ulen;
                if (copyLen > sizeof(peer)) copyLen = sizeof(peer);
                __builtin_memcpy(uaddr, &peer, copyLen);
                *ulen = sizeof(peer);
                return 0;
            }
        }
    }

    return -ENOTCONN;
}

static int64_t sys_shutdown(uint64_t fdVal, uint64_t howVal, uint64_t,
                             uint64_t, uint64_t, uint64_t)
{
    Process* proc=ProcessCurrent();if(!proc)return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef fd(proc,static_cast<int>(fdVal));if(!fd)return -EBADF;
    if(fd->type!=FdType::UnixSocket && fd->type!=FdType::Socket)return -88; // ENOTSOCK
    const int how=static_cast<int>(howVal);if(how<0 || how>2)return -EINVAL;
    // Internet socket shutdown needs a separate TCP/UDP state transition.
    if(fd->type!=FdType::UnixSocket)return -95; // EOPNOTSUPP
    auto* socket=static_cast<UnixSocketData*>(fd->handle);
    // Unbound/listening Unix shutdown also has Linux semantics, but is outside
    // this connected-stream implementation. Do not silently claim success.
    if(!socket || socket->state!=UnixSocketData::State::Connected || !socket->rxPipe || !socket->txPipe)
        return -95; // EOPNOTSUPP
    if(how==0 || how==2)UnixShutdownDirection(socket->rxPipe,false);
    if(how==1 || how==2)UnixShutdownDirection(socket->txPipe,true);
    return 0;
}

static int64_t sys_listen(uint64_t fdVal, uint64_t backlog, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // AF_UNIX: mark socket as listening and register in global table
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        UnixNameGuard nameGuard;
        if (usd->state == UnixSocketData::State::Connected) return -EINVAL;
        if (!usd->localAddressLength) return -EINVAL;
        SpinLockAcquire(&g_unixRegistryLock);
        // Linux captures listener identity on the first listen, not at accept.
        if (usd->state != UnixSocketData::State::Listening) {
            usd->peerCredentials = UnixCurrentCredentials(proc);
            usd->peerIncarnation=UnixCurrentIncarnation(proc);usd->peerLaunchId=proc->launchId;
        }
        usd->state = UnixSocketData::State::Listening;
        SpinLockRelease(&g_unixRegistryLock);
        if (!UnixRegisterServer(usd)) return -ENOMEM;
        UNIX_LIFE(Listen, usd, 0, 0, fd);
        SerialPrintf("sys_listen: AF_UNIX fd=%d path='%s'\n", fd, usd->path);
        return 0;
    }

    (void)backlog;
    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    int ret = brook::SockListen(sockIdx, static_cast<int>(backlog));
    return ret < 0 ? -EINVAL : 0;
}

static int64_t AcceptSocket(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                            uint32_t flags)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // AF_UNIX accept: dequeue a pending connection
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if (usd->state != UnixSocketData::State::Listening) return -EINVAL;

        auto* serverSock = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
        if (!serverSock) return -ENOMEM;
        UnixLife(UnixLifeEvent::Allocate, serverSock, 0, 1, -1,
                 reinterpret_cast<uint64_t>(__builtin_return_address(0)), sizeof(UnixSocketData));
        memset(serverSock, 0, sizeof(UnixSocketData));
        serverSock->refCount = 1;
        struct AcceptedAllocation {
            UnixSocketData* socket;
            ~AcceptedAllocation() { if (socket) UnixSocketHandleUnref(socket); }
        } allocation{serverSock};

        for (;;) {
            if (__atomic_load_n(&fde->closing, __ATOMIC_ACQUIRE)) return -EBADF;
            SpinLockAcquire(&g_unixRegistryLock);
            int slot = -1;
            for (int i = 0; i < UNIX_ACCEPT_QUEUE; ++i)
                if (usd->pending[i].used) { slot = i; break; }
            if (slot >= 0) {
                const UnixPendingConn& pending = usd->pending[slot];
                serverSock->state = UnixSocketData::State::Connected;
                serverSock->socketType = usd->socketType;
                serverSock->peerCredentials = pending.clientCredentials;
                serverSock->peerIncarnation=pending.clientIncarnation;
                serverSock->peerLaunchId=pending.clientLaunchId;serverSock->acceptedEndpoint=true;
                serverSock->localAddressLength=usd->localAddressLength;
                memcpy(serverSock->localAddress,usd->localAddress,sizeof(serverSock->localAddress));
                serverSock->nonblock = (flags & UNIX_SOCK_NONBLOCK) != 0;
                serverSock->rxPipe = pending.serverRx;
                serverSock->txPipe = pending.serverTx;
                serverSock->incomingFds = pending.serverRxFds;
                serverSock->peerIncomingFds = pending.serverTxFds;

                // Registry -> fd lock is safe: finalizers release fdLock before
                // taking the registry lock, and fd retain callbacks only use
                // atomic reference increments. Keep the pending owner intact
                // until publication succeeds so EMFILE remains retryable.
                int newFd = UnixInstallSocket(proc, serverSock, flags, "accept");
                if (newFd < 0) {
                    serverSock->rxPipe = serverSock->txPipe = nullptr;
                    serverSock->incomingFds = serverSock->peerIncomingFds = nullptr;
                    SpinLockRelease(&g_unixRegistryLock);
                    return newFd;
                }
                usd->pending[slot] = {};
                --usd->pendingCount;
                allocation.socket = nullptr; // pending ownership transferred to fd
                SpinLockRelease(&g_unixRegistryLock);
                if (addrVal >= 0x1000 && addrLenVal >= 0x1000)
                    *reinterpret_cast<uint32_t*>(addrLenVal) = 0;
                SerialPrintf("sys_accept: AF_UNIX fd=%d -> newFd=%d\n", fd, newFd);
                return newFd;
            }
            if (usd->nonblock) {
                SpinLockRelease(&g_unixRegistryLock);
                return -EAGAIN;
            }
            usd->acceptWaiter = proc;
            SpinLockRelease(&g_unixRegistryLock);
            proc->wakeupTick = ApicTickCount() + 100;
            SchedulerBlock(proc);
            SpinLockAcquire(&g_unixRegistryLock);
            if (usd->acceptWaiter == proc) usd->acceptWaiter = nullptr;
            SpinLockRelease(&g_unixRegistryLock);
            if (HasPendingSignals()) return -EINTR;
        }
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    brook::SockAddrIn peerAddr = {};
    int childIdx = brook::SockAccept(sockIdx, &peerAddr);
    if (childIdx < 0) return childIdx; // EAGAIN or error

    // Allocate a new fd for the accepted connection
    FdEntry accepted{};
    accepted.type = FdType::Socket;
    accepted.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(childIdx + 1));
    accepted.statusFlags = flags & UNIX_SOCK_NONBLOCK;
    accepted.fdFlags = (flags & UNIX_SOCK_CLOEXEC) ? 1 : 0;
    int newFd = FdTablePublishOwned(proc->fds, ProcessFdLock(proc), accepted, 0, -1, nullptr);
    if (newFd < 0) {
        brook::SockClose(childIdx);
        return -EMFILE;
    }

    // Copy peer address to user if requested
    if (addrVal && addrLenVal) {
        auto* userAddr = reinterpret_cast<brook::SockAddrIn*>(addrVal);
        auto* userLen  = reinterpret_cast<uint32_t*>(addrLenVal);
        *userAddr = peerAddr;
        *userLen  = sizeof(brook::SockAddrIn);
    }

    SerialPrintf("sys_accept: fd=%d -> newFd=%d childIdx=%d\n", fd, newFd, childIdx);
    return newFd;
}

static int64_t sys_accept(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                          uint64_t, uint64_t, uint64_t)
{
    return AcceptSocket(fdVal, addrVal, addrLenVal, 0);
}

static int64_t sys_accept4(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                            uint64_t flags, uint64_t, uint64_t)
{
    uint32_t options = static_cast<uint32_t>(flags);
    if (options & ~(UNIX_SOCK_NONBLOCK | UNIX_SOCK_CLOEXEC)) return -EINVAL;
    return AcceptSocket(fdVal, addrVal, addrLenVal, options);
}

static int64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol,
                               uint64_t svAddr, uint64_t, uint64_t)
{
    SerialPrintf("sys_socketpair: domain=%lu type=0x%lx proto=%lu sv=0x%lx\n",
                 domain, type, protocol, svAddr);
    // We only support AF_UNIX (domain=1) socketpairs
    if (domain != 1) return -EAFNOSUPPORT;

    auto* sv = reinterpret_cast<int32_t*>(svAddr);
    if (!UserBufferWritable(svAddr, 2 * sizeof(int32_t))) return -EFAULT;

    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;
    KernelOperationGuard operation(proc);

    // Allocate two PipeBuffers — one for each direction
    auto* pipeAtoB = PipeBufferCreate(PIPE_BUF_UNIX_SIZE);
    if (!pipeAtoB) return -ENOMEM;
    auto* pipeBtoA = PipeBufferCreate(PIPE_BUF_UNIX_SIZE);
    if (!pipeBtoA) { PipeBufferDestroy(pipeAtoB); return -ENOMEM; }
    auto* fdqAtoB = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue)));
    if (!fdqAtoB) { PipeBufferDestroy(pipeAtoB); PipeBufferDestroy(pipeBtoA); return -ENOMEM; }
    auto* fdqBtoA = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue)));
    if (!fdqBtoA) { kfree(fdqAtoB); PipeBufferDestroy(pipeAtoB); PipeBufferDestroy(pipeBtoA); return -ENOMEM; }
    auto* endA = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
    if (!endA) { kfree(fdqBtoA); kfree(fdqAtoB); PipeBufferDestroy(pipeAtoB); PipeBufferDestroy(pipeBtoA); return -ENOMEM; }
    UnixLife(UnixLifeEvent::Allocate, endA, 0, 1, -1,
             reinterpret_cast<uint64_t>(__builtin_return_address(0)), sizeof(UnixSocketData));
    auto* endB = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
    if (!endB) { UNIX_LIFE(DirectFree, endA, 1, 0, -1); kfree(endA); kfree(fdqBtoA); kfree(fdqAtoB); PipeBufferDestroy(pipeAtoB); PipeBufferDestroy(pipeBtoA); return -ENOMEM; }

    // Zero-init backing objects
    memset(fdqAtoB, 0, sizeof(UnixFdQueue));
    memset(fdqBtoA, 0, sizeof(UnixFdQueue));
    memset(endA, 0, sizeof(UnixSocketData));
    UnixLife(UnixLifeEvent::Allocate, endB, 0, 1, -1,
             reinterpret_cast<uint64_t>(__builtin_return_address(0)), sizeof(UnixSocketData));
    memset(endB, 0, sizeof(UnixSocketData));

    pipeAtoB->readers = 1;
    pipeAtoB->writers = 1;
    pipeBtoA->readers = 1;
    pipeBtoA->writers = 1;
    fdqAtoB->refCount = 2;
    fdqBtoA->refCount = 2;

    // fd[0]: reads from pipeBtoA, writes to pipeAtoB
    // fd[1]: reads from pipeAtoB, writes to pipeBtoA
    bool nonblock = (type & UNIX_SOCK_NONBLOCK) != 0;
    endA->state = UnixSocketData::State::Connected;
    endA->nonblock = nonblock;
    endA->refCount = 1;
    endA->socketType = static_cast<uint32_t>(type) & 0xfu;
    endA->peerCredentials = UnixCurrentCredentials(proc);
    endA->peerIncarnation=UnixCurrentIncarnation(proc);endA->peerLaunchId=proc->launchId;
    endA->rxPipe = pipeBtoA;
    endA->txPipe = pipeAtoB;
    endA->incomingFds = fdqBtoA;
    endA->peerIncomingFds = fdqAtoB;

    endB->state = UnixSocketData::State::Connected;
    endB->nonblock = nonblock;
    endB->refCount = 1;
    endB->socketType = static_cast<uint32_t>(type) & 0xfu;
    endB->peerCredentials = endA->peerCredentials;
    endB->peerIncarnation=endA->peerIncarnation;endB->peerLaunchId=endA->peerLaunchId;
    endB->rxPipe = pipeAtoB;
    endB->txPipe = pipeBtoA;
    endB->incomingFds = fdqAtoB;
    endB->peerIncomingFds = fdqBtoA;

    int fdA = UnixInstallSocket(proc, endA, static_cast<uint32_t>(type), "socketpair-a");
    if (fdA < 0) {
        UnixSocketHandleUnref(endA);
        UnixSocketHandleUnref(endB);
        return fdA;
    }
    int fdB = UnixInstallSocket(proc, endB, static_cast<uint32_t>(type), "socketpair-b");
    if (fdB < 0) {
        CloseProcessFd(proc, fdA);
        UnixSocketHandleUnref(endB);
        return fdB;
    }

    sv[0] = fdA;
    sv[1] = fdB;

    SerialPrintf("sys_socketpair: fd[%d,%d] for pid %u\n", fdA, fdB, proc->pid);
    return 0;
}

// Linux x86-64 ABI constants (match <sys/socket.h>).
static constexpr int SOL_SOCKET  = 1;
static constexpr int SCM_RIGHTS  = 1;

struct CmsgHdr {
    uint64_t cmsg_len;   // socklen_t in struct, but aligned to 8 bytes
    int      cmsg_level;
    int      cmsg_type;
    // followed by data, padded to 8
};

// CMSG alignment rounds to sizeof(size_t) = 8 on x86-64.
static inline uint64_t CmsgAlign(uint64_t n) { return (n + 7) & ~uint64_t{7}; }

// Take an FdEntry snapshot suitable for passing over AF_UNIX. Bumps the
// underlying handle's refcount so the sender can close their fd without
// freeing the kernel object out from under the receiver.
static bool UnixFdSnapFrom(const FdEntry* src, UnixFdSnap* out)
{
    if (!src || src->type == FdType::None) return false;
    switch (src->type) {
        case FdType::Pipe:
        case FdType::DevDri:
        case FdType::SignalFd:
        case FdType::SyncFile:
        case FdType::EventFd:
        case FdType::EpollFd:
        case FdType::TimerFd:
            // Own the same endpoint/object reference as dup(). In particular,
            // queued pipe rights keep readers/writers alive after sender close.
            FdBumpRefcount(src);
            break;
        case FdType::DevNull:
        case FdType::DevUrandom:
            // Stateless devices have no allocated handle to retain.
            break;
        case FdType::MemFd:
            if (src->handle) MemFdRef(static_cast<MemFdData*>(src->handle));
            break;
        case FdType::Path:
        case FdType::Vnode:
            if (src->handle)
                __atomic_fetch_add(&static_cast<Vnode*>(src->handle)->refCount,
                                    1, __ATOMIC_RELEASE);
            break;
        case FdType::Socket:
            if (src->handle) {
                int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(src->handle)) - 1;
                brook::SockRef(sockIdx);
            }
            break;
        case FdType::UnixSocket:
            if (src->handle)
                UnixSocketHandleRef(src->handle);
            break;
        case FdType::DrmPrime:
            // PRIME dmabuf fd: a client hands its render-target capability to the
            // compositor over the Wayland socket. Bump the object ref so the
            // sender closing its fd doesn't free it out from under the receiver.
            if (src->handle) DrmPrimeRef(static_cast<DrmPrimeObj*>(src->handle));
            break;
        case FdType::CompositorScene:
            if (src->handle) SceneHandleRef(static_cast<SceneHandle*>(src->handle));
            break;
        default:
            // Other fd types (DevDsp, DevTty, etc.)
            // don't have a portable snapshot refcount today.
            return false;
    }
    out->type        = src->type;
    out->flags       = src->flags;
    out->fdFlags     = src->fdFlags;
    out->statusFlags = src->statusFlags;
    out->handle      = src->handle;
    return true;
}

// Install a snapshot into the receiver's fd table. On success the snap's
// refcount becomes the new FdEntry's ref; on failure the caller must
// release it (drainSnap) so we don't leak.
static int UnixFdSnapInstall(Process* dst, const UnixFdSnap* snap)
{
    FdEntry owned{};
    owned.type = snap->type;
    owned.handle = snap->handle;
    owned.flags = snap->flags;
    owned.fdFlags = snap->fdFlags;
    owned.statusFlags = snap->statusFlags;
    return FdTablePublishOwned(dst->fds, ProcessFdLock(dst), owned, 0, -1, nullptr, UnixLifePublished);
}

static void UnixFdSnapRelease(const UnixFdSnap* snap)
{
    if (!snap || !snap->handle) return;
    switch (snap->type) {
        case FdType::Pipe:
            PipeReleaseEnd(static_cast<PipeBuffer*>(snap->handle), (snap->flags & 1) != 0);
            break;
        case FdType::SignalFd:
            SignalFdUnref(static_cast<SignalFd*>(snap->handle));break;
        case FdType::SyncFile:
            SyncFileUnref(static_cast<SyncFileData*>(snap->handle));break;
        case FdType::EventFd:
            EventFdUnref(static_cast<EventFdData*>(snap->handle));
            break;
        case FdType::EpollFd:
            EpollFdUnref(static_cast<EpollInstance*>(snap->handle));
            break;
        case FdType::TimerFd:
            TimerFdUnref(static_cast<TimerFdData*>(snap->handle));
            break;
        case FdType::MemFd:
            MemFdUnref(static_cast<MemFdData*>(snap->handle));
            break;
        case FdType::Path:
        case FdType::Vnode: {
            auto* vn = static_cast<Vnode*>(snap->handle);
            uint32_t prev = __atomic_fetch_sub(&vn->refCount, 1, __ATOMIC_ACQ_REL);
            if (prev <= 1) VfsClose(vn);
            break;
        }
        case FdType::Socket: {
            int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(snap->handle)) - 1;
            brook::SockUnref(sockIdx);
            break;
        }
        case FdType::UnixSocket: {
            UnixSocketHandleUnref(snap->handle);
            break;
        }
        case FdType::DevDri:
            // Queued rights own the same DRM context reference as dup().
            // Share ordinary final-close retirement when a message is discarded.
            FinalizeClosedFd({FdType::DevDri,snap->handle,snap->flags});
            break;
        case FdType::DrmPrime:
            DrmPrimeUnref(static_cast<DrmPrimeObj*>(snap->handle));
            break;
        case FdType::CompositorScene:
            SceneHandleUnref(static_cast<SceneHandle*>(snap->handle));
            break;
        default: break;
    }
}

struct LinuxMsgHdr {
    void*    msg_name;
    uint32_t msg_namelen;
    uint32_t _pad0;
    struct { void* iov_base; uint64_t iov_len; }* msg_iov;
    uint64_t msg_iovlen;
    void*    msg_control;
    uint64_t msg_controllen;
    int      msg_flags;
};

struct LinuxMMsgHdr {
    LinuxMsgHdr msg_hdr;
    uint32_t    msg_len;
    uint32_t    _pad0;
};

static int64_t sys_sendmsg(uint64_t fdVal, uint64_t msgVal, uint64_t flagsVal,
                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);
    if (msgVal < 0x1000) return -EFAULT;

    LinuxMsgHdr stagedHeader{};
    if(!CopyFromUser(&stagedHeader,msgVal,sizeof(stagedHeader)))return -EFAULT;
    auto* msg=&stagedHeader;
    if (!msg->msg_iovlen || msg->msg_iovlen>1024 || !msg->msg_iov) return -EINVAL;
    if (!UserBufferReadable(reinterpret_cast<uint64_t>(msg->msg_iov),msg->msg_iovlen*sizeof(*msg->msg_iov))) return -EFAULT;

    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (!fde) return -EBADF;
    if (fde->type == FdType::UnixSocket) {

    auto* usd = static_cast<UnixSocketData*>(fde->handle);
    auto writable=UnixSocketWriteSome(usd,true,nullptr,0,nullptr,0,(flagsVal&0x4000)!=0);
    if(writable<0)return writable;


    // Parse SCM_RIGHTS cmsg (if any) BEFORE writing payload. If cmsg parsing
    // fails (bad pointer, wrong type, fd full) we refuse the whole sendmsg
    // rather than send orphaned payload — matches Linux kernel behaviour.
    UnixFdSnap pendingSnaps[UNIX_FD_QUEUE_CAP];
    int pendingCount = 0;
    UnixCredentials explicitCredentials{};bool hasExplicitCredentials=false;
    auto failControl=[&](int error)->int64_t {
        for(int j=0;j<pendingCount;++j)UnixFdSnapRelease(&pendingSnaps[j]);
        return error;
    };

    if (msg->msg_control && msg->msg_controllen >= sizeof(CmsgHdr)) {
        uint64_t ctlAddr = reinterpret_cast<uint64_t>(msg->msg_control);
        if (!UserBufferReadable(ctlAddr,msg->msg_controllen)) return -EFAULT;

        uint64_t off = 0;
        while (off + sizeof(CmsgHdr) <= msg->msg_controllen) {
            CmsgHdr cm{};
            if(!CopyFromUser(&cm,ctlAddr+off,sizeof(cm)))return failControl(-EFAULT);
            if(cm.cmsg_len<sizeof(cm) || cm.cmsg_len>msg->msg_controllen-off)
                return failControl(-EINVAL);
            if(cm.cmsg_level==SOL_SOCKET && cm.cmsg_type==2) { // SCM_CREDENTIALS
                if(cm.cmsg_len!=sizeof(cm)+sizeof(UnixCredentials))return failControl(-EINVAL);
                UnixCredentials requested{};
                if(!CopyFromUser(&requested,ctlAddr+off+sizeof(cm),sizeof(requested)))return failControl(-EFAULT);
                requested.uid=ProcessResolveId(proc,false,requested.uid);requested.gid=ProcessResolveId(proc,true,requested.gid);
                const auto users=ProcessReadIds(proc,false),groups=ProcessReadIds(proc,true);
                const int32_t sender=proc->tgid?proc->tgid:proc->pid;
                if(int error=UnixCredentialsCheck(requested,sender,users,groups))return failControl(error);
                explicitCredentials=requested;hasExplicitCredentials=true;
            }
            if (cm.cmsg_level == SOL_SOCKET && cm.cmsg_type == SCM_RIGHTS) {
                uint64_t dataLen = cm.cmsg_len - sizeof(CmsgHdr);
                uint64_t nFds    = dataLen / sizeof(int);

                for (uint64_t i = 0; i < nFds; i++) {
                    if (pendingCount >= UNIX_FD_QUEUE_CAP-1) {
                        for (int j = 0; j < pendingCount; j++)
                            UnixFdSnapRelease(&pendingSnaps[j]);
                        return -90; // EMSGSIZE: this group can never fit.
                    }
                    int transferredFd=0;
                    if(!CopyFromUser(&transferredFd,ctlAddr+off+sizeof(cm)+i*sizeof(int),sizeof(int)))return failControl(-EFAULT);
                    FdRef sourceDescriptor(proc, transferredFd);
                    FdEntry* srcFde = sourceDescriptor.get();
                    if (!srcFde || !UnixFdSnapFrom(srcFde, &pendingSnaps[pendingCount])) {
                        for (int j = 0; j < pendingCount; j++)
                            UnixFdSnapRelease(&pendingSnaps[j]);
                        return -EBADF;
                    }
                    pendingCount++;
                }
            }

            uint64_t step = CmsgAlign(cm.cmsg_len);
            if (step == 0) break;
            off += step;
        }
    }

    // Commit the rights with the first actual payload byte. Failed writes own
    // no published snapshots, so there is no rollback racing a receiver.
    int64_t total=0;
    for(uint64_t i=0;i<msg->msg_iovlen;++i) {
        auto& iov=msg->msg_iov[i];if(!iov.iov_len)continue;
        if(!UserBufferReadable(reinterpret_cast<uint64_t>(iov.iov_base),iov.iov_len)) {
            for(int j=0;j<pendingCount;++j)UnixFdSnapRelease(&pendingSnaps[j]);
            return total?total:-EFAULT;
        }
        auto n=UnixSocketWriteSome(usd,(flagsVal&0x40)||(fde->statusFlags&0x800),
            static_cast<const char*>(iov.iov_base),iov.iov_len,pendingSnaps,pendingCount,(flagsVal&0x4000)!=0,
            hasExplicitCredentials?&explicitCredentials:nullptr);
        if(n>0) {pendingCount=0;total+=n;}
        if(n<0 || static_cast<uint64_t>(n)<iov.iov_len) {
            for(int j=0;j<pendingCount;++j)UnixFdSnapRelease(&pendingSnaps[j]);
            return total?total:n;
        }
    }
    for(int j=0;j<pendingCount;++j)UnixFdSnapRelease(&pendingSnaps[j]);
    return total;
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    if (msg->msg_iovlen == 1) {
        void* buf = msg->msg_iov[0].iov_base;
        uint32_t len = static_cast<uint32_t>(msg->msg_iov[0].iov_len);
        if (!buf || len == 0) return -EINVAL;
        return sys_sendto(fdVal, reinterpret_cast<uint64_t>(buf), len, flagsVal,
                          reinterpret_cast<uint64_t>(msg->msg_name), msg->msg_namelen);
    }

    uint64_t totalLen = 0;
    for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
        if (__builtin_add_overflow(totalLen, msg->msg_iov[i].iov_len, &totalLen))
            return -EINVAL;
    }
    if (totalLen == 0 || totalLen > 65507) return -EINVAL;

    auto* tmp = static_cast<uint8_t*>(kmalloc(totalLen));
    if (!tmp) return -ENOMEM;
    uint64_t off = 0;
    for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
        void* base = msg->msg_iov[i].iov_base;
        uint64_t len = msg->msg_iov[i].iov_len;
        if (!base && len != 0) {
            kfree(tmp);
            return -EFAULT;
        }
        auto* src = static_cast<uint8_t*>(base);
        for (uint64_t j = 0; j < len; j++)
            tmp[off + j] = src[j];
        off += len;
    }

    int64_t ret = sys_sendto(fdVal, reinterpret_cast<uint64_t>(tmp), totalLen, flagsVal,
                             reinterpret_cast<uint64_t>(msg->msg_name), msg->msg_namelen);
    kfree(tmp);
    return ret;
}

static int64_t sys_recvmsg(uint64_t fdVal, uint64_t msgVal, uint64_t flagsVal,
                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    constexpr uint64_t MSG_DONTWAIT = 0x40;
    bool dontwait = (flagsVal & MSG_DONTWAIT) != 0;

    int fd = static_cast<int>(fdVal);
    if (msgVal < 0x1000) return -EFAULT;

    if (!UserBufferWritable(msgVal,sizeof(LinuxMsgHdr))) return -EFAULT;
    auto* msg = reinterpret_cast<LinuxMsgHdr*>(msgVal);
    if (!msg->msg_iovlen || msg->msg_iovlen>1024 || !msg->msg_iov) return -EINVAL;
    if (!UserBufferReadable(reinterpret_cast<uint64_t>(msg->msg_iov),msg->msg_iovlen*sizeof(*msg->msg_iov))) return -EFAULT;

    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (fde && fde->type == FdType::UnixSocket) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);

        // MSG_NOSIGNAL is also accepted on receive by Linux. Receives never
        // generate SIGPIPE; audio IPC uses it with MSG_CMSG_CLOEXEC.
        if(flagsVal&~(0x40ULL|0x4000ULL|0x40000000ULL))return -95;
        if(msg->msg_controllen && !UserBufferWritable(reinterpret_cast<uint64_t>(msg->msg_control),msg->msg_controllen))return -EFAULT;
        const uint64_t capacity=msg->msg_controllen;
        UnixFdSnap descriptors[UNIX_FD_QUEUE_CAP];int received=0;
        int64_t total=0;
        const bool passCredentials = __atomic_load_n(&usd->passCredentials, __ATOMIC_ACQUIRE);
        UnixCredentials sender{};
        for(uint64_t i=0;i<msg->msg_iovlen;++i) {
            auto& iov=msg->msg_iov[i];if(!iov.iov_len)continue;
            if(!UserBufferWritable(reinterpret_cast<uint64_t>(iov.iov_base),iov.iov_len)) {
                for(int j=0;j<received;++j)UnixFdSnapRelease(&descriptors[j]);
                return total?total:-EFAULT;
            }
            int count=0;
            auto n=UnixSocketReadSome(usd,dontwait || total>0 || (fde->statusFlags&0x800),
                static_cast<char*>(iov.iov_base),iov.iov_len,descriptors,count,passCredentials ? &sender : nullptr);
            received=count;
            if(n<0 && !total)return n;
            if(n>0)total+=n;
            if(n<=0 || static_cast<uint64_t>(n)<iov.iov_len || received || passCredentials)break;
        }
        msg->msg_flags=0;msg->msg_controllen=0;
        uint64_t controlUsed = 0;
        if (passCredentials && total > 0) {
            sender.uid=ProcessReportId(proc,false,sender.uid);sender.gid=ProcessReportId(proc,true,sender.gid);
            const uint64_t required = sizeof(CmsgHdr) + sizeof(sender);
            if (capacity >= sizeof(CmsgHdr)) {
                auto* header = static_cast<CmsgHdr*>(msg->msg_control);
                uint64_t copied = capacity < required ? capacity : required;
                header->cmsg_len = copied;
                header->cmsg_level = SOL_SOCKET; header->cmsg_type = 2; // SCM_CREDENTIALS
                if (copied > sizeof(CmsgHdr)) memcpy(header + 1, &sender, copied - sizeof(CmsgHdr));
                controlUsed = CmsgAlign(copied); if (controlUsed > capacity) controlUsed = capacity;
                msg->msg_controllen = controlUsed;
            }
            if (capacity < required) msg->msg_flags |= 0x8;
        }
        const uint64_t remaining = capacity - controlUsed;
        uint64_t maxFds=remaining>=sizeof(CmsgHdr)?(remaining-sizeof(CmsgHdr))/sizeof(int):0;
        int installed=0;
        for(int i=0;i<received;++i) {
            if(static_cast<uint64_t>(installed)<maxFds) {
                descriptors[i].fdFlags=(flagsVal&0x40000000ULL)?1:0;
                int installedFd=UnixFdSnapInstall(proc,&descriptors[i]);
                if(installedFd>=0) {
                    auto* slots=reinterpret_cast<int*>(reinterpret_cast<uint64_t>(msg->msg_control)+controlUsed+sizeof(CmsgHdr));
                    slots[installed++]=installedFd;continue;
                }
            }
            msg->msg_flags|=0x8;UnixFdSnapRelease(&descriptors[i]);
        }
        if(installed) {
            auto* header=reinterpret_cast<CmsgHdr*>(reinterpret_cast<uint64_t>(msg->msg_control)+controlUsed);
            header->cmsg_len=sizeof(CmsgHdr)+installed*sizeof(int);
            header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;
            uint64_t used=CmsgAlign(header->cmsg_len);
            msg->msg_controllen=controlUsed+(used<remaining?used:remaining);
        }
        return total;
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    void* buf = msg->msg_iov[0].iov_base;
    uint32_t len = static_cast<uint32_t>(msg->msg_iov[0].iov_len);
    if (!buf || len == 0) return -EINVAL;

    brook::SockAddrIn srcAddr {};
    int ret = brook::SockRecvFrom(sockIdx, buf, len, &srcAddr);
    if (ret < 0) return -EAGAIN;

    if (msg->msg_name && msg->msg_namelen >= sizeof(brook::SockAddrIn)) {
        auto* dst = reinterpret_cast<brook::SockAddrIn*>(msg->msg_name);
        *dst = srcAddr;
        msg->msg_namelen = sizeof(brook::SockAddrIn);
    }

    msg->msg_flags = 0;
    msg->msg_controllen = 0;
    return ret;
}

static int64_t sys_sendmmsg(uint64_t fdVal, uint64_t msgvecVal, uint64_t vlenVal,
                             uint64_t flagsVal, uint64_t, uint64_t)
{
    if (vlenVal == 0) return 0;
    if (vlenVal > 1024) vlenVal = 1024;
    if (!UserBufferWritable(msgvecVal, vlenVal * sizeof(LinuxMMsgHdr)))
        return -EFAULT;

    auto* vec = reinterpret_cast<LinuxMMsgHdr*>(msgvecVal);
    uint64_t sent = 0;
    for (uint64_t i = 0; i < vlenVal; i++) {
        int64_t ret = sys_sendmsg(fdVal, reinterpret_cast<uint64_t>(&vec[i].msg_hdr),
                                  flagsVal, 0, 0, 0);
        if (ret < 0)
            return sent ? static_cast<int64_t>(sent) : ret;
        vec[i].msg_len = static_cast<uint32_t>(ret);
        sent++;
    }
    return static_cast<int64_t>(sent);
}

// recvmmsg(2) — receive multiple messages in one syscall.
// Signature: int recvmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
//                         int flags, struct timespec *timeout);
// We ignore the timeout parameter for now (treat as non-blocking batch).
static int64_t sys_recvmmsg(uint64_t fdVal, uint64_t msgvecVal, uint64_t vlenVal,
                             uint64_t flagsVal, uint64_t timeoutVal, uint64_t)
{
    (void)timeoutVal; // timeout not implemented
    if (vlenVal == 0) return 0;
    if (vlenVal > 1024) vlenVal = 1024;
    if (!UserBufferWritable(msgvecVal, vlenVal * sizeof(LinuxMMsgHdr)))
        return -EFAULT;

    auto* vec = reinterpret_cast<LinuxMMsgHdr*>(msgvecVal);
    uint64_t received = 0;
    for (uint64_t i = 0; i < vlenVal; i++) {
        // After the first message, use MSG_DONTWAIT to avoid blocking
        uint64_t flags = flagsVal;
        if (i > 0) flags |= 0x40; // MSG_DONTWAIT
        int64_t ret = sys_recvmsg(fdVal, reinterpret_cast<uint64_t>(&vec[i].msg_hdr),
                                  flags, 0, 0, 0);
        if (ret < 0)
            return received ? static_cast<int64_t>(received) : ret;
        vec[i].msg_len = static_cast<uint32_t>(ret);
        received++;
    }
    return static_cast<int64_t>(received);
}

// Lives in its own page (.syscall_table) so we can mark it read-only
// after init. See VmmKernelMarkReadOnly + linker.ld.
static SyscallFn g_syscallTable[SYSCALL_MAX]
    __attribute__((section(".syscall_table"), aligned(4096)));

extern "C" uint8_t __syscall_table_start[];
extern "C" uint8_t __syscall_table_end[];

void SyscallTableInit()
{
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        g_syscallTable[i] = sys_not_implemented;

    g_syscallTable[SYS_READ]            = sys_read;
    g_syscallTable[SYS_WRITE]           = sys_write;
    g_syscallTable[SYS_OPEN]            = sys_open;
    g_syscallTable[SYS_CREAT]           = sys_creat;
    g_syscallTable[SYS_CLOSE]           = sys_close;
    g_syscallTable[SYS_STAT]            = sys_stat;
    g_syscallTable[SYS_FSTAT]           = sys_fstat;
    g_syscallTable[SYS_LSTAT]           = sys_lstat;
    g_syscallTable[SYS_LSEEK]           = sys_lseek;
    g_syscallTable[SYS_MMAP]            = sys_mmap;
    g_syscallTable[SYS_MPROTECT]        = sys_mprotect;
    g_syscallTable[SYS_MUNMAP]          = sys_munmap;
    g_syscallTable[SYS_MREMAP]          = sys_mremap;
    g_syscallTable[SYS_MSYNC]           = sys_msync;
    g_syscallTable[SYS_BRK]             = sys_brk;
    g_syscallTable[SYS_RT_SIGACTION]    = sys_rt_sigaction;
    g_syscallTable[SYS_RT_SIGPROCMASK]  = sys_rt_sigprocmask;
    g_syscallTable[SYS_IOCTL]           = sys_ioctl;
    g_syscallTable[SYS_READV]           = sys_readv;
    g_syscallTable[SYS_WRITEV]          = sys_writev;
    g_syscallTable[SYS_ACCESS]          = sys_access;
    g_syscallTable[SYS_SCHED_YIELD]     = sys_sched_yield;
    g_syscallTable[SYS_NANOSLEEP]       = sys_nanosleep;
    g_syscallTable[SYS_GETPID]          = sys_getpid;
    g_syscallTable[SYS_PIPE]            = sys_pipe;
    g_syscallTable[SYS_DUP]             = sys_dup;
    g_syscallTable[SYS_DUP2]            = sys_dup2;
    g_syscallTable[SYS_CLONE]           = sys_clone;
    g_syscallTable[SYS_FORK]            = sys_fork;
    g_syscallTable[SYS_VFORK]           = sys_vfork;
    g_syscallTable[435]                 = sys_clone3;
    g_syscallTable[SYS_EXECVE]          = sys_execve;
    g_syscallTable[SYS_EXIT]            = sys_exit;
    g_syscallTable[SYS_WAIT4]           = sys_wait4;
    g_syscallTable[SYS_WAITID]          = sys_waitid;
    g_syscallTable[SYS_UNAME]           = sys_uname;
    g_syscallTable[SYS_FCNTL]           = sys_fcntl;
    g_syscallTable[SYS_FSYNC]           = sys_fsync;
    g_syscallTable[SYS_FDATASYNC]       = sys_fsync;
    g_syscallTable[SYS_SYNC]            = sys_sync;
    g_syscallTable[SYS_PIVOT_ROOT]      = sys_pivot_root;
    g_syscallTable[SYS_MOUNT]           = sys_mount;
    g_syscallTable[SYS_UMOUNT2]         = sys_umount2;
    g_syscallTable[SYS_GETCWD]          = sys_getcwd;
    g_syscallTable[SYS_GETTIMEOFDAY]    = sys_gettimeofday;
    g_syscallTable[SYS_TIME]             = sys_time;
    g_syscallTable[SYS_GETUID]          = sys_getuid;
    g_syscallTable[SYS_GETGID]          = sys_getgid;
    g_syscallTable[SYS_SETUID]          = sys_setuid;
    g_syscallTable[SYS_SETGID]          = sys_setgid;
    g_syscallTable[113] = sys_setreuid;
    g_syscallTable[114] = sys_setregid;
    g_syscallTable[117] = sys_setresuid;
    g_syscallTable[119] = sys_setresgid;
    g_syscallTable[SYS_CAPGET]          = sys_capget;
    g_syscallTable[SYS_CAPSET]          = sys_capset;
    // inotify: deliberately unimplemented. Glibc/GLib fall back to
    // polling/manual checks when inotify_init fails. Returning ENOSYS
    // quietly avoids UNIMPL serial spam from GIMP / file pickers.
    g_syscallTable[253]                  = sys_enosys_quiet; // inotify_init
    g_syscallTable[254]                  = sys_enosys_quiet; // inotify_add_watch
    g_syscallTable[255]                  = sys_enosys_quiet; // inotify_rm_watch
    g_syscallTable[294]                  = sys_enosys_quiet; // inotify_init1
    g_syscallTable[SYS_GETEUID]         = sys_geteuid;
    g_syscallTable[SYS_GETEGID]         = sys_getegid;
    g_syscallTable[SYS_GETPPID]         = sys_getppid;
    g_syscallTable[SYS_GETGROUPS]       = sys_getgroups;
    g_syscallTable[SYS_SETGROUPS]       = sys_setgroups;
    g_syscallTable[SYS_ARCH_PRCTL]      = sys_arch_prctl;
    g_syscallTable[SYS_GETDENTS64]      = sys_getdents64;
    g_syscallTable[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    g_syscallTable[SYS_CLOCK_GETTIME]   = sys_clock_gettime;
    g_syscallTable[SYS_CLOCK_GETRES]    = sys_clock_getres;
    g_syscallTable[SYS_CLOCK_NANOSLEEP] = sys_clock_nanosleep;
    g_syscallTable[SYS_EXIT_GROUP]      = sys_exit_group;
    g_syscallTable[SYS_OPENAT]          = sys_openat;
    g_syscallTable[SYS_NEWFSTATAT]      = sys_newfstatat;
    g_syscallTable[SYS_PRLIMIT64]       = sys_prlimit64;
    g_syscallTable[SYS_GETRANDOM]       = sys_getrandom;

    // New syscalls
    g_syscallTable[SYS_POLL]            = sys_poll;
    g_syscallTable[SYS_RT_SIGRETURN]    = sys_rt_sigreturn;
    g_syscallTable[SYS_SELECT]          = sys_select;
    g_syscallTable[SYS_PSELECT6]        = sys_pselect6;
    g_syscallTable[SYS_SENDFILE]        = sys_sendfile;
    g_syscallTable[SYS_KILL]            = sys_kill;
    g_syscallTable[SYS_CHDIR]           = sys_chdir;
    g_syscallTable[SYS_FCHDIR]          = sys_fchdir;
    g_syscallTable[SYS_RENAME]          = sys_rename;
    g_syscallTable[SYS_MKNOD] = sys_mknod;
    g_syscallTable[SYS_MKNODAT] = sys_mknodat;
    g_syscallTable[SYS_MKDIR]           = sys_mkdir;
    g_syscallTable[SYS_RMDIR]           = sys_rmdir;
    g_syscallTable[SYS_UNLINK]          = sys_unlink;
    g_syscallTable[SYS_SYMLINK]         = sys_symlink;
    g_syscallTable[SYS_READLINK]        = sys_readlink;
    g_syscallTable[SYS_UMASK]           = sys_umask;
    g_syscallTable[SYS_GETRLIMIT]       = sys_getrlimit;
    g_syscallTable[SYS_GETRUSAGE]       = sys_getrusage;
    g_syscallTable[SYS_SYSINFO]         = sys_sysinfo;
    g_syscallTable[SYS_TIMES]           = sys_times;
    g_syscallTable[SYS_SETPGID]         = sys_setpgid;
    g_syscallTable[SYS_GETPGRP]         = sys_getpgrp;
    g_syscallTable[SYS_SETSID]          = sys_setsid;
    g_syscallTable[SYS_GETPGID]         = sys_getpgid;
    g_syscallTable[SYS_GETSID]          = sys_getsid;
    g_syscallTable[SYS_SIGALTSTACK]     = sys_sigaltstack;
    g_syscallTable[SYS_ALARM]           = sys_alarm;
    g_syscallTable[SYS_PAUSE]           = sys_pause;
    g_syscallTable[SYS_RT_SIGPENDING] = sys_rt_sigpending;
    g_syscallTable[SYS_RT_SIGTIMEDWAIT] = sys_rt_sigtimedwait;
    g_syscallTable[SYS_RT_SIGSUSPEND]   = sys_rt_sigsuspend;
    g_syscallTable[SYS_STATFS]          = sys_statfs;
    g_syscallTable[SYS_FSTATFS]         = sys_fstatfs;
    g_syscallTable[SYS_GETTID]          = sys_gettid;
    g_syscallTable[SYS_TKILL]           = sys_tkill;
    g_syscallTable[SYS_TGKILL]          = sys_tgkill;
    g_syscallTable[SYS_READLINKAT]      = sys_readlinkat;
    g_syscallTable[SYS_SYMLINKAT]       = sys_symlinkat;
    g_syscallTable[SYS_PPOLL]           = sys_ppoll;
    g_syscallTable[SYS_PIPE2]           = sys_pipe2;
    g_syscallTable[SYS_EVENTFD2]        = sys_eventfd2;
    g_syscallTable[SYS_DUP3]            = sys_dup3;

    // Bash / POSIX compatibility
    g_syscallTable[SYS_PREAD64]         = sys_pread64;
    g_syscallTable[SYS_GETRESUID]       = sys_getresuid;
    g_syscallTable[SYS_GETRESGID]       = sys_getresgid;
    g_syscallTable[SYS_PRCTL]           = sys_prctl;
    g_syscallTable[SYS_FUTEX]           = sys_futex;
    g_syscallTable[SYS_SIGNALFD]        = sys_signalfd;
    g_syscallTable[SYS_SIGNALFD4]       = sys_signalfd4;
    g_syscallTable[SYS_SEMGET]          = sys_semget;
    g_syscallTable[SYS_SEMOP]           = sys_semop;
    g_syscallTable[SYS_SEMTIMEDOP]      = sys_semtimedop;
    g_syscallTable[SYS_SEMCTL]          = sys_semctl;
    g_syscallTable[SYS_SET_ROBUST_LIST] = sys_set_robust_list;
    g_syscallTable[SYS_GET_ROBUST_LIST] = sys_get_robust_list;
    g_syscallTable[SYS_FACCESSAT]       = sys_faccessat;
    g_syscallTable[SYS_RSEQ]            = sys_rseq;
    g_syscallTable[SYS_CLOSE_RANGE]     = sys_close_range;
    g_syscallTable[SYS_FACCESSAT2]      = sys_faccessat2;

    // Nix package manager syscalls
    g_syscallTable[SYS_FLOCK]           = sys_flock;
    g_syscallTable[SYS_TRUNCATE]        = sys_truncate;
    g_syscallTable[SYS_FTRUNCATE]       = sys_ftruncate;
    g_syscallTable[SYS_LINK]            = sys_link;
    g_syscallTable[SYS_CHMOD]           = sys_chmod;
    g_syscallTable[SYS_FCHMOD]          = sys_fchmod;
    g_syscallTable[SYS_CHOWN]           = sys_chown;
    g_syscallTable[SYS_FCHOWN]          = sys_fchown;
    g_syscallTable[SYS_LCHOWN]          = sys_lchown;
    g_syscallTable[SYS_SCHED_SETPARAM]  = sys_sched_setparam;
    g_syscallTable[SYS_SCHED_GETPARAM]  = sys_sched_getparam;
    g_syscallTable[SYS_SCHED_SETAFFINITY] = sys_sched_setaffinity;
    g_syscallTable[SYS_SCHED_GETAFFINITY] = sys_sched_getaffinity;
    g_syscallTable[SYS_SCHED_SETSCHEDULER] = sys_sched_setscheduler;
    g_syscallTable[SYS_SCHED_GETSCHEDULER] = sys_sched_getscheduler;
    g_syscallTable[SYS_SCHED_GET_PRIORITY_MAX] = sys_sched_get_priority_max;
    g_syscallTable[SYS_SCHED_GET_PRIORITY_MIN] = sys_sched_get_priority_min;
    g_syscallTable[SYS_MKDIRAT]         = sys_mkdirat;
    g_syscallTable[SYS_FCHOWNAT]        = sys_fchownat;
    g_syscallTable[SYS_UNLINKAT]        = sys_unlinkat;
    g_syscallTable[SYS_RENAMEAT]        = sys_renameat;
    g_syscallTable[SYS_LINKAT]          = sys_linkat;
    g_syscallTable[SYS_FCHMODAT]        = sys_fchmodat;
    g_syscallTable[SYS_UTIMENSAT]       = sys_utimensat;
    g_syscallTable[SYS_FALLOCATE]       = sys_fallocate;
    g_syscallTable[SYS_RENAMEAT2]       = sys_renameat2;
    g_syscallTable[SYS_STATX]           = sys_statx;
    g_syscallTable[SYS_MINCORE]         = sys_mincore;
    g_syscallTable[SYS_MADVISE]         = sys_madvise;
    g_syscallTable[SYS_SHMGET]          = sys_enosys_quiet;
    g_syscallTable[SYS_GETPRIORITY]     = sys_getpriority;
    g_syscallTable[SYS_SETPRIORITY]     = sys_setpriority;
    g_syscallTable[SYS_PIDFD_OPEN]      = sys_enosys_quiet;
    g_syscallTable[SYS_FADVISE64]       = sys_fadvise64;
    g_syscallTable[SYS_MBIND]           = sys_mbind;
    g_syscallTable[SYS_SET_MEMPOLICY]   = sys_enosys_quiet;
    g_syscallTable[SYS_GET_MEMPOLICY]   = sys_enosys_quiet;
    g_syscallTable[SYS_PWRITE64]        = sys_pwrite64;

    // Socket syscalls
    g_syscallTable[SYS_SOCKET]          = sys_socket;
    g_syscallTable[SYS_CONNECT]         = sys_connect;
    g_syscallTable[SYS_ACCEPT]          = sys_accept;
    g_syscallTable[SYS_ACCEPT4]         = sys_accept4;
    g_syscallTable[SYS_SENDTO]          = sys_sendto;
    g_syscallTable[SYS_RECVFROM]        = sys_recvfrom;
    g_syscallTable[SYS_SENDMSG]         = sys_sendmsg;
    g_syscallTable[SYS_RECVMSG]         = sys_recvmsg;
    g_syscallTable[SYS_SENDMMSG]        = sys_sendmmsg;
    g_syscallTable[SYS_RECVMMSG]        = sys_recvmmsg;
    g_syscallTable[SYS_SHUTDOWN]        = sys_shutdown;
    g_syscallTable[SYS_BIND]            = sys_bind;
    g_syscallTable[SYS_LISTEN]          = sys_listen;
    g_syscallTable[SYS_GETSOCKNAME]     = sys_getsockname;
    g_syscallTable[SYS_GETPEERNAME]     = sys_getpeername;
    g_syscallTable[SYS_SOCKETPAIR]      = sys_socketpair;
    g_syscallTable[SYS_SETSOCKOPT]      = sys_setsockopt;
    g_syscallTable[SYS_GETSOCKOPT]      = sys_getsockopt;

    // Wayland prerequisites
    g_syscallTable[SYS_EPOLL_CREATE]    = sys_epoll_create;
    g_syscallTable[SYS_EPOLL_CREATE1]   = sys_epoll_create1;
    g_syscallTable[SYS_EPOLL_CTL]       = sys_epoll_ctl;
    g_syscallTable[SYS_EPOLL_WAIT]      = sys_epoll_wait;
    g_syscallTable[SYS_EPOLL_PWAIT]     = sys_epoll_pwait;
    g_syscallTable[SYS_TIMER_CREATE] = sys_timer_create;
    g_syscallTable[SYS_TIMER_SETTIME] = sys_timer_settime;
    g_syscallTable[SYS_TIMER_GETTIME] = sys_timer_gettime;
    g_syscallTable[SYS_TIMER_GETOVERRUN] = sys_timer_getoverrun;
    g_syscallTable[SYS_TIMER_DELETE] = sys_timer_delete;
    g_syscallTable[SYS_TIMERFD_CREATE]  = sys_timerfd_create;
    g_syscallTable[SYS_TIMERFD_SETTIME] = sys_timerfd_settime;
    g_syscallTable[SYS_TIMERFD_GETTIME] = sys_timerfd_gettime;
    g_syscallTable[SYS_MEMFD_CREATE]    = sys_memfd_create;

    // Brook-specific syscalls (500+). 500 = profiler control.
    g_syscallTable[500]                  = sys_brook_profile;
    g_syscallTable[502]                  = sys_brook_set_crash_entry;
    g_syscallTable[503]                  = sys_brook_crash_complete;
    g_syscallTable[504]                  = sys_brook_input_pop;
    g_syscallTable[505]                  = sys_brook_input_grab;
    g_syscallTable[506]                  = sys_brook_wm_create_window;
    g_syscallTable[507]                  = sys_brook_wm_destroy_window;
    g_syscallTable[508]                  = sys_brook_wm_signal_dirty;
    g_syscallTable[509]                  = sys_brook_wm_set_title;
    g_syscallTable[510]                  = sys_brook_wm_pop_input;
    g_syscallTable[511]                  = sys_brook_wm_resize_vfb;
    g_syscallTable[512]                  = sys_brook_wm_set_decoration_mode;
    g_syscallTable[513]                  = sys_brook_wm_begin_move;
    g_syscallTable[514]                  = sys_brook_wm_move_relative;
    g_syscallTable[515]                  = sys_brook_wm_set_maximized;
    g_syscallTable[516]                  = sys_brook_wm_set_minimized;
    g_syscallTable[517]                  = sys_brook_wm_begin_resize;
    g_syscallTable[518]                  = sys_brook_wm_set_cursor_visible;
    g_syscallTable[519]                  = sys_brook_wm_set_cursor_image;
    g_syscallTable[520]                  = sys_brook_wm_get_screen_info;
    g_syscallTable[521]                  = sys_brook_wm_scanout_map;
    g_syscallTable[522]                  = sys_brook_wm_scanout_flip;
    g_syscallTable[523]                  = sys_brook_wm_get_windows;
    g_syscallTable[524]                  = sys_brook_wm_map_window_vfb;
    g_syscallTable[525]                  = sys_brook_wm_present_gres;
    // Brook windowing extensions live in a high 0xB00 block, clear of the Linux
    // and Windows syscall ranges.
    g_syscallTable[0xB00]                = sys_brook_wm_set_window_properties; // WM_SET_WINDOW_PROPERTIES
    g_syscallTable[0xB01]                = sys_brook_wm_set_app_id;
    g_syscallTable[0xB05]                = sys_brook_wm_set_client_socket;
    g_syscallTable[0xB06]                = sys_brook_scene_create;
    g_syscallTable[0xB07]                = sys_brook_scene_ctl;
    g_syscallTable[0xB02]                = sys_brook_wm_theme;
    g_syscallTable[0xB03]                = sys_brook_wm_refresh_apps;
    g_syscallTable[0xB04]                = sys_brook_wm_scale;
#ifdef BROOK_MAPPING_TEST_HOOKS
    g_syscallTable[0xB30]                = MappingFaultTestControl;
#endif
#ifdef BROOK_REGISTER_TEST_HOOKS
    g_syscallTable[0xB31]                = RegisterTestControl;
#endif
    // App-GPU (per-process virgl) syscalls for userspace GL rendering.
    g_syscallTable[0xB10]                = sys_brook_gpu_ctx_create;       // GPU_CTX_CREATE
    g_syscallTable[0xB11]                = sys_brook_gpu_resource_create;  // GPU_RESOURCE_CREATE
    g_syscallTable[0xB12]                = sys_brook_gpu_attach_window;    // GPU_ATTACH_WINDOW
    g_syscallTable[0xB16]                = sys_brook_gpu_upload_buffer;    // GPU_UPLOAD_BUFFER
    g_syscallTable[0xB13]                = sys_brook_gpu_submit;           // GPU_SUBMIT
    g_syscallTable[0xB14]                = sys_brook_gpu_transfer;         // GPU_TRANSFER
    g_syscallTable[0xB15]                = sys_brook_gpu_ctx_destroy;      // GPU_CTX_DESTROY

    uint32_t count = 0;
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        if (g_syscallTable[i] != sys_not_implemented) ++count;

    SerialPrintf("SYSCALL: table initialised (%u entries, %u implemented)\n",
                 static_cast<unsigned>(SYSCALL_MAX), count);

    // Lock the dispatch table read-only. Any subsequent write attempt
    // (corruption, stray pointer) will #PF immediately on the offending
    // RIP rather than silently leaving a null entry to be discovered
    // later by an indirect call. The table is page-aligned and may span
    // multiple pages as Brook-specific syscall ranges grow.
    uint64_t lockStart = reinterpret_cast<uint64_t>(__syscall_table_start);
    uint64_t lockEnd   = reinterpret_cast<uint64_t>(__syscall_table_end);
    if (VmmKernelMarkReadOnly(VirtualAddress(lockStart), lockEnd - lockStart))
        SerialPrintf("SYSCALL: dispatch table locked RO at 0x%lx (%lu bytes)\n",
                     lockStart, lockEnd - lockStart);
    else
        SerialPrintf("SYSCALL: WARNING failed to lock table RO\n");
}

SyscallFn* SyscallGetTable()
{
    return g_syscallTable;
}

uint64_t SyscallGetTableAddress()
{
    return reinterpret_cast<uint64_t>(g_syscallTable);
}

// ---------------------------------------------------------------------------
// Entry point address (for LSTAR MSR)
// ---------------------------------------------------------------------------

uint64_t SyscallGetEntryPoint()
{
    return reinterpret_cast<uint64_t>(&BrookSyscallDispatcher);
}

// ---------------------------------------------------------------------------
// Strace — syscall tracing facility
// ---------------------------------------------------------------------------

// Category bitmask for strace filtering
enum StraceCategory : uint32_t {
    SC_NET    = 1 << 0,  // socket, connect, sendto, recvfrom, etc.
    SC_FILE   = 1 << 1,  // open, read, write, close, stat, etc.
    SC_MEM    = 1 << 2,  // mmap, munmap, mprotect, brk
    SC_PROC   = 1 << 3,  // fork, clone, execve, exit, wait
    SC_SIG    = 1 << 4,  // rt_sigaction, rt_sigprocmask, kill, etc.
    SC_IO     = 1 << 5,  // ioctl, poll, pselect, ppoll, epoll
    SC_OTHER  = 1 << 6,  // everything else
};

static uint32_t SyscallCategory(uint64_t num)
{
    switch (num) {
    // Network
    case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48:
    case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    case 299: case 307:
        return SC_NET;  // socket..shutdown, recvmmsg, sendmmsg
    // File
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 8:
    case 17: case 18: case 19: case 20: case 21:
    case 32: case 33: case 72: case 73: case 76: case 77:
    case 79: case 80: case 82: case 83: case 84: case 85: case 86: case 87: case 88:
    case 89: case 90: case 91: case 92: case 93: case 94:
    case 217: case 257: case 258: case 260: case 262: case 263: case 264:
    case 265: case 266: case 267: case 268: case 280: case 285:
    case 292: case 293: case 316: case 332: case 439:
        return SC_FILE;
    // Memory
    case 9: case 10: case 11: case 12: case 25: case 26: case 27: case 28:
        return SC_MEM;  // mmap..mremap, madvise
    // Process
    case 56: case 57: case 58: case 59: case 60: case 61: case 62:
    case 231: case 234: case 247: case 435:
        return SC_PROC; // clone..wait4, exit_group, tgkill, clone3
    // Signal
    case 13: case 14: case 15: case 34: case 127: case 128: case 129: case 130: case 131:
    case 200:
        return SC_SIG;  // rt_sigaction..rt_sigreturn, kill, tkill
    // I/O control
    case 7: case 16: case 23: case 270: case 271:
    case 213: case 214: case 232: case 233:
    case 290:
        return SC_IO;   // poll, ioctl, select, pselect6, ppoll, epoll_*
    default:
        return SC_OTHER;
    }
}

static const char* SyscallName(uint64_t num)
{
    switch (num) {
    case 0: return "read";        case 1: return "write";
    case 2: return "open";        case 3: return "close";
    case 4: return "stat";        case 5: return "fstat";
    case 6: return "lstat";       case 7: return "poll";
    case 8: return "lseek";       case 9: return "mmap";
    case 10: return "mprotect";   case 11: return "munmap";
    case 12: return "brk";        case 13: return "rt_sigaction";
    case 14: return "rt_sigprocmask"; case 15: return "rt_sigreturn";
    case 16: return "ioctl";      case 17: return "pread64";
    case 19: return "readv";      case 20: return "writev";
    case 21: return "access";     case 22: return "pipe";
    case 23: return "select";     case 24: return "sched_yield";
    case 25: return "mremap";
    case 26: return "msync";
    case 32: return "dup";        case 33: return "dup2";
    case 35: return "nanosleep";  case 39: return "getpid";
    case 40: return "sendfile";   case 41: return "socket";
    case 42: return "connect";    case 43: return "accept";
    case 44: return "sendto";     case 45: return "recvfrom";
    case 46: return "sendmsg";    case 47: return "recvmsg";
    case 48: return "shutdown";   case 49: return "bind";
    case 50: return "listen";     case 51: return "getsockname";
    case 52: return "getpeername"; case 53: return "socketpair";
    case 54: return "setsockopt"; case 55: return "getsockopt";
    case 56: return "clone";
    case 57: return "fork";       case 58: return "vfork";
    case 59: return "execve";     case 60: return "exit";
    case 247: return "waitid";
    case 61: return "wait4";      case 62: return "kill";
    case 63: return "uname";      case 72: return "fcntl";
    case 74: return "fsync";      case 75: return "fdatasync";
    case 79: return "getcwd";     case 80: return "chdir";
    case 81: return "fchdir";     case 82: return "rename";
    case 83: return "mkdir";      case 84: return "rmdir";      case 87: return "unlink";
    case 85: return "creat";       case 88: return "symlink";    case 89: return "readlink";   case 95: return "umask";
    case 96: return "gettimeofday"; case 97: return "getrlimit";
    case 98: return "getrusage";  case 99: return "sysinfo";
    case 102: return "getuid";    case 104: return "getgid";
    case 105: return "setuid";    case 106: return "setgid";
    case 107: return "geteuid";   case 108: return "getegid";
    case 109: return "setpgid";   case 110: return "getppid";
    case 111: return "getpgrp";   case 112: return "setsid";
    case 115: return "getgroups"; case 116: return "setgroups";
    case 113: return "setreuid"; case 114: return "setregid";
    case 117: return "setresuid"; case 118: return "getresuid";
    case 119: return "setresgid"; case 120: return "getresgid";
    case 121: return "getpgid";   case 124: return "getsid";
    case 222: return "timer_create"; case 223: return "timer_settime";
    case 224: return "timer_gettime"; case 225: return "timer_getoverrun";
    case 226: return "timer_delete";
    case 127: return "rt_sigpending";
    case 128: return "rt_sigtimedwait"; case 131: return "sigaltstack"; case 130: return "rt_sigsuspend";
    case 134: return "statfs";
    case 34: return "pause"; case 37: return "alarm";
    case 135: return "fstatfs";   case 157: return "prctl";
    case 158: return "arch_prctl"; case 186: return "gettid";
    case 200: return "tkill";     case 201: return "time";       case 217: return "getdents64";
    case 218: return "set_tid_address"; case 228: return "clock_gettime";
    case 230: return "clock_nanosleep"; case 231: return "exit_group";
    case 234: return "tgkill";    case 257: return "openat";
    case 262: return "newfstatat"; case 266: return "symlinkat"; case 267: return "readlinkat";
    case 270: return "pselect6";  case 271: return "ppoll";     case 273: return "set_robust_list"; case 274: return "get_robust_list";
    case 292: return "dup3";
    case 290: return "eventfd2";  case 293: return "pipe2";     case 302: return "prlimit64";
    case 299: return "recvmmsg";  case 307: return "sendmmsg";
    case 318: return "getrandom"; case 334: return "rseq";
    case 439: return "faccessat2";
    case 73: return "flock";      case 76: return "truncate";
    case 77: return "ftruncate";  case 86: return "link";
    case 90: return "chmod";      case 91: return "fchmod";
    case 92: return "chown";      case 93: return "fchown";
    case 94: return "lchown";
    case 142: return "sched_setparam"; case 143: return "sched_getparam";
    case 144: return "sched_setscheduler"; case 145: return "sched_getscheduler";
    case 146: return "sched_get_priority_max"; case 147: return "sched_get_priority_min";
    case 203: return "sched_setaffinity"; case 204: return "sched_getaffinity";
    case 258: return "mkdirat";   case 260: return "fchownat";
    case 263: return "unlinkat";  case 264: return "renameat";
    case 265: return "linkat";    case 268: return "fchmodat";
    case 280: return "utimensat"; case 285: return "fallocate";
    case 316: return "renameat2"; case 332: return "statx";
    case 435: return "clone3";
    case 18: return "pwrite64"; case 27: return "mincore";
    case 28: return "madvise";
    default: return nullptr;
    }
}

static int64_t SyscallDispatchTraced(uint64_t num, uint64_t a0, uint64_t a1,
                                      uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    Process* proc = ProcessCurrent();
    const char* name = SyscallName(num);

    // Rate-limit: suppress repeated identical error syscalls to prevent
    // stack/serial flood (e.g. writes to a closed fd in a tight loop).
    static uint64_t s_lastNum   = ~0ULL;
    static uint64_t s_lastA0    = 0;
    static int64_t  s_lastRet   = 0;
    static uint32_t s_suppressed = 0;
    static constexpr uint32_t SUPPRESS_THRESHOLD = 3;

    SyscallFn fn = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
    if (!fn) {
        SerialPrintf("[syscall] FATAL-AVOIDED: pid=%u (%s) num=%lu has null/oob entry "
                     "(a0=%lx a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx) -> -ENOSYS\n",
                     proc ? proc->pid : 0, proc ? proc->name : "?",
                     num, a0, a1, a2, a3, a4, a5);
        return -38; // -ENOSYS
    }

    int64_t ret = fn(a0, a1, a2, a3, a4, a5);

    // In errors-only mode, skip successful syscalls entirely
    bool isError = (ret < 0 && ret > -4096);
    if (proc->straceErrorsOnly && !isError) {
        // Always log blocking syscalls from the main thread for debugging
        if (proc->pid == proc->tgid &&
            (num == 202 /*futex*/ || num == 232 /*epoll_wait*/ ||
             num == 271 /*ppoll*/ || num == 7 /*poll*/ ||
             num == 231 /*exit_group*/ || num == 60 /*exit*/))
        {
            SerialPrintf("[strace-main:%u] %s(%lx,%lx,%lx) = %ld\n",
                         proc->pid, name ? name : "?", a0, a1, a2, ret);
        }
        return ret;
    }
    bool isRepeat = (num == s_lastNum && a0 == s_lastA0 && ret == s_lastRet && isError);

    if (isRepeat) {
        s_suppressed++;
        if (s_suppressed == SUPPRESS_THRESHOLD)
            SerialPrintf("[strace:%u] ... suppressing repeated %s(%lu) = -%lu\n",
                         proc->pid, name ? name : "?", a0, -ret);
        if (s_suppressed >= SUPPRESS_THRESHOLD)
            return ret;
    } else {
        if (s_suppressed > SUPPRESS_THRESHOLD)
            SerialPrintf("[strace:%u] ... (%u identical errors suppressed)\n",
                         proc->pid, s_suppressed - SUPPRESS_THRESHOLD);
        s_suppressed = 0;
    }

    s_lastNum = num;
    s_lastA0  = a0;
    s_lastRet = ret;

    // Log entry — format depends on syscall type for readability
    if (num == SYS_OPEN || num == SYS_OPENAT) {
        const char* path = (num == SYS_OPENAT)
            ? reinterpret_cast<const char*>(a1)
            : reinterpret_cast<const char*>(a0);
        uint64_t flags = (num == SYS_OPENAT) ? a2 : a1;
        SerialPrintf("[strace:%u] %s(\"%s\", 0x%lx)",
                     proc->pid, name ? name : "?", path ? path : "(null)", flags);
    } else if (num == SYS_EXECVE) {
        const char* path = reinterpret_cast<const char*>(a0);
        SerialPrintf("[strace:%u] execve(\"%s\")", proc->pid, path ? path : "(null)");
    } else if (num == SYS_READ || num == SYS_WRITE) {
        SerialPrintf("[strace:%u] %s(%lu, ..., %lu)",
                     proc->pid, name ? name : "?", a0, a2);
    } else if (num == SYS_CLOSE || num == SYS_DUP || num == SYS_FSTAT) {
        SerialPrintf("[strace:%u] %s(%lu)",
                     proc->pid, name ? name : "?", a0);
    } else if (num == SYS_DUP2 || num == SYS_DUP3) {
        SerialPrintf("[strace:%u] %s(%lu, %lu)",
                     proc->pid, name ? name : "?", a0, a1);
    } else if (num == SYS_MMAP) {
        SerialPrintf("[strace:%u] mmap(0x%lx, %lu, 0x%lx, 0x%lx, %ld, 0x%lx)",
                     proc->pid, a0, a1, a2, a3, (int64_t)a4, a5);
    } else if (num == SYS_WAITID) {
        SerialPrintf("[strace:%u] waitid(%lu, %lu, 0x%lx, 0x%lx, 0x%lx)",
                     proc->pid, a0, a1, a2, a3, a4);
    } else if (num == SYS_LSEEK) {
        SerialPrintf("[strace:%u] lseek(%lu, %ld, %lu)",
                     proc->pid, a0, (int64_t)a1, a2);
    } else if (num == SYS_FCNTL) {
        SerialPrintf("[strace:%u] fcntl(%lu, %lu, 0x%lx)",
                     proc->pid, a0, a1, a2);
    } else if (name) {
        SerialPrintf("[strace:%u] %s(0x%lx, 0x%lx, 0x%lx)",
                     proc->pid, name, a0, a1, a2);
    } else {
        SerialPrintf("[strace:%u] syscall_%lu(0x%lx, 0x%lx, 0x%lx)",
                     proc->pid, num, a0, a1, a2);
    }

    // Log return value
    if (ret < 0 && ret > -4096)
        SerialPrintf(" = -%lu (err)\n", -ret);
    else if (num == SYS_MMAP || num == SYS_BRK)
        SerialPrintf(" = 0x%lx\n", static_cast<uint64_t>(ret));
    else
        SerialPrintf(" = %ld\n", ret);

    return ret;
}

int64_t SyscallDispatchInternal(uint64_t num, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    Process* proc = ProcessCurrent();
    SyscallTimingCall timing(proc?proc->pid:0,proc?proc->incarnation:0,num,a0);

    if (proc && proc->straceEnabled)
    {
        // If a filter is set, only trace matching categories
        if (proc->straceFilter == 0 || (SyscallCategory(num) & proc->straceFilter))
            return timing.Result(SyscallDispatchTraced(num, a0, a1, a2, a3, a4, a5));
        // Filtered out — dispatch without tracing
        SyscallFn fn2 = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
        if (!fn2) return timing.Result(-38);
        return timing.Result(fn2(a0, a1, a2, a3, a4, a5));
    }
    SyscallFn fn = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
    if (!fn) {
        SerialPrintf("[syscall] FATAL-AVOIDED: pid=%u (%s) num=%lu has null/oob entry "
                     "(a0=%lx a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx) -> -ENOSYS\n",
                     proc ? proc->pid : 0, proc ? proc->name : "?",
                     num, a0, a1, a2, a3, a4, a5);
        return timing.Result(-38); // -ENOSYS
    }

    // RequestServer uses the same explicitly enabled, filtered strace path
    // as every other process; no process-name-specific logging here.
    return timing.Result(fn(a0, a1, a2, a3, a4, a5));
}

// ---------------------------------------------------------------------------
// SwitchToUserMode -- enter ring 3 via IRETQ (naked)
// ---------------------------------------------------------------------------

__attribute__((naked)) void SwitchToUserMode(uint64_t, uint64_t)
{
    __asm__ volatile(
        "push %%rax\n\t"
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "pushfq\n\t"
        "mov %%rbp, %%gs:24\n\t"
        "mov %%rsp, %%gs:32\n\t"
        "cld\n\t"
        "pushq $0x23\n\t"
        "push %%rdi\n\t"
        "pushq $0x202\n\t"
        "pushq $0x2B\n\t"
        "push %%rsi\n\t"
        "xor %%rax, %%rax\n\t"
        "xor %%rbx, %%rbx\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rsi, %%rsi\n\t"
        "xor %%rdi, %%rdi\n\t"
        "xor %%r8, %%r8\n\t"
        "xor %%r9, %%r9\n\t"
        "xor %%r10, %%r10\n\t"
        "xor %%r11, %%r11\n\t"
        "xor %%r12, %%r12\n\t"
        "xor %%r13, %%r13\n\t"
        "xor %%r14, %%r14\n\t"
        "xor %%r15, %%r15\n\t"
        "xor %%rbp, %%rbp\n\t"
        // BRO-166: clear IF before swapgs so a LAPIC timer (or any maskable
        // IRQ) cannot fire in the ring-0 window between swapgs and iretq. In
        // that window active GS is already the user base (0); a timer landing
        // here pushes a ring-0 CS, so the ISR entry stub's conditional
        // `testq $3,CS; swapgs` would SKIP swapgs and run the handler with
        // GS=0 → #PF at gs:176 (CR2=0xB0). The hand-built frame's RFLAGS=0x202
        // re-enables IF atomically on iretq to ring 3, so this only closes the
        // kernel-side window with no change to the delivered user context.
        "cli\n\t"
        "swapgs\n\t"
        "iretq\n\t"
        ::: "memory"
    );
}

} // namespace brook

// ---------------------------------------------------------------------------
// Strace control functions
// ---------------------------------------------------------------------------

bool StraceEnablePid(uint32_t pid, bool enable, uint32_t filter)
{
    using namespace brook;
    Process* p = ProcessFindByPid(static_cast<uint16_t>(pid));
    if (!p) return false;
    p->straceEnabled = enable;
    p->straceFilter = filter;
    return true;
}

int StraceEnableName(const char* name, bool enable, uint32_t filter)
{
    using namespace brook;
    int count = 0;
    for (uint16_t pid = 1; pid < 256; pid++) {
        Process* p = ProcessFindByPid(pid);
        if (p && p->name[0]) {
            bool match = false;
            for (const char* s = p->name; *s; s++) {
                const char* a = s;
                const char* b = name;
                while (*a && *b && *a == *b) { a++; b++; }
                if (!*b) { match = true; break; }
            }
            if (match) {
                p->straceEnabled = enable;
                p->straceFilter = filter;
                count++;
            }
        }
    }
    return count;
}

void StraceEnableAll(bool enable, uint32_t filter)
{
    using namespace brook;
    for (uint16_t pid = 1; pid < 256; pid++) {
        Process* p = ProcessFindByPid(pid);
        if (p) {
            p->straceEnabled = enable;
            p->straceFilter = filter;
        }
    }
}

// ---------------------------------------------------------------------------
// ReturnToKernel
// ---------------------------------------------------------------------------

extern "C" __attribute__((naked)) void ReturnToKernel()
{
    __asm__ volatile(
        "mov %%gs:24, %%rbp\n\t"
        "mov %%gs:32, %%rsp\n\t"
        // BRO-207: normalize the restored kernel RFLAGS (clear NT|DF|AC) just
        // like context_switch — a resumed kernel continuation must not carry a
        // stale NT (which would #GP the next naked-handler IRETQ) or DF/AC.
        "andq $0xfffffffffffbbbff, (%%rsp)\n\t"
        "popfq\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rax\n\t"
        "ret\n\t"
        ::: "memory"
    );
}
