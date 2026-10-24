#include "signal_defaults.h"
#include "signal_restart.h"
#include "syscall_timing.h"
#include "file_locks.h"
#include "unix_lifetime_trace.h"
// process.cpp -- Process creation, fd table, user stack setup.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process.h"
#include "robust_futex.h"
#include "process_fs.h"
#include <stddef.h>
#include "posix_timer.h"
#include "sysv_sem.h"
#include "exec_limits.h"
#include "exec_image.h"
#include "scheduler.h"
#include "syscall.h"
#include "vfs.h"
#include "pipe.h"
#include "net.h"
#include "memory/virtual_memory.h"
#include "memory/user_mapping.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "serial.h"
#include "kprintf.h"
#include "compositor.h"
#include "terminal.h"
#include "window.h"
#include "gpu_app.h"
#include "ext2_vfs.h"
#include "string.h"
#include "spinlock.h"
#include "smp.h"
#include "apic.h"
#include "cpu.h"
#include "ksym_addrs.h"
#include "user_copy.h"
#include "signal_frame_layout.h"
#include "signal_fp_user.h"
#include "vdso.h"
#include "process_user_namespace.h"

namespace brook {
static_assert(offsetof(Process,mountNamespaceMember)+sizeof(Process::mountNamespaceMember)==offsetof(Process,fsContext),
              "fork/thread snapshot must exclude both filesystem ownership fields");
static_assert(sizeof(Process)-(offsetof(Process,fsContext)+sizeof(Process::fsContext))<alignof(Process),
              "filesystem ownership fields must remain last, allowing alignment padding");

namespace {
IrqSpinLock signalActionLock;
KernelSigaction signalActions[MAX_PROCESSES][64];
struct SignalActionGuard {
    uint64_t flags=IrqSpinLockAcquire(&signalActionLock);
    ~SignalActionGuard() { IrqSpinLockRelease(&signalActionLock,flags); }
};
}
KernelSigaction ExchangeSignalAction(uint16_t tgid,uint32_t index,const KernelSigaction* replacement) {
    if(tgid>=MAX_PROCESSES || index>=64)return {};
    SignalActionGuard held;
    const KernelSigaction previous=signalActions[tgid][index];
    if(replacement)signalActions[tgid][index]=*replacement;
    return previous;
}
KernelSigaction ReadSignalAction(uint16_t tgid,uint32_t index) {
    return ExchangeSignalAction(tgid,index,nullptr);
}
void ResetSignalActions(uint16_t tgid,bool preserveIgnore) {
    if(tgid>=MAX_PROCESSES)return;
    SignalActionGuard held;
    for(auto& action:signalActions[tgid])
        if(!preserveIgnore || action.handler!=1)action={};
}
void CopySignalActions(uint16_t destination,uint16_t source) {
    if(destination>=MAX_PROCESSES || source>=MAX_PROCESSES)return;
    SignalActionGuard held;
    for(unsigned index=0;index<64;++index)signalActions[destination][index]=signalActions[source][index];
}

struct ProcessOperationGuard {
    Process* process;
    explicit ProcessOperationGuard(Process* p) : process(p) {
        if (process) SchedulerKernelRegionEnter(process);
    }
    ~ProcessOperationGuard() { if (process) SchedulerKernelRegionLeave(process); }
};

// No credential lock is held across allocation, user copies or scheduling.
#include "process_credentials.inc"
#include "process_socket_metadata.inc"

// Exclude independently synchronized state from the raw process snapshot.
// NNP is inherited under the credentials lock below; other credentials are
// refreshed there using the existing path. The FS pointer is acquired separately
// and must never escape as a borrowed reference.
static void CopyParentProcess(Process* child, Process* parent) {
    constexpr size_t nnpEnd = offsetof(Process,noNewPrivs)+sizeof(child->noNewPrivs);
    constexpr size_t displayEnd = offsetof(Process,cwdDisplay)+sizeof(child->cwdDisplay);
    static_assert(nnpEnd <= offsetof(Process,cwdDisplay));
    memset(child,0,sizeof(Process));
    memcpy(child,parent,offsetof(Process,noNewPrivs));
    memcpy(reinterpret_cast<char*>(child)+nnpEnd,reinterpret_cast<const char*>(parent)+nnpEnd,
           offsetof(Process,cwdDisplay)-nnpEnd);
    memcpy(reinterpret_cast<char*>(child)+displayEnd,reinterpret_cast<const char*>(parent)+displayEnd,
           offsetof(Process,capabilities)-displayEnd);
    ProcessFsReadDisplay(parent,child->cwdDisplay,sizeof(child->cwdDisplay));
}

// Immediately after memcpy: the copied pointer is borrowed until this retains
// it. This same lock protects a concurrently running remote-thread target.
// Copies own references before publication; implemented with fd finalizers.
bool FdRetainCopy(FdEntry* copy);


// ProcessCurrent() is now in scheduler.cpp (g_currentProcess lives there).

// Helper: free a Process* and clear its magic so any stale pointer to this
// page that survives in the scheduler / wakeup lists fails its magic-check
// at next deref instead of corrupting the run.
//
// BRO-176 DIAGNOSTIC: every Process-struct free is also recorded in a small ring
// (ptr, freed pid, incarnation, caller return-address, seq). The scheduler's
// ReadyQueueInsertLocked guard, when it catches a stale/corrupt Process*, looks
// the pointer up here to name WHICH free site retired the struct and when —
// pinning the premature-free path for signature 2. Cheap: one locked ring write
// per process teardown (not a hot path). TEMPORARY — strip with the rest of the
// BRO-176 instrumentation before the fix commit.
struct ProcFreeRec {
    Process* ptr;
    uint64_t seq;
    uint64_t retaddr;     // __builtin_return_address(0) of FreeProcessStruct
    uint32_t incarnation;
    uint16_t pid;
};
static constexpr uint32_t PROCFREELOG_SIZE = 2048;
static ProcFreeRec  g_procFreeLog[PROCFREELOG_SIZE];
static uint64_t     g_procFreeSeq = 0;
static IrqSpinLock  g_procFreeLogLock;

// BRO-179: defined in sync/krwlock.cpp — true while KRwLockCleanupOnExit(p) is
// running on p. FreeProcessStruct uses it to detect a free racing the cleanup.
extern "C" int ProcessIsRwCleanupInFlight(void* p);

static inline void FreeProcessStruct(Process* p)
{
    if (p)
    {
        // Record the free BEFORE clearing magic / handing memory back, capturing
        // the caller so we can identify the exact teardown path.
        uint64_t ret = reinterpret_cast<uint64_t>(__builtin_return_address(0));
        uint64_t fl = IrqSpinLockAcquire(&g_procFreeLogLock);
        uint32_t i = g_procFreeSeq & (PROCFREELOG_SIZE - 1);
        g_procFreeLog[i] = { p, ++g_procFreeSeq, ret,
                             (uint32_t)p->incarnation, p->pid };
        IrqSpinLockRelease(&g_procFreeLogLock, fl);

        // BRO-179: detect the SIG1 process-lifetime UAF at the SOURCE — a free
        // racing an in-flight KRwLockCleanupOnExit(p). If this fires, the heap
        // poison we then write over p is exactly what the concurrent cleanup
        // reads out of p->heldWriteLock (kernel #GP). 'ret' names the racing
        // free path. (Detector only for now; the fix will serialize here.)
        if (ProcessIsRwCleanupInFlight(p)) {
            const char* sym = nullptr; uint64_t off = 0;
            bool haveSym = KsymFindByAddr(ret, &sym, &off);
            SerialPrintf("BRO179-FREE-DURING-RWCLEANUP: freeing proc=%p pid=%u "
                         "incarnation=%u while KRwLockCleanupOnExit IN-FLIGHT — "
                         "SIG1 UAF race! freedBy=0x%lx %s+0x%lx\n",
                         p, (unsigned)p->pid, (unsigned)p->incarnation, ret,
                         haveSym ? sym : "?", off);
        }

        ProcessFsClear(p);
        ProcessGroupsRelease(p->groups); p->groups = nullptr;
        UserNamespaceRelease(p->userNamespace);p->userNamespace=nullptr;
        MappingBackingUnref(p->fbBacking);p->fbBacking=nullptr;
        AddressSpaceUnref(p->addressSpace);
        p->addressSpace = nullptr;
        p->magic = 0;
    }
    kfree(p);
}

// BRO-176 diagnostic: print recent free records for a given Process* (newest
// first). Returns the number of matching records. Safe to call from the crash
// guard — takes only g_procFreeLogLock.
extern "C" int ProcessDumpFreeLog(void* ptr)
{
    Process* target = reinterpret_cast<Process*>(ptr);
    int found = 0;
    uint64_t fl = IrqSpinLockAcquire(&g_procFreeLogLock);
    uint32_t scan = (g_procFreeSeq < PROCFREELOG_SIZE)
                  ? (uint32_t)g_procFreeSeq : PROCFREELOG_SIZE;
    for (uint32_t n = 1; n <= scan; ++n)
    {
        uint32_t i = (uint32_t)((g_procFreeSeq - n) & (PROCFREELOG_SIZE - 1));
        ProcFreeRec& r = g_procFreeLog[i];
        if (r.ptr != target) continue;
        const char* sym = nullptr; uint64_t off = 0;
        bool haveSym = KsymFindByAddr(r.retaddr, &sym, &off);
        SerialPrintf("  BRO176-PROCFREE ptr=%p freedSeq=%lu pid=%u incarnation=%u "
                     "freedBy=0x%lx %s+0x%lx\n",
                     r.ptr, r.seq, (unsigned)r.pid, r.incarnation, r.retaddr,
                     haveSym ? sym : "?", off);
        if (++found >= 4) break;
    }
    IrqSpinLockRelease(&g_procFreeLogLock, fl);
    return found;
}

// BRO-176: monotonically-increasing incarnation id, never reused — lets a
// thread's teardown verify its leader struct hasn't been freed+reused for a
// different incarnation before decrementing that leader's asLiveThreads.
static uint64_t g_incarnationCounter = 0;
static inline uint64_t NextIncarnation()
{
    return __atomic_add_fetch(&g_incarnationCounter, 1, __ATOMIC_ACQ_REL);
}

static inline void VnodeHandleUnref(Vnode* vn)
{
    if (!vn) return;
    uint32_t prev = __atomic_fetch_sub(&vn->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1)
        VfsClose(vn);
}

void ProcessReleaseFileFault(Process* proc)
{
    if (!proc) return;
    Vnode* vn = __atomic_exchange_n(&proc->activeFileFaultVnode, nullptr, __ATOMIC_ACQ_REL);
    if (!vn) return;
    VfsFileMapUnpin(vn);
    VnodeHandleUnref(vn);
}

// ---------------------------------------------------------------------------
// File descriptor operations
// ---------------------------------------------------------------------------

// Allocate a fresh fd table. Returns nullptr on OOM. Each entry is zeroed
// so type==FdType::None and handle==nullptr.
static FdEntry* AllocFdTable()
{
    auto* table = static_cast<FdEntry*>(kmalloc(sizeof(FdEntry) * MAX_FDS));
    if (!table) return nullptr;
    memset(table, 0, sizeof(FdEntry) * MAX_FDS);
    return table;
}

int FdAlloc(Process* proc, FdType type, void* handle)
{
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    if (!proc || !proc->fds) return -24; // EMFILE
    int fd = FdTableAlloc(proc->fds, ProcessFdLock(proc), type, handle);
    if (fd < 0)
    {
        SerialPrintf("FD: pid %u exhausted %u fds\n", proc->pid, MAX_FDS);
        return -24; // EMFILE
    }
    return fd;
}

void FdFree(Process* proc, int fd)
{
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    if (!proc || !proc->fds) return;
    FdTableFree(proc->fds, ProcessFdLock(proc), fd);
}

FdEntry* FdGet(Process* proc, int fd)
{
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    if (!proc || !proc->fds) return nullptr;
    return FdTableGet(proc->fds, ProcessFdLock(proc), fd);
}

bool FdClaim(Process* proc, int fd, FdClaimResult* out)
{
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    if (!proc || !proc->fds) return false;
    FileLockCloseContext context(proc);
    return FdTableClaim(proc->fds, ProcessFdLock(proc), fd, out,FileLockCloseContext::Closing,&context);
}

FdEntry* FdGetRef(Process* proc, int fd)
{
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    if (!proc || !proc->fds) return nullptr;
    return FdTablePin(proc->fds, ProcessFdLock(proc), fd);
}

void FdPut(Process* proc, int fd)
{
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    if (!proc || !proc->fds) return;
    FdClaimResult c;
    // If this drops the last pin on a slot whose close() was deferred, the core
    // hands back the snapshot to finalize the handle outside the table lock.
    if (FdTableUnpin(proc->fds, ProcessFdLock(proc), fd, &c)) {
        UnixLifeClaim(fd, c);
        FinalizeClosedFd(c);
    }
}

FdCloseResult FdClose(Process* proc, int fd, FdClaimResult* out)
{
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    if (!proc || !proc->fds) return FdCloseResult::NotFound;
    FileLockCloseContext context(proc);
    return FdTableClose(proc->fds, ProcessFdLock(proc), fd, out,FileLockCloseContext::Closing,&context);
}

// ---------------------------------------------------------------------------
// Auxiliary vector constants (Linux ABI)
// ---------------------------------------------------------------------------

static constexpr uint64_t AT_NULL     = 0;
static constexpr uint64_t AT_PHDR     = 3;
static constexpr uint64_t AT_PHENT    = 4;
static constexpr uint64_t AT_PHNUM    = 5;
static constexpr uint64_t AT_PAGESZ   = 6;
static constexpr uint64_t AT_BASE     = 7;
static constexpr uint64_t AT_FLAGS    = 8;
static constexpr uint64_t AT_ENTRY    = 9;
static constexpr uint64_t AT_UID      = 11;
static constexpr uint64_t AT_EUID     = 12;
static constexpr uint64_t AT_GID      = 13;
static constexpr uint64_t AT_EGID     = 14;
static constexpr uint64_t AT_PLATFORM = 15;
static constexpr uint64_t AT_HWCAP    = 16;
static constexpr uint64_t AT_CLKTCK   = 17;
static constexpr uint64_t AT_SECURE   = 23;
static constexpr uint64_t AT_RANDOM   = 25;
static constexpr uint64_t AT_HWCAP2   = 26;
static constexpr uint64_t AT_EXECFN   = 31;

// Simple PRNG for AT_RANDOM (stack canary seed).
static uint64_t SimpleRand(uint64_t seed)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

// Generate a random 64-bit value using RDRAND if available, TSC fallback otherwise.
static uint64_t RandomU64()
{
    if (CpuHasRdrand()) {
        uint64_t val;
        uint8_t ok = 0;
        for (int i = 0; i < 10 && !ok; i++)
            __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        if (ok) return val;
    }
    uint64_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return SimpleRand((hi << 32) | lo);
}

// ---------------------------------------------------------------------------
// SetupTLS -- Allocate and initialize a TLS block for a process.
// Called by both ProcessCreate and ProcessExec.
// ---------------------------------------------------------------------------
static bool RegisterInitialRange(Process* proc, uint64_t begin, uint64_t end,
                                  uint32_t protection, MappingKind kind) {
    MappingRangePlan plan;
    MappingRange range{begin,end,0,0,nullptr,protection,7,kind,
                       MappingSharing::Private,MappingLeafOwnership::Managed};
    return plan.Prepare(1) && plan.Insert(proc->addressSpace->ranges,range);
}
static bool SetupTLS(Process* proc, uint64_t stackVirtBase, uint64_t guardPages)
{
    proc->fsBase = 0;
    proc->savedCtx.gsBase = 0;
    if (proc->elf.tlsTotalSize == 0) return true;

    uint64_t tlsPages = (proc->elf.tlsTotalSize + 64 + 4095) / 4096;
    uint64_t tlsBase = stackVirtBase - guardPages * 4096 - tlsPages * 4096;
    if (!RegisterInitialRange(proc,tlsBase,tlsBase+tlsPages*4096,3,MappingKind::Anonymous)) return false;

    auto tlsToKernel = [&](uint64_t userAddr) -> uint8_t* {
        PhysicalAddress phys = VmmVirtToPhys(proc->addressSpace->pageTable, VirtualAddress(userAddr));
        return phys ? reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw()) : nullptr;
    };

    bool tlsOk = true;
    for (uint64_t i = 0; i < tlsPages; i++)
    {
        VirtualAddress vaddr(tlsBase + i * 4096);
        PhysicalAddress phys = PmmAllocPage(MemTag::User, ProcessAddressSpacePid(proc));
        if (!phys || !VmmMapPage(proc->addressSpace->pageTable, vaddr, phys,
                                  VMM_WRITABLE | VMM_USER | VMM_NO_EXEC,
                                  MemTag::User, ProcessAddressSpacePid(proc)))
        {
            if (phys) PmmUnrefPage(phys);
            tlsOk = false;
            break;
        }
        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        memset(p, 0, 4096);
    }

    if (!tlsOk) return false;

    // Copy initial TLS data via direct map, page-at-a-time to avoid
    // a VmmVirtToPhys page-table walk per byte.
    if (proc->elf.tlsInitData && proc->elf.tlsInitSize > 0)
    {
        uint64_t srcAddr = reinterpret_cast<uint64_t>(proc->elf.tlsInitData);
        uint64_t remaining = proc->elf.tlsInitSize;
        uint64_t off = 0;
        while (remaining > 0)
        {
            uint64_t srcOff  = (srcAddr + off) & 0xFFF;
            uint64_t dstOff  = (tlsBase + off) & 0xFFF;
            uint64_t chunk = 4096 - (srcOff > dstOff ? srcOff : dstOff);
            if (chunk > remaining) chunk = remaining;

            uint8_t* src = tlsToKernel(srcAddr + off);
            uint8_t* dst = tlsToKernel(tlsBase + off);
            if (!src || !dst) return false;
            memcpy(dst, src, chunk);

            off += chunk;
            remaining -= chunk;
        }
    }

    // TCB pointer: variant II (x86-64), FS:0 = pointer to self
    uint64_t tcbAddr = tlsBase + proc->elf.tlsTotalSize;
    tcbAddr = (tcbAddr + 15) & ~15ULL;

    auto* tcbSlot = reinterpret_cast<uint64_t*>(tlsToKernel(tcbAddr));
    if (!tcbSlot) return false;
    *tcbSlot = tcbAddr;

    // Stack canary at offset 40 (0x28) from FS base
    uint64_t canary = RandomU64();
    if (tcbAddr + 48 < tlsBase + tlsPages * 4096)
    {
        auto* canarySlot = reinterpret_cast<uint64_t*>(
            tlsToKernel(tcbAddr + 0x28));
        if (canarySlot) *canarySlot = canary;
    }

    proc->fsBase = tcbAddr;
    return true;
}

// ---------------------------------------------------------------------------
// SetupUserStack -- Build the initial user stack with argc/argv/envp/auxv.
// ---------------------------------------------------------------------------
// Linux x86-64 process stack layout (top = high address, stack grows down):
//
//   [random bytes for AT_RANDOM]    (16 bytes)
//   [environment strings]           (null-terminated)
//   [argument strings]              (null-terminated)
//   [padding to 16-byte align]
//   [auxv: AT_NULL, 0]
//   [auxv: AT_RANDOM, ptr]
//   [auxv: AT_ENTRY, entry]
//   [auxv: AT_PAGESZ, 4096]
//   [auxv: AT_PHNUM, phnum]
//   [auxv: AT_PHENT, phentsize]
//   [auxv: AT_PHDR, phdr]
//   [envp[envc] = NULL]
//   [envp[0..envc-1]]
//   [argv[argc] = NULL]
//   [argv[0..argc-1]]
//   [argc]                          <-- RSP points here on entry

static uint64_t SetupUserStack(Process* proc,
                                int argc, const char* const* argv,
                                int envc, const char* const* envp,
                                uint64_t interpBase = 0, uint64_t vdsoBase = 0)
{
    // Work from the top of the stack downward.
    uint64_t sp = proc->stackTop;

    // Translate user vaddr to kernel-writable pointer via direct map.
    auto toKernel = [&](uint64_t userAddr) -> uint8_t* {
        PhysicalAddress phys = VmmVirtToPhys(proc->addressSpace->pageTable, VirtualAddress(userAddr));
        return phys ? reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw()) : nullptr;
    };

    // Helper: push bytes onto the stack (writes via direct map)
    auto pushBytes = [&](const void* data, uint64_t len) -> uint64_t {
        sp -= len;
        const auto* src = reinterpret_cast<const uint8_t*>(data);
        for (uint64_t i = 0; i < len; ++i)
        {
            uint8_t* dst = toKernel(sp + i);
            if (dst) *dst = src[i];
        }
        return sp;
    };

    auto pushU64 = [&](uint64_t val) -> uint64_t {
        return pushBytes(&val, 8);
    };

    // 1. Push 16 random bytes for AT_RANDOM (RDRAND if available, TSC fallback)
    pushU64(RandomU64());
    uint64_t randomAddr = pushU64(RandomU64());

    // 1b. Push AT_PLATFORM string + AT_EXECFN string
    static const char k_plat[] = "x86_64";
    uint64_t platformAddr = pushBytes(k_plat, sizeof(k_plat));
    uint64_t execfnAddr = (argc > 0 && argv && argv[0])
                          ? pushBytes(argv[0], 0) // populated below; kept 0 if no arg
                          : 0;
    if (argc > 0 && argv && argv[0]) {
        uint64_t len = 0; while (argv[0][len]) ++len;
        execfnAddr = pushBytes(argv[0], len + 1);
    }

    // 2. Push environment strings and record pointers
    // ProcessCreate/ProcessExec validate counts and byte budgets before
    // allocating or replacing the address space. Preserve every entry.
    uint64_t envAddrs[EXEC_MAX_ENV] = {};
    for (int i = envc - 1; i >= 0; --i)
    {
        uint64_t len = 0;
        while (envp[i][len]) ++len;
        envAddrs[i] = pushBytes(envp[i], len + 1);
    }

    // 3. Push argument strings and record pointers
    uint64_t argAddrs[EXEC_MAX_ARGS] = {};
    const uint64_t argumentEnd = sp;
    for (int i = argc - 1; i >= 0; --i)
    {
        uint64_t len = 0;
        while (argv[i][len]) ++len;
        argAddrs[i] = pushBytes(argv[i], len + 1);
    }
    KMutexLock(&proc->addressSpace->mutation);
    proc->addressSpace->argumentStart = sp;
    proc->addressSpace->argumentEnd = argumentEnd;
    KMutexUnlock(&proc->addressSpace->mutation);

    // 4. Align to 16 bytes
    sp &= ~0xFULL;

    // Compute total 8-byte slots to be pushed below, and pad so final RSP
    // is 16-byte aligned (Linux ABI: RSP mod 16 == 0 at process entry).
    //   auxv: 19 entries plus optional AT_SYSINFO_EHDR, two slots each
    //   envp: (envc + 1) slots (including NULL terminator)
    //   argv: (argc + 1) slots (including NULL terminator)
    //   argc: 1 slot
    int totalSlots = 38 + (vdsoBase ? 2 : 0) + (envc + 1) + (argc + 1) + 1;
    if (totalSlots & 1)
        pushU64(0); // padding to maintain 16-byte alignment

    // 5. Push auxiliary vectors (in reverse order, so first entry is at lowest address)
    pushU64(0); pushU64(AT_NULL);                               // AT_NULL
    if (vdsoBase) { pushU64(vdsoBase); pushU64(33); }            // AT_SYSINFO_EHDR
    pushU64(execfnAddr); pushU64(AT_EXECFN);                    // AT_EXECFN
    pushU64(0); pushU64(AT_HWCAP2);                             // AT_HWCAP2 = 0
    pushU64(randomAddr); pushU64(AT_RANDOM);                    // AT_RANDOM
    pushU64(proc->uid != proc->euid || proc->gid != proc->egid); pushU64(AT_SECURE);                             // AT_SECURE: differing real/effective credentials
    pushU64(100); pushU64(AT_CLKTCK);                           // AT_CLKTCK = 100
    pushU64(0); pushU64(AT_HWCAP);                              // AT_HWCAP = 0 (conservative; some IFUNC libs may need real bits)
    pushU64(platformAddr); pushU64(AT_PLATFORM);                // AT_PLATFORM = "x86_64"
    pushU64(ProcessReportId(proc,true,proc->egid)); pushU64(AT_EGID);                               // AT_EGID
    pushU64(ProcessReportId(proc,true,proc->gid)); pushU64(AT_GID);                                // AT_GID
    pushU64(ProcessReportId(proc,false,proc->euid)); pushU64(AT_EUID);                               // AT_EUID
    pushU64(ProcessReportId(proc,false,proc->uid)); pushU64(AT_UID);                                // AT_UID
    pushU64(proc->elf.entryPoint); pushU64(AT_ENTRY);           // AT_ENTRY
    pushU64(0); pushU64(AT_FLAGS);                              // AT_FLAGS = 0
    pushU64(interpBase); pushU64(AT_BASE);                       // AT_BASE (interpreter load addr, 0 if static)
    pushU64(4096); pushU64(AT_PAGESZ);                          // AT_PAGESZ
    pushU64(proc->elf.phdrNum); pushU64(AT_PHNUM);              // AT_PHNUM
    pushU64(proc->elf.phdrEntSize); pushU64(AT_PHENT);          // AT_PHENT
    pushU64(proc->elf.phdrVaddr); pushU64(AT_PHDR);             // AT_PHDR

    // 6. Push envp array (null-terminated).
    pushU64(0); // envp[envc] = NULL
    for (int i = envc - 1; i >= 0; --i)
        pushU64(envAddrs[i]);

    // 7. Push argv array (null-terminated)
    pushU64(0); // argv[argc] = NULL
    for (int i = argc - 1; i >= 0; --i)
        pushU64(argAddrs[i]);

    // 8. Push argc
    pushU64(static_cast<uint64_t>(argc));

    return sp;
}

// ---------------------------------------------------------------------------
// ProcessCreate
// ---------------------------------------------------------------------------

// Forward declaration — implemented in elf_loader.cpp
bool ElfLoad(const uint8_t* data, uint64_t size, ElfBinary* out,
             AddressSpaceState* space, uint16_t pid);
uint64_t ElfLoadAt(const uint8_t* data, uint64_t size,
                   uint64_t base, AddressSpaceState* space, uint16_t pid);

// Base address where the dynamic linker / interpreter is loaded.
// Placed above USER_MMAP_END so it can never collide with sys_mmap allocations.
static constexpr uint64_t INTERP_LOAD_BASE = 0x7F0000000000ULL; // ~127 TB

// ---------------------------------------------------------------------------
// LoadInterpreter -- Read and load the ELF interpreter specified in
// proc->elf.interpPath.  Returns the interpreter entry point, or 0 on
// failure.  The interpreter is loaded at INTERP_LOAD_BASE.
// ---------------------------------------------------------------------------
static int ReadInterpreterImage(Process* process,const char* path,ExecutableImage* image)
{
    *image={};Vnode* vnode=nullptr;char resolved[256];
    FsSnapshot fs{};int error=ProcessFsSnapshot(process,&fs);
    if(error && (!process || process->isKernelThread)) {
        error=VfsPathRoot(&fs.root);
        if(!error) {fs.cwd=fs.root;VfsPathRetain(fs.cwd);}
    }
    if(error)return error;
    struct Drop {FsSnapshot* fs;~Drop(){FsSnapshotRelease(fs);}} drop{&fs};
    error=VfsOpenExecutableAt(fs.root,fs.cwd,path,&vnode,resolved,sizeof(resolved));
    // Historical aliases remain compatibility fallbacks for missing paths,
    // never a way around an existing interpreter's permission denial.
    char alternate[256];
    if(error==-2) {
        const uint32_t length=strlen(path);
        if(length+5>=sizeof(alternate))return -36;
        memcpy(alternate,"/boot",5);memcpy(alternate+5,path,length+1);
        error=VfsOpenExecutableAt(fs.root,fs.cwd,alternate,&vnode,resolved,sizeof(resolved));
    }
    if(error==-2) {
        const char* name=path;
        for(const char* p=path;*p;++p)if(*p=='/')name=p+1;
        const uint32_t length=strlen(name);
        if(length+10>=sizeof(alternate))return -36;
        memcpy(alternate,"/boot/lib/",10);memcpy(alternate+10,name,length+1);
        error=VfsOpenExecutableAt(fs.root,fs.cwd,alternate,&vnode,resolved,sizeof(resolved));
    }
    if(error)return error;
    error=ReadExecutableImage(vnode,image);VfsClose(vnode);
    if(error)return error;
    if(image->size<64) {FreeExecutableImage(image);return -5;} // short ELF header: EIO
    if(!ElfValidateInterpreterImage(image->data,image->size,INTERP_LOAD_BASE)) {
        FreeExecutableImage(image);return -80; // ELIBBAD
    }
    return 0;
}

int PrepareExecInterpreter(const ExecutableImage& main,ExecutableImage* output)
{
    if(!output)return -22;
    *output={};char path[sizeof(ElfBinary{}.interpPath)];
    if(!ElfMainInterpreter(main.data,main.size,path,sizeof(path)))return -8;
    return path[0]?ReadInterpreterImage(ProcessCurrent(),path,output):0;
}

static uint64_t LoadInterpreter(Process* proc,const ExecutableImage* prepared=nullptr)
{
    if(!proc->elf.interpPath[0])return 0;
    ExecutableImage owned{};
    if(!prepared) {
        const int error=ReadInterpreterImage(proc,proc->elf.interpPath,&owned);
        if(error) {SerialPrintf("INTERP: open/load error %d for '%s'\n",error,proc->elf.interpPath);return 0;}
        prepared=&owned;
    }
    const uint64_t entry=ElfLoadAt(prepared->data,prepared->size,INTERP_LOAD_BASE,
                                   proc->addressSpace,proc->pid);
    FreeExecutableImage(&owned);return entry;
}

static Process* ProcessCreateImpl(const uint8_t* elfData, uint64_t elfSize,
                       int argc, const char* const* argv,
                       int envc, const char* const* envp,
                       const FdEntry* stdFds, uint32_t launchUid, uint32_t launchGid, const ExecutableImage* owned)
{
    if (!ExecArgumentsFit(argc, argv, envc, envp)) return nullptr;
    auto* proc = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!proc) return nullptr;

    // Zero-initialize
    auto* raw = reinterpret_cast<uint8_t*>(proc);
    memset(raw, 0, sizeof(Process));
    proc->magic = PROCESS_MAGIC;
    proc->incarnation = NextIncarnation();
    proc->uid = proc->euid = proc->suid = launchUid;
    proc->gid = proc->egid = proc->sgid = launchGid;
    proc->leaderIncarnation = 0;

    // Initialize FPU/SSE state with safe defaults so fxrstor works correctly
    // on the first context switch to this process.
    // FCW (bytes 0-1): 0x037F = mask all FPU exceptions
    // MXCSR (bytes 24-27): 0x1F80 = mask all SSE exceptions
    proc->fxsave.data[0] = 0x7F;
    proc->fxsave.data[1] = 0x03;
    proc->fxsave.data[24] = 0x80;
    proc->fxsave.data[25] = 0x1F;

    proc->pid = SchedulerAllocUserPid();
    if (!proc->pid) {
        FreeProcessStruct(proc);
        return nullptr;
    }
    proc->pgid = proc->pid;
    proc->sid = proc->pid;
    proc->tgid = proc->pid;         // Process is its own thread group leader
    proc->threadLeader = proc;
    proc->state = ProcessState::Ready;
    proc->runningOnCpu = -1;
    proc->cpuAffinity = -1;
    proc->tlbCpuMask = 0;

    proc->schedPriority = 2;  // SCHED_PRIORITY_NORMAL

    // Allocate shared fd table (shared across CLONE_FILES threads).
    proc->fds = AllocFdTable();
    if (!proc->fds)
    {
        SerialPuts("PROC: fd table allocation failed\n");
        SchedulerFreePid(proc->pid);
        FreeProcessStruct(proc);
        return nullptr;
    }

    if(ProcessFsInitialize(proc)) {
        kfree(proc->fds);SchedulerFreePid(proc->pid);FreeProcessStruct(proc);return nullptr;
    }

    // Allocate per-process kernel stack with guard pages (for ring 3→0 transitions).
    VirtualAddress kstackAddr = VmmAllocKernelStack(KERNEL_STACK_PAGES,
        MemTag::KernelData, proc->pid);
    if (!kstackAddr)
    {
        SerialPuts("PROC: kernel stack allocation failed\n");
        kfree(proc->fds);
        SchedulerFreePid(proc->pid);
        FreeProcessStruct(proc);
        return nullptr;
    }
    proc->kernelStackBase = kstackAddr.raw();
    proc->kernelStackTop  = kstackAddr.raw() + KERNEL_STACK_SIZE;

    // Create per-process page table
    proc->addressSpace = AddressSpaceCreate(proc->pid);
    if (!proc->addressSpace)
    {
        SerialPuts("PROC: page table allocation failed\n");
        VmmFreeKernelStack(kstackAddr, KERNEL_STACK_PAGES);
        kfree(proc->fds);
        SchedulerFreePid(proc->pid);
        FreeProcessStruct(proc);
        return nullptr;
    }

    // Load ELF binary into the process's page table (no CR3 switch needed —
    // ElfLoad writes via the direct physical map).
    if (!(owned ? ElfLoadExecutable(*owned,&proc->elf,proc->addressSpace,proc->pid) :
                  ElfLoad(elfData,elfSize,&proc->elf,proc->addressSpace,proc->pid)))
    {
        SerialPuts("PROC: ELF load failed\n");

        VmmFreeKernelStack(kstackAddr, KERNEL_STACK_PAGES);
        kfree(proc->fds);
        SchedulerFreePid(proc->pid);
        FreeProcessStruct(proc);
        return nullptr;
    }

    // Verify page table integrity: ELF load area (PML4[0]) must have USER bit.
    {
        auto* pml4 = reinterpret_cast<volatile uint64_t*>(PhysToVirt(proc->addressSpace->pageTable.pml4).raw());
        uint64_t pml4e = pml4[0]; // PML4 entry for vaddr 0x000000-0x7FFFFFFFFF
        if (pml4e && !(pml4e & 4))
            SerialPrintf("PROC: BUG: pid %u PML4[0]=0x%lx missing USER bit!\n",
                         proc->pid, pml4e);

        if (pml4e & 1) // present
        {
            auto* pdpt = reinterpret_cast<volatile uint64_t*>(
                PhysToVirt(PhysicalAddress(pml4e & 0x000FFFFFFFFFF000ULL)).raw());
            uint64_t pdpte = pdpt[0];
            if (pdpte && !(pdpte & 4))
                SerialPrintf("PROC: BUG: pid %u PDPT[0]=0x%lx missing USER bit!\n",
                             proc->pid, pdpte);

            if (pdpte & 1)
            {
                auto* pd = reinterpret_cast<volatile uint64_t*>(
                    PhysToVirt(PhysicalAddress(pdpte & 0x000FFFFFFFFFF000ULL)).raw());
                uint64_t pde = pd[2]; // PD[2] = 0x400000-0x5FFFFF
                if (pde & 0x80)
                    SerialPrintf("PROC: BUG: pid %u PD[2]=0x%lx is 2MB page (should be 4KB PT)!\n",
                                 proc->pid, pde);
                else if (pde && !(pde & 4))
                    SerialPrintf("PROC: BUG: pid %u PD[2]=0x%lx missing USER bit!\n",
                                 proc->pid, pde);
            }
        }
    }

    // Print FNV-1a hash of ELF data so we can verify the on-disk binary
    // matches the one we built (catches stale disk image issues).
    {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (uint64_t i = 0; i < elfSize; i++)
        {
            h ^= elfData[i];
            h *= 0x100000001b3ULL;
        }
        SerialPrintf("ELF HASH: %s size=%lu hash=0x%lx entry=0x%lx\n",
                     argv[0], elfSize, h, proc->elf.entryPoint);
    }

    proc->addressSpace->programBreak = proc->elf.programBreakLow;
    proc->addressSpace->programBreakLow = proc->elf.programBreakLow;
    proc->addressSpace->programBreakHigh = proc->elf.programBreakHigh;
    // Keep mmap beyond both the loaded image and its reserved brk range.
    uint64_t mmapFloor = (proc->elf.programBreakHigh + 16 * 1024 * 1024 + 4095) & ~uint64_t{4095};
    proc->addressSpace->mmapNext = mmapFloor > USER_MMAP_BASE ? mmapFloor : USER_MMAP_BASE;

    // Load dynamic linker if the ELF specifies PT_INTERP.
    uint64_t interpEntry = 0;
    uint64_t interpBase  = 0;
    if (proc->elf.interpPath[0] != '\0')
    {
        interpEntry = LoadInterpreter(proc);
        if (!interpEntry)
        {
            SerialPrintf("PROC: failed to load interpreter '%s'\n",
                         proc->elf.interpPath);

            VmmFreeKernelStack(VirtualAddress(proc->kernelStackBase), KERNEL_STACK_PAGES);
            kfree(proc->fds);
            SchedulerFreePid(proc->pid);
            FreeProcessStruct(proc);
            return nullptr;
        }
        interpBase = INTERP_LOAD_BASE;
    }

    // Allocate user stack at fixed user-space addresses (below USER_STACK_TOP).
    // Physical pages are allocated individually and mapped into the process
    // page table.  No VMALLOC — the stack lives entirely in user-half.
    uint64_t stackPages = USER_STACK_SIZE / 4096;
    uint64_t guardPages = 1;
    uint64_t totalStackPages = stackPages + guardPages;
    uint64_t stackVirtTop = USER_STACK_TOP;            // 0x7FFFFFFFE000
    uint64_t stackVirtBase = stackVirtTop - totalStackPages * 4096;

    bool stackOk = RegisterInitialRange(proc,stackVirtBase,stackVirtBase+guardPages*4096,0,MappingKind::Stack) &&
                   RegisterInitialRange(proc,stackVirtBase+guardPages*4096,stackVirtTop,3,MappingKind::Stack);
    for (uint64_t i = guardPages; stackOk && i < totalStackPages; i++)
    {
        VirtualAddress vaddr(stackVirtBase + i * 4096);
        PhysicalAddress phys = PmmAllocPage(MemTag::User, ProcessAddressSpacePid(proc));
        if (!phys || !VmmMapPage(proc->addressSpace->pageTable, vaddr, phys,
                                  VMM_WRITABLE | VMM_USER | VMM_NO_EXEC,
                                  MemTag::User, ProcessAddressSpacePid(proc)))
        {
            if (phys) PmmUnrefPage(phys);
            stackOk = false;
            break;
        }
        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        memset(p, 0, 4096);
    }
    if (!stackOk)
    {
        SerialPuts("PROC: stack allocation failed\n");

        VmmFreeKernelStack(VirtualAddress(proc->kernelStackBase), KERNEL_STACK_PAGES);
        kfree(proc->fds);
        SchedulerFreePid(proc->pid);
        FreeProcessStruct(proc);
        return nullptr;
    }
    // Guard page (first page) is left unmapped — faults on stack overflow.
    proc->stackBase = stackVirtBase + guardPages * 4096;
    proc->stackTop  = stackVirtTop - 8;
    if (!SetupTLS(proc,stackVirtBase,guardPages)) {
        VmmFreeKernelStack(VirtualAddress(proc->kernelStackBase),KERNEL_STACK_PAGES);
        kfree(proc->fds); SchedulerFreePid(proc->pid); FreeProcessStruct(proc); return nullptr;
    }

    // Set up standard file descriptors
    if (stdFds)
    {
        // Use caller-provided FDs (e.g. pipes from terminal emulator)
        proc->fds[0] = stdFds[0];
        proc->fds[1] = stdFds[1];
        proc->fds[2] = stdFds[2];
    }
    else
    {
        // Default: fd 0 = keyboard, fd 1/2 = serial
        proc->fds[0].type = FdType::DevKeyboard;
        proc->fds[1].type = FdType::Vnode; // treated as serial stdout in syscall
        proc->fds[1].statusFlags = 1; // O_WRONLY
        proc->fds[2].type = FdType::Vnode; // treated as serial stderr in syscall
        proc->fds[2].statusFlags = 2; // O_RDWR
    }
    // Initially provide debug serial at fd 3. Closing/reusing the slot must
    // obey the replacement descriptor's type, just like any other fd.
    proc->fds[3].type = FdType::DebugSerial;
    proc->fds[3].statusFlags = 1; // O_WRONLY

    // Default TTY mode: canonical with echo
    proc->ttyCanonical = true;
    proc->ttyEcho = true;
    proc->straceEnabled = false;
    proc->straceErrorsOnly = false;

    // Build user stack with argc/argv/envp/auxv
    const uint64_t vdsoBase = VdsoInstall(proc->addressSpace, RandomU64());
    uint64_t userSP = SetupUserStack(proc, argc, argv, envc, envp, interpBase, vdsoBase);

    // Store the final SP for SwitchToUserMode
    proc->stackTop = userSP;

    // TLS was constructed before accepting caller-owned standard descriptors.

    // Set actual initial entry point (interpreter entry if dynamically linked).
    proc->initialEntry = interpEntry ? interpEntry : proc->elf.entryPoint;

    DbgPrintf("PROC: created pid=%u, entry=0x%lx, stack=0x%lx, brk=0x%lx, cr3=0x%lx\n",
                 proc->pid, proc->initialEntry, proc->stackTop,
                 proc->addressSpace->programBreak, proc->addressSpace->pageTable.pml4.raw());

    AddressSpacePublish(proc->addressSpace);
    return proc;
}

Process* ProcessCreate(const uint8_t* elfData, uint64_t elfSize,
                       int argc, const char* const* argv,
                       int envc, const char* const* envp,
                       const FdEntry* stdFds, uint32_t launchUid, uint32_t launchGid)
{
    return ProcessCreateImpl(elfData,elfSize,argc,argv,envc,envp,stdFds,launchUid,launchGid,nullptr);
}

Process* ProcessCreateExecutable(const ExecutableImage& image, int argc, const char* const* argv,
    int envc, const char* const* envp, const FdEntry* stdFds,
    uint32_t launchUid, uint32_t launchGid)
{
    return ProcessCreateImpl(image.data,image.size,argc,argv,envc,envp,stdFds,launchUid,launchGid,&image);
}

Process* KernelThreadCreate(const char* name, KernelThreadFn fn, void* arg,
                            uint8_t priority)
{
    auto* proc = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!proc) return nullptr;

    // Zero-initialize
    auto* raw = reinterpret_cast<uint8_t*>(proc);
    memset(raw, 0, sizeof(Process));
    proc->magic = PROCESS_MAGIC;
    proc->incarnation = NextIncarnation();
    proc->leaderIncarnation = 0;

    // Initialize FPU/SSE state
    proc->fxsave.data[0] = 0x7F;
    proc->fxsave.data[1] = 0x03;
    proc->fxsave.data[24] = 0x80;
    proc->fxsave.data[25] = 0x1F;

    proc->pid = SchedulerAllocPid();
    if (!proc->pid) {
        FreeProcessStruct(proc);
        return nullptr;
    }
    proc->pgid = proc->pid;
    proc->sid = proc->pid;
    proc->tgid = proc->pid;
    proc->threadLeader = proc;
    proc->state = ProcessState::Ready;
    proc->runningOnCpu = -1;
    proc->cpuAffinity = -1;
    proc->tlbCpuMask = 0;

    proc->isKernelThread = true;
    proc->schedPriority = priority;

    // Kernel threads use the kernel's page table
    proc->addressSpace = AddressSpaceKernel();

    // Allocate kernel stack with guard pages
    VirtualAddress kstackAddr = VmmAllocKernelStack(KERNEL_STACK_PAGES,
        MemTag::KernelData, proc->pid);
    if (!kstackAddr)
    {
        SerialPuts("KTHREAD: kernel stack allocation failed\n");
        SchedulerFreePid(proc->pid);
        FreeProcessStruct(proc);
        return nullptr;
    }
    proc->kernelStackBase = kstackAddr.raw();
    proc->kernelStackTop  = kstackAddr.raw() + KERNEL_STACK_SIZE;

    // Store fn and arg at the top of the kernel stack for the trampoline.
    // stackSlots[-2] = fn, stackSlots[-1] = arg
    auto* stackSlots = reinterpret_cast<uint64_t*>(proc->kernelStackTop);
    stackSlots[-2] = reinterpret_cast<uint64_t>(fn);
    stackSlots[-1] = reinterpret_cast<uint64_t>(arg);

    // Copy name
    for (uint32_t i = 0; i < 31 && name[i]; ++i)
        proc->name[i] = name[i];

    DbgPrintf("KTHREAD: created '%s' pid=%u, stack=0x%lx\n",
                 proc->name, proc->pid, proc->kernelStackTop);

    return proc;
}

// Close a single FD's underlying resource and free the slot.
// Shared between ProcessCloseAllFds and ProcessCloseCloexecFds.
static void CloseFdEntry(Process* proc, uint32_t i)
{
    CloseProcessFd(proc, static_cast<int>(i));
}

// Close all FDs with FD_CLOEXEC set — called by ProcessExec (execve semantics).
void ProcessCloseCloexecFds(Process* proc)
{
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    if (!proc || !proc->fds) return;
    SpinLockAcquire(ProcessFdLock(proc));
    for (uint32_t i = 0; i < MAX_FDS; ++i)
    {
        FdEntry& fde = proc->fds[i];
        if (fde.type == FdType::None) continue;
        if (!(fde.fdFlags & 1)) continue; // not FD_CLOEXEC
        SpinLockRelease(ProcessFdLock(proc));
        CloseFdEntry(proc, i);
        SpinLockAcquire(ProcessFdLock(proc));
    }
    SpinLockRelease(ProcessFdLock(proc));
}

void ProcessCloseAllFds(Process* proc)
{
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    // Kernel tasks have no descriptor table. Never scan through a null table
    // on exit (small tables previously hid this behind low identity mappings).
    if (!proc || !proc->fds) return;

    SpinLockAcquire(ProcessFdLock(proc));
    for (uint32_t i = 0; i < MAX_FDS; ++i)
    {
        FdEntry& fde = proc->fds[i];
        if (fde.type == FdType::None) continue;
        SpinLockRelease(ProcessFdLock(proc));
        // BRO-156: force teardown even if a killed thread left this slot pinned;
        // all sibling threads are gone by now, so no FdPut will ever drain it.
        CloseProcessFdForced(proc, static_cast<int>(i));
        SpinLockAcquire(ProcessFdLock(proc));
    }
    SpinLockRelease(ProcessFdLock(proc));
}

void ProcessDestroy(Process* proc)
{
    if (proc) ProcessQueueDestroy(proc);
}

#include "process_destroy.inc"

// ---------------------------------------------------------------------------
// ProcessFork -- create a child process that is a copy of the parent.
// ---------------------------------------------------------------------------
// Uses Copy-on-Write (COW) for writable pages: both parent and child share
// the same physical page marked read-only with PTE_COW_BIT. First write
// triggers a page fault that copies the page (handled in idt.cpp).
// Read-only pages are shared directly with PmmRefPage.
// The child inherits open file descriptors (shallow copy).
// The child's trampoline returns to user mode with RAX=0 (fork return value).

Process* ProcessFork(Process* parent, uint64_t userRip,
                     uint64_t userRsp, uint64_t userRflags,bool shareFs)
{
    if (!parent || parent->isKernelThread) return nullptr;
    ProcessOperationGuard operation(parent);

    auto* child = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!child) return nullptr;

    // Copy entire parent process struct as a starting point
    // Exclude the concurrently replaceable filesystem-context pointer from
    // the raw snapshot. No borrowed reference can escape into an unwind.
    CopyParentProcess(child,parent);
    child->gpuAppCtx = 0; // Legacy GPU context belongs to the parent, not the fork.

    InheritCredentials(child, parent);
    child->sigPending = 0;
    memset(child->sigSenders, 0, sizeof(child->sigSenders));
    child->addressSpace = nullptr;
    child->fbBacking = nullptr;
    child->framebufferLock = {};
    child->destroyNext = nullptr;
    child->destroyCompletion = nullptr;
    child->activeFileFaultVnode = nullptr;
    child->fdLock.next = child->fdLock.serving = 0;
    child->magic = PROCESS_MAGIC;
    child->incarnation = NextIncarnation();
    child->leaderIncarnation = 0;   // a fork child is its own leader

    // Allocate new PID
    child->pid = SchedulerAllocUserPid();
    child->parentPid = parent->tgid;
    child->pgid = parent->pgid;      // Inherit process group from parent
    child->sid = parent->sid;         // Inherit session from parent
    child->tgid = child->pid;         // Fork creates a new thread group
    child->threadLeader = child;
    child->isThread = false;
    child->clearChildTid = 0;
    child->robustListHead=0;child->robustExitState=0;child->robustExitNext=nullptr;
    child->parentSetTid = 0;
    child->vforkParent = nullptr;
    child->blockedOnRwLock = nullptr;
    child->heldWriteLock = nullptr;
    child->heldReadLock = nullptr;
    child->blockedAsWriter = false;
    // Crash entry is inherited from the parent: fork keeps the same VAS
    // contents, so __brook_crash_entry is at the same VA in the child.
    // (exec() does clear it — see ProcessExec.)
    child->crashInProgress = false;
    if (child->pid == 0)
    {
        SerialPuts("FORK: PID allocation failed\n");
        FreeProcessStruct(child);
        return nullptr;
    }

    if(ProcessFsInherit(child,parent,shareFs)) {
        SchedulerFreePid(child->pid);FreeProcessStruct(child);return nullptr;
    }

    // Fork gets its own fd table (copied from parent below). The memcpy
    // above left child->fds pointing at parent's table — replace it.
    child->fds = AllocFdTable();
    if (!child->fds)
    {
        SerialPuts("FORK: fd table allocation failed\n");
        SchedulerFreePid(child->pid);
        FreeProcessStruct(child);
        return nullptr;
    }

    // Reset scheduler state
    child->state = ProcessState::Ready;
    child->runningOnCpu = -1;
    child->cpuAffinity = -1;
    child->tlbCpuMask = 0;

    child->reapable = false;
    child->compositorRegistered = false;
    child->refCount = 0;             // BRO-173/175: fresh process holds no external refs
    child->asLiveThreads = 0;        // BRO-176: a fresh address space has no extra threads yet
    child->groupKillOwned = false;   // BRO-173: fresh group, clean teardown state
    child->tgidExiting = false;
    child->schedNext = nullptr;
    child->schedPrev = nullptr;
    child->inReadyQueue = 0;
    child->wakeupTick = 0;
    child->kernelRegionDepth = 0;
    child->kernelOperationProtected = false;
    child->interruptibleWait = false;
    child->terminalPending = false;
    child->pendingWaitStatus = 0;
    child->syncNext = nullptr;
    child->pendingWakeup = 0;

    // Set fork-child trampoline state
    child->stopReported = false;
    child->continuedPending = false;
    child->resumePending = false;
    child->stopSignal = 0;
    child->isForkChild = true;
    child->forkReturnRip = userRip;
    child->forkReturnRsp = userRsp;
    child->forkReturnRflags = userRflags;

    // Clear framebuffer state (child starts without a VFB)
    child->fbVirtual = nullptr;
    child->fbVirtualSize = 0;
    child->fbVfbWidth = 0;
    child->fbVfbHeight = 0;
    child->fbVfbStride = 0;
    child->fbDestX = 0;
    child->fbDestY = 0;
    child->fbScale = 0;
    child->fbDirty = 0;
    child->fbExitColor = 0;

    // Allocate per-process kernel stack
    VirtualAddress kstackAddr = VmmAllocKernelStack(KERNEL_STACK_PAGES,
        MemTag::KernelData, child->pid);
    if (!kstackAddr)
    {
        SerialPuts("FORK: kernel stack allocation failed\n");
        kfree(child->fds);
        SchedulerFreePid(child->pid);
        FreeProcessStruct(child);
        return nullptr;
    }
    child->kernelStackBase = kstackAddr.raw();
    child->kernelStackTop  = kstackAddr.raw() + KERNEL_STACK_SIZE;

    // Create new page table (shares kernel upper half)
    child->addressSpace = AddressSpaceCreate(child->pid);
    if (!child->addressSpace)
    {
        SerialPuts("FORK: page table allocation failed\n");
        VmmFreeKernelStack(kstackAddr, KERNEL_STACK_PAGES);
        kfree(child->fds);
        SchedulerFreePid(child->pid);
        FreeProcessStruct(child);
        return nullptr;
    }

    // Snapshot ranges and populated leaves under the shared AS mutation lock.
    // The clone returns only after parent writable translations are invalidated.
    if (!UserMappingClone(parent, parent->addressSpace, child->addressSpace, child->pid))
    {
        SerialPuts("FORK: address space copy failed\n");

        VmmFreeKernelStack(kstackAddr, KERNEL_STACK_PAGES);
        kfree(child->fds);
        SchedulerFreePid(child->pid);
        FreeProcessStruct(child);
        return nullptr;
    }

    // Snapshot and retain under the source fd lock. Never reread mutable
    // parent slots after copying their handle into the child.
    if (!FdTableForkSnapshot(child->fds, parent->fds, ProcessFdLock(parent), FdRetainCopy, UnixLifePublished)) {
        ProcessCloseAllFds(child);

        VmmFreeKernelStack(kstackAddr, KERNEL_STACK_PAGES);
        kfree(child->fds);
        SchedulerFreePid(child->pid);
        FreeProcessStruct(child);
        return nullptr;
    }

    // Update child's TLS fsBase to point to the new address space's TLS
    // (same virtual address, but backed by the child's physical pages)
    // No change needed — the virtual address is the same and the page table
    // now maps it to the child's copy.

    // Inherit terminal mode from parent
    child->ttyCanonical = parent->ttyCanonical;
    child->ttyEcho = parent->ttyEcho;
    child->straceEnabled = parent->straceEnabled;
    child->straceErrorsOnly = parent->straceErrorsOnly;
    child->straceFilter = parent->straceFilter;

    // Inherit parent's signal handlers (POSIX fork semantics).
    // The table is indexed by group identity, so we must explicitly copy;
    // otherwise the child's slot contains stale handler pointers from
    // whatever previous process had the same pid — a deterministic #PF
    // waiting for the first signal delivery (e.g. SIGCHLD on waitpid).
    CopySignalActions(child->tgid,parent->tgid);

    // Set child's name
    {
        const char* suffix = "_child";
        uint32_t nameLen = 0;
        while (nameLen < 24 && parent->name[nameLen]) nameLen++;
        for (uint32_t i = 0; i < nameLen; i++) child->name[i] = parent->name[i];
        for (uint32_t i = 0; suffix[i] && nameLen + i < 31; i++)
            child->name[nameLen + i] = suffix[i];
        child->name[31] = '\0';
    }

    DbgPrintf("FORK: parent pid=%u -> child pid=%u '%s', rip=0x%lx rsp=0x%lx\n",
                 parent->pid, child->pid, child->name, userRip, userRsp);

    AddressSpacePublish(child->addressSpace);
    return child;
}

// ---------------------------------------------------------------------------
// ProcessCreateThread -- create a new thread sharing the parent's address space.
// ---------------------------------------------------------------------------

Process* ProcessCreateThread(Process* parent, uint64_t userRip,
                             uint64_t userRsp, uint64_t userRflags,
                             uint64_t tlsBase,bool shareFs)
{
    if (!parent || parent->isKernelThread) return nullptr;
    ProcessOperationGuard credentialOperation(ProcessCurrent());

    auto* thread = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!thread) return nullptr;

    // Copy parent process struct as starting point
    CopyParentProcess(thread,parent);
    InheritCredentials(thread, parent);
    thread->addressSpace = nullptr;
    thread->fbBacking = nullptr;
    thread->framebufferLock = {};
    thread->destroyNext = nullptr;
    thread->destroyCompletion = nullptr;
    thread->activeFileFaultVnode = nullptr;
    thread->magic = PROCESS_MAGIC;
    thread->incarnation = NextIncarnation();
    // Allocate TID (threads get unique PIDs but share tgid)
    thread->pid = SchedulerAllocUserPid();
    if (thread->pid == 0)
    {
        FreeProcessStruct(thread);
        return nullptr;
    }

    if(ProcessFsInherit(thread,parent,shareFs)) {
        SchedulerFreePid(thread->pid);FreeProcessStruct(thread);return nullptr;
    }

    // Thread group: inherit parent's tgid directly. We must NOT dereference
    // parent->threadLeader to read leader->pid here — if the original group
    // leader exited via plain sys_exit while threads were still running, that
    // Process struct is freed and reading from it returns heap-poison
    // (0xDFDF), corrupting tgid and panicking later in the signal-handler
    // copy. parent->tgid is cached on every thread at creation, so it is
    // always valid as long as parent itself is alive (which it must be — it
    // is gCurrentProcess).
    thread->tgid = parent->tgid;
    Process* leader = parent->threadLeader ? parent->threadLeader : parent;
    // Defensive: validate the leader pointer with a magic check before
    // storing it. If the leader was freed, fall back to parent so we don't
    // propagate a dangling pointer to the new thread.
    if (!leader || leader->magic != PROCESS_MAGIC)
    {
        SerialPrintf("THREAD: parent pid=%u has dangling leader, using parent\n",
                     parent->pid);
        leader = parent;
    }
    thread->threadLeader = leader;
    thread->isThread = true;
    thread->robustListHead=0;thread->robustExitState=0;thread->robustExitNext=nullptr;
    thread->stopReported = false;
    thread->continuedPending = false;
    thread->resumePending = false;
    thread->stopSignal = 0;
    // BRO-176 diagnostic: stamp the leader's incarnation at thread-creation so
    // a cross-incarnation decrement (leader struct freed+reused under us) is
    // detectable at teardown.
    thread->leaderIncarnation = leader->incarnation;

    // Share the leader's fd table (CLONE_FILES semantics). The memcpy above
    // already copied the pointer from parent->fds, but that pointer could be
    // parent's leader-chained fds; make this explicit so Go/pthread-style
    // thread groups correctly share one fd namespace. Opening a socket /
    // epoll in any thread must be visible in every sibling.
    thread->fds = leader->fds;
    thread->parentPid = parent->parentPid;
    thread->pgid = parent->pgid;
    thread->sid = parent->sid;

    // Reset scheduler state
    thread->state = ProcessState::Ready;
    thread->runningOnCpu = -1;
    thread->cpuAffinity = leader->cpuAffinity;
    thread->tlbCpuMask = 0;

    thread->reapable = false;
    thread->compositorRegistered = false;
    thread->refCount = 0;            // BRO-173/175: fresh thread holds no external refs
    thread->asLiveThreads = 0;      // BRO-176: per-thread copy unused; only the leader's count matters
    // BRO-173: these lifecycle latches were memcpy'd from the parent; a new
    // thread must start with a clean teardown state.
    thread->groupKillOwned = false;
    thread->tgidExiting = false;
    thread->schedNext = nullptr;
    thread->schedPrev = nullptr;
    thread->inReadyQueue = 0;
    thread->wakeupTick = 0;
    thread->kernelRegionDepth = 0;
    thread->kernelOperationProtected = false;
    thread->interruptibleWait = false;
    thread->terminalPending = false;
    thread->pendingWaitStatus = 0;
    thread->syncNext = nullptr;
    thread->pendingWakeup = 0;
    thread->vforkParent = nullptr;
    thread->blockedOnRwLock = nullptr;
    thread->heldWriteLock = nullptr;
    thread->heldReadLock = nullptr;
    thread->blockedAsWriter = false;

    // Fork-child trampoline state
    thread->isForkChild = true;
    thread->forkReturnRip = userRip;
    thread->forkReturnRsp = userRsp;
    thread->forkReturnRflags = userRflags;

    // Threads don't own a VFB — they share the leader's
    thread->fbVirtual = nullptr;
    thread->fbVirtualSize = 0;
    thread->fbVfbWidth = 0;
    thread->fbVfbHeight = 0;
    thread->fbVfbStride = 0;
    thread->fbDestX = 0;
    thread->fbDestY = 0;
    thread->fbScale = 0;
    thread->fbDirty = 0;
    thread->fbExitColor = 0;

    // Initialize FPU/SSE state
    memset(thread->fxsave.data, 0, 512);
    thread->fxsave.data[0] = 0x7F;
    thread->fxsave.data[1] = 0x03;
    thread->fxsave.data[24] = 0x80;
    thread->fxsave.data[25] = 0x1F;

    // Allocate per-thread kernel stack
    VirtualAddress kstackAddr = VmmAllocKernelStack(KERNEL_STACK_PAGES,
        MemTag::KernelData, thread->pid);
    if (!kstackAddr)
    {
        SchedulerFreePid(thread->pid);
        FreeProcessStruct(thread);
        return nullptr;
    }
    thread->kernelStackBase = kstackAddr.raw();
    thread->kernelStackTop  = kstackAddr.raw() + KERNEL_STACK_SIZE;

    // CLONE_VM: share the same page table — no page copy
    thread->addressSpace = parent->addressSpace;
    AddressSpaceRef(thread->addressSpace);

    // Set TLS base for this thread (musl allocates per-thread TLS)
    thread->fsBase = tlsBase;
    thread->savedCtx.fsBase = tlsBase;

    // Signal state: inherit mask, clear pending
    thread->sigPending = 0;
    memset(thread->sigSenders, 0, sizeof(thread->sigSenders));
    thread->inSignalHandler = false;
    thread->inSignalHandlerOnAltStack = false;
    thread->sigReturnPending = false;

    // Threads share the parent's tgid (set above), so they already index
    // into the same group disposition slot — no copy needed.

    // Set thread name
    {
        uint32_t nameLen = 0;
        while (nameLen < 24 && parent->name[nameLen]) nameLen++;
        for (uint32_t i = 0; i < nameLen; i++) thread->name[i] = parent->name[i];
        const char* suffix = "_t";
        for (uint32_t i = 0; suffix[i] && nameLen + i < 31; i++)
            thread->name[nameLen + i] = suffix[i];
        thread->name[31] = '\0';
    }

    // BRO-176: the increment that registers a thread against its leader's
    // address space lives in SchedulerAddProcess (the commit-to-g_allProcesses
    // point), balanced exactly against the decrement in ProcessDestroy (the
    // remove-from-g_allProcesses point).  Tying it to list membership keeps the
    // count correct regardless of which clone failure path frees a half-built
    // thread before it is ever published.
    DbgPrintf("THREAD: parent pid=%u -> thread tid=%u tgid=%u pt=0x%lx\n",
              parent->pid, thread->pid, thread->tgid,
              thread->addressSpace->pageTable.pml4.raw());

    return thread;
}

// ---------------------------------------------------------------------------
// CreateRemoteThread -- inject a new user-mode thread into a target process.
// ---------------------------------------------------------------------------
//
// Design: allocate user stack in target's VAS, copy argBytes to top of stack,
// create a CLONE_VM|CLONE_FILES thread whose ForkChildTrampoline restores
// entry at RIP, stack top at RSP, argBytes address in RDI, and all other
// GPRs zero.  The thread inherits target's signal mask + fs base (no TLS
// of its own — entry code must be TLS-free or set up %fs explicitly).

uint16_t CreateRemoteThread(Process* target, uint64_t entry,
                            uint32_t stackSize,
                            const void* argBytes, uint32_t argLen)
{
    if (!target || target->isKernelThread) return 0;
    if (!entry) return 0;
    if (stackSize < 4096) stackSize = 4096;
    // Round up to 4K and clamp to 1 MiB (defensive)
    if (stackSize > (1u << 20)) stackSize = 1u << 20;
    stackSize = (stackSize + 4095) & ~uint32_t(4095);

    // argBytes must fit in top of stack, leaving room for alignment + a
    // small amount of spare so the entry function doesn't immediately
    // clobber its own argument when it pushes stack frames.
    if (argLen > stackSize / 2) return 0;

    Process* actor=ProcessCurrent();
    if (!actor) return 0;
    ProcessOperationGuard operation(actor);
    // Construct the unpublished thread first: its AS reference remains stable
    // even if the target later execs. Stack construction uses that exact AS.
    Process* thread=ProcessCreateThread(target,entry,0,0x202,target->fsBase);
    if (!thread) return 0;
    uint64_t argVA=0;
    int64_t mapped=UserMappingStack(actor,thread->addressSpace,stackSize,argBytes,argLen,&argVA);
    if (mapped<0) {
        VmmFreeKernelStack(VirtualAddress(thread->kernelStackBase),KERNEL_STACK_PAGES);
        SchedulerFreePid(thread->pid);
        FreeProcessStruct(thread);
        return 0;
    }
    uint64_t stackBase=static_cast<uint64_t>(mapped),stackTop=stackBase+stackSize;
    // Linux ABI: at function entry (RSP + 8) is 16-byte aligned.
    uint64_t userRsp=((argVA?argVA-16:stackTop-16)&~uint64_t{15})-8;
    thread->forkReturnRsp=userRsp;

    // Override caller-saved registers: entry gets only RDI = argVA, all
    // others cleared.  Callee-saved (RBX, RBP, R12-R15) also cleared so
    // the frame walker stops cleanly at the entry frame.
    thread->forkRdi = argVA;
    thread->forkRsi = 0;
    thread->forkRdx = 0;
    thread->forkR8  = 0;
    thread->forkR9  = 0;
    thread->forkR10 = 0;
    thread->forkRbx = 0;
    thread->forkRbp = 0;
    thread->forkR12 = 0;
    thread->forkR13 = 0;
    thread->forkR14 = 0;
    thread->forkR15 = 0;

    // Rename for log hygiene.
    {
        const char* suffix = "_crt";
        uint32_t nameLen = 0;
        while (nameLen < 24 && target->name[nameLen]) nameLen++;
        for (uint32_t i = 0; i < nameLen; i++) thread->name[i] = target->name[i];
        for (uint32_t i = 0; suffix[i] && nameLen + i < 31; i++)
            thread->name[nameLen + i] = suffix[i];
        thread->name[31] = '\0';
    }

    SerialPrintf("CRT: injected tid=%u into tgid=%u entry=0x%lx rsp=0x%lx "
                 "arg=0x%lx (%u B) stack=0x%lx..0x%lx\n",
                 thread->pid, target->tgid, entry, userRsp, argVA, argLen,
                 stackBase, stackBase + stackSize);

    SchedulerAddProcess(thread);
    return thread->pid;
}

// ---------------------------------------------------------------------------
// ProcessExec -- replace a process's address space with a new ELF binary.
// ---------------------------------------------------------------------------

static uint64_t ProcessExecImpl(Process* proc, const uint8_t* elfData, uint64_t elfSize,
                     int argc, const char* const* argv,
                     int envc, const char* const* envp,
                     uint64_t* outStackPtr, const ExecutableImage* owned,
                     const ExecutableImage* interpreter=nullptr)
{
    if (!proc || proc->isKernelThread) return 0;
    if (!ExecArgumentsFit(argc, argv, envc, envp)) return 0;

    ProcessOperationGuard operation(proc);
    AddressSpaceState* replacement = AddressSpaceCreate(proc->pid);
    if (!replacement)return 0;
    bool wasThread = proc->isThread;
    FdEntry* privateFds = nullptr;
    if (wasThread) {
        privateFds = AllocFdTable();
        if (!privateFds || !FdTableForkSnapshot(privateFds, proc->fds,
                ProcessFdLock(proc), FdRetainCopy, UnixLifePublished)) {
            if (privateFds) {
                for (uint32_t i = 0; i < MAX_FDS; ++i)
                    if (privateFds[i].type != FdType::None) FdReleaseOwnedEntry(privateFds[i]);
                kfree(privateFds);
            }
            AddressSpaceUnref(replacement);
            return 0;
        }
    }
    PosixTimersCleanup(proc, true);
    RobustFutexCleanup(proc); // old address space and TID, before exec commit
    proc->robustExitState=0;
    AddressSpaceState* previous = proc->addressSpace;
    UserFramebufferPublish(proc,nullptr,nullptr,0,0,0,0);
    PageTable kernelPt = VmmKernelCR3();
    __asm__ volatile("mov %0, %%cr3" : : "r"(kernelPt.pml4.raw()) : "memory");
    SchedulerReplaceProcessAddressSpace(proc, replacement);
    proc->tlbCpuMask = 0;
    if (wasThread) {
        // Detach the previous shared FD ownership before becoming a leader.
        // Drop the old group's liveness hold while its identity is still saved.
        proc->fds = privateFds;
        proc->fdLock.next = proc->fdLock.serving = 0;
        SchedulerCommitExecThreadIdentity(proc);
    }
    // The source was cleared through framebufferLock before AS replacement.
    proc->fbDestX = 0;proc->fbDestY = 0;proc->fbScale = 0;proc->fbExitColor = 0;

    // Linux exec preserves CLONE_FS sharing of cwd/root/umask. Only the
    // descriptor table above is unshared; this process keeps its context ref.
    AddressSpaceUnref(previous);

    // Any user-space pointers the kernel held into the old address space are
    // now stale. If we fail later in ProcessExec, SchedulerExitCurrentProcess
    // must not dereference them. Linux does the same in flush_old_exec().
    proc->clearChildTid = 0;
    // exec() replaces the program image; a previously-registered crash
    // entry point is no longer valid (it pointed into the old VAS).
    proc->crashEntry = 0;
    proc->crashInProgress = false;

    // 3. Load the new ELF binary
    if (!(owned ? ElfLoadExecutable(*owned,&proc->elf,proc->addressSpace,proc->pid) :
                  ElfLoad(elfData,elfSize,&proc->elf,proc->addressSpace,proc->pid)))
    {
        SerialPuts("EXEC: ELF load failed\n");
        return 0;
    }

    proc->addressSpace->programBreak = proc->elf.programBreakLow;
    proc->addressSpace->programBreakLow = proc->elf.programBreakLow;
    proc->addressSpace->programBreakHigh = proc->elf.programBreakHigh;
    // Keep mmap beyond both the loaded image and its reserved brk range.
    uint64_t mmapFloor = (proc->elf.programBreakHigh + 16 * 1024 * 1024 + 4095) & ~uint64_t{4095};
    proc->addressSpace->mmapNext = mmapFloor > USER_MMAP_BASE ? mmapFloor : USER_MMAP_BASE;

    // Load dynamic linker if the ELF specifies PT_INTERP.
    uint64_t interpEntry = 0;
    uint64_t interpBase  = 0;
    if (proc->elf.interpPath[0] != '\0')
    {
        interpEntry = LoadInterpreter(proc,interpreter);
        if (!interpEntry)
        {
            SerialPrintf("EXEC: failed to load interpreter '%s'\n",
                         proc->elf.interpPath);
            return 0;
        }
        interpBase = INTERP_LOAD_BASE;
    }

    // 4. Allocate new user stack
    uint64_t stackPages = USER_STACK_SIZE / 4096;
    uint64_t guardPages = 1;
    uint64_t totalStackPages = stackPages + guardPages;
    uint64_t stackVirtTop = USER_STACK_TOP;
    uint64_t stackVirtBase = stackVirtTop - totalStackPages * 4096;

    if (!RegisterInitialRange(proc,stackVirtBase,stackVirtBase+guardPages*4096,0,MappingKind::Stack) ||
        !RegisterInitialRange(proc,stackVirtBase+guardPages*4096,stackVirtTop,3,MappingKind::Stack)) return 0;

    for (uint64_t i = guardPages; i < totalStackPages; i++)
    {
        VirtualAddress vaddr(stackVirtBase + i * 4096);
        PhysicalAddress phys = PmmAllocPage(MemTag::User, ProcessAddressSpacePid(proc));
        if (!phys || !VmmMapPage(proc->addressSpace->pageTable, vaddr, phys,
                                  VMM_WRITABLE | VMM_USER | VMM_NO_EXEC,
                                  MemTag::User, ProcessAddressSpacePid(proc)))
        {
            if (phys) PmmUnrefPage(phys);
            SerialPuts("EXEC: stack allocation failed\n");
            return 0;
        }
        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        memset(p, 0, 4096);
    }
    proc->stackBase = stackVirtBase + guardPages * 4096;
    proc->stackTop  = stackVirtTop - 8;

    // Complete TLS reservations before installing the optional helper.
    if (!SetupTLS(proc, stackVirtBase, guardPages)) return 0;

    // 5. Build user stack with argc/argv/envp/auxv
    const uint64_t vdsoBase = VdsoInstall(proc->addressSpace, RandomU64());
    uint64_t userSP = SetupUserStack(proc, argc, argv, envc, envp, interpBase, vdsoBase);
    proc->stackTop = userSP;

    // 8. Close FD_CLOEXEC file descriptors — POSIX execve semantics require
    //    that any fd opened with O_CLOEXEC / FD_CLOEXEC is closed when the
    //    process image is replaced.  Skipping this leaks inherited sockets and
    //    pipes into the new program's address space.
    ProcessCloseCloexecFds(proc);

    // 9. Reset signal handlers per POSIX execve semantics: handlers set to
    //    a user function must be reset to SIG_DFL (0) because the new image
    //    has no such function at that address. SIG_IGN (1) is preserved.
    //    Without this, inherited handler pointers (from the pre-exec image,
    //    or worse — stale pointers from a previous process that had the
    //    same PID if we never cleared on destroy) would redirect signal
    //    delivery to unmapped memory, causing a #PF in the new program on
    //    its first signal (e.g. SIGCHLD from a child exiting).
    ResetSignalActions(proc->tgid,true);

    // Ordinary exec preserves real IDs/groups and snapshots effective IDs.
    SpinLockAcquire(&g_credentialsLock);
    proc->suid = proc->euid; proc->sgid = proc->egid;
    if(proc->userNamespace) {
        const bool root=UserIdFromCanonical(proc->userNamespace,false,proc->uid)==0 || UserIdFromCanonical(proc->userNamespace,false,proc->euid)==0;
        UserCapabilitiesExec(proc->capabilities,root,proc->noNewPrivs!=0);
        if(UserIdFromCanonical(proc->userNamespace,false,proc->euid)!=0)proc->capabilities.effective=0;
    }
    SpinLockRelease(&g_credentialsLock);
    *outStackPtr = userSP;

    // The terminal owns its pixels independently of the replaced AS. Keep an
    // owning shell's display attached across exec, without restoring old user
    // mappings or publishing its source to a forked descendant.
    TerminalRestoreAfterExec(proc);

    // If dynamically linked, start at the interpreter's entry point;
    // otherwise, start at the main binary's entry.
    AddressSpacePublish(proc->addressSpace);
    return interpEntry ? interpEntry : proc->elf.entryPoint;
}

uint64_t ProcessExec(Process* proc, const uint8_t* elfData, uint64_t elfSize,
                     int argc, const char* const* argv,
                     int envc, const char* const* envp,
                     uint64_t* outStackPtr)
{
    return ProcessExecImpl(proc,elfData,elfSize,argc,argv,envc,envp,outStackPtr,nullptr);
}

uint64_t ProcessExecExecutable(Process* proc, const ExecutableImage& image,
    int argc, const char* const* argv, int envc, const char* const* envp, uint64_t* outStackPtr)
{
    return ProcessExecImpl(proc,image.data,image.size,argc,argv,envc,envp,outStackPtr,&image);
}

uint64_t ProcessExecPreparedExecutable(Process* proc,const ExecutableImage& image,
    const ExecutableImage& interpreter,int argc,const char* const* argv,
    int envc,const char* const* envp,uint64_t* outStackPtr)
{
    return ProcessExecImpl(proc,image.data,image.size,argc,argv,envc,envp,outStackPtr,&image,&interpreter);
}

// ---------------------------------------------------------------------------
// Signal delivery
// ---------------------------------------------------------------------------

// Signal constants
static constexpr int SIGKILL = 9;
[[maybe_unused]] static constexpr int SIGSTOP = 19;

#include "signal_pending.inc"

#include "process_signal_send.inc"

} // namespace brook

// ---------------------------------------------------------------------------
// SyscallCheckSignals — called from asm syscall return path
// ---------------------------------------------------------------------------
// Checks for pending signals and either:
//   1. Delivers a signal by building a SignalFrame on the user stack and
//      redirecting the SyscallFrame to the handler entry point.
//   2. Handles rt_sigreturn by restoring the SyscallFrame from the
//      SignalFrame's ucontext on the user stack.

using namespace brook;

extern "C" bool SyscallTakeWaitSignalMask(Process* proc, uint64_t* original);

extern "C" int64_t SyscallCheckSignals(SyscallFrame* frame, int64_t syscallResult,
                                      FxsaveArea* entryFp)
{
    Process* proc = ProcessCurrent();
    if (!proc || proc->isKernelThread) return syscallResult;

    SyscallTimingCall timing(proc->pid,proc->incarnation,proc->currentSyscallNum,0,SyscallTimingPhase::ReturnCheck);

    if (SchedulerKernelOperationBoundary()) {
        SchedulerYield();
        __builtin_trap(); // A terminal thread must never resume.
    }

    // ppoll/pselect6/epoll_pwait/sigsuspend must select pending signals while their
    // temporary mask is still installed. Every ordinary/error return restores
    // the original mask here; a delivered handler instead saves it in its
    // ucontext, while running with the temporary mask plus sa_mask.
    uint64_t returnMask = proc->sigMask;
    struct RestoreWaitMask {
        Process* proc;
        uint64_t mask;
        bool active;
        ~RestoreWaitMask() { if (active) proc->sigMask = mask; }
    } restoreMask{proc, 0, SyscallTakeWaitSignalMask(proc, &returnMask)};
    restoreMask.mask = returnMask;

    // --- Handle rt_sigreturn ---
    if (proc->sigReturnPending)
    {
        proc->sigReturnPending = false;
        proc->inSignalHandler = false;
        proc->inSignalHandlerOnAltStack = false;

        // The user RSP at the time of the rt_sigreturn syscall points to the
        // ucontext (pretcode was popped by handler's RET, then restorer did syscall).
        // frame->rsp is the user RSP saved on syscall entry.
        // The SignalFrame starts 8 bytes before the ucontext (pretcode field).
        SignalUcontext context{};
        SignalFpImage restoredFp{};
        // The dispatcher calls with IF clear. Snapshot without return-path
        // locks held; prefault may need interrupts, I/O and a context switch.
        __asm__ volatile("sti":::"memory");
        const bool copied=CopyFromUser(&context,frame->rsp,sizeof(context)) &&
                          SignalFpReadFromUser(restoredFp,context.uc_mcontext.fpstate);
        __asm__ volatile("cli":::"memory");
        // Copy introduced a new protected operation after the entry boundary.
        if(SchedulerKernelOperationBoundary()) { SchedulerYield();__builtin_trap(); }
        const SignalMcontext& mc=context.uc_mcontext;
        if(!copied || !PrepareSignalReturn(frame->returnState,mc.rip,mc.rsp,mc.eflags,mc.rcx)) {
            SerialPrintf("SIGRETURN: invalid context pid %u\n",proc->pid);
            SchedulerKillThreadGroup(proc->tgid,proc,128+11,11);
            SchedulerExitCurrentProcess(128+11,11);
        }

        // Restore signal mask
        // The wrapper's final XRSTOR applies this validated snapshot; restoring
        // hardware here would be overwritten by that wrapper on return.
        memcpy(entryFp->data,restoredFp.data,sizeof(entryFp->data));
        proc->sigMask = context.uc_sigmask & ~((1ULL<<8)|(1ULL<<18));

        // Restore all registers from mcontext into the SyscallFrame
        frame->rcx    = mc.rip;      // user RIP (sysret uses RCX)
        frame->rsp    = mc.rsp;
        frame->rflags = frame->returnState.flags;
        frame->rdi    = mc.rdi;
        frame->rsi    = mc.rsi;
        frame->rdx    = mc.rdx;
        frame->rbp    = mc.rbp;
        frame->rbx    = mc.rbx;
        frame->r8     = mc.r8;
        frame->r9     = mc.r9;
        frame->r10    = mc.r10;
        frame->r11    = mc.r11;
        frame->r12    = mc.r12;
        frame->r13    = mc.r13;
        frame->r14    = mc.r14;
        frame->r15    = mc.r15;

        DbgPrintf("SIGRETURN: pid %u restored rip=0x%lx rsp=0x%lx\n",
                  proc->pid, mc.rip, mc.rsp);

        // Debug: detect SA_RESTART sigreturn — if rax looks like a syscall
        // number and rip points to a syscall instruction, log it
        if (mc.rax <= 512 && mc.rip < 0x0000800000000000ULL)
            brook::SerialPrintf("SIGRETURN: pid %u rip=0x%lx rax=%lu (restart? rsp=0x%lx)\n",
                                proc->pid, mc.rip, mc.rax, mc.rsp);

        return timing.Result(static_cast<int64_t>(mc.rax)); // restore original RAX
    }

    // --- Signal delivery ---
    if (proc->inSignalHandler) return timing.Result(syscallResult);

    uint64_t deliverable = (ProcessPendingSignals(proc) | PosixTimerPending(proc)) & ~proc->sigMask;
    if (deliverable == 0) return timing.Result(syscallResult);

    // Find lowest pending signal
    int signum = __builtin_ctzll(deliverable) + 1;

    SignalInfo signalInfo{};
    if (!PosixTimerDequeue(proc, 1ULL << (signum - 1), &signalInfo)) {
        if (!ProcessDequeueSignal(proc, signum, &signalInfo)) return timing.Result(syscallResult);
    }

    uint16_t pid = proc->tgid;
    if (pid >= MAX_PROCESSES) return timing.Result(syscallResult);

    const KernelSigaction sa = ReadSignalAction(pid,signum - 1);

    // SIG_DFL (0) — default action
    if (sa.handler == 0)
    {
        const auto action = DefaultSignalAction(signum);
        if (action == SignalDefault::Ignore || action == SignalDefault::Continue)
            return timing.Result(syscallResult);
        if (action == SignalDefault::Stop) {
            SchedulerMarkStopped(proc, signum);
            return timing.Result(syscallResult);
        }
        // Syscall return is a process-context boundary with no sender locks.
        // The synchronous exit path can quiesce peers before exiting this task.
        SchedulerKillThreadGroup(proc->tgid, proc, 128 + signum, signum);
        SchedulerExitCurrentProcess(128 + signum, signum);
    }

    // SIG_IGN (1)
    if (sa.handler == 1)
        return timing.Result(syscallResult);

    if(!UserReturnAddress(sa.handler)) {
        SerialPrintf("SIGNAL: invalid handler pid %u address=%lx\n",proc->pid,sa.handler);
        SchedulerKillThreadGroup(proc->tgid,proc,128+11,11);
        SchedulerExitCurrentProcess(128+11,11);
    }

    // --- User handler: build SignalFrame on user stack ---
    DbgPrintf("SIGNAL: delivering signal %d to pid %u handler=0x%lx restorer=0x%lx\n",
              signum, proc->pid, sa.handler, sa.restorer);
    // Promote SIGWINCH delivery debug to SerialPrintf in release until the
    // resize-kills-terminal bug is closed.
    if (signum == 28)
        brook::SerialPrintf("SIGNAL: deliver SIGWINCH pid=%u tgid=%u handler=0x%lx restorer=0x%lx flags=0x%lx syscallResult=%ld syscall=%lu\n",
                            proc->pid, proc->tgid, sa.handler, sa.restorer, sa.flags,
                            syscallResult, proc->currentSyscallNum);

    const uint64_t savedMask=returnMask;
    const uint64_t handlerMask=(proc->sigMask|sa.mask|(1ULL<<(signum-1))) &
                              ~((1ULL<<8)|(1ULL<<18));
    const bool useAlt=(sa.flags&SA_ONSTACK) && proc->sigAltstackSp!=0 &&
                      !(proc->sigAltstackFlags&2) && !proc->inSignalHandlerOnAltStack;
    uint64_t userRsp=0;
    if(!PlaceSignalFrame(frame->rsp,proc->sigAltstackSp,proc->sigAltstackSize,
                         useAlt,sizeof(SignalFrame)+SIGNAL_FP_EXTRA,&userRsp)) {
        SerialPrintf("SIGNAL: invalid frame location pid %u signal %d\n",proc->pid,signum);
        __asm__ volatile("sti":::"memory");
        SchedulerKillThreadGroup(proc->tgid,proc,128+11,11);
        SchedulerExitCurrentProcess(128+11,11);
    }
    SignalFrame localFrame{};
    auto* sf=&localFrame;

    // Return address: sa_restorer (musl's __restore_rt which calls rt_sigreturn)
    sf->pretcode = sa.restorer;

    // Fill ucontext
    sf->uc.uc_sigmask = savedMask;
    sf->uc.uc_flags = 1; // UC_FP_XSTATE
    sf->uc.uc_mcontext.fpstate=SignalFpUserAddress(userRsp,sizeof(*sf));
    sf->uc.uc_stack.ss_sp    = proc->sigAltstackSp;
    sf->uc.uc_stack.ss_size  = proc->sigAltstackSize;
    sf->uc.uc_stack.ss_flags = useAlt ? 1 /*SS_ONSTACK*/
                                      : (proc->sigAltstackSp ? 0 : 2 /*SS_DISABLE*/);

    // Save all registers into mcontext
    SignalMcontext& mc = sf->uc.uc_mcontext;
    mc.rax    = static_cast<uint64_t>(syscallResult);
    mc.rbx    = frame->rbx;
    mc.rcx    = frame->rcx;     // user RIP
    mc.rdx    = frame->rdx;
    mc.rsi    = frame->rsi;
    mc.rdi    = frame->rdi;
    mc.rbp    = frame->rbp;
    mc.rsp    = frame->rsp;
    mc.r8     = frame->r8;
    mc.r9     = frame->r9;
    mc.r10    = frame->r10;
    mc.r11    = frame->r11;
    mc.r12    = frame->r12;
    mc.r13    = frame->r13;
    mc.r14    = frame->r14;
    mc.r15    = frame->r15;
    mc.rip    = frame->rcx;     // user RIP (stored in RCX by syscall)
    mc.eflags = frame->rflags;
    mc.cs     = GDT_USER_CODE;
    mc.ss     = GDT_USER_DATA;

    // Linux restarts selected interrupted syscalls after a signal handler
    // returns when the handler was installed with SA_RESTART. Bash relies on
    // this for SIGWINCH while blocked in terminal read(); otherwise resize
    // converts the read into EINTR and the interactive shell can exit.
    //
    // Polling and sleeping waits return EINTR even with SA_RESTART. Timed
    // futex waits do too; only untimed WAIT/WAIT_BITSET are replayable here.
    // Use the process's syscall snapshot and its saved argument frame: another
    // process may have overwritten per-CPU syscall scratch while we slept.
    const uint64_t syscallNum = proc->currentSyscallNum;
    const bool restartable = brook::SignalSyscallMayRestart(syscallNum, mc.rsi, mc.r10);
    if (syscallResult == -4 /* -EINTR */ && (sa.flags & SA_RESTART) &&
        restartable && mc.rip >= 2)
    {
        brook::SerialPrintf("SIGNAL: SA_RESTART restarting syscall %lu for pid %u (sig %d)\n",
                            syscallNum, proc->pid, signum);
        mc.rax = syscallNum;
        mc.rip -= 2; // x86_64 syscall instruction length
    }
    else if (syscallResult == -4 && signum == 28)
    {
        // Debug: SIGWINCH with -EINTR but NOT restarting — log why
        brook::SerialPrintf("SIGNAL: SIGWINCH NOT restarted for pid %u: syscall=%lu restartable=%d SA_RESTART=%d rip=0x%lx\n",
                            proc->pid, syscallNum, restartable,
                            (sa.flags & SA_RESTART) ? 1 : 0, mc.rip);
    }

    // Fill siginfo
    ProcessSignalInfoToUser(proc,&signalInfo);
    sf->info = signalInfo;

    // Prefault/COW may allocate or sleep. No return-path locks are held, and
    // precise usercopy faults return failure instead of nesting a kernel #PF.
    __asm__ volatile("sti":::"memory");
    const bool copied=SignalFpSaveToUser(mc.fpstate,entryFp->data) &&
                      CopyToUser(userRsp,sf,sizeof(*sf));
    __asm__ volatile("cli":::"memory");
    if(SchedulerKernelOperationBoundary()) {SchedulerYield();__builtin_trap();}
    if(!copied) {
        SerialPrintf("SIGNAL: inaccessible frame pid %u signal %d rsp=%lx\n",proc->pid,signum,userRsp);
        __asm__ volatile("sti":::"memory");
        SchedulerKillThreadGroup(proc->tgid,proc,128+11,11);
        SchedulerExitCurrentProcess(128+11,11);
    }
    SignalFpReset(entryFp->data);
    proc->sigSavedMask=savedMask;
    proc->sigMask=handlerMask;
    proc->inSignalHandler=true;
    if(useAlt)proc->inSignalHandlerOnAltStack=true;

    // Redirect execution to the signal handler
    frame->rcx = sa.handler;                           // RIP = handler
    frame->rsp = userRsp;                              // RSP = signal frame
    // The saved ucontext retains interrupted flags; C handlers enter with DF,
    // TF and RF clear, independently of the eventual rt_sigreturn context.
    frame->rflags &= ~uint64_t{0x10500};
    frame->rdi = static_cast<uint64_t>(signum);        // arg1 = signum
    if (sa.flags & SA_SIGINFO)
    {
        // SA_SIGINFO: handler(int signum, siginfo_t* info, void* ucontext)
        frame->rsi = userRsp+__builtin_offsetof(SignalFrame,info);
        frame->rdx = userRsp+__builtin_offsetof(SignalFrame,uc);
    }

    // The handler's mask remains active until rt_sigreturn restores returnMask.
    restoreMask.active = false;
    return timing.Result(syscallResult); // RAX value (handler ignores it, saved in ucontext)
}
