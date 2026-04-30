// syscall.cpp -- SYSCALL/SYSRET dispatcher, syscall table, and user-mode entry.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "syscall.h"
#include "cpu.h"
#include "gdt.h"
#include "serial.h"
#include "panic.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"
#include "string.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "vfs.h"
#include "tty.h"
#include "input.h"
#include "pipe.h"
#include "memory/heap.h"
#include "compositor.h"
#include "window.h"
#include "terminal.h"
#include "net.h"
#include "rtc.h"
#include "kvmclock.h"
#include "audio.h"
#include "profiler.h"

// Forward declaration
extern "C" __attribute__((naked)) void ReturnToKernel();

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
        "cmp $512, %%rax\n\t"
        "jae .Lsyscall_invalid\n\t"
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
        "mov -88(%%rbp), %%r15\n\t"      // restore R15 from saved slot
        "sti\n\t"
        "call SyscallDispatchC\n\t"
        "cli\n\t"
        // Check for pending signals before returning to userspace.
        // Pass the saved-register frame and syscall return value.
        "mov %%rsp, %%rdi\n\t"         // RDI = pointer to SyscallFrame
        "mov %%rax, %%rsi\n\t"         // RSI = syscall result
        "call SyscallCheckSignals\n\t"  // returns new RAX
        "jmp .Lsyscall_return\n\t"
        ".Lsyscall_invalid:\n\t"
        "mov $-38, %%rax\n\t"
        ".Lsyscall_return:\n\t"
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
        "pop %%r11\n\t"                // [3] user RFLAGS
        "pop %%rcx\n\t"                // [2] user return address
        // Validate RCX is a canonical user-mode address before sysret.
        // Use bt to test bit 47 without clobbering any GPR.
        "bt $47, %%rcx\n\t"
        "jc .Lsysret_bad_rcx\n\t"
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
        ::: "memory"
    );
}

namespace brook {

// ---------------------------------------------------------------------------
// Error codes (Linux)
// ---------------------------------------------------------------------------

static constexpr int64_t ENOENT  = 2;
static constexpr int64_t ESRCH   = 3;
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

// Check if the current process has deliverable signals pending.
// Call after SchedulerBlock() returns to decide whether to return -EINTR.
static bool HasPendingSignals()
{
    Process* proc = ProcessCurrent();
    if (!proc) return false;
    return (proc->sigPending & ~proc->sigMask) != 0;
}

// EventFd data structure (used in read/write/poll/close)
struct EventFdData {
    volatile uint64_t counter;
    uint32_t flags;
    Process* readerWaiter;
};
static constexpr uint32_t EFD_SEMAPHORE = 0x01;
[[maybe_unused]] static constexpr uint32_t EFD_CLOEXEC = 0x80000;
static constexpr uint32_t EFD_NONBLOCK  = 0x800;

// timerfd constants and data — defined here so sys_read/sys_poll can use them
static constexpr int TFD_NONBLOCK     = 0x800;
static constexpr int TFD_CLOEXEC      = 0x80000;
static constexpr int TFD_TIMER_ABSTIME = 1;
static constexpr uint64_t LAPIC_TICKS_PER_MS = 1; // LAPIC ticks ≈ 1ms each

struct TimerFdData {
    volatile uint64_t expiryCount; // number of expirations since last read
    uint64_t  intervalNs;          // interval in nanoseconds (0 = one-shot)
    uint64_t  nextExpiry;          // absolute LAPIC tick of next expiry
    int       clockId;
    bool      armed;
    Process*  waiter;              // process blocked in read() or epoll
};

// memfd constants and data — defined here so sys_read/sys_write/fstat can use them
static constexpr uint32_t MFD_CLOEXEC       = 0x0001u;
[[maybe_unused]] static constexpr uint32_t MFD_ALLOW_SEALING = 0x0002u;

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
    uint64_t* pageMap;          // [pageMapCount] entries; raw PhysicalAddress.value, 0 = unbacked
    uint64_t  pageMapCount;     // number of slots; covers `capacity` bytes
    uint64_t  size;             // logical size set by ftruncate / extended on write
    uint64_t  capacity;         // pageMapCount * 4096
    volatile uint32_t lock;     // spinlock protecting pageMap allocations + grow
    volatile uint32_t refCount; // +1 per fd, +1 per live mmap VMA
};

static inline void MfdLock(MemFdData* mfd)
{
    while (__atomic_exchange_n(&mfd->lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}
static inline void MfdUnlock(MemFdData* mfd)
{
    __atomic_store_n(&mfd->lock, 0, __ATOMIC_RELEASE);
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
    for (uint64_t i = 0; i < mfd->pageMapCount; i++) newMap[i] = mfd->pageMap[i];
    for (uint64_t i = mfd->pageMapCount; i < newCount; i++) newMap[i] = 0;
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
    MfdLock(mfd);
    if (pageIdx >= mfd->pageMapCount) { MfdUnlock(mfd); return PhysicalAddress(0); }
    uint64_t raw = mfd->pageMap[pageIdx];
    if (raw == 0) {
        PhysicalAddress phys = PmmAllocPage(MemTag::User, pid);
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

// User #PF demand-paging hook for memfd MAP_SHARED mappings. Returns true
// if the fault was a memfd-backed page that we just paged in; false if
// this fault is unrelated (caller should fall through to the kill path).
extern "C" bool MemFdHandleUserFault(uint64_t cr2, uint64_t errCode);

// External wrappers for fork() in process.cpp, which doesn't see MemFdData.
void MemFdHandleRef(void* handle) { MemFdRef(static_cast<MemFdData*>(handle)); }
void MemFdHandleUnref(void* handle) { MemFdUnref(static_cast<MemFdData*>(handle)); }

// Demand-page a memfd-backed user mapping. Called from the user #PF path
// in idt.cpp before the kill flow. Returns true if the fault was on a
// memfd VMA and was successfully resolved (PTE installed).
extern "C" bool MemFdHandleUserFault(uint64_t cr2, uint64_t errCode)
{
    (void)errCode;
    Process* proc = ProcessCurrent();
    if (!proc) return false;
    uint64_t pageVA = cr2 & ~uint64_t{0xFFF};

    for (uint32_t i = 0; i < Process::MAX_MEMFD_MAPS; i++) {
        auto& m = proc->memfdMaps[i];
        if (m.length == 0 || !m.mfd) continue;
        if (pageVA < m.vaddr || pageVA >= m.vaddr + m.length) continue;

        auto* mfd = static_cast<MemFdData*>(m.mfd);
        uint64_t offsetInVma = pageVA - m.vaddr;
        uint64_t mfdOffset = m.offset + offsetInVma;
        uint64_t pageIdx = mfdOffset / 4096;

        PhysicalAddress phys = MemFdGetOrAllocPage(mfd, pageIdx, proc->pid);
        if (!phys) return false;

        // Install a writable user PTE. We don't tag with PTE_TAG/PTE_PID
        // ownership bits because the page is owned by the mfd's pageMap,
        // not by this process's page table — VmmDestroyUserPageTable must
        // not free it. process.cpp::ProcessDestroy explicitly VmmUnmapPage's
        // every memfd VMA before destroying the page table, which clears
        // these PTEs and prevents the unref-on-destroy walk from touching
        // mfd-owned pages.
        if (!VmmMapPage(proc->pageTable, VirtualAddress(pageVA), phys,
                        VMM_PRESENT | VMM_WRITABLE | VMM_USER | VMM_NO_EXEC,
                        MemTag::User, proc->pid)) {
            return false;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// AF_UNIX path sockets — structs and helpers defined here so
// sys_read/sys_write/sys_close can use them
// ---------------------------------------------------------------------------

static constexpr int AF_UNIX           = 1;
static constexpr int UNIX_SOCK_NONBLOCK = 0x800;
static constexpr int UNIX_SOCK_CLOEXEC  = 0x80000;
static constexpr int UNIX_MAX_SERVERS   = 16;
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

// FIFO of in-flight fds for one direction of a connected AF_UNIX pair.
// Shared between sender (enqueue) and receiver (dequeue).
struct UnixFdQueue {
    UnixFdSnap   msgs[UNIX_FD_QUEUE_CAP];
    volatile int head;
    volatile int tail;
    volatile uint32_t refCount; // one ref per UnixSocketData pointing at us
};

static inline int UnixFdQueueCount(const UnixFdQueue* q)
{
    return (q->head - q->tail + UNIX_FD_QUEUE_CAP) % UNIX_FD_QUEUE_CAP;
}
[[maybe_unused]] static inline bool UnixFdQueueFull(const UnixFdQueue* q)
{
    return UnixFdQueueCount(q) >= UNIX_FD_QUEUE_CAP - 1;
}

struct UnixPendingConn {
    PipeBuffer* serverRx;     // server reads this (client→server data)
    PipeBuffer* serverTx;     // server writes this (server→client data)
    UnixFdQueue* serverRxFds; // server reads these fds (client→server)
    UnixFdQueue* serverTxFds; // server writes these fds (server→client)
    Process*    clientWaiter; // client blocked until accept() completes
    bool        accepted;
    bool        used;
};

struct UnixSocketData {
    enum class State : uint8_t { Unbound, Listening, Connected };
    State   state;
    bool    nonblock;
    volatile uint32_t refCount; // +1 per fd pointing at this struct
    char    path[108];
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

static UnixSocketData* g_unixServers[UNIX_MAX_SERVERS];

static UnixSocketData* UnixFindServer(const char* path)
{
    for (int i = 0; i < UNIX_MAX_SERVERS; i++) {
        if (!g_unixServers[i]) continue;
        if (g_unixServers[i]->state != UnixSocketData::State::Listening) continue;
        const char* a = g_unixServers[i]->path;
        const char* b = path;
        bool match = true;
        while (*a || *b) { if (*a++ != *b++) { match = false; break; } }
        if (match) return g_unixServers[i];
    }
    return nullptr;
}

static void UnixRegisterServer(UnixSocketData* usd)
{
    for (int i = 0; i < UNIX_MAX_SERVERS; i++) {
        if (!g_unixServers[i]) { g_unixServers[i] = usd; return; }
    }
}

static void UnixUnregisterServer(UnixSocketData* usd)
{
    for (int i = 0; i < UNIX_MAX_SERVERS; i++) {
        if (g_unixServers[i] == usd) { g_unixServers[i] = nullptr; return; }
    }
}

void UnixSocketHandleRef(void* handle)
{
    if (!handle) return;
    auto* usd = static_cast<UnixSocketData*>(handle);
    __atomic_fetch_add(&usd->refCount, 1, __ATOMIC_RELEASE);
}

// Wake any process blocked inside epoll_wait on this pipe. Called from
// sys_write / sys_sendmsg / sys_close after mutating pipe state in a way
// that could transition a watched fd to ready.
static inline void PipeWakeEpoll(PipeBuffer* pipe)
{
    if (!pipe) return;
    Process* ew = pipe->epollWaiter;
    if (ew) {
        pipe->epollWaiter = nullptr;
        __atomic_store_n(&ew->pendingWakeup, 1, __ATOMIC_RELEASE);
        SchedulerUnblock(ew);
    }
}

// Same for a listening AF_UNIX socket: wake epoll waiter when a new
// connection is posted to the pending queue.
static inline void UnixListenWakeEpoll(UnixSocketData* usd)
{
    if (!usd) return;
    Process* ew = usd->epollWaiter;
    if (ew) {
        usd->epollWaiter = nullptr;
        __atomic_store_n(&ew->pendingWakeup, 1, __ATOMIC_RELEASE);
        SchedulerUnblock(ew);
    }
}

// ---------------------------------------------------------------------------
// OSS /dev/dsp state and ioctl definitions
// ---------------------------------------------------------------------------

// OSS ioctl numbers (from <sys/soundcard.h>)
static constexpr uint64_t SNDCTL_DSP_RESET     = 0x5000;
static constexpr uint64_t SNDCTL_DSP_SPEED     = 0xC0045002; // _IOWR('P', 2, int)
static constexpr uint64_t SNDCTL_DSP_SETFMT    = 0xC0045005; // _IOWR('P', 5, int)
static constexpr uint64_t SNDCTL_DSP_CHANNELS  = 0xC0045006; // _IOWR('P', 6, int)
static constexpr uint64_t SNDCTL_DSP_STEREO    = 0xC0045003; // _IOWR('P', 3, int)
static constexpr uint64_t SNDCTL_DSP_GETOSPACE = 0x800C5012; // _IOR('P', 0x12, audio_buf_info)
static constexpr uint64_t SNDCTL_DSP_GETCAPS   = 0x8004500F; // _IOR('P', 0x0F, int)
static constexpr uint64_t SNDCTL_DSP_SETTRIGGER= 0x40045010; // _IOW('P', 0x10, int)
static constexpr uint64_t SNDCTL_DSP_GETFMTS   = 0x8004500B; // _IOR('P', 0x0B, int)
static constexpr uint64_t SNDCTL_DSP_SETFRAGMENT= 0xC004500A;// _IOWR('P', 0x0A, int)
static constexpr uint64_t SNDCTL_DSP_GETBLKSIZE = 0xC0045004;// _IOWR('P', 4, int)

// OSS audio formats
static constexpr int AFMT_U8     = 0x00000008;
static constexpr int AFMT_S16_LE = 0x00000010;
// OSS capabilities
static constexpr int DSP_CAP_TRIGGER = 0x00000010;

// Per-fd audio device state
struct DspState {
    uint32_t sampleRate;
    uint8_t  channels;
    uint8_t  bitsPerSample;
    uint16_t fragmentSize;   // bytes per fragment
    uint32_t bufferOffset;   // write cursor into staging buffer
    uint8_t* buffer;         // staging buffer (kmalloc'd)
    uint32_t bufferSize;     // total staging buffer size
    uint32_t mixerStreamId;  // mixer stream slot (0-7)
};

static constexpr uint32_t DSP_DEFAULT_RATE     = 48000;
static constexpr uint8_t  DSP_DEFAULT_CHANNELS = 1;
static constexpr uint8_t  DSP_DEFAULT_BITS     = 8;
static constexpr uint32_t DSP_HW_RATE          = 44100; // hardware playback rate
static constexpr uint32_t DSP_BUFFER_SIZE      = 65536; // 64KB staging buffer

// ---------------------------------------------------------------------------
// sys_write (1)
// ---------------------------------------------------------------------------

static int64_t sys_write(uint64_t fd, uint64_t bufAddr, uint64_t count,
                          uint64_t, uint64_t, uint64_t)
{
    // fd 3 = debug serial — writes directly, bypassing the async ring buffer.
    // Hold the serial lock across the entire write so multi-CPU output
    // doesn't interleave character-by-character.
    if (fd == 3)
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
        Process* proc = ProcessCurrent();
        FdEntry* fde = (proc ? FdGet(proc, static_cast<int>(fd)) : nullptr);
        // Redirected if FD entry is a pipe or other non-default type.
        // Default: Vnode with null handle = serial output
        if (!fde || fde->type == FdType::None ||
            (fde->type == FdType::Vnode && !fde->handle))
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
        // Fall through to FD-table-based write below. (Historically we
        // mirrored redirected stdout/stderr to the serial log here, but
        // that corrupts binary pipelines — e.g. curl | xz | nar-unpack
        // dumps the compressed NAR stream onto the serial console — and
        // makes debug output confusing. If you need pipe content in logs,
        // explicitly tee to fd 3.)
    }

    // File descriptor write
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;

    if (fde->type == FdType::Vnode && fde->handle)
    {
        auto* vn = static_cast<Vnode*>(fde->handle);
        int64_t ret = VfsWrite(vn, reinterpret_cast<const void*>(bufAddr),
                               static_cast<uint32_t>(count), &fde->seekPos);
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
    if (fde->type == FdType::DevDsp && fde->handle)
    {
        auto* dsp = static_cast<DspState*>(fde->handle);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(bufAddr);
        uint32_t appRate = dsp->sampleRate;
        uint32_t bytesPerFrame = (dsp->bitsPerSample / 8) * dsp->channels;
        if (bytesPerFrame == 0) bytesPerFrame = 1;

        const int16_t* mixSrc = nullptr;
        uint32_t mixFrames = 0;

        if (appRate == DSP_HW_RATE || appRate == 0)
        {
            // Already at hardware rate — can submit directly
            if (dsp->channels == 2 && dsp->bitsPerSample == 16)
            {
                mixSrc = reinterpret_cast<const int16_t*>(src);
                mixFrames = static_cast<uint32_t>(count) / 4;
            }
            else
            {
                // Mono→stereo or 8→16 bit conversion needed
                uint32_t inFrames = static_cast<uint32_t>(count) / bytesPerFrame;
                uint32_t outBytes = inFrames * 4; // stereo 16-bit
                if (outBytes > dsp->bufferSize) outBytes = dsp->bufferSize;
                uint32_t outFrames = outBytes / 4;
                int16_t* out = reinterpret_cast<int16_t*>(dsp->buffer);

                for (uint32_t i = 0; i < outFrames; i++)
                {
                    int16_t sample;
                    if (dsp->bitsPerSample == 8)
                        sample = (static_cast<int16_t>(src[i * (dsp->channels == 2 ? 2 : 1)]) - 128) << 8;
                    else
                        sample = reinterpret_cast<const int16_t*>(src)[i * (dsp->channels == 2 ? 2 : 1)];
                    out[i * 2 + 0] = sample; // L
                    out[i * 2 + 1] = (dsp->channels == 2)
                        ? (dsp->bitsPerSample == 8
                            ? (static_cast<int16_t>(src[i * 2 + 1]) - 128) << 8
                            : reinterpret_cast<const int16_t*>(src)[i * 2 + 1])
                        : sample; // R = L for mono
                }
                mixSrc = out;
                mixFrames = outFrames;
            }
        }
        else
        {
            // Resample to 44100 Hz stereo 16-bit
            uint32_t inFrames  = static_cast<uint32_t>(count) / bytesPerFrame;
            uint32_t outFrames = (inFrames * DSP_HW_RATE + appRate - 1) / appRate;
            uint32_t outBytes  = outFrames * 4; // stereo 16-bit
            if (outBytes > dsp->bufferSize) outBytes = dsp->bufferSize;
            outFrames = outBytes / 4;

            int16_t* out = reinterpret_cast<int16_t*>(dsp->buffer);

            for (uint32_t i = 0; i < outFrames; i++)
            {
                uint32_t srcFrame = (uint64_t)i * appRate / DSP_HW_RATE;
                if (srcFrame >= inFrames) srcFrame = inFrames - 1;

                int16_t sample;
                if (dsp->bitsPerSample == 8)
                    sample = (static_cast<int16_t>(src[srcFrame * (dsp->channels == 2 ? 2 : 1)]) - 128) << 8;
                else
                    sample = reinterpret_cast<const int16_t*>(src)[srcFrame * (dsp->channels == 2 ? 2 : 1)];

                out[i * 2 + 0] = sample; // L
                out[i * 2 + 1] = (dsp->channels == 2)
                    ? (dsp->bitsPerSample == 8
                        ? (static_cast<int16_t>(src[srcFrame * 2 + 1]) - 128) << 8
                        : reinterpret_cast<const int16_t*>(src)[srcFrame * 2 + 1])
                    : sample; // R = L for mono
            }
            mixSrc = out;
            mixFrames = outFrames;
        }

        if (mixSrc && mixFrames > 0)
        {
            bool nonblock = (fde->statusFlags & 0x800) != 0; // O_NONBLOCK
            AudioPlay(mixSrc, mixFrames * MIXER_FRAME_BYTES,
                      MIXER_HW_RATE, MIXER_HW_CHANNELS, MIXER_HW_BITS, nonblock);
        }

        return static_cast<int64_t>(count);
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

    // Write to pipe — block until at least some bytes are written
    if (fde->type == FdType::Pipe && fde->handle)
    {
        // For bidirectional socketpair (flags==2), write goes to the write pipe
        auto* pipe = (fde->flags == 2)
            ? reinterpret_cast<PipeBuffer*>(fde->seekPos)
            : static_cast<PipeBuffer*>(fde->handle);
        const char* src = reinterpret_cast<const char*>(bufAddr);
        uint64_t written = 0;

        while (written < count)
        {
            // If no readers, send SIGPIPE and return EPIPE
            if (__atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0)
            {
                constexpr int SIGPIPE = 13;
                Process* self = ProcessCurrent();
                if (self)
                    ProcessSendSignal(self, SIGPIPE);
                return written > 0 ? static_cast<int64_t>(written) : -EPIPE;
            }

            uint32_t chunk = pipe->write(src + written,
                static_cast<uint32_t>(count - written > 4096 ? 4096 : count - written));
            written += chunk;
            if (written > 0)
            {
                // Wake blocked reader
                Process* reader = pipe->readerWaiter;
                if (reader)
                {
                    pipe->readerWaiter = nullptr;
                    __atomic_store_n(&reader->pendingWakeup, 1, __ATOMIC_RELEASE);
                    SchedulerUnblock(reader);
                }
                PipeWakeEpoll(pipe);
                break;  // Return partial writes immediately
            }
            // Buffer full — block until reader drains some
            Process* self = ProcessCurrent();
            pipe->writerWaiter = self;
            SchedulerBlock(self);
            if (HasPendingSignals())
                return written > 0 ? static_cast<int64_t>(written) : -EINTR;
        }
        return static_cast<int64_t>(written);
    }

    // Write to eventfd — adds value to counter
    if (fde->type == FdType::EventFd && fde->handle)
    {
        if (count < 8) return -EINVAL;
        uint64_t val = *reinterpret_cast<const uint64_t*>(bufAddr);
        if (val == 0xFFFFFFFFFFFFFFFFULL) return -EINVAL;
        auto* efd = static_cast<EventFdData*>(fde->handle);
        __atomic_fetch_add(&efd->counter, val, __ATOMIC_ACQ_REL);
        // Wake blocked reader
        Process* reader = efd->readerWaiter;
        if (reader) {
            efd->readerWaiter = nullptr;
            __atomic_store_n(&reader->pendingWakeup, 1, __ATOMIC_RELEASE);
            SchedulerUnblock(reader);
        }
        // Note: eventfd readability may also be observed through epoll, but
        // EventFdData has no epollWaiter slot yet — covered by the 5ms
        // safety poll in epoll_wait_impl.
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
                    __atomic_store_n(&reader->pendingWakeup, 1, __ATOMIC_RELEASE);
                    SchedulerUnblock(reader);
                }
                PipeWakeEpoll(pipe);
                break;
            }
            Process* self = ProcessCurrent();
            pipe->writerWaiter = self;
            SchedulerBlock(self);
            if (HasPendingSignals())
                return written > 0 ? static_cast<int64_t>(written) : -EINTR;
        }
        return static_cast<int64_t>(written);
    }

    // Write to memfd — grows backing as needed, allocates pages on demand.
    if (fde->type == FdType::MemFd && fde->handle)
    {
        auto* mfd = static_cast<MemFdData*>(fde->handle);
        if (bufAddr < 0x1000) return -EFAULT;
        uint64_t pos = fde->seekPos;
        uint64_t end = pos + count;

        if (end > mfd->capacity) {
            if (!MemFdGrow(mfd, end)) return -ENOMEM;
        }

        const uint8_t* src = reinterpret_cast<const uint8_t*>(bufAddr);
        uint64_t remaining = count;
        while (remaining) {
            uint64_t pageIdx = pos / 4096;
            uint64_t pageOff = pos & 0xFFF;
            uint64_t chunk = 4096 - pageOff;
            if (chunk > remaining) chunk = remaining;
            PhysicalAddress phys = MemFdGetOrAllocPage(mfd, pageIdx, proc->pid);
            if (!phys) return -ENOMEM;
            auto* dst = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw()) + pageOff;
            __builtin_memcpy(dst, src, chunk);
            src += chunk; pos += chunk; remaining -= chunk;
        }
        fde->seekPos = pos;
        if (pos > mfd->size) mfd->size = pos;
        return static_cast<int64_t>(count);
    }

    // Write to AF_UNIX connected socket
    if (fde->type == FdType::UnixSocket && fde->handle)
    {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if (usd->state != UnixSocketData::State::Connected || !usd->txPipe)
            return -ENOTCONN;
        auto* pipe = usd->txPipe;
        const char* src = reinterpret_cast<const char*>(bufAddr);
        bool nonblock = usd->nonblock || (fde->statusFlags & 0x800);
        uint64_t written = 0;

        while (written < count) {
            if (__atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0)
                return written > 0 ? static_cast<int64_t>(written) : -EPIPE;
            uint32_t chunk = pipe->write(src + written,
                static_cast<uint32_t>(count - written > 4096 ? 4096 : count - written));
            written += chunk;
            if (written > 0) {
                Process* reader = pipe->readerWaiter;
                if (reader) {
                    pipe->readerWaiter = nullptr;
                    __atomic_store_n(&reader->pendingWakeup, 1, __ATOMIC_RELEASE);
                    SchedulerUnblock(reader);
                }
                PipeWakeEpoll(pipe);
                break;
            }
            if (nonblock) return -EAGAIN;
            Process* self = ProcessCurrent();
            pipe->writerWaiter = self;
            SchedulerBlock(self);
            if (HasPendingSignals()) return written > 0 ? static_cast<int64_t>(written) : -EINTR;
        }
        return static_cast<int64_t>(written);
    }

    return -EBADF;
}

// ---------------------------------------------------------------------------
// sys_read (0)
// ---------------------------------------------------------------------------

static int64_t sys_read(uint64_t fd, uint64_t bufAddr, uint64_t count,
                         uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;

    if (fde->type == FdType::Vnode && fde->handle)
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
    if (fde->type == FdType::Pipe && fde->handle)
    {
        auto* pipe = static_cast<PipeBuffer*>(fde->handle);
        auto* dst = reinterpret_cast<char*>(bufAddr);

        for (;;)
        {
            uint32_t got = pipe->read(dst, static_cast<uint32_t>(
                count > 4096 ? 4096 : count));
            if (got > 0)
            {
                // Wake blocked writer
                Process* writer = pipe->writerWaiter;
                if (writer)
                {
                    pipe->writerWaiter = nullptr;
                    __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
                    SchedulerUnblock(writer);
                }
                return static_cast<int64_t>(got);
            }

            // EOF — no writers left and buffer empty
            uint32_t wr = __atomic_load_n(&pipe->writers, __ATOMIC_ACQUIRE);
            if (wr == 0)
                return 0;

            // Non-blocking: return -EAGAIN instead of blocking
            if (fde->statusFlags & 0x800) // O_NONBLOCK
                return -EAGAIN;

            // Block until writer puts data or writer closes
            Process* self = ProcessCurrent();
            pipe->readerWaiter = self;
            // Use timed wakeup to recheck writer count periodically
            // (avoids permanent deadlock if close notification was missed)
            extern volatile uint64_t g_lapicTickCount;
            self->wakeupTick = g_lapicTickCount + 10; // recheck every ~10ms
            SchedulerBlock(self);
            if (HasPendingSignals())
            {
                // SIGCHLD is harmless during pipe reads — clear it and retry.
                // Only return EINTR for other signals.
                uint64_t pending = self->sigPending & ~self->sigMask;
                uint64_t sigchldBit = (1ULL << (17 - 1)); // SIGCHLD = 17
                if ((pending & ~sigchldBit) != 0)
                    return -EINTR;
                self->sigPending &= ~sigchldBit;
            }
        }
    }

    // Read from eventfd — returns counter as uint64, resets to 0
    if (fde->type == FdType::EventFd && fde->handle)
    {
        if (count < 8) return -EINVAL;
        auto* efd = static_cast<EventFdData*>(fde->handle);

        for (;;)
        {
            uint64_t val = __atomic_load_n(&efd->counter, __ATOMIC_ACQUIRE);
            if (val > 0)
            {
                if (efd->flags & EFD_SEMAPHORE) {
                    __atomic_fetch_sub(&efd->counter, 1, __ATOMIC_ACQ_REL);
                    val = 1;
                } else {
                    __atomic_store_n(&efd->counter, 0ULL, __ATOMIC_RELEASE);
                }
                *reinterpret_cast<uint64_t*>(bufAddr) = val;
                return 8;
            }

            // Non-blocking: return EAGAIN
            if (fde->statusFlags & 0x800)
                return -EAGAIN;

            // Block until counter becomes non-zero
            Process* self = ProcessCurrent();
            efd->readerWaiter = self;
            extern volatile uint64_t g_lapicTickCount;
            self->wakeupTick = g_lapicTickCount + 10;
            SchedulerBlock(self);
            if (HasPendingSignals())
                return -EINTR;
        }
    }

    // Read from socket (TCP stream or connected UDP)
    if (fde->type == FdType::Socket && fde->handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        bool nonblock = (fde->statusFlags & 0x800) != 0;
        if (brook::SockIsStream(sockIdx))
        {
            if (nonblock && brook::SockRxCount(sockIdx) == 0 && !brook::SockPollReady(sockIdx, true, false))
                return -EAGAIN;
            return brook::SockRecv(sockIdx,
                                   reinterpret_cast<void*>(bufAddr),
                                   static_cast<uint32_t>(count));
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

        // Poll for expiry
        for (;;) {
            extern volatile uint64_t g_lapicTickCount;
            uint64_t now = g_lapicTickCount;

            // Check if timer has fired
            if (tfd->armed && now >= tfd->nextExpiry) {
                // Count how many intervals have elapsed
                uint64_t elapsed = 1;
                if (tfd->intervalNs > 0) {
                    uint64_t intervalMs = tfd->intervalNs / 1000000ULL;
                    if (intervalMs == 0) intervalMs = 1;
                    uint64_t over = now - tfd->nextExpiry;
                    elapsed = 1 + over / (intervalMs * LAPIC_TICKS_PER_MS);
                    tfd->nextExpiry += elapsed * intervalMs * LAPIC_TICKS_PER_MS;
                } else {
                    tfd->armed = false; // one-shot
                }

                uint64_t total = __atomic_exchange_n(&tfd->expiryCount, 0, __ATOMIC_ACQ_REL) + elapsed;
                __builtin_memcpy(reinterpret_cast<void*>(bufAddr), &total, 8);
                return 8;
            }

            uint64_t pending = __atomic_exchange_n(&tfd->expiryCount, 0, __ATOMIC_ACQ_REL);
            if (pending > 0) {
                __builtin_memcpy(reinterpret_cast<void*>(bufAddr), &pending, 8);
                return 8;
            }

            if (nonblock) return -EAGAIN;
            if (!tfd->armed) return -EAGAIN;

            // Block until next expiry
            Process* self = ProcessCurrent();
            tfd->waiter = self;
            self->wakeupTick = tfd->nextExpiry;
            SchedulerBlock(self);
            tfd->waiter = nullptr;
            if (HasPendingSignals()) return -EINTR;
        }
    }

    // Read from memfd — sequential read; unbacked pages read as zero.
    if (fde->type == FdType::MemFd && fde->handle)
    {
        auto* mfd = static_cast<MemFdData*>(fde->handle);
        uint64_t pos = fde->seekPos;
        if (pos >= mfd->size) return 0; // EOF
        uint64_t avail = mfd->size - pos;
        uint64_t copyLen = (count < avail) ? count : avail;

        auto* dst = reinterpret_cast<uint8_t*>(bufAddr);
        uint64_t remaining = copyLen;
        while (remaining) {
            uint64_t pageIdx = pos / 4096;
            uint64_t pageOff = pos & 0xFFF;
            uint64_t chunk = 4096 - pageOff;
            if (chunk > remaining) chunk = remaining;
            uint64_t raw = (pageIdx < mfd->pageMapCount) ? mfd->pageMap[pageIdx] : 0;
            if (raw == 0) {
                __builtin_memset(dst, 0, chunk);
            } else {
                auto* src = reinterpret_cast<const uint8_t*>(PhysToVirt(PhysicalAddress(raw)).raw()) + pageOff;
                __builtin_memcpy(dst, src, chunk);
            }
            dst += chunk; pos += chunk; remaining -= chunk;
        }
        fde->seekPos = pos;
        return static_cast<int64_t>(copyLen);
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
            extern volatile uint64_t g_lapicTickCount;
            self->wakeupTick = g_lapicTickCount + 10;
            SchedulerBlock(self);
            // SIGCHLD alone shouldn't interrupt a blocking read — it's
            // default-ignored. Retry and re-poll the pipe instead.
            if (HasPendingSignals()) {
                Process* p = ProcessCurrent();
                uint64_t pending = p->sigPending & ~p->sigMask;
                uint64_t sigchld = (1ULL << (17 - 1));
                if ((pending & ~sigchld) != 0) return -EINTR;
                p->sigPending &= ~sigchld;
            }
        }
    }

    // Read from AF_UNIX connected socket
    if (fde->type == FdType::UnixSocket && fde->handle)
    {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if (usd->state != UnixSocketData::State::Connected || !usd->rxPipe)
            return -ENOTCONN;
        auto* pipe = usd->rxPipe;
        auto* dst  = reinterpret_cast<char*>(bufAddr);
        bool nonblock = usd->nonblock || (fde->statusFlags & 0x800);

        for (;;) {
            uint32_t got = pipe->read(dst, static_cast<uint32_t>(count > 4096 ? 4096 : count));
            if (got > 0) return static_cast<int64_t>(got);
            if (__atomic_load_n(&pipe->writers, __ATOMIC_ACQUIRE) == 0) return 0; // EOF
            if (nonblock) return -EAGAIN;
            Process* self = ProcessCurrent();
            pipe->readerWaiter = self;
            extern volatile uint64_t g_lapicTickCount;
            self->wakeupTick = g_lapicTickCount + 10;
            SchedulerBlock(self);
            if (HasPendingSignals()) {
                // SIGCHLD alone (default-ignored) shouldn't interrupt a
                // blocking AF_UNIX read; this is the classic case that
                // broke scm_rights_test when the client exits right after
                // sending.
                Process* p = ProcessCurrent();
                uint64_t pending = p->sigPending & ~p->sigMask;
                uint64_t sigchld = (1ULL << (17 - 1));
                if ((pending & ~sigchld) != 0) return -EINTR;
                p->sigPending &= ~sigchld;
            }
        }
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

static int64_t sys_open(uint64_t pathAddr, uint64_t flags, uint64_t mode,
                         uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path || pathAddr < 0x1000) return -EFAULT;

    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    // Resolve relative paths against the process's CWD.
    char resolvedPath[256];
    const char* lookupPath = path;
    if (path[0] != '/' && proc->cwd[0] != '\0')
    {
        uint32_t ci = 0;
        for (uint32_t j = 0; proc->cwd[j] && ci < 250; ++j)
            resolvedPath[ci++] = proc->cwd[j];
        if (ci > 0 && resolvedPath[ci - 1] != '/')
            resolvedPath[ci++] = '/';
        for (uint32_t j = 0; path[j] && ci < 254; ++j)
            resolvedPath[ci++] = path[j];
        resolvedPath[ci] = '\0';
        lookupPath = resolvedPath;
    }

    // Strip trailing "/." from resolved path (e.g., "/boot/." → "/boot")
    {
        uint32_t len = 0;
        while (lookupPath[len]) len++;
        if (lookupPath == resolvedPath && len >= 2 &&
            resolvedPath[len - 1] == '.' && resolvedPath[len - 2] == '/')
        {
            if (len > 2)
                resolvedPath[len - 2] = '\0'; // "/boot/." → "/boot"
            else
                resolvedPath[1] = '\0';        // "/." → "/"
        }
    }

    // Device paths
    if (StrEq(path, "/dev/fb0"))
    {
        int fd = FdAlloc(proc, FdType::DevFramebuf, nullptr);
        if (fd < 0) return -EMFILE;
        DbgPrintf("sys_open: /dev/fb0 → fd %d\n", fd);
        return fd;
    }

    if (StrEq(path, "keyboard") || StrEq(path, "/dev/keyboard"))
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
    if (StrEq(path, "/dev/tty") || StrEq(path, "/dev/console"))
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

    // /dev/null — discard writes, EOF on read
    if (StrEq(path, "/dev/null"))
    {
        int fd = FdAlloc(proc, FdType::DevNull, nullptr);
        if (fd < 0) return -EMFILE;
        return fd;
    }

    // /dev/urandom, /dev/random — RDRAND-backed random bytes
    if (StrEq(path, "/dev/urandom") || StrEq(path, "/dev/random"))
    {
        int fd = FdAlloc(proc, FdType::DevUrandom, nullptr);
        if (fd < 0) return -EMFILE;
        return fd;
    }

    // /dev/dsp — OSS audio output
    if (StrEq(path, "/dev/dsp"))
    {
        if (!AudioAvailable()) return -ENODEV;
        static uint32_t s_nextMixerStream = 0;
        auto* dsp = static_cast<DspState*>(kmalloc(sizeof(DspState)));
        if (!dsp) return -ENOMEM;
        dsp->sampleRate    = DSP_DEFAULT_RATE;
        dsp->channels      = DSP_DEFAULT_CHANNELS;
        dsp->bitsPerSample = DSP_DEFAULT_BITS;
        dsp->fragmentSize  = 4096;
        dsp->bufferOffset  = 0;
        dsp->bufferSize    = DSP_BUFFER_SIZE;
        dsp->mixerStreamId = s_nextMixerStream++ % 8;
        dsp->buffer        = static_cast<uint8_t*>(kmalloc(DSP_BUFFER_SIZE));
        if (!dsp->buffer) { kfree(dsp); return -ENOMEM; }
        int fd = FdAlloc(proc, FdType::DevDsp, dsp);
        if (fd < 0) { kfree(dsp->buffer); kfree(dsp); return -EMFILE; }
        return fd;
    }

    // Synthetic memory files: /etc/passwd, /etc/group, /proc/self/...
    {
        struct SyntheticFile { const char* path; const char* content; };
        static const SyntheticFile syntheticFiles[] = {
            { "/etc/passwd", "root:x:0:0:root:/:/boot/BIN/BASH\n" },
            { "/etc/group",  "root:x:0:\n" },
            { "/etc/shells", "/boot/BIN/BASH\n" },
            { "/etc/hostname", "brook\n" },
            { "/etc/nsswitch.conf", "passwd: files\ngroup: files\nhosts: files dns\n" },
            { "/etc/hosts", "127.0.0.1 localhost\n" },
            { nullptr, nullptr }
        };

        for (auto* sf = syntheticFiles; sf->path; ++sf)
        {
            if (StrEq(lookupPath, sf->path))
            {
                // Store pointer to static content in handle, seekPos=0
                int fd = FdAlloc(proc, FdType::SyntheticMem, const_cast<char*>(sf->content));
                if (fd < 0) return -EMFILE;
                proc->fds[fd].seekPos = 0;
                return fd;
            }
        }

        // Dynamic /etc/resolv.conf — generated from DHCP DNS server
        if (StrEq(lookupPath, "/etc/resolv.conf"))
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
                resolvBuf[pos] = '\0';
            } else {
                // Fallback: QEMU default DNS
                const char* fb = "nameserver 10.0.2.3\n";
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
    if (StrEq(lookupPath, "/proc/self/exe"))
        return -ENOENT;

    // /dev/shm/<name> — POSIX shared memory via memfd
    // glibc shm_open() calls open("/dev/shm/<name>", flags, mode)
    if (lookupPath[0] == '/' &&
        lookupPath[1] == 'd' && lookupPath[2] == 'e' && lookupPath[3] == 'v' &&
        lookupPath[4] == '/' && lookupPath[5] == 's' && lookupPath[6] == 'h' &&
        lookupPath[7] == 'm' && lookupPath[8] == '/')
    {
        static constexpr int SHM_MAX = 32;
        struct ShmEntry { char name[64]; MemFdData* mfd; bool used; };
        static ShmEntry s_shm[SHM_MAX];

        const char* shmName = lookupPath + 9; // skip "/dev/shm/"
        static constexpr uint64_t LINUX_O_CREAT_SHM  = 0x40;
        static constexpr uint64_t LINUX_O_TRUNC_SHM  = 0x200;
        bool create = (flags & LINUX_O_CREAT_SHM) != 0;
        bool trunc  = (flags & LINUX_O_TRUNC_SHM) != 0;

        // Find existing entry
        ShmEntry* entry = nullptr;
        for (int i = 0; i < SHM_MAX; i++) {
            if (!s_shm[i].used) continue;
            const char* a = s_shm[i].name; const char* b = shmName;
            bool match = true;
            while (*a || *b) { if (*a++ != *b++) { match = false; break; } }
            if (match) { entry = &s_shm[i]; break; }
        }

        if (!entry && !create) return -ENOENT;

        if (!entry) {
            // Allocate new shm entry
            for (int i = 0; i < SHM_MAX; i++) {
                if (!s_shm[i].used) { entry = &s_shm[i]; break; }
            }
            if (!entry) return -ENOMEM;
            for (int i = 0; i < 63 && shmName[i]; i++) entry->name[i] = shmName[i];
            entry->name[63] = 0;
            entry->mfd = static_cast<MemFdData*>(kmalloc(sizeof(MemFdData)));
            if (!entry->mfd) return -ENOMEM;
            entry->mfd->pageMap = nullptr;
            entry->mfd->pageMapCount = 0;
            entry->mfd->size = 0;
            entry->mfd->capacity = 0;
            entry->mfd->lock = 0;
            entry->mfd->refCount = 0;
            entry->used = true;
        }

        if (trunc) {
            MfdLock(entry->mfd);
            if (entry->mfd->pageMap) {
                for (uint64_t i = 0; i < entry->mfd->pageMapCount; i++) {
                    if (entry->mfd->pageMap[i]) PmmFreePage(PhysicalAddress(entry->mfd->pageMap[i]));
                }
                kfree(entry->mfd->pageMap);
                entry->mfd->pageMap = nullptr;
            }
            entry->mfd->pageMapCount = 0;
            entry->mfd->size = 0;
            entry->mfd->capacity = 0;
            MfdUnlock(entry->mfd);
        }

        int fd = FdAlloc(proc, FdType::MemFd, entry->mfd);
        if (fd < 0) return -EMFILE;
        MemFdRef(entry->mfd);
        SerialPrintf("shm_open: '%s' fd=%d size=%llu\n", shmName, fd, entry->mfd->size);
        return fd;
    }

    // Translate Linux open flags to VFS flags
    uint32_t vfsFlags = VFS_O_READ;
    static constexpr uint64_t LINUX_O_WRONLY = 1;
    static constexpr uint64_t LINUX_O_RDWR   = 2;
    static constexpr uint64_t LINUX_O_CREAT  = 0x40;
    static constexpr uint64_t LINUX_O_TRUNC  = 0x200;
    static constexpr uint64_t LINUX_O_APPEND = 0x400;
    if (flags & LINUX_O_WRONLY || flags & LINUX_O_RDWR) vfsFlags |= VFS_O_WRITE;
    if (flags & LINUX_O_CREAT)  vfsFlags |= VFS_O_CREATE;
    if (flags & LINUX_O_TRUNC)  vfsFlags |= VFS_O_TRUNC;
    if (flags & LINUX_O_APPEND) vfsFlags |= VFS_O_APPEND;

    Vnode* vn = VfsOpen(lookupPath, vfsFlags);

    // Fallback: if path starts with /lib/ or /nix/, try /boot/lib/<filename>.
    // Our boot disk is mounted at /boot, but dynamic linkers expect /lib/.
    if (!vn && lookupPath[0] == '/')
    {
        // Try /boot prefix first
        char bootPath[256] = "/boot";
        uint32_t bi = 5;
        const char* p = lookupPath;
        while (*p && bi + 1 < sizeof(bootPath)) bootPath[bi++] = *p++;
        bootPath[bi] = '\0';
        vn = VfsOpen(bootPath, vfsFlags);

        // If that fails, try /boot/lib/<basename> for .so files
        if (!vn)
        {
            const char* fname = lookupPath;
            for (p = lookupPath; *p; ++p)
                if (*p == '/') fname = p + 1;

            char libPath[256] = "/boot/lib/";
            uint32_t li = 10;
            while (*fname && li + 1 < sizeof(libPath)) libPath[li++] = *fname++;
            libPath[li] = '\0';
            vn = VfsOpen(libPath, vfsFlags);
        }
    }

    if (!vn)
    {
        // Diagnostic: log pak open failures
        bool isPak = false;
        for (const char* p = lookupPath; *p; ++p)
            if (p[0] == 'p' && p[1] == 'a' && p[2] == 'k') { isPak = true; break; }
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

    int fd = FdAlloc(proc, FdType::Vnode, vn);
    if (fd < 0)
    {
        VfsClose(vn);
        return -EMFILE;
    }
    proc->fds[fd].statusFlags = static_cast<uint32_t>(flags);

    // Store directory path for openat resolution
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

// ---------------------------------------------------------------------------
// sys_close (3)
// ---------------------------------------------------------------------------

static int64_t sys_close(uint64_t fd, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;

    if (fde->type == FdType::Vnode && fde->handle)
    {
        auto* vn = static_cast<Vnode*>(fde->handle);
        uint32_t prev = __atomic_fetch_sub(&vn->refCount, 1, __ATOMIC_ACQ_REL);
        if (prev <= 1)
            VfsClose(vn);
        // else: other processes still reference this vnode
    }

    if (fde->type == FdType::Pipe && fde->handle)
    {
        auto* pipe = static_cast<PipeBuffer*>(fde->handle);
        // flags bit 0: 1=write end, 0=read end
        if (fde->flags & 1)
        {
            __atomic_fetch_sub(&pipe->writers, 1, __ATOMIC_RELEASE);
            // Wake blocked reader so it sees EOF
            Process* reader = pipe->readerWaiter;
            if (reader)
            {
                pipe->readerWaiter = nullptr;
                __atomic_store_n(&reader->pendingWakeup, 1, __ATOMIC_RELEASE);
                SchedulerUnblock(reader);
            }
            // EOF is also readable for epoll (EPOLLHUP/EPOLLIN).
            PipeWakeEpoll(pipe);
        }
        else
        {
            __atomic_fetch_sub(&pipe->readers, 1, __ATOMIC_RELEASE);
            // Wake blocked writer so it sees EPIPE
            Process* writer = pipe->writerWaiter;
            if (writer)
            {
                pipe->writerWaiter = nullptr;
                __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
                SchedulerUnblock(writer);
            }
        }

        // Free pipe buffer when both ends are closed
        if (__atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0 &&
            __atomic_load_n(&pipe->writers, __ATOMIC_ACQUIRE) == 0)
        {
            kfree(pipe);
        }
    }

    if (fde->type == FdType::Socket && fde->handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        brook::SockUnref(sockIdx);
    }

    if (fde->type == FdType::EventFd && fde->handle)
    {
        kfree(fde->handle);
    }

    if (fde->type == FdType::EpollFd && fde->handle)
    {
        kfree(fde->handle);
    }

    if (fde->type == FdType::TimerFd && fde->handle)
    {
        kfree(fde->handle);
    }

    if (fde->type == FdType::MemFd && fde->handle)
    {
        auto* mfd = static_cast<MemFdData*>(fde->handle);
        MemFdUnref(mfd);
    }

    if (fde->type == FdType::UnixSocket && fde->handle)
    {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        uint32_t prev = __atomic_fetch_sub(&usd->refCount, 1, __ATOMIC_ACQ_REL);
        if (prev == 1) {
        if (usd->state == UnixSocketData::State::Listening)
            UnixUnregisterServer(usd);
        // Decrement refcounts on connected pipes so the other end sees EOF
        if (usd->rxPipe) __atomic_fetch_sub(&usd->rxPipe->readers, 1, __ATOMIC_RELEASE);
        if (usd->txPipe) __atomic_fetch_sub(&usd->txPipe->writers, 1, __ATOMIC_RELEASE);

        // Drop our refs on the per-direction fd queues; if this was the last
        // holder, drain any undelivered fds so we don't leak kernel objects.
        auto drainQueue = [](UnixFdQueue* q) {
            if (!q) return;
            uint32_t prev = __atomic_fetch_sub(&q->refCount, 1, __ATOMIC_ACQ_REL);
            if (prev > 1) return;
            while (q->head != q->tail) {
                UnixFdSnap& m = q->msgs[q->tail];
                q->tail = (q->tail + 1) % UNIX_FD_QUEUE_CAP;
                // Release the ref we took at send time
                if (m.type == FdType::MemFd && m.handle)
                    MemFdUnref(static_cast<MemFdData*>(m.handle));
                else if (m.type == FdType::Vnode && m.handle) {
                    auto* vn = static_cast<Vnode*>(m.handle);
                    uint32_t prev2 = __atomic_fetch_sub(&vn->refCount, 1, __ATOMIC_ACQ_REL);
                    if (prev2 <= 1) VfsClose(vn);
                }
                else if (m.type == FdType::Socket && m.handle) {
                    int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(m.handle)) - 1;
                    brook::SockUnref(sockIdx);
                }
                // Other types (Pipe, UnixSocket, EventFd, etc.) aren't currently
                // ref-counted via snap-level ops; passing them isn't implemented
                // either so draining is a no-op.
            }
            kfree(q);
        };
        drainQueue(usd->incomingFds);
        drainQueue(usd->peerIncomingFds);

        kfree(usd);
        }
    }

    if (fde->type == FdType::DevDsp && fde->handle)
    {
        auto* dsp = static_cast<DspState*>(fde->handle);
        // Stop any in-flight playback — don't block on flush during teardown
        AudioStop();
        kfree(dsp->buffer);
        kfree(dsp);
    }

    FdFree(proc, static_cast<int>(fd));
    return 0;
}

// ---------------------------------------------------------------------------
// sys_pipe (22)
// ---------------------------------------------------------------------------

static int64_t sys_pipe(uint64_t pipefdAddr, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    auto* pipefd = reinterpret_cast<int32_t*>(pipefdAddr);
    if (!pipefd) return -EFAULT;

    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    // Allocate pipe buffer
    auto* pipe = static_cast<PipeBuffer*>(kmalloc(sizeof(PipeBuffer)));
    if (!pipe) return -ENOMEM;

    // Zero-init
    for (uint64_t i = 0; i < sizeof(PipeBuffer); i++)
        reinterpret_cast<uint8_t*>(pipe)[i] = 0;
    pipe->readers = 1;
    pipe->writers = 1;

    // Allocate read end (flags=0) and write end (flags=1)
    int readFd = FdAlloc(proc, FdType::Pipe, pipe);
    if (readFd < 0) { kfree(pipe); return -EMFILE; }
    proc->fds[readFd].flags = 0;  // read end
    proc->fds[readFd].statusFlags = 0;  // O_RDONLY

    int writeFd = FdAlloc(proc, FdType::Pipe, pipe);
    if (writeFd < 0)
    {
        FdFree(proc, readFd);
        kfree(pipe);
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

static int64_t sys_dup(uint64_t oldfd, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* old = FdGet(proc, static_cast<int>(oldfd));
    if (!old) return -EBADF;

    // Find lowest free fd
    int newfd = FdAlloc(proc, old->type, old->handle);
    if (newfd < 0) return -EMFILE;

    proc->fds[newfd].flags = old->flags;
    proc->fds[newfd].seekPos = old->seekPos;
    proc->fds[newfd].statusFlags = old->statusFlags;

    // Bump pipe refcount
    if (old->type == FdType::Pipe && old->handle)
    {
        auto* pipe = static_cast<PipeBuffer*>(old->handle);
        if (old->flags & 1)
            __atomic_fetch_add(&pipe->writers, 1, __ATOMIC_RELEASE);
        else
            __atomic_fetch_add(&pipe->readers, 1, __ATOMIC_RELEASE);
    }

    // Bump vnode refcount
    if (old->type == FdType::Vnode && old->handle)
        __atomic_fetch_add(&static_cast<Vnode*>(old->handle)->refCount, 1, __ATOMIC_RELEASE);

    // Bump socket refcount
    if (old->type == FdType::Socket && old->handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(old->handle)) - 1;
        brook::SockRef(sockIdx);
    }

    // Bump memfd refcount
    if (old->type == FdType::MemFd && old->handle)
        MemFdRef(static_cast<MemFdData*>(old->handle));

    // Bump unix socket refcount
    if (old->type == FdType::UnixSocket && old->handle)
    {
        auto* usd = static_cast<UnixSocketData*>(old->handle);
        __atomic_fetch_add(&usd->refCount, 1, __ATOMIC_RELEASE);
    }

    return newfd;
}

static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    if (oldfd == newfd) return static_cast<int64_t>(newfd);

    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* old = FdGet(proc, static_cast<int>(oldfd));
    if (!old) return -EBADF;

    if (newfd >= MAX_FDS) return -EBADF;

    // Close newfd if open
    FdEntry* existing = FdGet(proc, static_cast<int>(newfd));
    if (existing)
        sys_close(newfd, 0, 0, 0, 0, 0);

    // Copy the FD entry
    proc->fds[newfd].type = old->type;
    proc->fds[newfd].flags = old->flags;
    proc->fds[newfd].handle = old->handle;
    proc->fds[newfd].seekPos = old->seekPos;
    proc->fds[newfd].statusFlags = old->statusFlags;
    proc->fds[newfd].refCount = 1;

    // Bump pipe refcount
    if (old->type == FdType::Pipe && old->handle)
    {
        auto* pipe = static_cast<PipeBuffer*>(old->handle);
        if (old->flags & 1)
            __atomic_fetch_add(&pipe->writers, 1, __ATOMIC_RELEASE);
        else
            __atomic_fetch_add(&pipe->readers, 1, __ATOMIC_RELEASE);
    }

    // Bump vnode refcount
    if (old->type == FdType::Vnode && old->handle)
        __atomic_fetch_add(&static_cast<Vnode*>(old->handle)->refCount, 1, __ATOMIC_RELEASE);

    // Bump socket refcount
    if (old->type == FdType::Socket && old->handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(old->handle)) - 1;
        brook::SockRef(sockIdx);
    }

    // Bump memfd refcount
    if (old->type == FdType::MemFd && old->handle)
        MemFdRef(static_cast<MemFdData*>(old->handle));

    // Bump unix socket refcount
    if (old->type == FdType::UnixSocket && old->handle)
    {
        auto* usd = static_cast<UnixSocketData*>(old->handle);
        __atomic_fetch_add(&usd->refCount, 1, __ATOMIC_RELEASE);
    }

    return static_cast<int64_t>(newfd);
}

// sys_dup3 (292) — like dup2 with flags (O_CLOEXEC)
static int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                         uint64_t, uint64_t, uint64_t)
{
    if (oldfd == newfd) return -EINVAL; // dup3 differs from dup2 here

    int64_t ret = sys_dup2(oldfd, newfd, 0, 0, 0, 0);
    if (ret >= 0 && (flags & 0x80000)) // O_CLOEXEC
    {
        Process* proc = ProcessCurrent();
        if (proc) proc->fds[newfd].fdFlags = 1; // FD_CLOEXEC
    }
    return ret;
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
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;

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

static int64_t sys_brk(uint64_t newBreak, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOMEM;

    // brk(0) = query current break
    if (newBreak == 0)
    {
        DbgPrintf("sys_brk: query → 0x%lx\n", proc->programBreak);
        return static_cast<int64_t>(proc->programBreak);
    }

    // Validate within program break limits
    if (newBreak < proc->elf.programBreakLow)
        return static_cast<int64_t>(proc->programBreak);
    if (newBreak > proc->elf.programBreakHigh)
    {
        SerialPrintf("sys_brk: 0x%lx exceeds limit 0x%lx\n",
                     newBreak, proc->elf.programBreakHigh);
        return static_cast<int64_t>(proc->programBreak);
    }

    // Map any new pages needed between old and new break
    uint64_t oldPage = (proc->programBreak + 4095) & ~4095ULL;
    uint64_t newPage = (newBreak + 4095) & ~4095ULL;

    for (uint64_t addr = oldPage; addr < newPage; addr += 4096)
    {
        PhysicalAddress phys = PmmAllocPage(MemTag::User, proc->pid);
        if (!phys) return static_cast<int64_t>(proc->programBreak);

        if (!VmmMapPage(proc->pageTable, VirtualAddress(addr), phys,
                        VMM_WRITABLE | VMM_USER, MemTag::User, proc->pid))
            return static_cast<int64_t>(proc->programBreak);

        // Zero via kernel direct map (safe regardless of page permissions)
        auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        for (uint64_t b = 0; b < 4096; ++b) kp[b] = 0;
    }

    DbgPrintf("sys_brk: 0x%lx → 0x%lx\n", proc->programBreak, newBreak);
    proc->programBreak = newBreak;
    return static_cast<int64_t>(newBreak);
}

// ---------------------------------------------------------------------------
// sys_mmap (9)
// ---------------------------------------------------------------------------

enum MmapFlags : uint64_t {
    MAP_SHARED    = 0x01,
    MAP_PRIVATE   = 0x02,
    MAP_FIXED     = 0x10,
    MAP_ANONYMOUS = 0x20,
    MAP_DENYWRITE = 0x0800,
};

enum MmapProt : uint64_t {
    PROT_READ     = 0x1,
    PROT_WRITE    = 0x2,
    PROT_EXEC     = 0x4,
};

// Convert prot flags to VMM page flags.
static uint64_t ProtToVmmFlags(uint64_t prot)
{
    uint64_t f = VMM_USER;
    if (prot & PROT_WRITE) f |= VMM_WRITABLE;
    if (!(prot & PROT_EXEC)) f |= VMM_NO_EXEC;
    return f;
}

static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset)
{
    if (length == 0) return -EINVAL;

    Process* proc = ProcessCurrent();
    if (!proc) return -ENOMEM;

    uint64_t pages = (length + 4095) / 4096;
    uint64_t vmmFlags = ProtToVmmFlags(prot);

    // Threads in the same thread-group share the page table but each
    // Process struct has its own mmapNext.  Route all address-space
    // bookkeeping through the thread-group leader so parallel mmaps from
    // multiple threads don't hand out overlapping regions.  This is the
    // minimum we need for Go's multi-threaded runtime to work correctly.
    Process* leader = (proc->tgid && proc->tgid != proc->pid)
                    ? ProcessFindByPid(proc->tgid)
                    : nullptr;
    if (!leader) leader = proc;
    static SpinLock s_mmapLock;

    // Determine virtual address.  The critical section covers the
    // read-modify-write of leader->mmapNext AND any unmap-in-place
    // operation for MAP_FIXED, so two threads can't race on the same range.
    auto pickAddr = [&]() -> uint64_t {
        uint64_t lf = SpinLockAcquire(&s_mmapLock);
        uint64_t result = 0;
        if (flags & MAP_FIXED) {
            if (addr == 0) { SpinLockRelease(&s_mmapLock, lf); return 0; }
            for (uint64_t i = 0; i < pages; i++) {
                VirtualAddress va(addr + i * 4096);
                PhysicalAddress existing = VmmVirtToPhys(proc->pageTable, va);
                if (existing) {
                    VmmUnmapPage(proc->pageTable, va);
                    PmmFreePage(existing);
                }
            }
            result = addr;
        } else {
            uint64_t base = leader->mmapNext;
            if (addr >= USER_MMAP_BASE && addr + pages * 4096 <= USER_MMAP_END) {
                bool free = true;
                for (uint64_t i = 0; i < pages && free; i++)
                    if (VmmVirtToPhys(proc->pageTable, VirtualAddress(addr + i * 4096)))
                        free = false;
                if (free) base = addr;
            }
            if (base + pages * 4096 > USER_MMAP_END) {
                SpinLockRelease(&s_mmapLock, lf);
                return 0;
            }
            if (base >= leader->mmapNext)
                leader->mmapNext = base + pages * 4096;
            result = base;
        }
        SpinLockRelease(&s_mmapLock, lf);
        return result;
    };

    // Helper: allocate pages at a specific address.
    auto allocAt = [&](uint64_t vaddr, MemTag tag) -> bool {
        for (uint64_t i = 0; i < pages; i++) {
            PhysicalAddress phys = PmmAllocPage(tag, proc->pid);
            if (!phys) return false;
            if (!VmmMapPage(proc->pageTable, VirtualAddress(vaddr + i * 4096), phys,
                            vmmFlags, tag, proc->pid)) {
                PmmFreePage(phys);
                return false;
            }
        }
        return true;
    };

    // Helper: zero a user page via the kernel direct physical map (works
    // regardless of user-space page permissions).
    auto zeroUserPage = [&](uint64_t userVA) {
        PhysicalAddress phys = VmmVirtToPhys(proc->pageTable,
                                             VirtualAddress(userVA));
        if (!phys) return;
        auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        // PhysToVirt includes page offset; align to page start
        kp = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uint64_t>(kp) & ~0xFFFULL);
        for (uint64_t b = 0; b < 4096; ++b) kp[b] = 0;
    };

    if (flags & MAP_ANONYMOUS)
    {
        uint64_t vaddr = pickAddr();
        if (!vaddr) return -ENOMEM;

        // PROT_NONE anonymous mappings are pure reservations: do NOT back
        // them with physical pages.  Go's runtime reserves multi-GB arenas
        // up front with PROT_NONE and then mprotects sub-ranges to commit.
        // Allocating backing pages eagerly would immediately OOM, AND would
        // defeat the PROT_NONE guard-page contract (reads should fault).
        // sys_mprotect lazily backs pages when prot upgrades to non-zero.
        if (prot == 0)
            return static_cast<int64_t>(vaddr);

        if (!allocAt(vaddr, MemTag::User)) return -ENOMEM;

        // Zero via direct map (safe even for PROT_NONE / read-only pages).
        for (uint64_t p = 0; p < pages; ++p)
            zeroUserPage(vaddr + p * 4096);

        // Log large mmap allocations for debugging
        if (pages >= 32) {
            [[maybe_unused]] PhysicalAddress firstPhys = VmmVirtToPhys(proc->pageTable,
                                                       VirtualAddress(vaddr));
            [[maybe_unused]] PhysicalAddress lastPhys = VmmVirtToPhys(proc->pageTable,
                                                      VirtualAddress(vaddr + (pages-1) * 4096));
            DbgPrintf("mmap: pid=%u virt=0x%lx pages=%lu firstPhys=0x%lx lastPhys=0x%lx\n",
                         proc->pid, vaddr, pages,
                         firstPhys.raw() & ~0xFFFULL,
                         lastPhys.raw() & ~0xFFFULL);
        }

        return static_cast<int64_t>(vaddr);
    }

    // Device or file-backed mmap
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;

    // Framebuffer device: map physical framebuffer memory into user space
    if (fde->type == FdType::DevFramebuf)
    {
        uint64_t physBase;
        uint32_t fbW, fbH, fbStride;
        if (!TtyGetFramebufferPhys(&physBase, &fbW, &fbH, &fbStride))
            return -ENODEV;

        bool useVirtFb = (proc->fbVirtual != nullptr);
        uint64_t fbSize = useVirtFb
            ? proc->fbVirtualSize
            : static_cast<uint64_t>(fbStride) * fbH;

        uint64_t fbPages = (fbSize + 4095) / 4096;
        if (pages > fbPages) pages = fbPages;

        uint64_t vaddr = pickAddr();
        if (!vaddr) return -ENOMEM;

        for (uint64_t i = 0; i < pages; ++i)
        {
            PhysicalAddress pagePhys;
            if (useVirtFb)
            {
                auto kernVirt = VirtualAddress(
                    reinterpret_cast<uint64_t>(proc->fbVirtual) + i * 4096);
                pagePhys = VmmVirtToPhys(KernelPageTable, kernVirt);
            }
            else
            {
                pagePhys = PhysicalAddress(physBase + i * 4096);
            }

            if (!VmmMapPage(proc->pageTable, VirtualAddress(vaddr + i * 4096),
                            pagePhys,
                            VMM_WRITABLE | VMM_USER | VMM_NO_EXEC,
                            MemTag::Device, proc->pid))
            {
                SerialPrintf("sys_mmap: failed to map fb page %lu\n", i);
                return -ENOMEM;
            }
        }

        SerialPrintf("sys_mmap: fb mapped %lu pages at virt 0x%lx..0x%lx pid=%u (%s, vfb=%ux%u)\n",
                     pages, vaddr, vaddr + pages * 4096 - 1,
                     proc->pid,
                     useVirtFb ? "virtual" : "physical",
                     proc->fbVfbWidth, proc->fbVfbHeight);

        // Record the fb mmap so sys_munmap leaves the physical pages
        // alone (they're owned by proc->fbVirtual when useVirtFb=true,
        // and by the firmware framebuffer when not).
        for (uint32_t i = 0; i < Process::MAX_FB_MAPS; ++i) {
            if (proc->fbMaps[i].length == 0) {
                proc->fbMaps[i].vaddr  = vaddr;
                proc->fbMaps[i].length = pages * 4096;
                break;
            }
        }
        return static_cast<int64_t>(vaddr);
    }

    // MemFd-backed mmap (MAP_SHARED) — used by wl_shm.
    //
    // We DO NOT install PTEs eagerly. The user's virtual range is left
    // unmapped; the first access on each page traps to MemFdHandleUserFault
    // (idt.cpp), which calls MemFdGetOrAllocPage and installs the PTE. This
    // is the lazy/sparse semantics gdk-wayland depends on: ftruncate(700MB)
    // and mmap(700MB) consume only the index array (≈1.4 MB) until pages
    // are actually touched.
    if (fde->type == FdType::MemFd && fde->handle)
    {
        auto* mfd = static_cast<MemFdData*>(fde->handle);

        // Grow the page index array to cover [offset, offset+length). Pages
        // remain unbacked until first access.
        uint64_t needed = offset + length;
        if (needed > mfd->capacity) {
            if (!MemFdGrow(mfd, needed)) return -ENOMEM;
        }
        if (needed > mfd->size) mfd->size = needed;

        uint64_t vaddr = pickAddr();
        if (!vaddr) return -ENOMEM;

        // Record the mapping (with page-aligned mfd offset) so the PF handler
        // can resolve faults, and so munmap can tear it down without freeing
        // the mfd-owned phys pages. Take a VMA-lifetime ref on the MemFdData
        // so close(fd) doesn't drop the backing while the mapping is live.
        bool tracked = false;
        for (uint32_t i = 0; i < Process::MAX_MEMFD_MAPS; i++) {
            if (proc->memfdMaps[i].length == 0) {
                proc->memfdMaps[i].vaddr  = vaddr;
                proc->memfdMaps[i].length = pages * 4096;
                proc->memfdMaps[i].offset = offset;
                proc->memfdMaps[i].mfd    = mfd;
                MemFdRef(mfd);
                tracked = true;
                break;
            }
        }
        if (!tracked) return -ENOMEM;
        return static_cast<int64_t>(vaddr);
    }

    // File-backed mmap
    if (fde->type != FdType::Vnode || !fde->handle)
        return -EBADF;

    uint64_t vaddr = pickAddr();
    if (!vaddr) return -ENOMEM;
    if (!allocAt(vaddr, MemTag::User)) return -ENOMEM;

    // Zero then read file data (via direct map for permission safety)
    for (uint64_t pg = 0; pg < pages; ++pg)
        zeroUserPage(vaddr + pg * 4096);

    auto* vn = static_cast<Vnode*>(fde->handle);
    uint64_t readOff = offset;

    // Read file data page by page through the direct map.
    uint64_t bytesLeft = length;
    uint64_t dstOff = 0;
    while (bytesLeft > 0)
    {
        uint64_t chunk = (bytesLeft > 4096) ? 4096 : bytesLeft;
        uint64_t userVA = vaddr + dstOff;
        PhysicalAddress phys = VmmVirtToPhys(proc->pageTable,
                                             VirtualAddress(userVA & ~0xFFFULL));
        if (!phys) break;
        uint64_t pageOff = dstOff & 0xFFF;
        auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        kp = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uint64_t>(kp) & ~0xFFFULL);
        if (chunk > 4096 - pageOff) chunk = 4096 - pageOff;
        int got = VfsRead(vn, kp + pageOff, chunk, &readOff);
        if (got <= 0) break;
        dstOff += static_cast<uint64_t>(got);
        bytesLeft -= static_cast<uint64_t>(got);
    }

    // Diagnostic: for the code segment mmap (offset=0x28000), verify data
    // at the offset where the crash occurs (VA offset 0x179FD7 from vaddr).
    if (offset == 0x28000 && length > 0x179FD7)
    {
        SerialPrintf("mmap DIAG: vaddr=0x%lx addr=0x%lx offset=0x%lx len=%lu pages=%lu flags=0x%lx\n",
                     vaddr, addr, offset, length, pages, flags);
        uint64_t checkVA = vaddr + 0x179FD7;
        PhysicalAddress cp = VmmVirtToPhys(proc->pageTable,
                                           VirtualAddress(checkVA & ~0xFFFULL));
        if (cp) {
            auto* ck = reinterpret_cast<uint8_t*>(PhysToVirt(cp).raw());
            ck = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uint64_t>(ck) & ~0xFFFULL);
            uint64_t po = checkVA & 0xFFF;
            SerialPrintf("mmap VERIFY @VA 0x%lx (off=0x%lx): "
                         "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                         checkVA, offset + 0x179FD7,
                         ck[po], ck[po+1], ck[po+2], ck[po+3],
                         ck[po+4], ck[po+5], ck[po+6], ck[po+7]);
            // Expected: 69 09 00 00 4d 89 cb 48
        }
    }

    return static_cast<int64_t>(vaddr);
}

// ---------------------------------------------------------------------------
// sys_mprotect (10) -- stub
// ---------------------------------------------------------------------------

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot,
                             uint64_t, uint64_t, uint64_t)
{
    if (len == 0) return 0;
    if (addr & 0xFFF) return -EINVAL;

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    uint64_t pages = (len + 4095) / 4096;
    uint64_t newFlags = ProtToVmmFlags(prot);

    for (uint64_t i = 0; i < pages; ++i)
    {
        VirtualAddress va(addr + i * 4096);
        PhysicalAddress phys = VmmVirtToPhys(proc->pageTable, va);

        if (!phys)
        {
            // Page not mapped.  If the caller is upgrading to a real
            // protection (non-PROT_NONE), this is a lazy-backing request
            // — allocate and zero a fresh page.  This is the pattern Go's
            // runtime uses: reserve a large region with PROT_NONE, then
            // mprotect sub-ranges to commit.
            if (prot == 0)
                continue;

            PhysicalAddress newPhys = PmmAllocPage(MemTag::User, proc->pid);
            if (!newPhys) return -ENOMEM;

            if (!VmmMapPage(proc->pageTable, va, newPhys, newFlags,
                            MemTag::User, proc->pid))
            {
                PmmFreePage(newPhys);
                return -ENOMEM;
            }

            // Zero via kernel direct map (safe even for RO/NX pages).
            auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(newPhys).raw());
            kp = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uint64_t>(kp) & ~0xFFFULL);
            for (uint64_t b = 0; b < 4096; ++b) kp[b] = 0;
            continue;
        }

        // Remap the existing page with the new flags.
        VmmUnmapPage(proc->pageTable, va);
        VmmMapPage(proc->pageTable, va, phys, newFlags, MemTag::User, proc->pid);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// sys_munmap (11) -- stub
// ---------------------------------------------------------------------------

static int64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    if (addr & 0xFFF) return -EINVAL;
    if (length == 0) return 0;

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    uint64_t pages = (length + 4095) / 4096;

    // Is this range a MemFd mmap? If so, unmap only — the physical pages
    // are kernel heap pages owned by the MemFd, not by the process.
    bool isMemFd = false;
    MemFdData* unmappedMfd = nullptr;
    for (uint32_t i = 0; i < Process::MAX_MEMFD_MAPS; i++) {
        auto& m = proc->memfdMaps[i];
        if (m.length == 0) continue;
        if (addr >= m.vaddr && addr + length <= m.vaddr + m.length) {
            isMemFd = true;
            // Clear the slot if we're unmapping the whole range.
            if (addr == m.vaddr && length == m.length) {
                unmappedMfd = static_cast<MemFdData*>(m.mfd);
                m.vaddr = 0; m.length = 0; m.mfd = nullptr;
            }
            break;
        }
    }

    // Is this range a /dev/fb0 mmap?  If so, unmap only — the physical
    // pages are owned by proc->fbVirtual (or the firmware FB) and
    // freeing them here would corrupt the compositor's view of the VFB
    // and leave dangling references in fbVirtual's PT entries.
    bool isFbMap = false;
    for (uint32_t i = 0; i < Process::MAX_FB_MAPS; i++) {
        auto& m = proc->fbMaps[i];
        if (m.length == 0) continue;
        if (addr >= m.vaddr && addr + length <= m.vaddr + m.length) {
            isFbMap = true;
            if (addr == m.vaddr && length == m.length) {
                m.vaddr = 0; m.length = 0;
            }
            break;
        }
    }

    for (uint64_t i = 0; i < pages; ++i)
    {
        VirtualAddress va(addr + i * 4096);
        PhysicalAddress phys = VmmVirtToPhys(proc->pageTable, va);
        if (phys)
        {
            VmmUnmapPage(proc->pageTable, va);
            if (!isMemFd && !isFbMap) PmmFreePage(phys);
        }
    }

    if (unmappedMfd) MemFdUnref(unmappedMfd);

    return 0;
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

static int64_t sys_mremap(uint64_t old_addr, uint64_t old_size, uint64_t new_size,
                           uint64_t flags, uint64_t /*new_addr*/, uint64_t)
{
    constexpr uint64_t MREMAP_MAYMOVE = 1;

    if (old_addr & 0xFFF) return -EINVAL;
    if (old_size == 0 || new_size == 0) return -EINVAL;

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    uint64_t oldPages = (old_size + 4095) / 4096;
    uint64_t newPages = (new_size + 4095) / 4096;

    if (newPages == oldPages) return static_cast<int64_t>(old_addr);

    // Shrink: unmap the tail.
    if (newPages < oldPages)
    {
        for (uint64_t i = newPages; i < oldPages; ++i)
        {
            VirtualAddress va(old_addr + i * 4096);
            PhysicalAddress phys = VmmVirtToPhys(proc->pageTable, va);
            if (phys)
            {
                VmmUnmapPage(proc->pageTable, va);
                PmmFreePage(phys);
            }
        }
        return static_cast<int64_t>(old_addr);
    }

    // Grow.  Refuse if caller can't tolerate the move.
    if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM;

    // Allocate a fresh anonymous region of newPages.  We reuse sys_mmap to
    // avoid duplicating the address-picking + page-allocation logic.
    int64_t mmRet = sys_mmap(0, newPages * 4096,
                             3 /*PROT_READ|PROT_WRITE*/,
                             0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/,
                             static_cast<uint64_t>(-1), 0);
    if (mmRet < 0) return mmRet;
    uint64_t newAddr = static_cast<uint64_t>(mmRet);

    // Copy old contents into the new region.
    for (uint64_t i = 0; i < oldPages; ++i)
    {
        VirtualAddress src(old_addr + i * 4096);
        VirtualAddress dst(newAddr + i * 4096);
        PhysicalAddress sp = VmmVirtToPhys(proc->pageTable, src);
        PhysicalAddress dp = VmmVirtToPhys(proc->pageTable, dst);
        if (!sp || !dp) continue;
        // Both pages are mapped in the current page table (current process).
        // We can copy directly via the user virtual addresses.
        __builtin_memcpy(reinterpret_cast<void*>(newAddr + i * 4096),
                         reinterpret_cast<void*>(old_addr + i * 4096), 4096);
    }

    // Unmap the old range and free its pages.
    for (uint64_t i = 0; i < oldPages; ++i)
    {
        VirtualAddress va(old_addr + i * 4096);
        PhysicalAddress phys = VmmVirtToPhys(proc->pageTable, va);
        if (phys)
        {
            VmmUnmapPage(proc->pageTable, va);
            PmmFreePage(phys);
        }
    }

    DbgPrintf("sys_mremap: 0x%lx (%lu) -> 0x%lx (%lu)\n",
              old_addr, oldPages, newAddr, newPages);
    return static_cast<int64_t>(newAddr);
}

// ---------------------------------------------------------------------------
// sys_arch_prctl (158) -- TLS setup
// ---------------------------------------------------------------------------

static constexpr uint64_t ARCH_SET_FS    = 0x1002;
static constexpr uint64_t ARCH_GET_FS    = 0x1003;
static constexpr uint64_t ARCH_CET_STATUS = 0x3001;

static int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t,
                               uint64_t, uint64_t, uint64_t)
{
    switch (code)
    {
    case ARCH_SET_FS:
    {
        // Write FS base to MSR_FS_BASE (0xC0000100)
        uint32_t lo = static_cast<uint32_t>(addr);
        uint32_t hi = static_cast<uint32_t>(addr >> 32);
        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000100U));

        Process* proc = ProcessCurrent();
        if (proc)
        {
            proc->fsBase = addr;
            proc->savedCtx.fsBase = addr;
        }

        DbgPrintf("arch_prctl: SET_FS 0x%lx\n", addr);
        return 0;
    }
    case ARCH_GET_FS:
    {
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
    SerialPrintf("sys_exit: process exited with status %lu\n", status);

    // If the thread group leader calls plain sys_exit while sibling threads
    // are still alive, treat it as exit_group. Otherwise the leader's user
    // page table is destroyed under live threads and other threads' cached
    // threadLeader pointers become dangling references to freed memory
    // (later reads return heap free-poison 0xDFDF). Linux zombifies the
    // leader instead — for our hobby OS, killing the whole group is a
    // simpler invariant that keeps shared resources consistent.
    Process* proc = ProcessCurrent();
    if (proc && proc->pid == proc->tgid && !proc->isThread)
    {
        // SchedulerKillThreadGroup is a no-op if no siblings exist, so we
        // can call it unconditionally for the leader.
        SchedulerKillThreadGroup(proc->tgid, proc,
                                 static_cast<int>(status));
    }

    SchedulerExitCurrentProcess(static_cast<int>(status));
    // never reached
    return 0;
}

static int64_t sys_exit_group(uint64_t status, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (status != 0) {
        SerialPrintf("sys_exit_group: tgid %u exiting with status %lu\n",
                     proc ? proc->tgid : 0, status);
    } else {
        DbgPrintf("sys_exit_group: tgid %u exiting with status %lu\n",
                  proc ? proc->tgid : 0, status);
    }

    // Kill all other threads in this thread group so they don't linger
    // and cause use-after-free on shared resources after the leader exits.
    if (proc)
        SchedulerKillThreadGroup(proc->tgid, proc);

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
    const auto* iov = reinterpret_cast<const iovec*>(iovAddr);
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; ++i)
    {
        int64_t ret = sys_write(fd, iov[i].iov_base, iov[i].iov_len, 0, 0, 0);
        if (ret < 0) return ret;
        total += ret;
    }
    return total;
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
    auto* buf = reinterpret_cast<utsname*>(bufAddr);
    StrCopyN(buf->sysname,    "Brook",     65);
    StrCopyN(buf->nodename,   "brook",     65);
    StrCopyN(buf->release,    "6.0.0",     65);
    StrCopyN(buf->version,    "#1",        65);
    StrCopyN(buf->machine,    "x86_64",    65);
    StrCopyN(buf->domainname, "(none)",    65);
    return 0;
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

    // Snapshot ALL user registers from gs: IMMEDIATELY — before any function
    // that might enable interrupts (e.g. serial lock spin-wait).  If a timer
    // fires and the scheduler switches to another process that does a syscall,
    // gs: fields would be overwritten.  Capturing here is always safe because
    // FMASK cleared IF on syscall entry and nothing has re-enabled it yet.
    uint64_t userRip, userRsp, userRflags;
    uint64_t savedRbx, savedRbp, savedR12, savedR13, savedR14, savedR15;
    uint64_t savedRdi, savedRsi, savedRdx, savedR8, savedR9, savedR10;

    __asm__ volatile(
        "movq %%gs:48, %0\n\t"   // syscallUserRip
        "movq %%gs:56, %1\n\t"   // syscallUserRsp
        "movq %%gs:64, %2\n\t"   // syscallUserRflags
        : "=r"(userRip), "=r"(userRsp), "=r"(userRflags)
    );
    __asm__ volatile(
        "movq %%gs:72,  %0\n\t"
        "movq %%gs:80,  %1\n\t"
        "movq %%gs:88,  %2\n\t"
        "movq %%gs:96,  %3\n\t"
        "movq %%gs:104, %4\n\t"
        "movq %%gs:112, %5\n\t"
        : "=r"(savedRbx), "=r"(savedRbp),
          "=r"(savedR12), "=r"(savedR13),
          "=r"(savedR14), "=r"(savedR15)
    );
    __asm__ volatile(
        "movq %%gs:128, %0\n\t"
        "movq %%gs:136, %1\n\t"
        "movq %%gs:144, %2\n\t"
        "movq %%gs:152, %3\n\t"
        "movq %%gs:160, %4\n\t"
        "movq %%gs:168, %5\n\t"
        : "=r"(savedRdi), "=r"(savedRsi),
          "=r"(savedRdx), "=r"(savedR8),
          "=r"(savedR9), "=r"(savedR10)
    );

    Process* child = ProcessFork(parent, userRip, userRsp, userRflags);
    if (!child) return -ENOMEM;

    // Write captured register values into the child process struct.
    child->forkRbx = savedRbx;
    child->forkRbp = savedRbp;
    child->forkR12 = savedR12;
    child->forkR13 = savedR13;
    child->forkR14 = savedR14;
    child->forkR15 = savedR15;
    child->forkRdi = savedRdi;
    child->forkRsi = savedRsi;
    child->forkRdx = savedRdx;
    child->forkR8  = savedR8;
    child->forkR9  = savedR9;
    child->forkR10 = savedR10;

    SchedulerAddProcess(child);

    SerialPrintf("FORK: parent pid=%u tgid=%u -> child pid=%u tgid=%u\n",
                 parent->pid, parent->tgid, child->pid, child->tgid);
    return static_cast<int64_t>(child->pid);
}

static int64_t sys_clone(uint64_t flags, uint64_t newStack, uint64_t parentTidAddr,
                          uint64_t childTidAddr, uint64_t tlsAddr, uint64_t)
{
    // Clone flags used by musl pthread_create:
    //   CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
    //   CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID
    static constexpr uint64_t CLONE_THREAD         = 0x00010000;
    static constexpr uint64_t CLONE_SETTLS          = 0x00080000;
    static constexpr uint64_t CLONE_PARENT_SETTID   = 0x00100000;
    static constexpr uint64_t CLONE_CHILD_CLEARTID  = 0x00200000;

    Process* parent = ProcessCurrent();
    if (!parent) return -ENOSYS;

    // Snapshot ALL user registers from gs: before anything can clobber them.
    uint64_t userRip, userRsp, userRflags;
    uint64_t savedRbx, savedRbp, savedR12, savedR13, savedR14, savedR15;
    uint64_t savedRdi, savedRsi, savedRdx, savedR8, savedR9, savedR10;

    __asm__ volatile(
        "movq %%gs:48, %0\n\t"
        "movq %%gs:56, %1\n\t"
        "movq %%gs:64, %2\n\t"
        : "=r"(userRip), "=r"(userRsp), "=r"(userRflags)
    );
    __asm__ volatile(
        "movq %%gs:72,  %0\n\t"  "movq %%gs:80,  %1\n\t"
        "movq %%gs:88,  %2\n\t"  "movq %%gs:96,  %3\n\t"
        "movq %%gs:104, %4\n\t"  "movq %%gs:112, %5\n\t"
        : "=r"(savedRbx), "=r"(savedRbp),
          "=r"(savedR12), "=r"(savedR13),
          "=r"(savedR14), "=r"(savedR15)
    );
    __asm__ volatile(
        "movq %%gs:128, %0\n\t"  "movq %%gs:136, %1\n\t"
        "movq %%gs:144, %2\n\t"  "movq %%gs:152, %3\n\t"
        "movq %%gs:160, %4\n\t"  "movq %%gs:168, %5\n\t"
        : "=r"(savedRdi), "=r"(savedRsi),
          "=r"(savedRdx), "=r"(savedR8),
          "=r"(savedR9), "=r"(savedR10)
    );

    // If caller provided a new stack, use it for the child.
    if (newStack)
        userRsp = newStack;

    Process* child;

    if (flags & CLONE_THREAD)
    {
        // Thread creation: share address space
        uint64_t tls = (flags & CLONE_SETTLS) ? tlsAddr : parent->fsBase;
        child = ProcessCreateThread(parent, userRip, userRsp, userRflags, tls);
        if (!child) return -ENOMEM;

        // CLONE_PARENT_SETTID: write child TID to parent's user space
        if ((flags & CLONE_PARENT_SETTID) && parentTidAddr)
        {
            auto* tidPtr = reinterpret_cast<volatile int32_t*>(parentTidAddr);
            *tidPtr = static_cast<int32_t>(child->pid);
        }

        // CLONE_CHILD_CLEARTID: store address for thread exit cleanup
        if (flags & CLONE_CHILD_CLEARTID)
            child->clearChildTid = childTidAddr;
    }
    else
    {
        // Fork: copy address space
        child = ProcessFork(parent, userRip, userRsp, userRflags);
        if (!child) return -ENOMEM;
    }

    child->forkRbx = savedRbx;  child->forkRbp = savedRbp;
    child->forkR12 = savedR12;  child->forkR13 = savedR13;
    child->forkR14 = savedR14;  child->forkR15 = savedR15;
    child->forkRdi = savedRdi;  child->forkRsi = savedRsi;
    child->forkRdx = savedRdx;  child->forkR8  = savedR8;
    child->forkR9  = savedR9;   child->forkR10 = savedR10;

    SchedulerAddProcess(child);

    SerialPrintf("CLONE: parent pid=%u tgid=%u -> child pid=%u tgid=%u flags=0x%lx %s rip=0x%lx rsp=0x%lx\n",
                 parent->pid, parent->tgid, child->pid, child->tgid, flags,
                 (flags & CLONE_THREAD) ? "THREAD" : "FORK", userRip, userRsp);
    return static_cast<int64_t>(child->pid);
}

static int64_t sys_vfork(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    // vfork is like fork but parent blocks until child exec/exit.
    // For now, implement as plain fork (parent doesn't block).
    return sys_fork(0, 0, 0, 0, 0, 0);
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

    const CloneArgs* ca = reinterpret_cast<const CloneArgs*>(argsPtr);

    // Stack: clone3 passes base+size; clone expects the top (stack grows down)
    uint64_t stackTop = ca->stack;
    if (stackTop && ca->stack_size)
        stackTop = ca->stack + ca->stack_size;

    // Delegate to sys_clone with the same argument layout:
    //   flags, newStack, parentTidAddr, childTidAddr, tlsAddr
    return sys_clone(ca->flags, stackTop, ca->parent_tid, ca->child_tid, ca->tls, 0);
}

// ---------------------------------------------------------------------------
// sys_wait4 (61)
// ---------------------------------------------------------------------------
// Wait for a child process to terminate. Supports pid=-1 (any child)
// or a specific child PID. WNOHANG (options & 1) returns 0 if no child
// has exited yet instead of blocking.

static constexpr uint64_t WNOHANG    = 1;
static constexpr uint64_t WUNTRACED  = 2;
[[maybe_unused]] static constexpr uint64_t WCONTINUED = 8;

static int64_t sys_wait4(uint64_t pidArg, uint64_t statusAddr, uint64_t options,
                          uint64_t, uint64_t, uint64_t)
{
    Process* parent = ProcessCurrent();
    if (!parent) return -ENOSYS;

    int64_t targetPid = static_cast<int64_t>(static_cast<int32_t>(pidArg));
    // pid < -1 means wait for any child in process group |pid| — treat as -1
    if (targetPid < -1) targetPid = -1;
    // pid == 0 means wait for any child in same process group — treat as -1
    if (targetPid == 0) targetPid = -1;

    // Spin until a terminated (or stopped, if WUNTRACED) child is found
    for (;;)
    {
        // Check for stopped children if WUNTRACED
        if (options & WUNTRACED)
        {
            Process* stopped = SchedulerFindStoppedChild(parent->pid, targetPid);
            if (stopped)
            {
                int32_t childPid = stopped->pid;
                if (statusAddr)
                {
                    // Linux wait status for stopped: (signum << 8) | 0x7F
                    auto* wstatus = reinterpret_cast<int32_t*>(statusAddr);
                    *wstatus = (20 << 8) | 0x7F; // SIGTSTP (20)
                }
                // Mark as reported so we don't report it again
                stopped->stopReported = true;
                return static_cast<int64_t>(childPid);
            }
        }

        Process* child = SchedulerFindTerminatedChild(parent->pid, targetPid);
        if (child)
        {
            int32_t childPid = child->pid;
            int32_t childStatus = child->exitStatus;

            if (statusAddr)
            {
                // Linux wait status encoding: (status & 0xFF) << 8 for normal exit
                auto* wstatus = reinterpret_cast<int32_t*>(statusAddr);
                if (childStatus >= 128) // Killed by signal
                    *wstatus = (childStatus - 128); // Signal number in low byte
                else
                    *wstatus = (childStatus & 0xFF) << 8;
            }

            SchedulerReapChild(child);
            // Diagnostic: log the user-mode RIP we'll return to — catches
            // cases where a stray corruption lands control flow off in the weeds.
            {
                uint64_t retRip = 0, retRsp = 0;
                __asm__ volatile("movq %%gs:48, %0\n\t"
                                 "movq %%gs:56, %1"
                                 : "=r"(retRip), "=r"(retRsp));
                DbgPrintf("WAIT4: pid=%u reaped child=%u status=0x%x -> user RIP=0x%lx RSP=0x%lx\n",
                          parent->pid, childPid, childStatus, retRip, retRsp);
            }
            return static_cast<int64_t>(childPid);
        }

        if (options & WNOHANG)
            return 0;

        extern volatile uint64_t g_lapicTickCount;
        parent->wakeupTick = g_lapicTickCount + 5;
        SchedulerBlock(parent);
        if (HasPendingSignals())
        {
            // SIGCHLD is expected during wait — clear it and retry
            // instead of returning EINTR. Only return EINTR for other signals.
            uint64_t pending = parent->sigPending & ~parent->sigMask;
            uint64_t sigchldBit = (1ULL << (17 - 1)); // SIGCHLD = 17
            if ((pending & ~sigchldBit) != 0)
                return -EINTR;
            // Only SIGCHLD pending — clear it and retry the wait loop
            parent->sigPending &= ~sigchldBit;
        }
    }
}

// ---------------------------------------------------------------------------
// sys_execve (59)
// ---------------------------------------------------------------------------
// Replace the current process image with a new ELF binary.
// rdi = pathname, rsi = argv[], rdx = envp[]
// This function does NOT return on success — it enters the new program.

static int64_t sys_execve(uint64_t pathAddr, uint64_t argvAddr, uint64_t envpAddr,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    const char* userPath = reinterpret_cast<const char*>(pathAddr);
    if (!userPath) return -EFAULT;

    // --- Copy filename from user memory into kernel buffer ---
    char kPath[256];
    {
        uint32_t i = 0;
        while (i < 255 && userPath[i]) { kPath[i] = userPath[i]; i++; }
        kPath[i] = '\0';
    }

    // Resolve path: try as-is, then /boot/BIN/<UPPER>, then /boot/<UPPER>
    char resolvedPath[256];
    bool found = false;

    // Try path as-is (or with CWD prefix)
    const char* lookupPath = kPath;
    if (kPath[0] != '/' && proc->cwd[0] != '\0')
    {
        uint32_t ci = 0;
        for (uint32_t j = 0; proc->cwd[j] && ci < 250; ++j)
            resolvedPath[ci++] = proc->cwd[j];
        if (ci > 0 && resolvedPath[ci - 1] != '/')
            resolvedPath[ci++] = '/';
        for (uint32_t j = 0; kPath[j] && ci < 254; ++j)
            resolvedPath[ci++] = kPath[j];
        resolvedPath[ci] = '\0';
        lookupPath = resolvedPath;
    }

    {
        VnodeStat st;
        if (VfsStatPath(lookupPath, &st) == 0 && !st.isDir)
            found = true;
    }

    if (!found)
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

            VnodeStat st;
            if (VfsStatPath(tryPath, &st) == 0 && !st.isDir)
            {
                for (uint32_t k = 0; k <= pLen; ++k) resolvedPath[k] = tryPath[k];
                lookupPath = resolvedPath;
                found = true;
            }
        }
    }

    // Busybox fallback: if not found, try running busybox with the command name
    // as argv[0]. Busybox reads argv[0] to determine which applet to run.
    bool busyboxFallback = false;
    if (!found)
    {
        const char* cmdName = kPath;
        for (const char* p = kPath; *p; ++p)
            if (*p == '/') cmdName = p + 1;
        (void)cmdName;

        VnodeStat st;
        if (VfsStatPath("/boot/BIN/BUSYBOX", &st) == 0 && !st.isDir)
        {
            const char* bbPath = "/boot/BIN/BUSYBOX";
            uint32_t k = 0;
            while (bbPath[k]) { resolvedPath[k] = bbPath[k]; k++; }
            resolvedPath[k] = '\0';
            lookupPath = resolvedPath;
            found = true;
            busyboxFallback = true;
            DbgPrintf("sys_execve: busybox fallback for '%s'\n", cmdName);
            SerialPrintf("sys_execve: busybox fallback — original='%s' resolved='%s'\n",
                         kPath, lookupPath);
        }
    }

    if (!found)
    {
        DbgPrintf("sys_execve: not found: %s\n", kPath);
        return -ENOENT;
    }

    // --- Copy argv from user memory ---
    static constexpr int MAX_EXEC_ARGS = 64;
    static constexpr int MAX_EXEC_ENVP = 64;
    static constexpr uint64_t MAX_STR_LEN = 4096;

    const char* kArgv[MAX_EXEC_ARGS];
    // Kernel-side string storage (simple: one big buffer)
    static constexpr uint64_t ARG_BUF_SIZE = 16384;
    char argBuf[ARG_BUF_SIZE];
    uint32_t argBufPos = 0;
    int argc = 0;

    if (argvAddr)
    {
        auto** userArgv = reinterpret_cast<const char**>(argvAddr);
        for (int i = 0; i < MAX_EXEC_ARGS - 1; i++)
        {
            const char* arg = userArgv[i];
            if (!arg) break;

            uint32_t len = 0;
            while (len < MAX_STR_LEN && arg[len]) len++;
            if (argBufPos + len + 1 > ARG_BUF_SIZE) break;

            for (uint32_t j = 0; j < len; j++)
                argBuf[argBufPos + j] = arg[j];
            argBuf[argBufPos + len] = '\0';
            kArgv[argc] = &argBuf[argBufPos];
            argBufPos += len + 1;
            argc++;
        }
    }

    // If no argv provided, use the path as argv[0]
    if (argc == 0)
    {
        uint32_t len = 0;
        while (kPath[len]) len++;
        if (len + 1 <= ARG_BUF_SIZE)
        {
            for (uint32_t j = 0; j <= len; j++)
                argBuf[j] = kPath[j];
            kArgv[0] = argBuf;
            argBufPos = len + 1;
            argc = 1;
        }
    }

    // --- Copy envp from user memory ---
    const char* kEnvp[MAX_EXEC_ENVP];
    int envc = 0;
    char envBuf[ARG_BUF_SIZE];
    uint32_t envBufPos = 0;

    if (envpAddr)
    {
        auto** userEnvp = reinterpret_cast<const char**>(envpAddr);
        for (int i = 0; i < MAX_EXEC_ENVP - 1; i++)
        {
            const char* env = userEnvp[i];
            if (!env) break;

            uint32_t len = 0;
            while (len < MAX_STR_LEN && env[len]) len++;
            if (envBufPos + len + 1 > ARG_BUF_SIZE) break;

            for (uint32_t j = 0; j < len; j++)
                envBuf[envBufPos + j] = env[j];
            envBuf[envBufPos + len] = '\0';
            kEnvp[envc] = &envBuf[envBufPos];
            envBufPos += len + 1;
            envc++;
        }
    }

    // --- Load ELF from VFS ---
    Vnode* vn = VfsOpen(lookupPath, 0);
    if (!vn)
    {
        DbgPrintf("sys_execve: VfsOpen failed: %s\n", lookupPath);
        return -ENOENT;
    }

    // 32 MB accommodates most real binaries, including statically-linked
    // Go executables which routinely run 10-20 MB.  The buffer is freed
    // immediately after ELF loading, so it's only a transient cost.
    constexpr uint64_t MAX_ELF_SIZE = 32 * 1024 * 1024;
    constexpr uint64_t ELF_BUF_PAGES = MAX_ELF_SIZE / 4096;

    VirtualAddress bufAddr = VmmAllocPages(ELF_BUF_PAGES,
        VMM_WRITABLE, MemTag::Heap, KernelPid);
    if (!bufAddr)
    {
        VfsClose(vn);
        return -ENOMEM;
    }

    auto* elfBuf = reinterpret_cast<uint8_t*>(bufAddr.raw());
    uint64_t elfSize = 0;
    uint64_t offset = 0;
    while (elfSize < MAX_ELF_SIZE)
    {
        int ret = VfsRead(vn, elfBuf + elfSize, 4096, &offset);
        if (ret <= 0) break;
        elfSize += static_cast<uint64_t>(ret);
    }
    VfsClose(vn);

    if (elfSize < 64) // Too small to be a valid ELF
    {
        VmmFreePages(bufAddr, ELF_BUF_PAGES);
        DbgPrintf("sys_execve: file too small (%lu bytes)\n", elfSize);
        return -ENOEXEC;
    }

    DbgPrintf("sys_execve: loaded '%s' (%lu bytes) for pid %u\n",
              lookupPath, elfSize, proc->pid);

    // --- Shebang (#!) support ---
    // If the file starts with "#!", extract the interpreter path and re-exec.
    if (elfSize >= 2 && elfBuf[0] == '#' && elfBuf[1] == '!')
    {
        // Parse interpreter line: "#!<interp> [arg]\n"
        // Do this BEFORE freeing the buffer.
        const char* line = reinterpret_cast<const char*>(elfBuf);
        const char* lineEnd = line + (elfSize < 256 ? elfSize : 256);
        const char* p = line + 2;

        // Skip whitespace
        while (p < lineEnd && (*p == ' ' || *p == '\t')) p++;

        // Extract interpreter path
        char interpPath[128];
        uint32_t interpLen = 0;
        while (p < lineEnd && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\0')
        {
            if (interpLen < sizeof(interpPath) - 1)
                interpPath[interpLen++] = *p;
            p++;
        }
        interpPath[interpLen] = '\0';

        // Skip whitespace to optional arg
        while (p < lineEnd && (*p == ' ' || *p == '\t')) p++;

        char interpArg[128];
        uint32_t argLen = 0;
        while (p < lineEnd && *p != '\n' && *p != '\r' && *p != '\0')
        {
            if (argLen < sizeof(interpArg) - 1)
                interpArg[argLen++] = *p;
            p++;
        }
        interpArg[argLen] = '\0';

        // Done reading elfBuf — free it now
        VmmFreePages(bufAddr, ELF_BUF_PAGES);

        SerialPrintf("sys_execve: shebang interp='%s' arg='%s' script='%s'\n",
                     interpPath, interpArg, lookupPath);

        // Build new argv: [interp, interpArg?, script, original_argv[1:]]
        static constexpr int MAX_SHEBANG_ARGS = 34;
        const char* newArgv[MAX_SHEBANG_ARGS];
        int newArgc = 0;
        newArgv[newArgc++] = interpPath;
        if (argLen > 0)
            newArgv[newArgc++] = interpArg;
        newArgv[newArgc++] = lookupPath; // the script itself

        // Append original argv[1..] (skip argv[0] which was the script)
        for (int i = 1; i < argc && newArgc < MAX_SHEBANG_ARGS - 1; i++)
            newArgv[newArgc++] = kArgv[i];
        newArgv[newArgc] = nullptr;

        // Build pointer array and recurse
        uint64_t newArgvPtrs[MAX_SHEBANG_ARGS];
        for (int i = 0; i < newArgc; i++)
            newArgvPtrs[i] = reinterpret_cast<uint64_t>(newArgv[i]);
        newArgvPtrs[newArgc] = 0;

        return sys_execve(reinterpret_cast<uint64_t>(interpPath),
                          reinterpret_cast<uint64_t>(newArgvPtrs),
                          envpAddr, 0, 0, 0);
    }

    // --- Replace the process image ---
    uint64_t newStackPtr = 0;
    uint64_t newEntry = ProcessExec(proc, elfBuf, elfSize,
                                     argc, kArgv, envc, kEnvp,
                                     &newStackPtr);

    // Free ELF buffer
    VmmFreePages(bufAddr, ELF_BUF_PAGES);

    if (!newEntry)
    {
        SerialPrintf("sys_execve: ProcessExec failed for pid %u\n", proc->pid);
        // Process is in a broken state — kill it
        SchedulerExitCurrentProcess(-1);
        __builtin_unreachable();
    }

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

    SerialPrintf("sys_execve: entering user mode for '%s' entry=0x%lx sp=0x%lx\n",
                 proc->name, newEntry, newStackPtr);

    // Validate that entry point and stack are mapped in the new address space.
    {
        PhysicalAddress entryPhys = VmmVirtToPhys(proc->pageTable, VirtualAddress(newEntry));
        PhysicalAddress stackPhys = VmmVirtToPhys(proc->pageTable, VirtualAddress(newStackPtr & ~0xFFFULL));
        if (!entryPhys || !stackPhys) {
            SerialPrintf("sys_execve: FATAL unmapped pages! entry phys=0x%lx stack phys=0x%lx\n",
                         entryPhys.raw(), stackPhys.raw());
            SchedulerExitCurrentProcess(-1);
            __builtin_unreachable();
        }
    }

    // --- Switch to new address space and enter user mode ---
    // Load the new page table
    __asm__ volatile("mov %0, %%cr3" : : "r"(proc->pageTable.pml4.raw()) : "memory");

    // Set FS base for the new TLS
    if (proc->fsBase)
    {
        uint64_t lo = proc->fsBase & 0xFFFFFFFF;
        uint64_t hi = proc->fsBase >> 32;
        __asm__ volatile(
            "mov $0xC0000100, %%ecx\n\t"
            "mov %0, %%eax\n\t"
            "mov %1, %%edx\n\t"
            "wrmsr\n\t"
            : : "r"(static_cast<uint32_t>(lo)),
                "r"(static_cast<uint32_t>(hi))
            : "ecx", "eax", "edx"
        );
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
// sys_clock_gettime (228) / sys_gettimeofday (96)
// ---------------------------------------------------------------------------

// LAPIC fires every 1ms — monotonic tick counter.
extern volatile uint64_t g_lapicTickCount; // defined in apic.cpp

struct timespec {
    int64_t  tv_sec;
    int64_t  tv_nsec;
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

// CLOCK_REALTIME=0  CLOCK_MONOTONIC=1  CLOCK_MONOTONIC_RAW=4  CLOCK_BOOTTIME=7
static int64_t sys_clock_gettime(uint64_t clockid, uint64_t tsAddr, uint64_t,
                                  uint64_t, uint64_t, uint64_t)
{
    auto* ts = reinterpret_cast<timespec*>(tsAddr);
    if (!ts) return -EFAULT;

    // Under KVM, the pvclock source gives us nanosecond precision directly.
    // LAPIC-derived ms is only ms-accurate and drifts between RTC
    // re-anchors, so prefer pvclock whenever it's live.
    if (KvmClockAvailable())
    {
        uint64_t ns = KvmClockReadNs();
        if (clockid == 0) // CLOCK_REALTIME
        {
            // Wall-clock: RtcNow() gives the epoch base, pvclock the sub-sec.
            uint64_t epoch = RtcNow();
            ts->tv_sec  = static_cast<int64_t>(epoch);
            ts->tv_nsec = static_cast<int64_t>(ns % 1000000000ULL);
        }
        else
        {
            ts->tv_sec  = static_cast<int64_t>(ns / 1000000000ULL);
            ts->tv_nsec = static_cast<int64_t>(ns % 1000000000ULL);
        }
        return 0;
    }

    if (clockid == 0) // CLOCK_REALTIME — wall-clock via RTC
    {
        uint64_t epoch = RtcNow();
        uint64_t ms = g_lapicTickCount;
        ts->tv_sec  = static_cast<int64_t>(epoch);
        ts->tv_nsec = static_cast<int64_t>((ms % 1000) * 1000000);
    }
    else // CLOCK_MONOTONIC and variants — boot-relative
    {
        uint64_t ms = g_lapicTickCount;
        ts->tv_sec  = static_cast<int64_t>(ms / 1000);
        ts->tv_nsec = static_cast<int64_t>((ms % 1000) * 1000000);
    }
    return 0;
}

static int64_t sys_gettimeofday(uint64_t tvAddr, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    auto* tv = reinterpret_cast<timeval*>(tvAddr);
    if (!tv) return -EFAULT;

    uint64_t epoch = RtcNow();
    if (KvmClockAvailable())
    {
        uint64_t ns = KvmClockReadNs();
        tv->tv_sec  = static_cast<int64_t>(epoch);
        tv->tv_usec = static_cast<int64_t>((ns / 1000) % 1000000ULL);
    }
    else
    {
        uint64_t ms = g_lapicTickCount;
        tv->tv_sec  = static_cast<int64_t>(epoch);
        tv->tv_usec = static_cast<int64_t>((ms % 1000) * 1000);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sys_time (201) — returns seconds since epoch
// ---------------------------------------------------------------------------

static int64_t sys_time(uint64_t tloc, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    uint64_t epoch = RtcNow();
    if (tloc) {
        auto* p = reinterpret_cast<int64_t*>(tloc);
        *p = static_cast<int64_t>(epoch);
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
    auto* req = reinterpret_cast<const timespec*>(reqAddr);
    if (!req) return -EFAULT;

    uint64_t sleepMs = static_cast<uint64_t>(req->tv_sec) * 1000 +
                       static_cast<uint64_t>(req->tv_nsec) / 1000000;

    if (sleepMs > 0)
    {
        // Block the process and let the scheduler wake us up.
        Process* proc = ProcessCurrent();
        proc->wakeupTick = g_lapicTickCount + sleepMs;
        SchedulerBlock(proc);
        // When we return here, the scheduler has woken us up.
        if (HasPendingSignals())
        {
            // Fill remainder if caller wants it
            if (remAddr)
            {
                auto* rem = reinterpret_cast<timespec*>(remAddr);
                // Approximate remaining time (may be 0 if wakeup was near end)
                uint64_t elapsed = g_lapicTickCount - (proc->wakeupTick - sleepMs);
                uint64_t remainMs = (elapsed < sleepMs) ? (sleepMs - elapsed) : 0;
                rem->tv_sec = static_cast<int64_t>(remainMs / 1000);
                rem->tv_nsec = static_cast<int64_t>((remainMs % 1000) * 1000000);
            }
            return -EINTR;
        }
    }

    if (remAddr)
    {
        auto* rem = reinterpret_cast<timespec*>(remAddr);
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

static int64_t sys_clock_nanosleep(uint64_t, uint64_t, uint64_t reqAddr,
                                    uint64_t remAddr, uint64_t, uint64_t)
{
    return sys_nanosleep(reqAddr, remAddr, 0, 0, 0, 0);
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
        uint64_t now = g_lapicTickCount;
        if (proc->alarmTick > now)
            prevRemaining = (proc->alarmTick - now + 999) / 1000; // round up
    }

    if (seconds == 0)
        proc->alarmTick = 0; // Cancel alarm
    else
        proc->alarmTick = g_lapicTickCount + seconds * 1000;

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
        proc->wakeupTick = g_lapicTickCount + 100; // recheck every 100ms
        SchedulerBlock(proc);
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

    uint64_t newMask = *reinterpret_cast<const uint64_t*>(maskAddr);

    // Save current mask and replace with new one
    proc->sigSavedMask = proc->sigMask;
    proc->sigMask = newMask;

    // Block until a signal that isn't blocked is pending
    while (!HasPendingSignals())
    {
        proc->wakeupTick = g_lapicTickCount + 100;
        SchedulerBlock(proc);
    }

    // Restore original mask (signal handler will save/restore it too)
    proc->sigMask = proc->sigSavedMask;
    return -EINTR;
}

// ---------------------------------------------------------------------------
// sys_access (21)
// ---------------------------------------------------------------------------

static bool BusyboxStatFallback(const char* path, VnodeStat* vs);

static int64_t sys_access(uint64_t pathAddr, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path) return -EFAULT;

    // Try to open the file to check existence
    Vnode* vn = VfsOpen(path, 0);
    if (!vn)
    {
        VnodeStat vs;
        if (!BusyboxStatFallback(path, &vs))
            return -ENOENT;
        return 0;
    }
    VfsClose(vn);
    return 0;
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

static int64_t sys_ioctl(uint64_t fd, uint64_t cmd_raw, uint64_t arg,
                          uint64_t, uint64_t, uint64_t)
{
    // ioctl cmd is a 32-bit value; mask off sign-extension from syscall ABI
    uint64_t cmd = cmd_raw & 0xFFFFFFFFULL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;

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
                                   winX + static_cast<int16_t>(WM_BORDER_WIDTH),
                                   winY + static_cast<int16_t>(WM_TITLE_BAR_HEIGHT + WM_BORDER_WIDTH),
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
            auto* info = reinterpret_cast<FbVarScreeninfo*>(arg);
            auto* raw = reinterpret_cast<uint8_t*>(info);
            for (uint64_t i = 0; i < sizeof(FbVarScreeninfo); ++i) raw[i] = 0;

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
            auto* info = reinterpret_cast<FbFixScreeninfo*>(arg);
            auto* raw = reinterpret_cast<uint8_t*>(info);
            for (uint64_t i = 0; i < sizeof(FbFixScreeninfo); ++i) raw[i] = 0;

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
            auto* ws = reinterpret_cast<uint16_t*>(arg);
            Terminal* term = TerminalFindByProcess(proc);
            ws[0] = term ? static_cast<uint16_t>(term->rows) : 25;
            ws[1] = term ? static_cast<uint16_t>(term->cols) : 80;
            ws[2] = term ? static_cast<uint16_t>(term->vfbW) : 0;
            ws[3] = term ? static_cast<uint16_t>(term->vfbH) : 0;
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
    bool isTtyFd = (fde->type == FdType::DevKeyboard) ||
                   (fde->type == FdType::Vnode && !fde->handle) ||
                   (fde->type == FdType::DevTty) ||
                   (fde->type == FdType::Pipe && (fde->flags & 0x04)) ||
                   ((fd <= 2) && fde->type != FdType::Pipe &&
                                 fde->type != FdType::Socket);
    if (isTtyFd && cmd == 0x5401)
    {
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
        auto* ws = reinterpret_cast<winsize*>(arg);
        Terminal* term = TerminalFindByProcess(proc);
        ws->ws_row = term ? static_cast<uint16_t>(term->rows) : 25;
        ws->ws_col = term ? static_cast<uint16_t>(term->cols) : 80;
        ws->ws_xpixel = term ? static_cast<uint16_t>(term->vfbW) : 0;
        ws->ws_ypixel = term ? static_cast<uint16_t>(term->vfbH) : 0;
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

        // SNDCTL_DSP_RESET — stop playback, reset buffer
        if (cmd == SNDCTL_DSP_RESET)
        {
            AudioStop();
            dsp->bufferOffset = 0;
            return 0;
        }

        // SNDCTL_DSP_SPEED — set/get sample rate
        if (cmd == SNDCTL_DSP_SPEED)
        {
            auto* rate = reinterpret_cast<int*>(arg);
            if (*rate > 0) dsp->sampleRate = static_cast<uint32_t>(*rate);
            *rate = static_cast<int>(dsp->sampleRate);
            return 0;
        }

        // SNDCTL_DSP_SETFMT — set/get audio format
        if (cmd == SNDCTL_DSP_SETFMT)
        {
            auto* fmt = reinterpret_cast<int*>(arg);
            if (*fmt == AFMT_S16_LE)
                dsp->bitsPerSample = 16;
            else if (*fmt == AFMT_U8)
                dsp->bitsPerSample = 8;
            // Report back current format
            *fmt = (dsp->bitsPerSample == 16) ? AFMT_S16_LE : AFMT_U8;
            return 0;
        }

        // SNDCTL_DSP_CHANNELS — set/get channel count
        if (cmd == SNDCTL_DSP_CHANNELS)
        {
            auto* ch = reinterpret_cast<int*>(arg);
            if (*ch >= 1 && *ch <= 2) dsp->channels = static_cast<uint8_t>(*ch);
            *ch = dsp->channels;
            return 0;
        }

        // SNDCTL_DSP_STEREO — set mono(0)/stereo(1)
        if (cmd == SNDCTL_DSP_STEREO)
        {
            auto* stereo = reinterpret_cast<int*>(arg);
            dsp->channels = (*stereo) ? 2 : 1;
            *stereo = (dsp->channels == 2) ? 1 : 0;
            return 0;
        }

        // SNDCTL_DSP_GETOSPACE — report available buffer space
        if (cmd == SNDCTL_DSP_GETOSPACE)
        {
            struct audio_buf_info { int fragments; int fragstotal; int fragsize; int bytes; };
            auto* info = reinterpret_cast<audio_buf_info*>(arg);
            uint32_t avail = dsp->bufferSize - dsp->bufferOffset;
            info->fragsize   = static_cast<int>(dsp->fragmentSize);
            info->fragstotal = static_cast<int>(dsp->bufferSize / dsp->fragmentSize);
            info->fragments  = static_cast<int>(avail / dsp->fragmentSize);
            info->bytes      = static_cast<int>(avail);
            return 0;
        }

        // SNDCTL_DSP_GETCAPS — report capabilities
        if (cmd == SNDCTL_DSP_GETCAPS)
        {
            auto* caps = reinterpret_cast<int*>(arg);
            *caps = DSP_CAP_TRIGGER;
            return 0;
        }

        // SNDCTL_DSP_GETFMTS — report supported formats
        if (cmd == SNDCTL_DSP_GETFMTS)
        {
            auto* fmts = reinterpret_cast<int*>(arg);
            *fmts = AFMT_U8 | AFMT_S16_LE;
            return 0;
        }

        // SNDCTL_DSP_SETFRAGMENT — set fragment size (accept, clamp)
        if (cmd == SNDCTL_DSP_SETFRAGMENT)
        {
            auto* val = reinterpret_cast<int*>(arg);
            int fragExp = (*val) & 0xFFFF;
            if (fragExp < 8) fragExp = 8;    // min 256 bytes
            if (fragExp > 16) fragExp = 16;  // max 64KB
            dsp->fragmentSize = static_cast<uint16_t>(1u << fragExp);
            return 0;
        }

        // SNDCTL_DSP_GETBLKSIZE — return fragment size
        if (cmd == SNDCTL_DSP_GETBLKSIZE)
        {
            auto* size = reinterpret_cast<int*>(arg);
            *size = static_cast<int>(dsp->fragmentSize);
            return 0;
        }

        // SNDCTL_DSP_SETTRIGGER — enable/disable output, accept but noop
        if (cmd == SNDCTL_DSP_SETTRIGGER)
            return 0;

        SerialPrintf("sys_ioctl: dsp unknown cmd 0x%lx\n", cmd);
        return -EINVAL;
    }

    // TCGETS2 = 0x802c542a — termios2 (like TCGETS plus ispeed/ospeed uint32s)
    if (isTtyFd && cmd == 0x802c542aUL)
    {
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

static void FillStat(LinuxStat* st, const VnodeStat& vs)
{
    auto* raw = reinterpret_cast<uint8_t*>(st);
    for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;

    // Generate a unique inode number from the file's attributes.
    // This is critical: musl's dynamic linker uses dev+ino to detect
    // already-loaded libraries.  Every distinct file MUST have a distinct ino.
    static volatile uint64_t s_nextIno = 100;
    st->st_ino = __atomic_fetch_add(&s_nextIno, 1, __ATOMIC_RELAXED);
    st->st_dev = 1;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_size = static_cast<int64_t>(vs.size);
    st->st_blocks = (st->st_size + 511) / 512;

    if (vs.isSymlink)
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

    return VfsStatPath("/boot/BIN/BUSYBOX", vs) == 0;
}

static int64_t sys_stat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    auto* st = reinterpret_cast<LinuxStat*>(statAddr);

    // Resolve relative paths (including ".") against CWD
    char resolved[256];
    const char* lookup = path;
    if (path[0] != '/')
    {
        Process* proc = ProcessCurrent();
        const char* cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
        uint32_t ci = 0;
        // Special case: "." means CWD itself
        if (path[0] == '.' && (path[1] == '\0' || path[1] == '/'))
        {
            for (uint32_t j = 0; cwd[j] && ci < 254; ++j)
                resolved[ci++] = cwd[j];
            // Append anything after "."
            if (path[1] == '/')
                for (uint32_t j = 1; path[j] && ci < 254; ++j)
                    resolved[ci++] = path[j];
        }
        else
        {
            for (uint32_t j = 0; cwd[j] && ci < 250; ++j)
                resolved[ci++] = cwd[j];
            if (ci > 0 && resolved[ci - 1] != '/')
                resolved[ci++] = '/';
            for (uint32_t j = 0; path[j] && ci < 254; ++j)
                resolved[ci++] = path[j];
        }
        resolved[ci] = '\0';
        lookup = resolved;
    }

    VnodeStat vs; vs.size = 0; vs.isDir = false; vs.isSymlink = false;
    if (VfsStatPath(lookup, &vs) < 0)
    {
        if (!BusyboxStatFallback(lookup, &vs))
            return -ENOENT;
    }

    FillStat(st, vs);
    return 0;
}

static int64_t sys_lstat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    auto* st = reinterpret_cast<LinuxStat*>(statAddr);

    // Resolve relative paths against CWD (same logic as sys_stat)
    char resolved[256];
    const char* lookup = path;
    if (path[0] != '/')
    {
        Process* proc = ProcessCurrent();
        const char* cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
        uint32_t ci = 0;
        if (path[0] == '.' && (path[1] == '\0' || path[1] == '/'))
        {
            for (uint32_t j = 0; cwd[j] && ci < 254; ++j)
                resolved[ci++] = cwd[j];
            if (path[1] == '/')
                for (uint32_t j = 1; path[j] && ci < 254; ++j)
                    resolved[ci++] = path[j];
        }
        else
        {
            for (uint32_t j = 0; cwd[j] && ci < 250; ++j)
                resolved[ci++] = cwd[j];
            if (ci > 0 && resolved[ci - 1] != '/')
                resolved[ci++] = '/';
            for (uint32_t j = 0; path[j] && ci < 254; ++j)
                resolved[ci++] = path[j];
        }
        resolved[ci] = '\0';
        lookup = resolved;
    }

    VnodeStat vs; vs.size = 0; vs.isDir = false; vs.isSymlink = false;
    if (VfsLstatPath(lookup, &vs) < 0)
    {
        if (!BusyboxStatFallback(lookup, &vs))
            return -ENOENT;
    }

    FillStat(st, vs);
    return 0;
}

static int64_t sys_fstat(uint64_t fd, uint64_t statAddr, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    auto* st = reinterpret_cast<LinuxStat*>(statAddr);

    if (fd <= 2) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0020666; // S_IFCHR | rw-rw-rw-
        st->st_rdev = 0x8800 + fd;
        st->st_blksize = 4096;
        return 0;
    }

    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;

    if (fde->type == FdType::Vnode && fde->handle) {
        auto* vn = static_cast<Vnode*>(fde->handle);
        VnodeStat vs; vs.size = 0; vs.isDir = false; vs.isSymlink = false;
        if (VfsStat(vn, &vs) < 0) return -EBADF;
        FillStat(st, vs);
        return 0;
    }

    if (fde->type == FdType::DevFramebuf) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0020666; // S_IFCHR
        st->st_rdev = 0x1D00;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::DevKeyboard || fde->type == FdType::DevNull ||
        fde->type == FdType::DevUrandom || fde->type == FdType::DevDsp) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0020666; // S_IFCHR
        st->st_rdev = (fde->type == FdType::DevNull) ? 0x0103 :
                      (fde->type == FdType::DevUrandom) ? 0x0109 :
                      (fde->type == FdType::DevDsp) ? 0x0E03 : 0x0400; // 14,3 = /dev/dsp
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::Pipe) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0010666; // S_IFIFO | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::SyntheticMem) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
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
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0140666; // S_IFSOCK | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::EventFd) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0100666; // S_IFREG | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::EpollFd) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0100666;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::TimerFd) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0100666;
        st->st_blksize = 4096;
        return 0;
    }

    if (fde->type == FdType::MemFd && fde->handle) {
        auto* mfd = static_cast<MemFdData*>(fde->handle);
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0100666; // S_IFREG | rw-rw-rw-
        st->st_size = static_cast<int64_t>(mfd->size);
        st->st_blksize = 4096;
        st->st_blocks  = static_cast<int64_t>((mfd->size + 511) / 512);
        return 0;
    }

    if (fde->type == FdType::UnixSocket) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0140666; // S_IFSOCK | rw-rw-rw-
        st->st_blksize = 4096;
        return 0;
    }

    return -EBADF;
}

// Forward decl for *at path resolution (defined after sys_openat).
static bool ResolveAtPath(int dirfd, const char* path, char* out, uint32_t outSize);

static int64_t sys_newfstatat(uint64_t dirfd, uint64_t pathAddr, uint64_t statAddr,
                               uint64_t flags, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path || path[0] == '\0')
        return sys_fstat(dirfd, statAddr, 0, 0, 0, 0);

    static constexpr uint64_t AT_SYMLINK_NOFOLLOW = 0x100;
    bool noFollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;

    char resolved[256];
    uint64_t arg = pathAddr;
    if (ResolveAtPath(static_cast<int>(dirfd), path, resolved, sizeof(resolved)))
        arg = reinterpret_cast<uint64_t>(resolved);

    if (noFollow)
        return sys_lstat(arg, statAddr, 0, 0, 0, 0);
    return sys_stat(arg, statAddr, 0, 0, 0, 0);
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
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde || fde->type != FdType::Vnode || !fde->handle) return -EBADF;

    auto* vn = static_cast<Vnode*>(fde->handle);
    auto* buf = reinterpret_cast<uint8_t*>(bufAddr);
    uint64_t pos = 0;
    uint32_t cookie = static_cast<uint32_t>(fde->seekPos);

    DirEntry de; de.name[0] = 0; de.size = 0; de.isDir = false;
    while (pos < count) {
        int ret = VfsReaddir(vn, &de, &cookie);
        if (ret <= 0) break;

        uint64_t nameLen = 0;
        while (de.name[nameLen] && nameLen < 255) ++nameLen;

        // d_name starts at offset 19 in LinuxDirent64
        uint64_t reclen = (19 + nameLen + 1 + 7) & ~7ULL;
        if (pos + reclen > count) break;

        auto* ent = reinterpret_cast<LinuxDirent64*>(buf + pos);
        ent->d_ino = cookie + 1;
        ent->d_off = static_cast<int64_t>(cookie);
        ent->d_reclen = static_cast<uint16_t>(reclen);
        ent->d_type = de.isDir ? 4 : 8; // DT_DIR : DT_REG

        for (uint64_t i = 0; i < nameLen; ++i) ent->d_name[i] = de.name[i];
        ent->d_name[nameLen] = '\0';
        for (uint64_t i = nameLen + 1; i < reclen - 19; ++i)
            ent->d_name[i] = '\0';

        pos += reclen;
    }

    fde->seekPos = cookie;
    return static_cast<int64_t>(pos);
}

// ---------------------------------------------------------------------------
// Identity syscalls: getuid/getgid/geteuid/getegid/setuid/setgid
// ---------------------------------------------------------------------------

static int64_t sys_getuid(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t) { return 0; }
static int64_t sys_getgid(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t) { return 0; }
static int64_t sys_geteuid(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t) { return 0; }
static int64_t sys_getegid(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t) { return 0; }
static int64_t sys_setuid(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t) { return 0; }
static int64_t sys_setgid(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t) { return 0; }

// capget(125): probe / fetch process capabilities. We have no capability
// model — pretend the caller has nothing set. glibc/libcap calls this to
// detect kernel version support; on non-NULL datap we must write the
// effective/permitted/inheritable triple (zeroed) so userland doesn't
// read uninitialised stack.
static int64_t sys_capget(uint64_t hdrAddr, uint64_t dataAddr,
                           uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (hdrAddr) {
        // header is { __u32 version; int pid; }. Echo back the version
        // userland gave us; that's what Linux does on a successful probe.
        // Leave version unchanged; nothing else to do.
        (void)hdrAddr;
    }
    if (dataAddr) {
        // V3 layout: two `struct __user_cap_data_struct` (12 bytes each).
        // Zero 24 bytes covering both halves; harmless if caller used V1/V2.
        auto* p = reinterpret_cast<uint32_t*>(dataAddr);
        for (int i = 0; i < 6; ++i) p[i] = 0;
    }
    return 0;
}

// capset(126): silently succeed. We don't track caps.
static int64_t sys_capset(uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t, uint64_t) { return 0; }

// getgroups(115): return supplementary group list
static int64_t sys_getgroups(uint64_t size, uint64_t listAddr, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    // Root has one supplementary group: 0
    if (size == 0) return 1;  // just return count
    if (size >= 1 && listAddr)
    {
        auto* list = reinterpret_cast<uint32_t*>(listAddr);
        list[0] = 0;
    }
    return 1;
}

// setgroups(116): stub — always succeed
static int64_t sys_setgroups(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t) { return 0; }

// ---------------------------------------------------------------------------
// Signal: rt_sigaction (13), rt_sigprocmask (14)
// ---------------------------------------------------------------------------

KernelSigaction g_sigHandlers[MAX_PROCESSES][64];

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

    if (oldactAddr)
    {
        auto* old = reinterpret_cast<KernelSigaction*>(oldactAddr);
        *old = g_sigHandlers[pid][idx];
    }

    if (actAddr)
    {
        auto* act = reinterpret_cast<const KernelSigaction*>(actAddr);
        g_sigHandlers[pid][idx] = *act;
    }

    return 0;
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
        *reinterpret_cast<uint64_t*>(oldAddr) = proc->sigMask;
    }

    // Modify mask
    if (setAddr)
    {
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
    auto* buf = reinterpret_cast<uint8_t*>(bufAddr);
    if (!buf) return -EFAULT;
    if (count > 256) count = 256; // cap per-call to avoid long stalls

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
    return static_cast<int64_t>(count);
}

// ---------------------------------------------------------------------------
// sys_openat (257) -- delegate to sys_open
// ---------------------------------------------------------------------------

static int64_t sys_openat(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                           uint64_t mode, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path) return -EFAULT;

    // If path is absolute or dirfd is AT_FDCWD, delegate to sys_open
    static constexpr int64_t AT_FDCWD = -100;
    if (path[0] == '/' || static_cast<int64_t>(dirfd) == AT_FDCWD)
        return sys_open(pathAddr, flags, mode, 0, 0, 0);

    // Resolve relative path against dirfd's directory path
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* fde = FdGet(proc, static_cast<int>(dirfd));
    if (!fde || fde->dirPath[0] == '\0')
        return sys_open(pathAddr, flags, mode, 0, 0, 0);

    // Build absolute path: dirPath + relative path
    char resolved[256];
    uint32_t ri = 0;
    for (uint32_t i = 0; fde->dirPath[i] && ri < 250; ++i)
        resolved[ri++] = fde->dirPath[i];
    if (ri > 0 && resolved[ri - 1] != '/' && ri < 254)
        resolved[ri++] = '/';
    for (uint32_t i = 0; path[i] && ri < 254; ++i)
        resolved[ri++] = path[i];
    resolved[ri] = '\0';

    return sys_open(reinterpret_cast<uint64_t>(resolved), flags, mode, 0, 0, 0);
}

// Shared helper for *at syscalls: resolves a path relative to dirfd into an
// absolute path in 'out'. Returns true on success. If path is already absolute
// or dirfd is AT_FDCWD, copies path verbatim.
static bool ResolveAtPath(int dirfd, const char* path, char* out, uint32_t outSize)
{
    static constexpr int AT_FDCWD = -100;
    if (!path || !out || outSize < 2) return false;
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        uint32_t i = 0;
        while (path[i] && i < outSize - 1) { out[i] = path[i]; ++i; }
        out[i] = '\0';
        return true;
    }
    Process* proc = ProcessCurrent();
    if (!proc) return false;
    FdEntry* fde = FdGet(proc, dirfd);
    if (!fde || fde->dirPath[0] == '\0') return false;
    uint32_t ri = 0;
    for (uint32_t i = 0; fde->dirPath[i] && ri < outSize - 2; ++i)
        out[ri++] = fde->dirPath[i];
    if (ri > 0 && out[ri - 1] != '/' && ri < outSize - 2)
        out[ri++] = '/';
    for (uint32_t i = 0; path[i] && ri < outSize - 1; ++i)
        out[ri++] = path[i];
    out[ri] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// sys_getcwd (79)
// ---------------------------------------------------------------------------

static int64_t sys_getcwd(uint64_t bufAddr, uint64_t size, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    const char* cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
    uint32_t len = 0;
    while (cwd[len]) len++;
    if (size < len + 1) return -ERANGE;
    auto* buf = reinterpret_cast<char*>(bufAddr);
    for (uint32_t i = 0; i <= len; i++) buf[i] = cwd[i];
    return static_cast<int64_t>(bufAddr);
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

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                          uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;

    switch (static_cast<int>(cmd))
    {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
    {
        // Find lowest free fd >= arg
        int minFd = static_cast<int>(arg);
        int newfd = -1;
        for (int i = minFd; i < static_cast<int>(MAX_FDS); i++)
        {
            if (proc->fds[i].type == FdType::None)
            {
                newfd = i;
                break;
            }
        }
        if (newfd < 0) return -EMFILE;

        proc->fds[newfd].type = fde->type;
        proc->fds[newfd].flags = fde->flags;
        proc->fds[newfd].fdFlags = (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
        proc->fds[newfd].handle = fde->handle;
        proc->fds[newfd].seekPos = fde->seekPos;
        proc->fds[newfd].statusFlags = fde->statusFlags;
        proc->fds[newfd].refCount = 1;

        // Bump pipe refcount
        if (fde->type == FdType::Pipe && fde->handle)
        {
            auto* pipe = static_cast<PipeBuffer*>(fde->handle);
            if (fde->flags & 1)
                __atomic_fetch_add(&pipe->writers, 1, __ATOMIC_RELEASE);
            else
                __atomic_fetch_add(&pipe->readers, 1, __ATOMIC_RELEASE);
        }

        // Bump vnode refcount
        if (fde->type == FdType::Vnode && fde->handle)
            __atomic_fetch_add(&static_cast<Vnode*>(fde->handle)->refCount, 1, __ATOMIC_RELEASE);

        // Bump socket refcount
        if (fde->type == FdType::Socket && fde->handle)
        {
            int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
            brook::SockRef(sockIdx);
        }

        // Bump unix socket refcount
        if (fde->type == FdType::UnixSocket && fde->handle)
        {
            auto* usd = static_cast<UnixSocketData*>(fde->handle);
            __atomic_fetch_add(&usd->refCount, 1, __ATOMIC_RELEASE);
        }

        return newfd;
    }
    case F_GETFD:
        return fde->fdFlags;
    case F_SETFD:
        fde->fdFlags = static_cast<uint8_t>(arg & FD_CLOEXEC);
        return 0;
    case F_GETFL:
        return fde->statusFlags;
    case F_SETFL:
        // We don't really support changing flags yet, but return success
        return 0;
    default:
        return 0; // Unknown command, pretend success
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
static constexpr short POLLOUT = 0x0004;
static constexpr short POLLERR = 0x0008;
static constexpr short POLLHUP = 0x0010;
static constexpr short POLLNVAL = 0x0020;

static int64_t sys_poll(uint64_t fdsAddr, uint64_t nfds, uint64_t timeout_ms,
                         uint64_t, uint64_t, uint64_t)
{
    if (nfds == 0) return 0;
    auto* fds = reinterpret_cast<pollfd*>(fdsAddr);
    Process* proc = ProcessCurrent();
    if (!proc) return -EFAULT;

    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++)
    {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;

        FdEntry* fde = FdGet(proc, fds[i].fd);

        if (!fde || fde->type == FdType::None)
        {
            fds[i].revents = POLLNVAL;
            ready++;
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
                if (pb->count() < PIPE_BUF_SIZE || pb->readers == 0)
                {
                    fds[i].revents |= (pb->readers > 0) ? POLLOUT : POLLERR;
                    ready++;
                }
            }
            continue;
        }

        // Regular files are always ready
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

            if (rdReady) fds[i].revents |= POLLIN;
            if (wrReady) fds[i].revents |= POLLOUT;
            if (fds[i].revents) ready++;
            continue;
        }

        // Unix domain socket
        if (fde->type == FdType::UnixSocket && fde->handle)
        {
            auto* usd = static_cast<UnixSocketData*>(fde->handle);
            if (fds[i].events & POLLIN)
            {
                bool readable = false;
                if (usd->state == UnixSocketData::State::Listening)
                    readable = (usd->pendingCount > 0);
                else if (usd->state == UnixSocketData::State::Connected && usd->rxPipe)
                    readable = (usd->rxPipe->count() > 0 ||
                                __atomic_load_n(&usd->rxPipe->writers, __ATOMIC_ACQUIRE) == 0);
                if (readable) fds[i].revents |= POLLIN;
            }
            if (fds[i].events & POLLOUT)
            {
                bool writable = false;
                if (usd->state == UnixSocketData::State::Connected && usd->txPipe)
                    writable = (usd->txPipe->space() > 0 ||
                                __atomic_load_n(&usd->txPipe->readers, __ATOMIC_ACQUIRE) == 0);
                if (writable) fds[i].revents |= POLLOUT;
            }
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

        // EventFd: readable when counter > 0, always writable
        if (fde->type == FdType::EventFd && fde->handle)
        {
            auto* efd = static_cast<EventFdData*>(fde->handle);
            if ((fds[i].events & POLLIN) &&
                __atomic_load_n(&efd->counter, __ATOMIC_ACQUIRE) > 0)
            {
                fds[i].revents |= POLLIN;
            }
            if (fds[i].events & POLLOUT)
                fds[i].revents |= POLLOUT;
            if (fds[i].revents) ready++;
            continue;
        }

        if (fde->type == FdType::TimerFd && fde->handle)
        {
            auto* tfd = static_cast<TimerFdData*>(fde->handle);
            if (fds[i].events & POLLIN)
            {
                extern volatile uint64_t g_lapicTickCount;
                bool fired = (tfd->expiryCount > 0) ||
                             (tfd->armed && g_lapicTickCount >= tfd->nextExpiry);
                if (fired) fds[i].revents |= POLLIN;
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

        // /dev/dsp: always writable (we buffer)
        if (fde->type == FdType::DevDsp)
        {
            if (fds[i].events & POLLOUT)
                fds[i].revents |= POLLOUT;
            if (fds[i].revents) ready++;
            continue;
        }

        // Default: assume ready for whatever was asked
        fds[i].revents = fds[i].events;
        ready++;
    }

    // If nothing ready and timeout != 0, block and retry
    if (ready == 0 && timeout_ms != 0)
    {
        // Register as waiter BEFORE re-checking data availability.
        // This closes the race where data arrives between the initial
        // scan and the block call.
        Process* self = ProcessCurrent();

        // For pipe FDs, register as waiter on the pipe
        for (uint64_t i = 0; i < nfds && i < 16; i++)
        {
            if (fds[i].fd < 0) continue;
            FdEntry* fde = FdGet(proc, fds[i].fd);
            if (!fde) continue;
            if (fde->type == FdType::Pipe && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                if (!(fde->flags & 1)) // read end
                    pb->readerWaiter = self;
            }
            if (fde->type == FdType::DevKeyboard && (fds[i].events & POLLIN))
                InputAddWaiter(self);
            if (fde->type == FdType::DevTty && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pair = static_cast<TtyDevicePair*>(fde->handle);
                auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                rp->readerWaiter = self;
            }
            if (fde->type == FdType::Socket && fde->handle)
            {
                int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                brook::SockSetPollWaiter(sockIdx, self);
            }
            if (fde->type == FdType::UnixSocket && fde->handle && (fds[i].events & POLLIN))
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if (usd->state == UnixSocketData::State::Connected && usd->rxPipe)
                    usd->rxPipe->readerWaiter = self;
                else if (usd->state == UnixSocketData::State::Listening)
                    usd->epollWaiter = self;
            }
        }

        // Re-check data availability after registration.
        // If data arrived between the initial scan and registration,
        // we catch it here instead of blocking forever.
        for (uint64_t i = 0; i < nfds; i++)
        {
            if (fds[i].fd < 0) continue;
            FdEntry* fde = FdGet(proc, fds[i].fd);
            if (!fde) continue;
            if (fde->type == FdType::DevKeyboard && (fds[i].events & POLLIN) && InputHasEvents())
            { ready++; break; }
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
                                                  (fds[i].events & POLLOUT) != 0))
                { ready++; break; }
            }
            if (fde->type == FdType::UnixSocket && fde->handle && (fds[i].events & POLLIN))
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if (usd->state == UnixSocketData::State::Listening) {
                    if (usd->pendingCount > 0) { ready++; break; }
                } else if (usd->state == UnixSocketData::State::Connected && usd->rxPipe) {
                    if (usd->rxPipe->count() > 0 ||
                        __atomic_load_n(&usd->rxPipe->writers, __ATOMIC_ACQUIRE) == 0)
                    { ready++; break; }
                }
            }
        }

        if (ready == 0)
        {
            // Set timed wakeup if timeout > 0
            if (timeout_ms > 0 && timeout_ms != static_cast<uint64_t>(-1))
            {
                extern volatile uint64_t g_lapicTickCount;
                self->wakeupTick = g_lapicTickCount + timeout_ms;

            }
            SchedulerBlock(self);
            if (HasPendingSignals())
            {
                // Clean up waiters before returning
                InputRemoveWaiter(self);
                for (uint64_t i = 0; i < nfds && i < 16; i++)
                {
                    if (fds[i].fd < 0) continue;
                    FdEntry* fde2 = FdGet(proc, fds[i].fd);
                    if (!fde2) continue;
                    if (fde2->type == FdType::Pipe && fde2->handle && (fds[i].events & POLLIN))
                    {
                        auto* pb2 = static_cast<PipeBuffer*>(fde2->handle);
                        if (!(fde2->flags & 1) && pb2->readerWaiter == self)
                            pb2->readerWaiter = nullptr;
                    }
                    if (fde2->type == FdType::Socket && fde2->handle)
                    {
                        int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde2->handle)) - 1;
                        brook::SockSetPollWaiter(si, nullptr);
                    }
                }
                return -EINTR;
            }
        }

        // Clean up waiters
        InputRemoveWaiter(self);
        for (uint64_t i = 0; i < nfds && i < 16; i++)
        {
            if (fds[i].fd < 0) continue;
            FdEntry* fde = FdGet(proc, fds[i].fd);
            if (!fde) continue;
            if (fde->type == FdType::Pipe && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                if (!(fde->flags & 1) && pb->readerWaiter == self)
                    pb->readerWaiter = nullptr;
            }
            if (fde->type == FdType::DevTty && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pair = static_cast<TtyDevicePair*>(fde->handle);
                auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                if (rp->readerWaiter == self)
                    rp->readerWaiter = nullptr;
            }
            if (fde->type == FdType::UnixSocket && fde->handle && (fds[i].events & POLLIN))
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if (usd->state == UnixSocketData::State::Connected && usd->rxPipe &&
                    usd->rxPipe->readerWaiter == self)
                    usd->rxPipe->readerWaiter = nullptr;
                if (usd->state == UnixSocketData::State::Listening &&
                    usd->epollWaiter == self)
                    usd->epollWaiter = nullptr;
            }
            // Socket waiter is already cleared by SockDeliverUdp/TcpEnqueueData on wake
        }

        // Re-scan after wake — reset ready count
        ready = 0;
        for (uint64_t i = 0; i < nfds; i++)
        {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            FdEntry* fde = FdGet(proc, fds[i].fd);
            if (!fde || fde->type == FdType::None) { fds[i].revents = POLLNVAL; ready++; continue; }
            if (fde->type == FdType::Pipe && fde->handle)
            {
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                bool isWrite = (fde->flags & 1);
                if (!isWrite && (fds[i].events & POLLIN) && (pb->count() > 0 || pb->writers == 0))
                { fds[i].revents |= (pb->count() > 0) ? POLLIN : POLLHUP; ready++; }
                if (isWrite && (fds[i].events & POLLOUT) && (pb->count() < PIPE_BUF_SIZE || pb->readers == 0))
                { fds[i].revents |= (pb->readers > 0) ? POLLOUT : POLLERR; ready++; }
                continue;
            }
            if (fde->type == FdType::Vnode)
            {
                if (fds[i].events & POLLIN)  fds[i].revents |= POLLIN;
                if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
                if (fds[i].revents) ready++;
                continue;
            }
            if (fde->type == FdType::DevKeyboard && (fds[i].events & POLLIN) && InputHasEvents())
            { fds[i].revents |= POLLIN; ready++; continue; }
            if (fde->type == FdType::DevTty && fde->handle)
            {
                auto* pair = static_cast<TtyDevicePair*>(fde->handle);
                if (fds[i].events & POLLIN)
                {
                    auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                    if (rp->count() > 0 || rp->writers == 0)
                    { fds[i].revents |= POLLIN; }
                }
                if (fds[i].events & POLLOUT)
                    fds[i].revents |= POLLOUT;
                if (fds[i].revents) ready++;
                continue;
            }
            if (fde->type == FdType::Socket && fde->handle)
            {
                int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                if ((fds[i].events & POLLIN) && brook::SockPollReady(sockIdx, true, false))
                    fds[i].revents |= POLLIN;
                if ((fds[i].events & POLLOUT) && brook::SockPollReady(sockIdx, false, true))
                    fds[i].revents |= POLLOUT;
                if (fds[i].revents) ready++;
                continue;
            }
            if (fde->type == FdType::EventFd && fde->handle)
            {
                auto* efd = static_cast<EventFdData*>(fde->handle);
                if ((fds[i].events & POLLIN) &&
                    __atomic_load_n(&efd->counter, __ATOMIC_ACQUIRE) > 0)
                    fds[i].revents |= POLLIN;
                if (fds[i].events & POLLOUT)
                    fds[i].revents |= POLLOUT;
                if (fds[i].revents) ready++;
                continue;
            }
            if (fde->type == FdType::DevDsp)
            {
                if (fds[i].events & POLLOUT)
                    fds[i].revents |= POLLOUT;
                if (fds[i].revents) ready++;
                continue;
            }
            if (fde->type == FdType::UnixSocket && fde->handle)
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if (fds[i].events & POLLIN)
                {
                    bool readable = false;
                    if (usd->state == UnixSocketData::State::Listening)
                        readable = (usd->pendingCount > 0);
                    else if (usd->state == UnixSocketData::State::Connected && usd->rxPipe)
                        readable = (usd->rxPipe->count() > 0 ||
                                    __atomic_load_n(&usd->rxPipe->writers, __ATOMIC_ACQUIRE) == 0);
                    if (readable) fds[i].revents |= POLLIN;
                }
                if (fds[i].events & POLLOUT)
                {
                    if (usd->state == UnixSocketData::State::Connected && usd->txPipe &&
                        (usd->txPipe->space() > 0 ||
                         __atomic_load_n(&usd->txPipe->readers, __ATOMIC_ACQUIRE) == 0))
                        fds[i].revents |= POLLOUT;
                }
                if (fds[i].revents) ready++;
                continue;
            }
            // Unknown fd type: don't claim readiness
        }
    }

    return ready;
}

// sys_ppoll (271) — redirects to poll
static int64_t sys_ppoll(uint64_t fdsAddr, uint64_t nfds, uint64_t tspecAddr,
                          uint64_t, uint64_t, uint64_t)
{
    int timeout_ms = -1;
    if (tspecAddr)
    {
        auto* ts = reinterpret_cast<const uint64_t*>(tspecAddr);
        timeout_ms = static_cast<int>(ts[0] * 1000 + ts[1] / 1000000);
    }
    return sys_poll(fdsAddr, nfds, static_cast<uint64_t>(timeout_ms), 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_readlink (89) / sys_readlinkat (267)
// ---------------------------------------------------------------------------

static int64_t sys_readlink(uint64_t pathAddr, uint64_t bufAddr, uint64_t bufsiz,
                             uint64_t, uint64_t, uint64_t)
{
    auto* path = reinterpret_cast<const char*>(pathAddr);
    auto* buf  = reinterpret_cast<char*>(bufAddr);

    // /proc/self/exe → return the process's executable path
    auto streq = [](const char* a, const char* b) {
        while (*a && *a == *b) { a++; b++; }
        return *a == *b;
    };
    if (streq(path, "/proc/self/exe"))
    {
        const char* exe = "/boot/BIN/UNKNOWN";
        Process* proc = ProcessCurrent();
        if (proc && proc->name[0])
        {
            // Build "/boot/BIN/<NAME>" from process name
            static __thread char exePath[128];
            int len = 0;
            const char* prefix = "/boot/BIN/";
            for (int i = 0; prefix[i]; i++) exePath[len++] = prefix[i];
            for (int i = 0; proc->name[i] && len < 120; i++) exePath[len++] = proc->name[i];
            exePath[len] = '\0';
            exe = exePath;
        }
        uint64_t slen = 0;
        while (exe[slen]) slen++;
        if (slen > bufsiz) slen = bufsiz;
        __builtin_memcpy(buf, exe, slen);
        return static_cast<int64_t>(slen);
    }

    // Try VFS readlink for real symlinks
    int r = VfsReadlink(path, buf, bufsiz);
    if (r >= 0) return static_cast<int64_t>(r);
    return -EINVAL;
}

static int64_t sys_readlinkat(uint64_t dirfd, uint64_t pathAddr, uint64_t bufAddr,
                               uint64_t bufsiz, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    char resolved[256];
    if (!ResolveAtPath(static_cast<int>(dirfd), path, resolved, sizeof(resolved)))
        return sys_readlink(pathAddr, bufAddr, bufsiz, 0, 0, 0);
    return sys_readlink(reinterpret_cast<uint64_t>(resolved), bufAddr, bufsiz, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_symlink (88) / sys_symlinkat (266)
// ---------------------------------------------------------------------------

static int64_t sys_symlink(uint64_t targetAddr, uint64_t linkpathAddr, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    auto* target   = reinterpret_cast<const char*>(targetAddr);
    auto* linkpath = reinterpret_cast<const char*>(linkpathAddr);
    int r = VfsSymlink(target, linkpath);
    return (r == 0) ? 0 : static_cast<int64_t>(r);
}

static int64_t sys_symlinkat(uint64_t targetAddr, uint64_t newdirfd, uint64_t linkpathAddr,
                              uint64_t, uint64_t, uint64_t)
{
    const char* linkpath = reinterpret_cast<const char*>(linkpathAddr);
    char resolved[256];
    if (!ResolveAtPath(static_cast<int>(newdirfd), linkpath, resolved, sizeof(resolved)))
        return sys_symlink(targetAddr, linkpathAddr, 0, 0, 0, 0);
    return sys_symlink(targetAddr, reinterpret_cast<uint64_t>(resolved), 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_pipe2 (293) — like pipe() but with flags
// ---------------------------------------------------------------------------

static int64_t sys_pipe2(uint64_t pipefdAddr, uint64_t flags, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    (void)flags; // O_CLOEXEC etc. — ignore for now
    return sys_pipe(pipefdAddr, 0, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_eventfd2 (290) — event notification file descriptor
// ---------------------------------------------------------------------------

static int64_t sys_eventfd2(uint64_t initval, uint64_t flagsVal, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    auto* efd = static_cast<EventFdData*>(kmalloc(sizeof(EventFdData)));
    if (!efd) return -ENOMEM;

    efd->counter = initval;
    efd->flags = static_cast<uint32_t>(flagsVal);
    efd->readerWaiter = nullptr;

    int fd = FdAlloc(proc, FdType::EventFd, efd);
    if (fd < 0) { kfree(efd); return -EMFILE; }

    if (flagsVal & EFD_NONBLOCK)
        proc->fds[fd].statusFlags = 0x800; // O_NONBLOCK

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
[[maybe_unused]] static constexpr uint32_t EPOLLET      = (1u << 31);  // edge-triggered (ignored, always LT)
[[maybe_unused]] static constexpr uint32_t EPOLLONESHOT = (1u << 30);

[[maybe_unused]] static constexpr int EPOLL_CTL_ADD = 1;
static constexpr int EPOLL_CTL_DEL = 2;
static constexpr int EPOLL_CTL_MOD = 3;

struct EpollEvent {
    uint32_t events;
    union {
        int      fd;
        uint64_t u64;
        uint32_t u32;
        void*    ptr;
    } data;
} __attribute__((packed)); // Linux x86_64 ABI: packed to 12 bytes

static constexpr int EPOLL_MAX_FDS = 64;

struct EpollEntry {
    int      fd;      // watched fd (-1 = free)
    uint32_t events;  // requested events
    uint64_t data;    // user data (uint64)
};

struct EpollInstance {
    EpollEntry entries[EPOLL_MAX_FDS];
    int        count;
    Process*   waiter; // process blocked in epoll_wait
};

// Check if a single fd is ready given the requested events mask.
// Returns the set of events that are actually ready.
static uint32_t EpollFdReady(Process* proc, int fd, uint32_t events)
{
    FdEntry* fde = FdGet(proc, fd);
    if (!fde) return EPOLLERR | EPOLLHUP;

    uint32_t ready = 0;

    if (events & EPOLLIN) {
        bool readable = false;
        if (fde->type == FdType::Pipe && fde->handle) {
            auto* pb = static_cast<PipeBuffer*>(fde->handle);
            readable = (pb->count() > 0 || pb->writers == 0);
        } else if (fde->type == FdType::EventFd && fde->handle) {
            auto* efd = static_cast<EventFdData*>(fde->handle);
            readable = (efd->counter > 0);
        } else if (fde->type == FdType::Socket && fde->handle) {
            int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
            readable = brook::SockPollReady(si, true, false);
        } else if (fde->type == FdType::TimerFd && fde->handle) {
            // TimerFd is readable when its expiry count > 0 or timer has fired
            auto* tfd = static_cast<TimerFdData*>(fde->handle);
            extern volatile uint64_t g_lapicTickCount;
            readable = (tfd->expiryCount > 0) ||
                       (tfd->armed && g_lapicTickCount >= tfd->nextExpiry);
        } else if (fde->type == FdType::Vnode) {
            readable = true; // files always readable
        } else if (fde->type == FdType::DevTty && fde->handle) {
            auto* pair = static_cast<TtyDevicePair*>(fde->handle);
            auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
            readable = (rp->count() > 0 || rp->writers == 0);
        } else if (fde->type == FdType::UnixSocket && fde->handle) {
            auto* usd = static_cast<UnixSocketData*>(fde->handle);
            if (usd->state == UnixSocketData::State::Listening) {
                readable = (usd->pendingCount > 0);
            } else if (usd->state == UnixSocketData::State::Connected && usd->rxPipe) {
                readable = (usd->rxPipe->count() > 0 ||
                            __atomic_load_n(&usd->rxPipe->writers, __ATOMIC_ACQUIRE) == 0);
            }
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
        } else if (fde->type == FdType::EventFd) {
            writable = true;
        } else if (fde->type == FdType::Vnode) {
            writable = true;
        } else if (fde->type == FdType::DevTty) {
            writable = true;
        } else if (fde->type == FdType::MemFd) {
            writable = true;
        } else if (fde->type == FdType::UnixSocket && fde->handle) {
            auto* usd = static_cast<UnixSocketData*>(fde->handle);
            if (usd->state == UnixSocketData::State::Connected && usd->txPipe)
                writable = (__atomic_load_n(&usd->txPipe->readers, __ATOMIC_ACQUIRE) > 0 &&
                            usd->txPipe->space() > 0);
        }
        if (writable) ready |= EPOLLOUT;
    }

    return ready;
}

// Scan interest list and fill events[]. Returns count of ready fds.
static int EpollScanReady(Process* proc, EpollInstance* ep,
                           EpollEvent* events, int maxevents)
{
    int n = 0;
    for (int i = 0; i < ep->count && n < maxevents; i++) {
        EpollEntry& e = ep->entries[i];
        if (e.fd < 0) continue;
        uint32_t ready = EpollFdReady(proc, e.fd, e.events);
        if (ready) {
            events[n].events = ready;
            events[n].data.u64 = e.data;
            n++;
        }
    }
    return n;
}

static int64_t epoll_create_impl(uint64_t flagsVal)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    auto* ep = static_cast<EpollInstance*>(kmalloc(sizeof(EpollInstance)));
    if (!ep) return -ENOMEM;
    for (int i = 0; i < EPOLL_MAX_FDS; i++) ep->entries[i].fd = -1;
    ep->count = 0;
    ep->waiter = nullptr;

    int fd = FdAlloc(proc, FdType::EpollFd, ep);
    if (fd < 0) { kfree(ep); return -EMFILE; }

    if (flagsVal & 0x80000) // EPOLL_CLOEXEC
        proc->fds[fd].fdFlags |= 1;

    SerialPrintf("EPOLL: create fd=%d pid=%u tgid=%u flags=0x%lx ep=0x%lx\n",
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

    FdEntry* epfde = FdGet(proc, static_cast<int>(epfd));
    if (!epfde || epfde->type != FdType::EpollFd || !epfde->handle) {
        SerialPrintf("sys_epoll_ctl: EBADF epfd=%lu pid=%u op=%lu "
                     "fde=%p type=%d handle=%p\n",
                     epfd, proc->pid, op, epfde,
                     epfde ? static_cast<int>(epfde->type) : -1,
                     epfde ? epfde->handle : nullptr);
        return -EBADF;
    }
    auto* ep = static_cast<EpollInstance*>(epfde->handle);

    int wfd = static_cast<int>(watchfd);

    if (op == EPOLL_CTL_DEL) {
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == wfd) {
                ep->entries[i].fd = -1;

                return 0;
            }
        }
        return -ENOENT;
    }

    // ADD or MOD — read the epoll_event from userspace
    EpollEvent ev = {};
    if (eventAddr < 0x1000) return -EFAULT;
    __builtin_memcpy(&ev, reinterpret_cast<const void*>(eventAddr), sizeof(ev));

    if (op == EPOLL_CTL_MOD) {
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == wfd) {
                ep->entries[i].events = ev.events;
                ep->entries[i].data   = ev.data.u64;

                return 0;
            }
        }
        return -ENOENT;
    }

    // EPOLL_CTL_ADD
    if (ep->count >= EPOLL_MAX_FDS) return -ENOMEM;

    // Find free slot (may have gaps from DEL)
    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        if (ep->entries[i].fd < 0) {
            ep->entries[i].fd     = wfd;
            ep->entries[i].events = ev.events;
            ep->entries[i].data   = ev.data.u64;
            if (i >= ep->count) ep->count = i + 1;

            return 0;
        }
    }
    return -ENOMEM;
}

static int64_t epoll_wait_impl(Process* proc, EpollInstance* ep,
                                EpollEvent* kEvents, int maxevents,
                                int timeout_ms)
{
    // Immediate scan
    int n = EpollScanReady(proc, ep, kEvents, maxevents);
    if (n > 0 || timeout_ms == 0) return n;

    // Block until something is ready (or timeout)
    extern volatile uint64_t g_lapicTickCount;
    uint64_t startTick = g_lapicTickCount;
    uint64_t timeoutTicks = (timeout_ms < 0)
        ? (~(uint64_t)0)  // UINT64_MAX
        : startTick + (uint64_t)timeout_ms;

    ep->waiter = proc;
    proc->wakeupTick = timeoutTicks;

    // Register proc as the epoll waiter on every watched pipe / listen
    // socket so writers can SchedulerUnblock us directly. For resources
    // that don't yet have epoll-waiter slots (EventFd, Socket, TimerFd,
    // Vnode) we fall back to the 50ms safety poll below.
    auto registerWaiters = [&]() {
        for (int i = 0; i < ep->count; i++) {
            int wfd = ep->entries[i].fd;
            if (wfd < 0) continue;
            FdEntry* fde = FdGet(proc, wfd);
            if (!fde || !fde->handle) continue;
            if (fde->type == FdType::Pipe) {
                static_cast<PipeBuffer*>(fde->handle)->epollWaiter = proc;
            } else if (fde->type == FdType::UnixSocket) {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if (usd->state == UnixSocketData::State::Listening) {
                    usd->epollWaiter = proc;
                } else if (usd->state == UnixSocketData::State::Connected && usd->rxPipe) {
                    usd->rxPipe->epollWaiter = proc;
                }
            }
        }
    };
    auto unregisterWaiters = [&]() {
        for (int i = 0; i < ep->count; i++) {
            int wfd = ep->entries[i].fd;
            if (wfd < 0) continue;
            FdEntry* fde = FdGet(proc, wfd);
            if (!fde || !fde->handle) continue;
            if (fde->type == FdType::Pipe) {
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                if (pb->epollWaiter == proc) pb->epollWaiter = nullptr;
            } else if (fde->type == FdType::UnixSocket) {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if (usd->epollWaiter == proc) usd->epollWaiter = nullptr;
                if (usd->rxPipe && usd->rxPipe->epollWaiter == proc)
                    usd->rxPipe->epollWaiter = nullptr;
            }
        }
    };

    while (true) {
        registerWaiters();

        // Safety poll: 50ms for resources without epoll-waiter wiring.
        // Much longer than the original 5ms stopgap because writers on
        // Pipe/UnixSocket now wake us directly.
        uint64_t pollDeadline = g_lapicTickCount + 50;
        if (pollDeadline < timeoutTicks)
            proc->wakeupTick = pollDeadline;
        else
            proc->wakeupTick = timeoutTicks;

        SchedulerBlock(proc);
        unregisterWaiters();
        if (HasPendingSignals()) {
            ep->waiter = nullptr;
            return -EINTR;
        }

        n = EpollScanReady(proc, ep, kEvents, maxevents);
        if (n > 0) { ep->waiter = nullptr; return n; }

        uint64_t now = g_lapicTickCount;
        if (timeout_ms >= 0 && now >= timeoutTicks) {
            ep->waiter = nullptr;
            return 0; // timeout
        }

        // Re-arm for another wait cycle
        ep->waiter = proc;
    }
}

static int64_t sys_epoll_wait(uint64_t epfd, uint64_t eventsAddr,
                               uint64_t maxevents, uint64_t timeout_ms,
                               uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    FdEntry* epfde = FdGet(proc, static_cast<int>(epfd));
    if (!epfde || epfde->type != FdType::EpollFd || !epfde->handle) {
        SerialPrintf("sys_epoll_wait: EBADF epfd=%lu pid=%u tgid=%u "
                     "fde=%p type=%d handle=%p\n",
                     epfd, proc->pid, proc->tgid, epfde,
                     epfde ? static_cast<int>(epfde->type) : -1,
                     epfde ? epfde->handle : nullptr);
        return -EBADF;
    }
    auto* ep = static_cast<EpollInstance*>(epfde->handle);

    if (maxevents <= 0 || maxevents > 1024) return -EINVAL;

    // Allocate kernel-side event buffer
    auto* kEvents = static_cast<EpollEvent*>(
        kmalloc(sizeof(EpollEvent) * static_cast<uint32_t>(maxevents)));
    if (!kEvents) return -ENOMEM;

    int n = epoll_wait_impl(proc, ep, kEvents,
                             static_cast<int>(maxevents),
                             static_cast<int>(static_cast<int64_t>(timeout_ms)));

    if (n > 0) {
        if (eventsAddr < 0x1000) { kfree(kEvents); return -EFAULT; }
        __builtin_memcpy(reinterpret_cast<void*>(eventsAddr), kEvents,
                        sizeof(EpollEvent) * static_cast<uint32_t>(n));
    }

    kfree(kEvents);
    return n;
}

static int64_t sys_epoll_pwait(uint64_t epfd, uint64_t eventsAddr,
                                uint64_t maxevents, uint64_t timeout_ms,
                                uint64_t sigmaskAddr, uint64_t)
{
    (void)sigmaskAddr; // signal mask ignored — we have no RT signals
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

    auto* tfd = static_cast<TimerFdData*>(kmalloc(sizeof(TimerFdData)));
    if (!tfd) return -ENOMEM;
    tfd->expiryCount = 0;
    tfd->intervalNs  = 0;
    tfd->nextExpiry  = 0;
    tfd->clockId     = static_cast<int>(clockid);
    tfd->armed       = false;
    tfd->waiter      = nullptr;

    int fd = FdAlloc(proc, FdType::TimerFd, tfd);
    if (fd < 0) { kfree(tfd); return -EMFILE; }

    if (flags & TFD_NONBLOCK)
        proc->fds[fd].statusFlags = 0x800;
    if (flags & TFD_CLOEXEC)
        proc->fds[fd].fdFlags |= 1;

    SerialPrintf("sys_timerfd_create: fd=%d clock=%lu\n", fd, clockid);
    return fd;
}

static int64_t sys_timerfd_settime(uint64_t fd, uint64_t flagsVal,
                                    uint64_t newValAddr, uint64_t oldValAddr,
                                    uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde || fde->type != FdType::TimerFd || !fde->handle) return -EBADF;
    auto* tfd = static_cast<TimerFdData*>(fde->handle);

    ITimerSpec newVal = {};
    if (newValAddr < 0x1000) return -EFAULT;
    __builtin_memcpy(&newVal, reinterpret_cast<const void*>(newValAddr), sizeof(newVal));

    // Save old value if requested
    if (oldValAddr) {
        ITimerSpec oldVal = {};
        if (tfd->armed) {
            extern volatile uint64_t g_lapicTickCount;
            uint64_t remaining = (tfd->nextExpiry > g_lapicTickCount)
                ? (tfd->nextExpiry - g_lapicTickCount) : 0;
            oldVal.it_value.tv_sec  = static_cast<int64_t>(remaining / 1000);
            oldVal.it_value.tv_nsec = static_cast<int64_t>((remaining % 1000) * 1000000);
            oldVal.it_interval.tv_sec  = static_cast<int64_t>(tfd->intervalNs / 1000000000ULL);
            oldVal.it_interval.tv_nsec = static_cast<int64_t>(tfd->intervalNs % 1000000000ULL);
        }
        if (oldValAddr >= 0x1000)
            __builtin_memcpy(reinterpret_cast<void*>(oldValAddr), &oldVal, sizeof(oldVal));
    }

    // Disarm if both it_value fields are zero
    int64_t valueSec  = newVal.it_value.tv_sec;
    int64_t valueNsec = newVal.it_value.tv_nsec;
    if (valueSec == 0 && valueNsec == 0) {
        tfd->armed = false;
        tfd->expiryCount = 0;
        return 0;
    }

    extern volatile uint64_t g_lapicTickCount;
    uint64_t nowTick = g_lapicTickCount;
    uint64_t valueMs = static_cast<uint64_t>(valueSec) * 1000ULL
                       + static_cast<uint64_t>(valueNsec) / 1000000ULL;

    if (flagsVal & TFD_TIMER_ABSTIME) {
        // Convert absolute time to tick (rough: treat as ms from epoch offset)
        tfd->nextExpiry = valueMs; // absolute - not great but acceptable for now
    } else {
        tfd->nextExpiry = nowTick + valueMs * LAPIC_TICKS_PER_MS;
    }

    uint64_t intervalSec  = static_cast<uint64_t>(newVal.it_interval.tv_sec);
    uint64_t intervalNsec = static_cast<uint64_t>(newVal.it_interval.tv_nsec);
    tfd->intervalNs = intervalSec * 1000000000ULL + intervalNsec;
    tfd->armed = true;
    tfd->expiryCount = 0;

    SerialPrintf("sys_timerfd_settime: fd=%lu nextExpiry=%llu intervalNs=%llu\n",
                 fd, tfd->nextExpiry, tfd->intervalNs);
    return 0;
}

static int64_t sys_timerfd_gettime(uint64_t fd, uint64_t currValAddr,
                                    uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde || fde->type != FdType::TimerFd || !fde->handle) return -EBADF;
    auto* tfd = static_cast<TimerFdData*>(fde->handle);

    ITimerSpec curr = {};
    if (tfd->armed) {
        extern volatile uint64_t g_lapicTickCount;
        uint64_t remaining = (tfd->nextExpiry > g_lapicTickCount)
            ? (tfd->nextExpiry - g_lapicTickCount) : 0;
        curr.it_value.tv_sec  = static_cast<int64_t>(remaining / 1000);
        curr.it_value.tv_nsec = static_cast<int64_t>((remaining % 1000) * 1000000LL);
        curr.it_interval.tv_sec  = static_cast<int64_t>(tfd->intervalNs / 1000000000ULL);
        curr.it_interval.tv_nsec = static_cast<int64_t>(tfd->intervalNs % 1000000000ULL);
    }

    if (currValAddr < 0x1000) return -EFAULT;
    __builtin_memcpy(reinterpret_cast<void*>(currValAddr), &curr, sizeof(curr));
    return 0;
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
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    // Name is just for debugging; we don't enforce it
    char name[64] = "<memfd>";
    if (nameAddr >= 0x1000) {
        const char* src = reinterpret_cast<const char*>(nameAddr);
        for (int i = 0; i < (int)(sizeof(name) - 1) && src[i]; i++) name[i] = src[i], name[i+1] = 0;
    }

    auto* mfd = static_cast<MemFdData*>(kmalloc(sizeof(MemFdData)));
    if (!mfd) return -ENOMEM;
    mfd->pageMap      = nullptr;
    mfd->pageMapCount = 0;
    mfd->size         = 0;
    mfd->capacity     = 0;
    mfd->lock         = 0;
    mfd->refCount     = 0; // bumped by FdAlloc below

    int fd = FdAlloc(proc, FdType::MemFd, mfd);
    if (fd < 0) { kfree(mfd); return -EMFILE; }
    MemFdRef(mfd);

    if (flags & MFD_CLOEXEC)
        proc->fds[fd].fdFlags |= 1;

    SerialPrintf("sys_memfd_create: fd=%d name='%s' flags=0x%lx\n", fd, name, flags);
    return fd;
}

// ---------------------------------------------------------------------------
// sys_sendfile (40) — stub: copies between fds in kernel
// ---------------------------------------------------------------------------

static int64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd, uint64_t offsetAddr,
                             uint64_t count, uint64_t, uint64_t)
{
    (void)offsetAddr;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* in_fde = FdGet(proc, static_cast<int>(in_fd));
    FdEntry* out_fde = FdGet(proc, static_cast<int>(out_fd));
    if (!in_fde || !out_fde) return -EBADF;

    // Simple implementation: read from in_fd, write to out_fd via syscalls
    // For now, just return 0 (no bytes transferred) — apps will fallback to read+write
    return 0;
}

// ---------------------------------------------------------------------------
// sys_getrusage (98) — stub
// ---------------------------------------------------------------------------

static int64_t sys_getrusage(uint64_t who, uint64_t usageAddr, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    (void)who;
    if (!usageAddr) return -EFAULT;
    __builtin_memset(reinterpret_cast<void*>(usageAddr), 0, 144); // sizeof(struct rusage)
    return 0;
}

// ---------------------------------------------------------------------------
// sys_sysinfo (99) — basic system info
// ---------------------------------------------------------------------------

static int64_t sys_sysinfo(uint64_t infoAddr, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    if (!infoAddr) return -EFAULT;
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
    auto* info = reinterpret_cast<sysinfo_s*>(infoAddr);
    __builtin_memset(info, 0, sizeof(sysinfo_s));
    info->uptime = 60; // placeholder
    info->totalram = 6ULL * 1024 * 1024 * 1024;
    info->freeram  = 4ULL * 1024 * 1024 * 1024;
    info->procs = 32;
    info->mem_unit = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// sys_umask (95) — stub, always returns 022
// ---------------------------------------------------------------------------

static int64_t sys_umask(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return 022;
}

// ---------------------------------------------------------------------------
// sys_chdir (80) / sys_fchdir (81) — stub
// ---------------------------------------------------------------------------

static int64_t sys_chdir(uint64_t pathAddr, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path) return -EFAULT;

    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    // Resolve relative paths against current CWD
    char resolved[64];
    const char* newCwd = path;
    if (path[0] != '/')
    {
        uint32_t ri = 0;
        for (uint32_t i = 0; proc->cwd[i] && ri < 58; ++i)
            resolved[ri++] = proc->cwd[i];
        if (ri > 0 && resolved[ri-1] != '/')
            resolved[ri++] = '/';
        for (uint32_t i = 0; path[i] && ri < 62; ++i)
            resolved[ri++] = path[i];
        resolved[ri] = '\0';
        newCwd = resolved;
    }

    // Verify path exists and is a directory
    VnodeStat vs; vs.size = 0; vs.isDir = false; vs.isSymlink = false;
    if (VfsStatPath(newCwd, &vs) < 0) return -ENOENT;
    if (!vs.isDir) return -ENOTDIR;

    // Update CWD
    uint32_t ci = 0;
    while (newCwd[ci] && ci < 62) { proc->cwd[ci] = newCwd[ci]; ci++; }
    proc->cwd[ci] = '\0';

    return 0;
}

static int64_t sys_fchdir(uint64_t fd, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde || fde->dirPath[0] == '\0') return -EBADF;

    // Use the stored dirPath (without trailing slash)
    uint32_t ci = 0;
    while (fde->dirPath[ci] && ci < 62) { proc->cwd[ci] = fde->dirPath[ci]; ci++; }
    // Remove trailing slash if not root
    if (ci > 1 && proc->cwd[ci-1] == '/') ci--;
    proc->cwd[ci] = '\0';

    return 0;
}

// ---------------------------------------------------------------------------
// sys_unlink (87) — delete a file
// ---------------------------------------------------------------------------

static int64_t sys_unlink(uint64_t pathAddr, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path) return -EFAULT;

    // /dev/shm/<name> — shm_unlink: mark entry as free
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' &&
        path[4]=='/' && path[5]=='s' && path[6]=='h' && path[7]=='m' && path[8]=='/') {
        // Leave the MemFdData alive (other fds may still reference it)
        // Just make it invisible to future shm_open calls
        SerialPrintf("shm_unlink: '%s' (ignored — fds stay valid)\n", path + 9);
        return 0;
    }

    // Try as-is first, then with /boot prefix
    if (VfsUnlink(path) == 0) return 0;

    char bootPath[256] = "/boot";
    uint32_t bi = 5;
    for (const char* p = path; *p && bi + 1 < sizeof(bootPath); ++p)
        bootPath[bi++] = *p;
    bootPath[bi] = '\0';
    if (VfsUnlink(bootPath) == 0) return 0;

    return -ENOENT;
}

// ---------------------------------------------------------------------------
// sys_rename (82) — rename a file or directory
// ---------------------------------------------------------------------------

static int64_t sys_rename(uint64_t oldAddr, uint64_t newAddr, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    const char* oldPath = reinterpret_cast<const char*>(oldAddr);
    const char* newPath = reinterpret_cast<const char*>(newAddr);
    if (!oldPath || !newPath) return -EFAULT;

    if (VfsRename(oldPath, newPath) == 0) return 0;

    // Try with /boot prefix on both
    char bootOld[256] = "/boot", bootNew[256] = "/boot";
    uint32_t oi = 5, ni = 5;
    for (const char* p = oldPath; *p && oi + 1 < sizeof(bootOld); ++p)
        bootOld[oi++] = *p;
    bootOld[oi] = '\0';
    for (const char* p = newPath; *p && ni + 1 < sizeof(bootNew); ++p)
        bootNew[ni++] = *p;
    bootNew[ni] = '\0';
    if (VfsRename(bootOld, bootNew) == 0) return 0;

    return -ENOENT;
}

// ---------------------------------------------------------------------------
// sys_mkdir (83) — create a directory
// ---------------------------------------------------------------------------

static int64_t sys_mkdir(uint64_t pathAddr, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path) return -EFAULT;

    if (VfsMkdir(path) == 0) return 0;

    char bootPath[256] = "/boot";
    uint32_t bi = 5;
    for (const char* p = path; *p && bi + 1 < sizeof(bootPath); ++p)
        bootPath[bi++] = *p;
    bootPath[bi] = '\0';
    if (VfsMkdir(bootPath) == 0) return 0;

    return -ENOENT;
}

// ---------------------------------------------------------------------------
// sys_mkdirat (258) — create a directory relative to dirfd
// ---------------------------------------------------------------------------

static int64_t sys_mkdirat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                            uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    char resolved[256];
    if (!ResolveAtPath(static_cast<int>(dirfd), path, resolved, sizeof(resolved)))
        return sys_mkdir(pathAddr, mode, 0, 0, 0, 0);
    return sys_mkdir(reinterpret_cast<uint64_t>(resolved), mode, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_unlinkat (263) — unlink file or rmdir relative to dirfd
// ---------------------------------------------------------------------------

static int64_t sys_unlinkat(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                             uint64_t, uint64_t, uint64_t)
{
    (void)flags; // TODO: handle AT_REMOVEDIR (0x200) for rmdir
    const char* path = reinterpret_cast<const char*>(pathAddr);
    char resolved[256];
    if (!ResolveAtPath(static_cast<int>(dirfd), path, resolved, sizeof(resolved)))
        return sys_unlink(pathAddr, 0, 0, 0, 0, 0);
    return sys_unlink(reinterpret_cast<uint64_t>(resolved), 0, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_renameat / sys_renameat2 (264 / 316)
// ---------------------------------------------------------------------------

static int64_t sys_renameat(uint64_t olddirfd, uint64_t oldAddr,
                             uint64_t newdirfd, uint64_t newAddr,
                             uint64_t, uint64_t)
{
    const char* oldP = reinterpret_cast<const char*>(oldAddr);
    const char* newP = reinterpret_cast<const char*>(newAddr);
    char oldR[256], newR[256];
    uint64_t oArg = oldAddr, nArg = newAddr;
    if (ResolveAtPath(static_cast<int>(olddirfd), oldP, oldR, sizeof(oldR)))
        oArg = reinterpret_cast<uint64_t>(oldR);
    if (ResolveAtPath(static_cast<int>(newdirfd), newP, newR, sizeof(newR)))
        nArg = reinterpret_cast<uint64_t>(newR);
    return sys_rename(oArg, nArg, 0, 0, 0, 0);
}

static int64_t sys_renameat2(uint64_t olddirfd, uint64_t oldAddr,
                              uint64_t newdirfd, uint64_t newAddr,
                              uint64_t flags, uint64_t)
{
    (void)flags; // RENAME_NOREPLACE etc.
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
    Process* proc = ProcessCurrent();
    if (proc) {
        FdEntry* fde = FdGet(proc, static_cast<int>(fd));
        if (fde && fde->type == FdType::MemFd && fde->handle) {
            auto* mfd = static_cast<MemFdData*>(fde->handle);
            if (length > mfd->capacity) {
                if (!MemFdGrow(mfd, length)) return -ENOMEM;
            }
            // Shrinking ftruncate: free pages past the new tail. (Growth is
            // free — pages are unbacked / read-as-zero until first access.)
            if (length < mfd->size) {
                MfdLock(mfd);
                uint64_t firstFreeIdx = (length + 4095) / 4096;
                for (uint64_t i = firstFreeIdx; i < mfd->pageMapCount; i++) {
                    if (mfd->pageMap[i]) {
                        PmmFreePage(PhysicalAddress(mfd->pageMap[i]));
                        mfd->pageMap[i] = 0;
                    }
                }
                MfdUnlock(mfd);
            }
            mfd->size = length;
            return 0;
        }
    }
    // For Vnode files and stubs, succeed silently (Nix uses this for DB journal files)
    return 0;
}

static int64_t sys_truncate(uint64_t pathAddr, uint64_t length, uint64_t,
                             uint64_t, uint64_t, uint64_t)
{
    (void)pathAddr; (void)length;
    return 0; // stub
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
// — stub: succeed silently (no permission model yet)
// ---------------------------------------------------------------------------

static int64_t sys_chmod(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
}

static int64_t sys_fchmod(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
}

static int64_t sys_fchmodat(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
}

// ---------------------------------------------------------------------------
// sys_chown (92) / sys_fchown (93) / sys_lchown (94) / sys_fchownat (260)
// — stub: succeed silently (no user model yet)
// ---------------------------------------------------------------------------

static int64_t sys_chown(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
}

static int64_t sys_fchown(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
}

static int64_t sys_lchown(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
}

static int64_t sys_fchownat(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
}

// ---------------------------------------------------------------------------
// sys_utimensat (280) — change file timestamps (stub: succeed)
// ---------------------------------------------------------------------------

static int64_t sys_utimensat(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op — we don't track file timestamps yet
}

// ---------------------------------------------------------------------------
// sys_sched_getaffinity (204) — get CPU affinity mask
// ---------------------------------------------------------------------------

static int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
                                      uint64_t maskAddr, uint64_t, uint64_t, uint64_t)
{
    (void)pid;
    if (!maskAddr) return -EFAULT;
    if (cpusetsize < 8) return -EINVAL;

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
    (void)dirfd; (void)flags; (void)mask;
    const char* path = reinterpret_cast<const char*>(pathAddr);
    auto* stx = reinterpret_cast<linux_statx*>(bufAddr);
    if (!path || !stx) return -EFAULT;

    // Use VFS stat
    Vnode* vn = VfsOpen(path);
    if (!vn) return -ENOENT;

    VnodeStat st;
    int ret = VfsStat(vn, &st);
    VfsClose(vn);
    if (ret != 0) return -EIO;

    // Zero-fill then populate
    auto* raw = reinterpret_cast<uint8_t*>(stx);
    for (uint64_t i = 0; i < sizeof(linux_statx); i++) raw[i] = 0;

    stx->stx_mask = 0x7FF; // STATX_BASIC_STATS
    stx->stx_blksize = 4096;
    stx->stx_nlink = 1;
    // Derive mode from stat info
    uint16_t mode = st.isDir ? 0040755 : 0100644;
    if (st.isSymlink) mode = 0120777;
    stx->stx_mode = mode;
    stx->stx_ino = 1; // placeholder
    stx->stx_size = st.size;
    stx->stx_blocks = (st.size + 511) / 512;

    return 0;
}

// ---------------------------------------------------------------------------
// sys_fallocate (285) — allocate file space (stub: succeed)
// ---------------------------------------------------------------------------

static int64_t sys_fallocate(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op for now
}

// ---------------------------------------------------------------------------
// sys_mincore (27) — check if pages are resident (stub: all resident)
// ---------------------------------------------------------------------------

static int64_t sys_mincore(uint64_t addr, uint64_t length, uint64_t vecAddr,
                            uint64_t, uint64_t, uint64_t)
{
    (void)addr;
    if (!vecAddr) return -EFAULT;
    auto* vec = reinterpret_cast<uint8_t*>(vecAddr);
    uint64_t pages = (length + 4095) / 4096;
    for (uint64_t i = 0; i < pages; ++i)
        vec[i] = 1; // all pages resident
    return 0;
}

// ---------------------------------------------------------------------------
// sys_madvise (28) — memory advice (stub: succeed)
// ---------------------------------------------------------------------------

static int64_t sys_madvise(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
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
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;
    if (fde->type != FdType::Vnode || !fde->handle) return -EINVAL;

    auto* vn = static_cast<Vnode*>(fde->handle);
    const void* buf = reinterpret_cast<const void*>(bufAddr);
    uint64_t off = offset;
    return VfsWrite(vn, buf, count, &off);
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

static int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig,
                           uint64_t, uint64_t, uint64_t)
{
    (void)tgid;
    if (sig == 0) return 0; // Signal 0 = check permissions only
    Process* target = ProcessFindByPid(static_cast<uint16_t>(tid));
    if (!target) return -ESRCH;
    // If the handler asks for SA_ONSTACK but the target thread hasn't
    // configured a signal alt stack, dropping the signal is safer than
    // delivering on the regular stack — Go's runtime panics with
    // "non-Go code set up signal handler without SA_ONSTACK flag" if
    // SP isn't on the gsignal stack at delivery time. This bites SIGURG
    // (used for goroutine preemption) on threads that haven't reached
    // minit() yet.
    if (sig >= 1 && sig <= 64 && target->tgid < MAX_PROCESSES)
    {
        const KernelSigaction& sa = g_sigHandlers[target->tgid][sig - 1];
        if ((sa.flags & SA_ONSTACK) && target->sigAltstackSp == 0)
            return 0;
    }
    return ProcessSendSignal(target, static_cast<int>(sig));
}

static int64_t sys_tkill(uint64_t tid, uint64_t sig, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    if (sig == 0) return 0;
    Process* target = ProcessFindByPid(static_cast<uint16_t>(tid));
    if (!target) return -ESRCH;
    return ProcessSendSignal(target, static_cast<int>(sig));
}

// ---------------------------------------------------------------------------
// sys_kill (62)
// ---------------------------------------------------------------------------

static int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    if (sig == 0) return 0; // Permission check only
    Process* target = ProcessFindByPid(static_cast<uint16_t>(pid));
    if (!target) return -ESRCH;
    return ProcessSendSignal(target, static_cast<int>(sig));
}

// ---------------------------------------------------------------------------
// sys_getrlimit (97) — stub
// ---------------------------------------------------------------------------

static int64_t sys_getrlimit(uint64_t resource, uint64_t rlimAddr, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    (void)resource;
    if (!rlimAddr) return -EFAULT;
    auto* rlim = reinterpret_cast<uint64_t*>(rlimAddr);
    rlim[0] = 0x7FFFFFFFFFFFFFFFULL; // rlim_cur = unlimited
    rlim[1] = 0x7FFFFFFFFFFFFFFFULL; // rlim_max = unlimited
    return 0;
}

// ---------------------------------------------------------------------------
// sys_statfs (137) / sys_fstatfs (138) — stub
// ---------------------------------------------------------------------------

static int64_t sys_statfs(uint64_t, uint64_t bufAddr, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    if (!bufAddr) return -EFAULT;
    __builtin_memset(reinterpret_cast<void*>(bufAddr), 0, 120); // sizeof(struct statfs)
    return 0;
}

static int64_t sys_fstatfs(uint64_t, uint64_t bufAddr, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    return sys_statfs(0, bufAddr, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_select (23) — proper implementation checking pipe/device readiness
// ---------------------------------------------------------------------------

static int64_t sys_select(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                           uint64_t exceptfdsAddr, uint64_t timeoutAddr, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    auto* rfds = readfdsAddr ? reinterpret_cast<uint64_t*>(readfdsAddr) : nullptr;
    auto* wfds = writefdsAddr ? reinterpret_cast<uint64_t*>(writefdsAddr) : nullptr;

    // Parse timeout: NULL = block forever, {0,0} = poll, else timeout
    int64_t timeout_ms = -1; // -1 = infinite
    if (timeoutAddr)
    {
        // pselect6 uses struct timespec {tv_sec, tv_nsec}
        // select uses struct timeval {tv_sec, tv_usec}
        // Both start with tv_sec at offset 0
        auto* ts = reinterpret_cast<const int64_t*>(timeoutAddr);
        int64_t sec  = ts[0];
        int64_t nsec = ts[1]; // could be nsec (pselect) or usec (select)
        // Heuristic: if nsec > 1000000, treat as nanoseconds
        if (nsec > 1000000)
            timeout_ms = sec * 1000 + nsec / 1000000;
        else
            timeout_ms = sec * 1000 + nsec / 1000;
        if (timeout_ms == 0) timeout_ms = 0; // poll mode
    }

    // Clear except fds
    if (exceptfdsAddr)
    {
        auto* efds = reinterpret_cast<uint64_t*>(exceptfdsAddr);
        for (uint64_t w = 0; w < (nfds + 63) / 64; w++)
            efds[w] = 0;
    }

    // Build result fd_sets — only mark fds that are actually ready
    uint64_t rResult[2] = {0, 0}; // supports up to 128 fds
    uint64_t wResult[2] = {0, 0};
    int ready = 0;

    for (uint64_t fd = 0; fd < nfds && fd < 128; fd++)
    {
        uint64_t mask = 1ULL << (fd % 64);
        uint64_t word = fd / 64;

        bool wantRead  = rfds && (rfds[word] & mask);
        bool wantWrite = wfds && (wfds[word] & mask);
        if (!wantRead && !wantWrite) continue;

        FdEntry* fde = FdGet(proc, static_cast<int>(fd));
        if (!fde)
        {
            // Bad fd in set — EBADF per POSIX
            continue;
        }

        // Pipe fd
        if (fde->type == FdType::Pipe && fde->handle)
        {
            auto* pb = static_cast<PipeBuffer*>(fde->handle);
            bool isWrite = (fde->flags & 1);
            if (wantRead && !isWrite)
            {
                if (pb->count() > 0 || pb->writers == 0)
                {
                    rResult[word] |= mask;
                    ready++;
                }
            }
            if (wantWrite && isWrite)
            {
                if (pb->count() < PIPE_BUF_SIZE || pb->readers == 0)
                {
                    wResult[word] |= mask;
                    ready++;
                }
            }
            continue;
        }

        // DevTty fd (e.g. /dev/tty → fd 63)
        if (fde->type == FdType::DevTty && fde->handle)
        {
            auto* pair = static_cast<TtyDevicePair*>(fde->handle);
            if (wantRead)
            {
                auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                if (rp->count() > 0 || rp->writers == 0)
                {
                    rResult[word] |= mask;
                    ready++;
                }
            }
            if (wantWrite)
            {
                wResult[word] |= mask;
                ready++;
            }
            continue;
        }

        // Keyboard device
        if (fde->type == FdType::DevKeyboard)
        {
            if (wantRead && InputHasEvents())
            {
                rResult[word] |= mask;
                ready++;
            }
            if (wantWrite)
            {
                wResult[word] |= mask;
                ready++;
            }
            continue;
        }

        // Regular files, sockets — always ready
        if (wantRead)  { rResult[word] |= mask; ready++; }
        if (wantWrite) { wResult[word] |= mask; ready++; }
    }

    // If something is ready, return immediately
    if (ready > 0)
    {
        if (rfds) { rfds[0] = rResult[0]; rfds[1] = rResult[1]; }
        if (wfds) { wfds[0] = wResult[0]; wfds[1] = wResult[1]; }
        return ready;
    }

    // Nothing ready — if timeout is 0, return 0 (poll mode)
    if (timeout_ms == 0)
    {
        if (rfds) { rfds[0] = 0; rfds[1] = 0; }
        if (wfds) { wfds[0] = 0; wfds[1] = 0; }
        return 0;
    }

    // Block until data arrives or timeout
    // Register as waiter on all monitored pipe fds
    Process* self = ProcessCurrent();
    for (uint64_t fd = 0; fd < nfds && fd < 128; fd++)
    {
        uint64_t mask = 1ULL << (fd % 64);
        uint64_t word = fd / 64;
        bool wantRead = rfds && (rfds[word] & mask);
        if (!wantRead) continue;

        FdEntry* fde = FdGet(proc, static_cast<int>(fd));
        if (!fde) continue;

        if (fde->type == FdType::Pipe && fde->handle && !(fde->flags & 1))
        {
            auto* pb = static_cast<PipeBuffer*>(fde->handle);
            pb->readerWaiter = self;
        }
        if (fde->type == FdType::DevKeyboard)
            InputAddWaiter(self);
        if (fde->type == FdType::DevTty && fde->handle)
        {
            auto* pair = static_cast<TtyDevicePair*>(fde->handle);
            auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
            rp->readerWaiter = self;
        }
    }

    // Re-check after registration (close race window)
    ready = 0;
    rResult[0] = rResult[1] = 0;
    wResult[0] = wResult[1] = 0;
    for (uint64_t fd = 0; fd < nfds && fd < 128; fd++)
    {
        uint64_t mask = 1ULL << (fd % 64);
        uint64_t word = fd / 64;
        bool wantRead  = rfds && (rfds[word] & mask);
        bool wantWrite = wfds && (wfds[word] & mask);
        if (!wantRead && !wantWrite) continue;

        FdEntry* fde = FdGet(proc, static_cast<int>(fd));
        if (!fde) continue;

        if (fde->type == FdType::Pipe && fde->handle)
        {
            auto* pb = static_cast<PipeBuffer*>(fde->handle);
            bool isWrite = (fde->flags & 1);
            if (wantRead && !isWrite && (pb->count() > 0 || pb->writers == 0))
            {
                rResult[word] |= mask;
                ready++;
            }
            if (wantWrite && isWrite && (pb->count() < PIPE_BUF_SIZE || pb->readers == 0))
            {
                wResult[word] |= mask;
                ready++;
            }
        }
        else if (fde->type == FdType::DevTty && fde->handle)
        {
            auto* pair = static_cast<TtyDevicePair*>(fde->handle);
            if (wantRead)
            {
                auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                if (rp->count() > 0 || rp->writers == 0)
                {
                    rResult[word] |= mask;
                    ready++;
                }
            }
        }
        else if (fde->type == FdType::DevKeyboard && wantRead && InputHasEvents())
        {
            rResult[word] |= mask;
            ready++;
        }
    }

    if (ready > 0)
    {
        if (rfds) { rfds[0] = rResult[0]; rfds[1] = rResult[1]; }
        if (wfds) { wfds[0] = wResult[0]; wfds[1] = wResult[1]; }
        return ready;
    }

    // Block
    SchedulerBlock(self);

    // After wakeup, do one final scan
    ready = 0;
    rResult[0] = rResult[1] = 0;
    wResult[0] = wResult[1] = 0;
    for (uint64_t fd = 0; fd < nfds && fd < 128; fd++)
    {
        uint64_t mask = 1ULL << (fd % 64);
        uint64_t word = fd / 64;
        bool wantRead  = rfds && (rfds[word] & mask);
        bool wantWrite = wfds && (wfds[word] & mask);
        if (!wantRead && !wantWrite) continue;

        FdEntry* fde = FdGet(proc, static_cast<int>(fd));
        if (!fde) continue;

        if (fde->type == FdType::Pipe && fde->handle)
        {
            auto* pb = static_cast<PipeBuffer*>(fde->handle);
            bool isWrite = (fde->flags & 1);
            if (wantRead && !isWrite && (pb->count() > 0 || pb->writers == 0))
            {
                rResult[word] |= mask;
                ready++;
            }
            if (wantWrite && isWrite && (pb->count() < PIPE_BUF_SIZE || pb->readers == 0))
            {
                wResult[word] |= mask;
                ready++;
            }
        }
        else if (fde->type == FdType::DevTty && fde->handle)
        {
            auto* pair = static_cast<TtyDevicePair*>(fde->handle);
            if (wantRead)
            {
                auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                if (rp->count() > 0 || rp->writers == 0)
                {
                    rResult[word] |= mask;
                    ready++;
                }
            }
            if (wantWrite)
            {
                wResult[word] |= mask;
                ready++;
            }
        }
        else if (fde->type == FdType::DevKeyboard)
        {
            if (wantRead && InputHasEvents())
            {
                rResult[word] |= mask;
                ready++;
            }
        }
        else
        {
            // Vnode, socket etc — ready
            if (wantRead)  { rResult[word] |= mask; ready++; }
            if (wantWrite) { wResult[word] |= mask; ready++; }
        }
    }

    if (rfds) { rfds[0] = rResult[0]; rfds[1] = rResult[1]; }
    if (wfds) { wfds[0] = wResult[0]; wfds[1] = wResult[1]; }
    return ready;
}

// ---------------------------------------------------------------------------
// sys_pselect6 (270) — pselect6, delegate to sys_select (ignore sigmask)
// ---------------------------------------------------------------------------

static int64_t sys_pselect6(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                             uint64_t exceptfdsAddr, uint64_t timeoutAddr, uint64_t sigmaskAddr)
{
    (void)sigmaskAddr;
    return sys_select(nfds, readfdsAddr, writefdsAddr, exceptfdsAddr, timeoutAddr, 0);
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
// Returns 0 on success, -errno on failure.
struct BrookWmCreateOut {
    uint32_t wmId;
    uint32_t vfbStride;
    uint64_t vfbUser;
};

static int64_t sys_brook_wm_create_window(uint64_t cW, uint64_t cH,
                                           uint64_t titlePtr, uint64_t outPtr,
                                           uint64_t, uint64_t)
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

    auto res = brook::WmCreateWindowForProcess(proc,
                                                static_cast<uint16_t>(cW),
                                                static_cast<uint16_t>(cH),
                                                titlePtr ? titleBuf : nullptr);
    if (res.wmId == 0) return -ENOMEM;

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

    int idx = static_cast<int>(wmId) - 1;
    brook::Window* w = brook::WmGetWindow(idx);
    if (!w || w->proc != proc) return -EINVAL;
    brook::WmSetClientSideDecoration(idx, csd != 0);
    return 0;
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

static int64_t sys_getresuid(uint64_t ruidAddr, uint64_t euidAddr, uint64_t suidAddr,
                              uint64_t, uint64_t, uint64_t)
{
    if (ruidAddr) *reinterpret_cast<uint32_t*>(ruidAddr) = 0;
    if (euidAddr) *reinterpret_cast<uint32_t*>(euidAddr) = 0;
    if (suidAddr) *reinterpret_cast<uint32_t*>(suidAddr) = 0;
    return 0;
}

static int64_t sys_getresgid(uint64_t rgidAddr, uint64_t egidAddr, uint64_t sgidAddr,
                              uint64_t, uint64_t, uint64_t)
{
    if (rgidAddr) *reinterpret_cast<uint32_t*>(rgidAddr) = 0;
    if (egidAddr) *reinterpret_cast<uint32_t*>(egidAddr) = 0;
    if (sgidAddr) *reinterpret_cast<uint32_t*>(sgidAddr) = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// sys_pread64 (17) — read at offset without changing file position
// ---------------------------------------------------------------------------

static int64_t sys_pread64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                            uint64_t offset, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde) return -EBADF;

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

static int64_t sys_prctl(uint64_t option, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    (void)option;
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
    (void)dirfd; (void)mode;
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path) return -EFAULT;

    // Resolve relative paths
    char resolved[256];
    const char* lookup = path;
    if (path[0] != '/') {
        Process* proc = ProcessCurrent();
        uint32_t ci = 0;
        if (proc && proc->cwd[0]) {
            for (uint32_t j = 0; proc->cwd[j] && ci < 250; ++j)
                resolved[ci++] = proc->cwd[j];
            if (ci > 0 && resolved[ci - 1] != '/')
                resolved[ci++] = '/';
        }
        for (uint32_t j = 0; path[j] && ci < 254; ++j)
            resolved[ci++] = path[j];
        resolved[ci] = '\0';
        lookup = resolved;
    }

    Vnode* vn = VfsOpen(lookup, 0);
    if (!vn)
    {
        VnodeStat vs;
        if (!BusyboxStatFallback(lookup, &vs))
            return -ENOENT;
        return 0;
    }
    VfsClose(vn);
    return 0;
}

static int64_t sys_faccessat2(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                               uint64_t flags, uint64_t, uint64_t)
{
    (void)flags;
    return sys_faccessat(dirfd, pathAddr, mode, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_set_robust_list (273) — robust futex list (stub)
// ---------------------------------------------------------------------------

static int64_t sys_set_robust_list(uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t)
{
    return 0; // Success stub — no futex support yet
}

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
static constexpr int FUTEX_WAIT_BITSET  = 9;
static constexpr int FUTEX_WAKE_BITSET  = 10;
static constexpr int FUTEX_PRIVATE_FLAG    = 128;
static constexpr int FUTEX_CLOCK_REALTIME  = 256;

// Simple futex wait queue: hash table of blocked processes keyed by user VA.
// Since threads share address space (same page tables), the VA is sufficient.
static constexpr uint32_t FUTEX_HASH_SIZE = 64;

struct FutexWaiter {
    uint64_t uaddr;     // User virtual address being waited on
    Process* proc;      // Blocked process
    FutexWaiter* next;  // Next in hash bucket chain
};

static FutexWaiter* g_futexBuckets[FUTEX_HASH_SIZE];
static volatile uint64_t g_futexLock = 0;  // Spinlock for the hash table

// Pool of waiter nodes (avoid kmalloc from IRQ context)
static constexpr uint32_t FUTEX_MAX_WAITERS = 128;
static FutexWaiter g_futexWaiterPool[FUTEX_MAX_WAITERS];
static bool        g_futexWaiterUsed[FUTEX_MAX_WAITERS];

static FutexWaiter* FutexAllocWaiter()
{
    for (uint32_t i = 0; i < FUTEX_MAX_WAITERS; ++i) {
        if (!g_futexWaiterUsed[i]) {
            g_futexWaiterUsed[i] = true;
            return &g_futexWaiterPool[i];
        }
    }
    return nullptr;
}

static void FutexFreeWaiter(FutexWaiter* w)
{
    uint32_t idx = static_cast<uint32_t>(w - g_futexWaiterPool);
    if (idx < FUTEX_MAX_WAITERS)
        g_futexWaiterUsed[idx] = false;
}

static uint32_t FutexHash(uint64_t addr)
{
    return static_cast<uint32_t>((addr >> 2) % FUTEX_HASH_SIZE);
}

// Callable from outside syscall dispatch (e.g., scheduler thread exit)
extern "C" int64_t FutexWake(uint64_t uaddr, uint32_t maxWake)
{
    uint32_t bucket = FutexHash(uaddr);
    uint32_t woken = 0;

    while (__atomic_test_and_set(&g_futexLock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }

    FutexWaiter** pp = &g_futexBuckets[bucket];
    while (*pp && woken < maxWake) {
        FutexWaiter* w = *pp;
        if (w->uaddr == uaddr) {
            Process* waiter = w->proc;
            *pp = w->next;
            FutexFreeWaiter(w);
            __atomic_clear(&g_futexLock, __ATOMIC_RELEASE);

            __atomic_store_n(&waiter->pendingWakeup, 1, __ATOMIC_RELEASE);
            if (waiter->state == ProcessState::Blocked)
                SchedulerUnblock(waiter);
            woken++;

            while (__atomic_test_and_set(&g_futexLock, __ATOMIC_ACQUIRE)) {
                __asm__ volatile("pause");
            }
            pp = &g_futexBuckets[bucket];
        } else {
            pp = &(*pp)->next;
        }
    }

    __atomic_clear(&g_futexLock, __ATOMIC_RELEASE);
    return static_cast<int64_t>(woken);
}

static int64_t sys_futex(uint64_t uaddrVal, uint64_t opVal, uint64_t val,
                          uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    int op = static_cast<int>(opVal) & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

    if (op == FUTEX_WAKE || op == FUTEX_WAKE_BITSET) {
        uint32_t maxWake = static_cast<uint32_t>(val);
        if (maxWake == 0) return 0;
        int64_t r = FutexWake(uaddrVal, maxWake);
        return r;
    }

    if (op == FUTEX_WAIT || op == FUTEX_WAIT_BITSET) {
        auto* uaddr = reinterpret_cast<volatile uint32_t*>(uaddrVal);

        Process* proc = ProcessCurrent();
        if (!proc) {
            SerialPrintf("sys_futex: WAIT no current process!\n");
            return -ENOSYS;
        }

        // Acquire futex lock, atomically check value, and enqueue
        while (__atomic_test_and_set(&g_futexLock, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
        }

        // Check if *uaddr == val while holding the lock
        if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != static_cast<uint32_t>(val)) {
            __atomic_clear(&g_futexLock, __ATOMIC_RELEASE);
            return -EAGAIN;
        }

        // Allocate and enqueue waiter
        FutexWaiter* w = FutexAllocWaiter();
        if (!w) {
            __atomic_clear(&g_futexLock, __ATOMIC_RELEASE);
            SerialPrintf("sys_futex: ENOMEM (pool exhausted) pid=%u\n", proc->pid);
            return -ENOMEM;
        }
        w->uaddr = uaddrVal;
        w->proc = proc;
        uint32_t bucket = FutexHash(uaddrVal);
        w->next = g_futexBuckets[bucket];
        g_futexBuckets[bucket] = w;

        // Clear pending wakeup flag before blocking
        __atomic_store_n(&proc->pendingWakeup, 0, __ATOMIC_RELEASE);

        __atomic_clear(&g_futexLock, __ATOMIC_RELEASE);

        // Block this process until woken by FUTEX_WAKE
        SchedulerBlock(proc);

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

    DbgPrintf("sys_socket: domain=%lu type=%lu proto=%lu\n", domain, type, protocol);

    if (domain == AF_UNIX) {
        auto* usd = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
        if (!usd) return -ENOMEM;
        for (uint64_t i = 0; i < sizeof(UnixSocketData); i++)
            reinterpret_cast<uint8_t*>(usd)[i] = 0;
        usd->state    = UnixSocketData::State::Unbound;
        usd->nonblock = (type & UNIX_SOCK_NONBLOCK) != 0;
        usd->refCount = 1;

        int fd = FdAlloc(proc, FdType::UnixSocket, usd);
        if (fd < 0) { kfree(usd); return -EMFILE; }
        if (type & UNIX_SOCK_CLOEXEC) proc->fds[fd].fdFlags |= 1;
        DbgPrintf("sys_socket: AF_UNIX fd=%d\n", fd);
        return fd;
    }

    // AF_INET6 (10) — not supported
    if (domain != AF_INET) return -EAFNOSUPPORT;

    int sockIdx = SockCreate(static_cast<int>(domain),
                              static_cast<int>(type & 0xFF), // mask SOCK_NONBLOCK etc
                              static_cast<int>(protocol));
    if (sockIdx < 0) return -ENOMEM;

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
    FdEntry* fde = FdGet(proc, fd);
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if (addrVal < 0x1000) return -EFAULT;
        auto* ua = reinterpret_cast<const SockAddrUn*>(addrVal);
        if (ua->sun_family != AF_UNIX) return -EINVAL;
        // Copy path
        const char* src = ua->sun_path;
        char* dst = usd->path;
        for (int i = 0; i < 107 && src[i]; i++) dst[i] = src[i], dst[i+1] = 0;
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
    FdEntry* fde = FdGet(proc, fd);
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if (addrVal < 0x1000) return -EFAULT;
        auto* ua = reinterpret_cast<const SockAddrUn*>(addrVal);
        if (ua->sun_family != AF_UNIX) return -EINVAL;

        // Copy destination path
        char path[108] = {};
        for (int i = 0; i < 107 && ua->sun_path[i]; i++) path[i] = ua->sun_path[i];

        DbgPrintf("sys_connect: AF_UNIX fd=%d path='%s'\n", fd, path);

        // Find the listening server
        UnixSocketData* server = UnixFindServer(path);
        if (!server) return -ECONNREFUSED;
        if (server->pendingCount >= UNIX_ACCEPT_QUEUE) return -EAGAIN;

        // Create bidirectional pipe pair
        auto* pipeCS = static_cast<PipeBuffer*>(kmalloc(sizeof(PipeBuffer))); // client→server
        auto* pipeSC = static_cast<PipeBuffer*>(kmalloc(sizeof(PipeBuffer))); // server→client
        auto* fdqCS  = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue))); // fds client→server
        auto* fdqSC  = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue))); // fds server→client
        if (!pipeCS || !pipeSC || !fdqCS || !fdqSC) {
            if (pipeCS) kfree(pipeCS);
            if (pipeSC) kfree(pipeSC);
            if (fdqCS)  kfree(fdqCS);
            if (fdqSC)  kfree(fdqSC);
            return -ENOMEM;
        }
        for (uint64_t i = 0; i < sizeof(PipeBuffer); i++) {
            reinterpret_cast<uint8_t*>(pipeCS)[i] = 0;
            reinterpret_cast<uint8_t*>(pipeSC)[i] = 0;
        }
        for (uint64_t i = 0; i < sizeof(UnixFdQueue); i++) {
            reinterpret_cast<uint8_t*>(fdqCS)[i] = 0;
            reinterpret_cast<uint8_t*>(fdqSC)[i] = 0;
        }
        pipeCS->readers = 1; pipeCS->writers = 1;
        pipeSC->readers = 1; pipeSC->writers = 1;
        fdqCS->refCount = 1; // client side will point at it as peerIncomingFds; server gets a ref in accept
        fdqSC->refCount = 1; // server points at it as peerIncomingFds; client uses it as incomingFds

        // Set up client side
        usd->state   = UnixSocketData::State::Connected;
        usd->rxPipe  = pipeSC; // client reads server→client pipe
        usd->txPipe  = pipeCS; // client writes client→server pipe
        usd->incomingFds     = fdqSC; // client reads fds from server→client queue
        usd->peerIncomingFds = fdqCS; // client sends fds into client→server queue
        for (int i = 0; i < 107 && path[i]; i++) usd->path[i] = path[i];

        // Enqueue pending connection on server
        int slot = -1;
        for (int i = 0; i < UNIX_ACCEPT_QUEUE; i++) {
            if (!server->pending[i].used) { slot = i; break; }
        }
        if (slot < 0) { kfree(pipeCS); kfree(pipeSC); kfree(fdqCS); kfree(fdqSC); return -EAGAIN; }

        server->pending[slot].serverRx     = pipeCS; // server reads what client wrote
        server->pending[slot].serverTx     = pipeSC; // server writes what client reads
        server->pending[slot].serverRxFds  = fdqCS;  // server drains client-sent fds
        server->pending[slot].serverTxFds  = fdqSC;  // server posts fds for client
        server->pending[slot].clientWaiter = proc;
        server->pending[slot].accepted     = false;
        server->pending[slot].used         = true;
        server->pendingCount++;

        // Wake server if it's blocked in accept
        if (server->acceptWaiter) {
            Process* w = server->acceptWaiter;
            server->acceptWaiter = nullptr;
            __atomic_store_n(&w->pendingWakeup, 1, __ATOMIC_RELEASE);
            SchedulerUnblock(w);
        }
        // Also wake server if it's blocked in epoll_wait on this listen fd.
        UnixListenWakeEpoll(server);

        // Block until accepted (or nonblock)
        if (!usd->nonblock) {
            for (int iter = 0; iter < 100000 && !server->pending[slot].accepted; iter++) {
                Process* self = ProcessCurrent();
                self->wakeupTick = g_lapicTickCount + 10;
                SchedulerBlock(self);
                if (HasPendingSignals()) return -EINTR;
            }
            if (!server->pending[slot].accepted) return -ETIMEDOUT;
        }

        (void)addrLen;
        return 0;
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    (void)addrLen;

    auto* uaddr = reinterpret_cast<const brook::SockAddrIn*>(addrVal);
    if (!uaddr) return -EFAULT;

    DbgPrintf("sys_connect: fd=%d sockIdx=%d type=%d addr=%u.%u.%u.%u:%u\n",
                 fd, sockIdx, brook::SockGetType(sockIdx),
                 (brook::ntohl(uaddr->sin_addr) >> 24) & 0xFF,
                 (brook::ntohl(uaddr->sin_addr) >> 16) & 0xFF,
                 (brook::ntohl(uaddr->sin_addr) >> 8) & 0xFF,
                 brook::ntohl(uaddr->sin_addr) & 0xFF,
                 brook::ntohs(uaddr->sin_port));

    // For UDP, "connect" just sets the default destination
    // For TCP, perform the 3-way handshake
    return brook::SockConnect(sockIdx, uaddr);
}

static int64_t sys_sendto(uint64_t fdVal, uint64_t bufVal, uint64_t lenVal,
                           uint64_t flagsVal, uint64_t destVal, uint64_t addrLenVal)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);
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
    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    (void)flagsVal;

    // For TCP sockets, use SockRecv (stream receive)
    if (brook::SockIsStream(sockIdx))
    {
        auto* fde = FdGet(proc, fd);
        bool nonblock = fde && (fde->statusFlags & 0x800) != 0;
        if (nonblock && brook::SockRxCount(sockIdx) == 0 && !brook::SockPollReady(sockIdx, true, false))
            return -EAGAIN;
        int ret = brook::SockRecv(sockIdx,
                                   reinterpret_cast<void*>(bufVal),
                                   static_cast<uint32_t>(lenVal));
        return static_cast<int64_t>(ret);
    }

    // UDP path
    auto* src = srcVal ? reinterpret_cast<SockAddrIn*>(srcVal) : nullptr;
    int ret = SockRecvFrom(sockIdx, reinterpret_cast<void*>(bufVal),
                            static_cast<uint32_t>(lenVal), src);
    if (ret < 0) return -EAGAIN;
    return static_cast<int64_t>(ret);
}

static int64_t sys_setsockopt(uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t)
{
    return 0; // stub — pretend success
}

static int64_t sys_getsockopt(uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t)
{
    return 0; // stub
}

static int64_t sys_getsockname(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                                uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

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

    uint32_t copyLen = *ulen;
    if (copyLen > sizeof(local)) copyLen = sizeof(local);
    __builtin_memcpy(uaddr, &local, copyLen);
    *ulen = sizeof(local);
    return 0;
}

static int64_t sys_getpeername(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t)
{
    return -ENOTCONN;
}

static int64_t sys_shutdown(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t)
{
    return 0; // stub
}

static int64_t sys_listen(uint64_t fdVal, uint64_t backlog, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // AF_UNIX: mark socket as listening and register in global table
    FdEntry* fde = FdGet(proc, fd);
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        usd->state = UnixSocketData::State::Listening;
        UnixRegisterServer(usd);
        SerialPrintf("sys_listen: AF_UNIX fd=%d path='%s'\n", fd, usd->path);
        return 0;
    }

    (void)backlog;
    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    int ret = brook::SockListen(sockIdx, static_cast<int>(backlog));
    return ret < 0 ? -EINVAL : 0;
}

static int64_t sys_accept(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                           uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // AF_UNIX accept: dequeue a pending connection
    FdEntry* fde = FdGet(proc, fd);
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if (usd->state != UnixSocketData::State::Listening) return -EINVAL;

        // Wait for a pending connection
        for (;;) {
            // Find a pending (not yet accepted) slot
            int slot = -1;
            for (int i = 0; i < UNIX_ACCEPT_QUEUE; i++) {
                if (usd->pending[i].used && !usd->pending[i].accepted) {
                    slot = i; break;
                }
            }

            if (slot >= 0) {
                // Create a connected server-side socket
                auto* serverSock = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
                if (!serverSock) return -ENOMEM;
                for (uint64_t i = 0; i < sizeof(UnixSocketData); i++)
                    reinterpret_cast<uint8_t*>(serverSock)[i] = 0;
                serverSock->state  = UnixSocketData::State::Connected;
                serverSock->refCount = 1;
                serverSock->rxPipe = usd->pending[slot].serverRx;
                serverSock->txPipe = usd->pending[slot].serverTx;
                serverSock->incomingFds     = usd->pending[slot].serverRxFds; // fds client→server
                serverSock->peerIncomingFds = usd->pending[slot].serverTxFds; // fds server→client
                if (serverSock->incomingFds)
                    __atomic_fetch_add(&serverSock->incomingFds->refCount, 1, __ATOMIC_RELEASE);
                if (serverSock->peerIncomingFds)
                    __atomic_fetch_add(&serverSock->peerIncomingFds->refCount, 1, __ATOMIC_RELEASE);

                int newFd = FdAlloc(proc, FdType::UnixSocket, serverSock);
                if (newFd < 0) { kfree(serverSock); return -EMFILE; }

                // Mark accepted and wake the client
                usd->pending[slot].accepted = true;
                usd->pendingCount--;
                Process* client = usd->pending[slot].clientWaiter;
                usd->pending[slot].used = false;

                if (client) {
                    __atomic_store_n(&client->pendingWakeup, 1, __ATOMIC_RELEASE);
                    SchedulerUnblock(client);
                }

                // Zero out peer addr if requested (AF_UNIX has no meaningful peer addr)
                if (addrVal >= 0x1000 && addrLenVal >= 0x1000) {
                    auto* lenPtr = reinterpret_cast<uint32_t*>(addrLenVal);
                    *lenPtr = 0;
                }

                SerialPrintf("sys_accept: AF_UNIX fd=%d -> newFd=%d\n", fd, newFd);
                return newFd;
            }

            if (usd->nonblock) return -EAGAIN;

            // Block until a connection arrives
            usd->acceptWaiter = proc;
            proc->wakeupTick = g_lapicTickCount + 100;
            SchedulerBlock(proc);
            usd->acceptWaiter = nullptr;
            if (HasPendingSignals()) return -EINTR;
        }
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    brook::SockAddrIn peerAddr = {};
    int childIdx = brook::SockAccept(sockIdx, &peerAddr);
    if (childIdx < 0) return childIdx; // EAGAIN or error

    // Allocate a new fd for the accepted connection
    int newFd = FdAlloc(proc, FdType::Socket,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(childIdx + 1)));
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

// accept4 = accept with flags (SOCK_NONBLOCK, SOCK_CLOEXEC)
static int64_t sys_accept4(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                            uint64_t flags, uint64_t, uint64_t)
{
    int64_t ret = sys_accept(fdVal, addrVal, addrLenVal, 0, 0, 0);
    if (ret < 0) return ret;

    Process* proc = ProcessCurrent();
    if (proc) {
        int newFd = static_cast<int>(ret);
        if (flags & UNIX_SOCK_NONBLOCK) proc->fds[newFd].statusFlags |= 0x800;
        if (flags & UNIX_SOCK_CLOEXEC)  proc->fds[newFd].fdFlags |= 1;
    }
    return ret;
}

static int64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol,
                               uint64_t svAddr, uint64_t, uint64_t)
{
    SerialPrintf("sys_socketpair: domain=%lu type=0x%lx proto=%lu sv=0x%lx\n",
                 domain, type, protocol, svAddr);
    // We only support AF_UNIX (domain=1) socketpairs
    if (domain != 1) return -EAFNOSUPPORT;

    auto* sv = reinterpret_cast<int32_t*>(svAddr);
    if (!sv) return -EFAULT;

    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    // Allocate two PipeBuffers — one for each direction
    auto* pipeAtoB = static_cast<PipeBuffer*>(kmalloc(sizeof(PipeBuffer)));
    if (!pipeAtoB) return -ENOMEM;
    auto* pipeBtoA = static_cast<PipeBuffer*>(kmalloc(sizeof(PipeBuffer)));
    if (!pipeBtoA) { kfree(pipeAtoB); return -ENOMEM; }

    // Zero-init both
    for (uint64_t i = 0; i < sizeof(PipeBuffer); i++) {
        reinterpret_cast<uint8_t*>(pipeAtoB)[i] = 0;
        reinterpret_cast<uint8_t*>(pipeBtoA)[i] = 0;
    }
    pipeAtoB->readers = 1;
    pipeAtoB->writers = 1;
    pipeBtoA->readers = 1;
    pipeBtoA->writers = 1;

    // fd[0]: reads from pipeBtoA, writes to pipeAtoB
    // fd[1]: reads from pipeAtoB, writes to pipeBtoA
    // We store a small struct with both pipe pointers as the fd handle.
    struct SocketPairEnd {
        PipeBuffer* readPipe;
        PipeBuffer* writePipe;
    };

    auto* endA = static_cast<SocketPairEnd*>(kmalloc(sizeof(SocketPairEnd)));
    auto* endB = static_cast<SocketPairEnd*>(kmalloc(sizeof(SocketPairEnd)));
    if (!endA || !endB) {
        if (endA) kfree(endA);
        if (endB) kfree(endB);
        kfree(pipeAtoB);
        kfree(pipeBtoA);
        return -ENOMEM;
    }
    endA->readPipe = pipeBtoA;
    endA->writePipe = pipeAtoB;
    endB->readPipe = pipeAtoB;
    endB->writePipe = pipeBtoA;

    int fdA = FdAlloc(proc, FdType::Pipe, endA->readPipe);
    if (fdA < 0) { kfree(endA); kfree(endB); kfree(pipeAtoB); kfree(pipeBtoA); return -EMFILE; }
    // Store write pipe pointer in seekPos as a hack (we need both pipes accessible)
    proc->fds[fdA].seekPos = reinterpret_cast<uint64_t>(endA->writePipe);
    proc->fds[fdA].flags = 2;  // bidirectional marker
    proc->fds[fdA].statusFlags = (type & 0x800) ? 0x800 : 0;  // O_NONBLOCK if SOCK_NONBLOCK

    int fdB = FdAlloc(proc, FdType::Pipe, endB->readPipe);
    if (fdB < 0) { FdFree(proc, fdA); kfree(endA); kfree(endB); kfree(pipeAtoB); kfree(pipeBtoA); return -EMFILE; }
    proc->fds[fdB].seekPos = reinterpret_cast<uint64_t>(endB->writePipe);
    proc->fds[fdB].flags = 2;  // bidirectional marker
    proc->fds[fdB].statusFlags = (type & 0x800) ? 0x800 : 0;

    kfree(endA);
    kfree(endB);

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
        case FdType::MemFd:
            if (src->handle) MemFdRef(static_cast<MemFdData*>(src->handle));
            break;
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
        default:
            // Other fd types (Pipe, UnixSocket, EventFd, EpollFd, TimerFd,
            // DevDsp, etc.) don't have a portable snapshot refcount today.
            // Wayland only needs MemFd (for wl_shm), so refuse quietly.
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
    int fd = FdAlloc(dst, snap->type, snap->handle);
    if (fd < 0) return -EMFILE;
    dst->fds[fd].flags       = snap->flags;
    dst->fds[fd].fdFlags     = snap->fdFlags;
    dst->fds[fd].statusFlags = snap->statusFlags;
    return fd;
}

static void UnixFdSnapRelease(const UnixFdSnap* snap)
{
    if (!snap || !snap->handle) return;
    switch (snap->type) {
        case FdType::MemFd:
            MemFdUnref(static_cast<MemFdData*>(snap->handle));
            break;
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

static int64_t sys_sendmsg(uint64_t fdVal, uint64_t msgVal, uint64_t flagsVal,
                            uint64_t, uint64_t, uint64_t)
{
    (void)flagsVal;
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);
    if (msgVal < 0x1000) return -EFAULT;

    auto* msg = reinterpret_cast<LinuxMsgHdr*>(msgVal);
    if (!msg || msg->msg_iovlen == 0 || !msg->msg_iov) return -EINVAL;

    FdEntry* fde = FdGet(proc, fd);
    if (!fde || fde->type != FdType::UnixSocket) return -ENOSYS;

    auto* usd = static_cast<UnixSocketData*>(fde->handle);

    // Parse SCM_RIGHTS cmsg (if any) BEFORE writing payload. If cmsg parsing
    // fails (bad pointer, wrong type, fd full) we refuse the whole sendmsg
    // rather than send orphaned payload — matches Linux kernel behaviour.
    UnixFdSnap pendingSnaps[UNIX_FD_QUEUE_CAP];
    int pendingCount = 0;

    if (msg->msg_control && msg->msg_controllen >= sizeof(CmsgHdr)) {
        uint64_t ctlAddr = reinterpret_cast<uint64_t>(msg->msg_control);
        if (ctlAddr < 0x1000) return -EFAULT;

        uint64_t off = 0;
        while (off + sizeof(CmsgHdr) <= msg->msg_controllen) {
            auto* cm = reinterpret_cast<CmsgHdr*>(ctlAddr + off);
            if (cm->cmsg_len < sizeof(CmsgHdr) || cm->cmsg_len > msg->msg_controllen - off)
                break;

            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                uint64_t dataLen = cm->cmsg_len - sizeof(CmsgHdr);
                uint64_t nFds    = dataLen / sizeof(int);
                auto* fds = reinterpret_cast<int*>(
                    reinterpret_cast<uint64_t>(cm) + sizeof(CmsgHdr));

                for (uint64_t i = 0; i < nFds; i++) {
                    if (pendingCount >= UNIX_FD_QUEUE_CAP) {
                        for (int j = 0; j < pendingCount; j++)
                            UnixFdSnapRelease(&pendingSnaps[j]);
                        return -EAGAIN;
                    }
                    FdEntry* srcFde = FdGet(proc, fds[i]);
                    if (!srcFde || !UnixFdSnapFrom(srcFde, &pendingSnaps[pendingCount])) {
                        for (int j = 0; j < pendingCount; j++)
                            UnixFdSnapRelease(&pendingSnaps[j]);
                        return -EBADF;
                    }
                    pendingCount++;
                }
            }

            uint64_t step = CmsgAlign(cm->cmsg_len);
            if (step == 0) break;
            off += step;
        }
    }

    // Now enqueue the snapshots on the peer's incoming queue. If the queue
    // is full, fail — don't partial-send cmsg.
    if (pendingCount > 0) {
        UnixFdQueue* q = usd->peerIncomingFds;
        if (!q) {
            for (int j = 0; j < pendingCount; j++) UnixFdSnapRelease(&pendingSnaps[j]);
            return -ENOTCONN;
        }
        if (UnixFdQueueCount(q) + pendingCount >= UNIX_FD_QUEUE_CAP) {
            for (int j = 0; j < pendingCount; j++) UnixFdSnapRelease(&pendingSnaps[j]);
            return -EAGAIN;
        }
        for (int j = 0; j < pendingCount; j++) {
            q->msgs[q->head] = pendingSnaps[j];
            q->head = (q->head + 1) % UNIX_FD_QUEUE_CAP;
        }
    }

    // Write the iov payload. If we enqueued fds but the payload write fails,
    // drain the snaps back out so we don't leak/strand them. Linux actually
    // delivers cmsg with the first data byte; we approximate by requiring at
    // least one payload byte alongside any fds — Wayland always sends both.
    int64_t total = 0;
    for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
        void* base = msg->msg_iov[i].iov_base;
        uint64_t len = msg->msg_iov[i].iov_len;
        if (!base || len == 0) continue;
        int64_t ret = sys_write(fd, reinterpret_cast<uint64_t>(base), len, 0, 0, 0);
        if (ret < 0) {
            // If nothing was sent at all and we'd pushed fds, back them out.
            if (total == 0 && pendingCount > 0) {
                UnixFdQueue* q = usd->peerIncomingFds;
                for (int j = 0; j < pendingCount; j++) {
                    q->head = (q->head - 1 + UNIX_FD_QUEUE_CAP) % UNIX_FD_QUEUE_CAP;
                    UnixFdSnapRelease(&q->msgs[q->head]);
                }
            }
            return (total > 0) ? total : ret;
        }
        total += ret;
    }
    return total;
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

    auto* msg = reinterpret_cast<LinuxMsgHdr*>(msgVal);
    if (!msg || msg->msg_iovlen == 0 || !msg->msg_iov) return -EINVAL;

    FdEntry* fde = FdGet(proc, fd);
    if (fde && fde->type == FdType::UnixSocket) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);

        // Honor MSG_DONTWAIT: if no data is ready and no cmsg fds queued,
        // return EAGAIN immediately instead of blocking in sys_read.
        if (dontwait && usd->state == UnixSocketData::State::Connected && usd->rxPipe) {
            uint32_t avail = usd->rxPipe->count();
            int ctlN = usd->incomingFds ? UnixFdQueueCount(usd->incomingFds) : 0;
            bool writersGone = (__atomic_load_n(&usd->rxPipe->writers, __ATOMIC_ACQUIRE) == 0);
            if (avail == 0 && ctlN == 0 && !writersGone) {
                msg->msg_controllen = 0;
                msg->msg_flags = 0;
                return -EAGAIN;
            }
        }

        // Temporarily enable nonblock for the per-iov reads so even if the
        // first iov drains the pipe, a second iov doesn't block waiting for
        // bytes the peer hasn't sent yet (Wayland sends full messages at once).
        bool savedNonblock = usd->nonblock;
        if (dontwait) usd->nonblock = true;

        int64_t total = 0;
        for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
            void* base = msg->msg_iov[i].iov_base;
            uint64_t len = msg->msg_iov[i].iov_len;
            if (!base || len == 0) continue;
            int64_t ret = sys_read(fd, reinterpret_cast<uint64_t>(base), len, 0, 0, 0);
            if (ret < 0) {
                if (dontwait) usd->nonblock = savedNonblock;
                if (ret == -EAGAIN && total > 0) return total;
                return (total > 0) ? total : ret;
            }
            if (ret == 0) break;
            total += ret;
            // Partial read on an iov means pipe is now drained; don't spin
            // into the next iov expecting more.
            if ((uint64_t)ret < len) break;
        }

        if (dontwait) usd->nonblock = savedNonblock;

        // Drain any pending SCM_RIGHTS fds from our incoming queue into the
        // caller's cmsg buffer. We only deliver fds when we also delivered
        // at least one data byte — Wayland's preferred semantics and what
        // libc callers expect.
        msg->msg_flags = 0;
        uint64_t wroteCtl = 0;

        if (total > 0 && usd->incomingFds &&
            msg->msg_control && msg->msg_controllen >= sizeof(CmsgHdr))
        {
            uint64_t ctlAddr = reinterpret_cast<uint64_t>(msg->msg_control);
            if (ctlAddr >= 0x1000) {
                UnixFdQueue* q = usd->incomingFds;
                uint64_t cap   = msg->msg_controllen;
                uint64_t off   = 0;

                int n = UnixFdQueueCount(q);
                if (n > 0) {
                    // One SCM_RIGHTS cmsg can hold multiple fds back-to-back.
                    uint64_t fdsMax = (cap - sizeof(CmsgHdr)) / sizeof(int);
                    int installed = 0;
                    int wantFds   = n;
                    if ((uint64_t)wantFds > fdsMax) {
                        wantFds = static_cast<int>(fdsMax);
                        msg->msg_flags |= 0x8; // MSG_CTRUNC
                    }
                    if (wantFds > 0) {
                        auto* cm = reinterpret_cast<CmsgHdr*>(ctlAddr + off);
                        auto* fdSlot = reinterpret_cast<int*>(
                            reinterpret_cast<uint64_t>(cm) + sizeof(CmsgHdr));

                        for (int j = 0; j < wantFds; j++) {
                            UnixFdSnap snap = q->msgs[q->tail];
                            q->tail = (q->tail + 1) % UNIX_FD_QUEUE_CAP;

                            int newFd = UnixFdSnapInstall(proc, &snap);
                            if (newFd < 0) {
                                // Out of fds — put it back at the tail head-side
                                // (we already advanced tail; rewind one slot).
                                q->tail = (q->tail - 1 + UNIX_FD_QUEUE_CAP) % UNIX_FD_QUEUE_CAP;
                                q->msgs[q->tail] = snap;
                                break;
                            }
                            fdSlot[installed++] = newFd;
                        }

                        if (installed > 0) {
                            cm->cmsg_len   = sizeof(CmsgHdr) + installed * sizeof(int);
                            cm->cmsg_level = SOL_SOCKET;
                            cm->cmsg_type  = SCM_RIGHTS;
                            wroteCtl = CmsgAlign(cm->cmsg_len);
                        }
                    }
                }
            }
        }

        msg->msg_controllen = wroteCtl;
        return total;
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    void* buf = msg->msg_iov[0].iov_base;
    uint32_t len = static_cast<uint32_t>(msg->msg_iov[0].iov_len);
    if (!buf || len == 0) return -EINVAL;

    brook::SockAddrIn srcAddr;
    int ret = brook::SockRecvFrom(sockIdx, buf, len, &srcAddr);

    if (ret > 0 && msg->msg_name && msg->msg_namelen >= sizeof(brook::SockAddrIn)) {
        auto* dst = reinterpret_cast<brook::SockAddrIn*>(msg->msg_name);
        *dst = srcAddr;
    }

    msg->msg_flags = 0;
    msg->msg_controllen = 0;
    return ret;
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
    g_syscallTable[SYS_CLOSE]           = sys_close;
    g_syscallTable[SYS_STAT]            = sys_stat;
    g_syscallTable[SYS_FSTAT]           = sys_fstat;
    g_syscallTable[SYS_LSTAT]           = sys_lstat;
    g_syscallTable[SYS_LSEEK]           = sys_lseek;
    g_syscallTable[SYS_MMAP]            = sys_mmap;
    g_syscallTable[SYS_MPROTECT]        = sys_mprotect;
    g_syscallTable[SYS_MUNMAP]          = sys_munmap;
    g_syscallTable[SYS_MREMAP]          = sys_mremap;
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
    g_syscallTable[SYS_UNAME]           = sys_uname;
    g_syscallTable[SYS_FCNTL]           = sys_fcntl;
    g_syscallTable[SYS_GETCWD]          = sys_getcwd;
    g_syscallTable[SYS_GETTIMEOFDAY]    = sys_gettimeofday;
    g_syscallTable[SYS_TIME]             = sys_time;
    g_syscallTable[SYS_GETUID]          = sys_getuid;
    g_syscallTable[SYS_GETGID]          = sys_getgid;
    g_syscallTable[SYS_SETUID]          = sys_setuid;
    g_syscallTable[SYS_SETGID]          = sys_setgid;
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
    g_syscallTable[SYS_MKDIR]           = sys_mkdir;
    g_syscallTable[SYS_UNLINK]          = sys_unlink;
    g_syscallTable[SYS_SYMLINK]         = sys_symlink;
    g_syscallTable[SYS_READLINK]        = sys_readlink;
    g_syscallTable[SYS_UMASK]           = sys_umask;
    g_syscallTable[SYS_GETRLIMIT]       = sys_getrlimit;
    g_syscallTable[SYS_GETRUSAGE]       = sys_getrusage;
    g_syscallTable[SYS_SYSINFO]         = sys_sysinfo;
    g_syscallTable[SYS_SETPGID]         = sys_setpgid;
    g_syscallTable[SYS_GETPGRP]         = sys_getpgrp;
    g_syscallTable[SYS_SETSID]          = sys_setsid;
    g_syscallTable[SYS_GETPGID]         = sys_getpgid;
    g_syscallTable[SYS_GETSID]          = sys_getsid;
    g_syscallTable[SYS_SIGALTSTACK]     = sys_sigaltstack;
    g_syscallTable[SYS_ALARM]           = sys_alarm;
    g_syscallTable[SYS_PAUSE]           = sys_pause;
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
    g_syscallTable[SYS_SET_ROBUST_LIST] = sys_set_robust_list;
    g_syscallTable[SYS_FACCESSAT]       = sys_faccessat;
    g_syscallTable[SYS_RSEQ]            = sys_rseq;
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
    g_syscallTable[SYS_SCHED_SETAFFINITY] = sys_sched_setaffinity;
    g_syscallTable[SYS_SCHED_GETAFFINITY] = sys_sched_getaffinity;
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
    g_syscallTable[SYS_MBIND]           = sys_mbind;
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

    uint32_t count = 0;
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        if (g_syscallTable[i] != sys_not_implemented) ++count;

    SerialPrintf("SYSCALL: table initialised (%u entries, %u implemented)\n",
                 static_cast<unsigned>(SYSCALL_MAX), count);

    // Lock the dispatch table read-only. Any subsequent write attempt
    // (corruption, stray pointer) will #PF immediately on the offending
    // RIP rather than silently leaving a null entry to be discovered
    // later by an indirect call. The table fills exactly one 4KB page
    // (512 entries × 8 bytes) and lives in its own .syscall_table
    // section, so this is a single-page protect.
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
    case 61: return "wait4";      case 62: return "kill";
    case 63: return "uname";      case 72: return "fcntl";
    case 79: return "getcwd";     case 80: return "chdir";
    case 81: return "fchdir";     case 82: return "rename";
    case 83: return "mkdir";      case 87: return "unlink";
    case 88: return "symlink";    case 89: return "readlink";   case 95: return "umask";
    case 96: return "gettimeofday"; case 97: return "getrlimit";
    case 98: return "getrusage";  case 99: return "sysinfo";
    case 102: return "getuid";    case 104: return "getgid";
    case 105: return "setuid";    case 106: return "setgid";
    case 107: return "geteuid";   case 108: return "getegid";
    case 109: return "setpgid";   case 110: return "getppid";
    case 111: return "getpgrp";   case 112: return "setsid";
    case 115: return "getgroups"; case 116: return "setgroups";
    case 117: return "getresuid"; case 120: return "getresgid";
    case 121: return "getpgid";   case 124: return "getsid";
    case 131: return "sigaltstack"; case 130: return "rt_sigsuspend";
    case 134: return "statfs";
    case 34: return "pause"; case 37: return "alarm";
    case 135: return "fstatfs";   case 157: return "prctl";
    case 158: return "arch_prctl"; case 186: return "gettid";
    case 200: return "tkill";     case 201: return "time";       case 217: return "getdents64";
    case 218: return "set_tid_address"; case 228: return "clock_gettime";
    case 230: return "clock_nanosleep"; case 231: return "exit_group";
    case 234: return "tgkill";    case 257: return "openat";
    case 262: return "newfstatat"; case 266: return "symlinkat"; case 267: return "readlinkat";
    case 270: return "pselect6";  case 271: return "ppoll";     case 273: return "set_robust_list";
    case 292: return "dup3";
    case 290: return "eventfd2";  case 293: return "pipe2";     case 302: return "prlimit64";
    case 318: return "getrandom"; case 334: return "rseq";
    case 439: return "faccessat2";
    case 73: return "flock";      case 76: return "truncate";
    case 77: return "ftruncate";  case 86: return "link";
    case 90: return "chmod";      case 91: return "fchmod";
    case 92: return "chown";      case 93: return "fchown";
    case 94: return "lchown";
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

    bool isError = (ret < 0 && ret > -4096);
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
    if (proc && proc->straceEnabled)
        return SyscallDispatchTraced(num, a0, a1, a2, a3, a4, a5);
    SyscallFn fn = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
    if (!fn) {
        SerialPrintf("[syscall] FATAL-AVOIDED: pid=%u (%s) num=%lu has null/oob entry "
                     "(a0=%lx a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx) -> -ENOSYS\n",
                     proc ? proc->pid : 0, proc ? proc->name : "?",
                     num, a0, a1, a2, a3, a4, a5);
        return -38; // -ENOSYS
    }
    return fn(a0, a1, a2, a3, a4, a5);
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
        "swapgs\n\t"
        "iretq\n\t"
        ::: "memory"
    );
}

} // namespace brook

// ---------------------------------------------------------------------------
// Strace control functions
// ---------------------------------------------------------------------------

bool StraceEnablePid(uint32_t pid, bool enable)
{
    using namespace brook;
    Process* p = ProcessFindByPid(static_cast<uint16_t>(pid));
    if (!p) return false;
    p->straceEnabled = enable;
    return true;
}

int StraceEnableName(const char* name, bool enable)
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
                count++;
            }
        }
    }
    return count;
}

void StraceEnableAll(bool enable)
{
    using namespace brook;
    for (uint16_t pid = 1; pid < 256; pid++) {
        Process* p = ProcessFindByPid(pid);
        if (p) p->straceEnabled = enable;
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
