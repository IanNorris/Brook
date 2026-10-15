#include "unix_lifetime_trace.h"
// process.cpp -- Process creation, fd table, user stack setup.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process.h"
#include "posix_timer.h"
#include "exec_limits.h"
#include "exec_image.h"
#include "scheduler.h"
#include "syscall.h"
#include "vfs.h"
#include "pipe.h"
#include "net.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "serial.h"
#include "kprintf.h"
#include "compositor.h"
#include "window.h"
#include "gpu_app.h"
#include "ext2_vfs.h"
#include "string.h"
#include "spinlock.h"
#include "smp.h"
#include "apic.h"
#include "cpu.h"
#include "ksym_addrs.h"

namespace brook {

// Copies own references before publication; implemented with fd finalizers.
bool FdRetainCopy(FdEntry* copy);

struct ProcessOperationGuard {
    Process* process;
    explicit ProcessOperationGuard(Process* p) : process(p) {
        if (process) SchedulerKernelRegionEnter(process);
    }
    ~ProcessOperationGuard() { if (process) SchedulerKernelRegionLeave(process); }
};

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

static void ProcessClearLazyMappings(Process* proc)
{
    if (!proc) return;

    for (uint32_t i = 0; i < Process::MAX_MEMFD_MAPS; i++) {
        auto& m = proc->memfdMaps[i];
        if (m.length == 0) continue;
        uint64_t pages = (m.length + 4095) / 4096;
        for (uint64_t p = 0; p < pages; p++)
            VmmUnmapPage(proc->pageTable, VirtualAddress(m.vaddr + p * 4096));
        if (m.mfd) brook::MemFdHandleUnref(m.mfd);
        m.vaddr = 0; m.length = 0; m.offset = 0; m.vmmFlags = 0; m.mfd = nullptr;
    }

    for (uint32_t i = 0; i < Process::MAX_FILE_MAPS; i++) {
        auto& m = proc->fileMaps[i];
        if (m.length == 0) continue;
        // Each present file PTE owns one physical-page reference, independent
        // of any cache or fork co-owner. Release it when removing the mapping:
        // the later page-table walk can no longer see an unmapped leaf. In
        // particular, uncached pages are not necessarily on the PID cleanup
        // list (a prior shared-reference drop removes them from that list).
        // PmmUnrefPage preserves any remaining cache/fork references and
        // removes shared frames from PID cleanup so they cannot be freed twice.
        uint64_t pages = (m.length + 4095) / 4096;
        for (uint64_t p = 0; p < pages; p++) {
            VirtualAddress va(m.vaddr + p * 4096);
            PhysicalAddress phys = VmmVirtToPhys(proc->pageTable, va);
            if (!phys) continue;
            VmmUnmapPage(proc->pageTable, va);
            PmmUnrefPage(phys);
        }
        // Hold the fileMapLock to prevent FileMapHandleUserFault from
        // loading a stale vnode pointer while we clear and free it.
        SpinLockAcquire(&proc->fileMapLock);
        Vnode* vn = m.vnode;
        m.vnode = nullptr;
        m.vaddr = 0; m.length = 0; m.offset = 0; m.vmmFlags = 0;
        SpinLockRelease(&proc->fileMapLock);
        if (vn) { VfsFileMapUnpin(vn); VnodeHandleUnref(vn); }
    }
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
    ProcessOperationGuard operation(ProcessCurrent());
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
    ProcessOperationGuard operation(ProcessCurrent());
    if (!proc || !proc->fds) return;
    FdTableFree(proc->fds, ProcessFdLock(proc), fd);
}

FdEntry* FdGet(Process* proc, int fd)
{
    ProcessOperationGuard operation(ProcessCurrent());
    if (!proc || !proc->fds) return nullptr;
    return FdTableGet(proc->fds, ProcessFdLock(proc), fd);
}

bool FdClaim(Process* proc, int fd, FdClaimResult* out)
{
    ProcessOperationGuard operation(ProcessCurrent());
    if (!proc || !proc->fds) return false;
    return FdTableClaim(proc->fds, ProcessFdLock(proc), fd, out);
}

FdEntry* FdGetRef(Process* proc, int fd)
{
    ProcessOperationGuard operation(ProcessCurrent());
    if (!proc || !proc->fds) return nullptr;
    return FdTablePin(proc->fds, ProcessFdLock(proc), fd);
}

void FdPut(Process* proc, int fd)
{
    ProcessOperationGuard operation(ProcessCurrent());
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
    ProcessOperationGuard operation(ProcessCurrent());
    if (!proc || !proc->fds) return FdCloseResult::NotFound;
    return FdTableClose(proc->fds, ProcessFdLock(proc), fd, out);
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
static void SetupTLS(Process* proc, uint64_t stackVirtBase, uint64_t guardPages)
{
    proc->fsBase = 0;
    proc->savedCtx.gsBase = 0;
    if (proc->elf.tlsTotalSize == 0) return;

    uint64_t tlsPages = (proc->elf.tlsTotalSize + 64 + 4095) / 4096;
    uint64_t tlsBase = stackVirtBase - guardPages * 4096 - tlsPages * 4096;

    auto tlsToKernel = [&](uint64_t userAddr) -> uint8_t* {
        PhysicalAddress phys = VmmVirtToPhys(proc->pageTable, VirtualAddress(userAddr));
        return phys ? reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw()) : nullptr;
    };

    bool tlsOk = true;
    for (uint64_t i = 0; i < tlsPages; i++)
    {
        VirtualAddress vaddr(tlsBase + i * 4096);
        PhysicalAddress phys = PmmAllocPage(MemTag::User, ProcessAddressSpacePid(proc));
        if (!phys || !VmmMapPage(proc->pageTable, vaddr, phys,
                                  VMM_WRITABLE | VMM_USER,
                                  MemTag::User, ProcessAddressSpacePid(proc)))
        {
            tlsOk = false;
            break;
        }
        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        memset(p, 0, 4096);
    }

    if (!tlsOk) return;

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
            if (src && dst) memcpy(dst, src, chunk);

            off += chunk;
            remaining -= chunk;
        }
    }

    // TCB pointer: variant II (x86-64), FS:0 = pointer to self
    uint64_t tcbAddr = tlsBase + proc->elf.tlsTotalSize;
    tcbAddr = (tcbAddr + 15) & ~15ULL;

    auto* tcbSlot = reinterpret_cast<uint64_t*>(tlsToKernel(tcbAddr));
    if (tcbSlot) *tcbSlot = tcbAddr;

    // Stack canary at offset 40 (0x28) from FS base
    uint64_t canary = RandomU64();
    if (tcbAddr + 48 < tlsBase + tlsPages * 4096)
    {
        auto* canarySlot = reinterpret_cast<uint64_t*>(
            tlsToKernel(tcbAddr + 0x28));
        if (canarySlot) *canarySlot = canary;
    }

    proc->fsBase = tcbAddr;
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
                                uint64_t interpBase = 0)
{
    // Work from the top of the stack downward.
    uint64_t sp = proc->stackTop;

    // Translate user vaddr to kernel-writable pointer via direct map.
    auto toKernel = [&](uint64_t userAddr) -> uint8_t* {
        PhysicalAddress phys = VmmVirtToPhys(proc->pageTable, VirtualAddress(userAddr));
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
    for (int i = 0; i < envc; ++i)
    {
        uint64_t len = 0;
        while (envp[i][len]) ++len;
        envAddrs[i] = pushBytes(envp[i], len + 1);
    }

    // 3. Push argument strings and record pointers
    uint64_t argAddrs[EXEC_MAX_ARGS] = {};
    for (int i = 0; i < argc; ++i)
    {
        uint64_t len = 0;
        while (argv[i][len]) ++len;
        argAddrs[i] = pushBytes(argv[i], len + 1);
    }

    // 4. Align to 16 bytes
    sp &= ~0xFULL;

    // Compute total 8-byte slots to be pushed below, and pad so final RSP
    // is 16-byte aligned (Linux ABI: RSP mod 16 == 0 at process entry).
    //   auxv: 19 entries × 2 slots = 38
    //   envp: (envc + 1) slots (including NULL terminator)
    //   argv: (argc + 1) slots (including NULL terminator)
    //   argc: 1 slot
    int totalSlots = 38 + (envc + 1) + (argc + 1) + 1;
    if (totalSlots & 1)
        pushU64(0); // padding to maintain 16-byte alignment

    // 5. Push auxiliary vectors (in reverse order, so first entry is at lowest address)
    pushU64(0); pushU64(AT_NULL);                               // AT_NULL
    pushU64(execfnAddr); pushU64(AT_EXECFN);                    // AT_EXECFN
    pushU64(0); pushU64(AT_HWCAP2);                             // AT_HWCAP2 = 0
    pushU64(randomAddr); pushU64(AT_RANDOM);                    // AT_RANDOM
    pushU64(0); pushU64(AT_SECURE);                             // AT_SECURE = 0 (not setuid)
    pushU64(100); pushU64(AT_CLKTCK);                           // AT_CLKTCK = 100
    pushU64(0); pushU64(AT_HWCAP);                              // AT_HWCAP = 0 (conservative; some IFUNC libs may need real bits)
    pushU64(platformAddr); pushU64(AT_PLATFORM);                // AT_PLATFORM = "x86_64"
    pushU64(0); pushU64(AT_EGID);                               // AT_EGID = 0 (root)
    pushU64(0); pushU64(AT_GID);                                // AT_GID = 0
    pushU64(0); pushU64(AT_EUID);                               // AT_EUID = 0
    pushU64(0); pushU64(AT_UID);                                // AT_UID = 0
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
             PageTable pt, uint16_t pid);
uint64_t ElfLoadAt(const uint8_t* data, uint64_t size,
                   uint64_t base, PageTable pt, uint16_t pid);

// Base address where the dynamic linker / interpreter is loaded.
// Placed above USER_MMAP_END so it can never collide with sys_mmap allocations.
static constexpr uint64_t INTERP_LOAD_BASE = 0x7F0000000000ULL; // ~127 TB

// ---------------------------------------------------------------------------
// LoadInterpreter -- Read and load the ELF interpreter specified in
// proc->elf.interpPath.  Returns the interpreter entry point, or 0 on
// failure.  The interpreter is loaded at INTERP_LOAD_BASE.
// ---------------------------------------------------------------------------
static uint64_t LoadInterpreter(Process* proc)
{
    if (proc->elf.interpPath[0] == '\0') return 0;

    SerialPrintf("INTERP: loading '%s' for pid %u\n",
                 proc->elf.interpPath, proc->pid);

    // Try the exact path first.
    const char* whichPath = proc->elf.interpPath;
    Vnode* vn = VfsOpen(proc->elf.interpPath);

    // Fallback: try /boot prefix.
    char bootPath[256];
    if (!vn)
    {
        bootPath[0]='/',bootPath[1]='b',bootPath[2]='o',bootPath[3]='o',bootPath[4]='t';
        uint32_t bi = 5;
        const char* p = proc->elf.interpPath;
        while (*p && bi + 1 < sizeof(bootPath)) bootPath[bi++] = *p++;
        bootPath[bi] = '\0';
        whichPath = bootPath;
        vn = VfsOpen(bootPath);
    }

    // Fallback: extract filename and look in /boot/lib/.
    char libPath[256];
    if (!vn)
    {
        const char* fname = proc->elf.interpPath;
        for (const char* p = proc->elf.interpPath; *p; ++p)
            if (*p == '/') fname = p + 1;

        libPath[0]='/',libPath[1]='b',libPath[2]='o',libPath[3]='o',libPath[4]='t';
        libPath[5]='/',libPath[6]='l',libPath[7]='i',libPath[8]='b',libPath[9]='/';
        uint32_t li = 10;
        while (*fname && li + 1 < sizeof(libPath)) libPath[li++] = *fname++;
        libPath[li] = '\0';

        SerialPrintf("INTERP: loading '%s' for pid %d\n", libPath, proc->pid);
        whichPath = libPath;
        vn = VfsOpen(libPath);
    }

    if (!vn)
    {
        SerialPrintf("INTERP: failed to open '%s'\n", proc->elf.interpPath);
        return 0;
    }

    ExecutableImage image{};
    int readResult = ReadExecutableImage(vn, &image);
    VfsClose(vn);
    if (readResult != 0) {
        SerialPrintf("INTERP: read error %d for '%s'\n", readResult, whichPath);
        return 0;
    }
    SerialPrintf("INTERP: opened '%s' size=%lu\n", whichPath, image.size);
    uint64_t entry = ElfLoadAt(image.data, image.size, INTERP_LOAD_BASE,
                               proc->pageTable, proc->pid);
    FreeExecutableImage(&image);

    if (!entry)
        SerialPrintf("INTERP: ElfLoadAt failed for '%s'\n", proc->elf.interpPath);

    return entry;
}

Process* ProcessCreate(const uint8_t* elfData, uint64_t elfSize,
                       int argc, const char* const* argv,
                       int envc, const char* const* envp,
                       const FdEntry* stdFds)
{
    if (!ExecArgumentsFit(argc, argv, envc, envp)) return nullptr;
    auto* proc = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!proc) return nullptr;

    // Zero-initialize
    auto* raw = reinterpret_cast<uint8_t*>(proc);
    memset(raw, 0, sizeof(Process));
    proc->magic = PROCESS_MAGIC;
    proc->incarnation = NextIncarnation();
    proc->leaderIncarnation = 0;

    // Initialize FPU/SSE state with safe defaults so fxrstor works correctly
    // on the first context switch to this process.
    // FCW (bytes 0-1): 0x037F = mask all FPU exceptions
    // MXCSR (bytes 24-27): 0x1F80 = mask all SSE exceptions
    proc->fxsave.data[0] = 0x7F;
    proc->fxsave.data[1] = 0x03;
    proc->fxsave.data[24] = 0x80;
    proc->fxsave.data[25] = 0x1F;

    proc->pid = SchedulerAllocPid();
    proc->pgid = proc->pid;
    proc->sid = proc->pid;
    proc->tgid = proc->pid;         // Process is its own thread group leader
    proc->threadLeader = proc;
    proc->state = ProcessState::Ready;
    proc->runningOnCpu = -1;
    proc->cpuAffinity = -1;
    proc->tlbCpuMask = 0;
    proc->asTlbCpuMask = 0;
    proc->schedPriority = 2;  // SCHED_PRIORITY_NORMAL
    proc->umask = 022;         // Default umask

    // Allocate shared fd table (shared across CLONE_FILES threads).
    proc->fds = AllocFdTable();
    if (!proc->fds)
    {
        SerialPuts("PROC: fd table allocation failed\n");
        SchedulerFreePid(proc->pid);
        FreeProcessStruct(proc);
        return nullptr;
    }

    // Default working directory
    proc->cwd[0] = '/'; proc->cwd[1] = 'b'; proc->cwd[2] = 'o';
    proc->cwd[3] = 'o'; proc->cwd[4] = 't'; proc->cwd[5] = '\0';

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
    proc->pageTable = VmmCreateUserPageTable();
    if (!proc->pageTable)
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
    if (!ElfLoad(elfData, elfSize, &proc->elf, proc->pageTable, proc->pid))
    {
        SerialPuts("PROC: ELF load failed\n");
        VmmDestroyUserPageTable(proc->pageTable);
        VmmFreeKernelStack(kstackAddr, KERNEL_STACK_PAGES);
        kfree(proc->fds);
        SchedulerFreePid(proc->pid);
        FreeProcessStruct(proc);
        return nullptr;
    }

    // Verify page table integrity: ELF load area (PML4[0]) must have USER bit.
    {
        auto* pml4 = reinterpret_cast<volatile uint64_t*>(PhysToVirt(proc->pageTable.pml4).raw());
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

    proc->programBreak = proc->elf.programBreakLow;
    // Keep mmap beyond both the loaded image and its reserved brk range.
    uint64_t mmapFloor = (proc->elf.programBreakHigh + 16 * 1024 * 1024 + 4095) & ~uint64_t{4095};
    proc->mmapNext = mmapFloor > USER_MMAP_BASE ? mmapFloor : USER_MMAP_BASE;

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
            PmmKillPid(proc->pid);
            VmmDestroyUserPageTable(proc->pageTable);
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

    bool stackOk = true;
    for (uint64_t i = guardPages; i < totalStackPages; i++)
    {
        VirtualAddress vaddr(stackVirtBase + i * 4096);
        PhysicalAddress phys = PmmAllocPage(MemTag::User, ProcessAddressSpacePid(proc));
        if (!phys || !VmmMapPage(proc->pageTable, vaddr, phys,
                                  VMM_WRITABLE | VMM_USER,
                                  MemTag::User, ProcessAddressSpacePid(proc)))
        {
            stackOk = false;
            break;
        }
        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        memset(p, 0, 4096);
    }
    if (!stackOk)
    {
        SerialPuts("PROC: stack allocation failed\n");
        PmmKillPid(proc->pid);
        VmmDestroyUserPageTable(proc->pageTable);
        VmmFreeKernelStack(VirtualAddress(proc->kernelStackBase), KERNEL_STACK_PAGES);
        kfree(proc->fds);
        SchedulerFreePid(proc->pid);
        FreeProcessStruct(proc);
        return nullptr;
    }
    // Guard page (first page) is left unmapped — faults on stack overflow.
    proc->stackBase = stackVirtBase + guardPages * 4096;
    proc->stackTop  = stackVirtTop - 8;

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
    uint64_t userSP = SetupUserStack(proc, argc, argv, envc, envp, interpBase);

    // Store the final SP for SwitchToUserMode
    proc->stackTop = userSP;

    // Set up TLS if the ELF has a PT_TLS segment
    SetupTLS(proc, stackVirtBase, guardPages);

    // Set actual initial entry point (interpreter entry if dynamically linked).
    proc->initialEntry = interpEntry ? interpEntry : proc->elf.entryPoint;

    DbgPrintf("PROC: created pid=%u, entry=0x%lx, stack=0x%lx, brk=0x%lx, cr3=0x%lx\n",
                 proc->pid, proc->initialEntry, proc->stackTop,
                 proc->programBreak, proc->pageTable.pml4.raw());

    return proc;
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
    proc->pgid = proc->pid;
    proc->sid = proc->pid;
    proc->tgid = proc->pid;
    proc->threadLeader = proc;
    proc->state = ProcessState::Ready;
    proc->runningOnCpu = -1;
    proc->cpuAffinity = -1;
    proc->tlbCpuMask = 0;
    proc->asTlbCpuMask = 0;
    proc->isKernelThread = true;
    proc->schedPriority = priority;

    // Kernel threads use the kernel's page table
    proc->pageTable = VmmKernelCR3();

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
    ProcessOperationGuard operation(ProcessCurrent());
    if (!proc) return;
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
    ProcessOperationGuard operation(ProcessCurrent());
    if (!proc) return;

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
    if (!proc) return;

    PosixTimersCleanup(proc, false);
    bool isThread = proc->isThread;

    // Filesystem operations finish cooperatively before terminal publication;
    // there are no abandoned mutexes or backend mutations to force-release.
    ProcessReleaseFileFault(proc);

    // Threads don't own FDs — the leader does. Kernel threads never had
    // an fd table allocated (proc->fds is NULL), so skip the close loop
    // for them as well; otherwise ProcessCloseAllFds would deref NULL.
    if (!isThread && !proc->isKernelThread && proc->fds)
        ProcessCloseAllFds(proc);

    // Free the shared fd table once the leader (or fork root) is destroyed.
    // Threads get their fds pointer cleared so the leader still sees the
    // live table until its own ProcessDestroy runs.
    if (!isThread && proc->fds)
    {
        kfree(proc->fds);
        proc->fds = nullptr;
    }
    else if (isThread)
    {
        proc->fds = nullptr;
    }

    // Only the leader owns compositor/window state.  Do this before tearing
    // down the user page table because per-window VFB mappings are unmapped
    // through proc->pageTable.  The scheduler reaper can run from the LAPIC
    // timer path, so this path must not block waiting for a compositor frame.
    if (!isThread)
    {
        CompositorUnregisterProcess(proc);
        WmDestroyWindowForProcess(proc);
        // Release the app's GPU (virgl) context, if it created one.
        if (proc->gpuAppCtx > 0)
        {
            const brook::GpuAppOps* ga = brook::GpuAppGet();
            if (ga && ga->CtxDestroy) ga->CtxDestroy(proc->gpuAppCtx);
            proc->gpuAppCtx = 0;
        }
    }

    // Threads share the page table with the leader — don't destroy it
    if (!proc->isKernelThread && !isThread)
    {
        // Flush stale TLB entries for this address space on all remote CPUs
        // before tearing down the page table and freeing the physical pages,
        // so a CPU that still has this AS cached cannot read/write memory that
        // is about to be freed (and possibly reallocated). Mirrors the exec
        // teardown path (ProcessExec). The reaper runs on CPU0 and never has a
        // terminated process's page table loaded, so unlike exec there is no
        // local CR3 to switch away first.
        TlbShootdownFull(proc->pageTable.pml4.raw(), *AddressSpaceTlbMaskPtr(proc));

        // Clear lazy VMAs before page-table destruction. MemFd PTEs point at
        // memfd-owned pages, and file VMAs hold vnode references independent of
        // the fd table.
        ProcessClearLazyMappings(proc);
        VmmDestroyUserPageTable(proc->pageTable);
    }

    // Only free user pages for the leader process
    if (!isThread)
        VmmKillPid(proc->pid);

    // Free per-process kernel stack (each thread has its own).  This must be
    // after teardown work that may still need helper calls using this proc.
    if (proc->kernelStackBase)
        VmmFreeKernelStack(VirtualAddress(proc->kernelStackBase), KERNEL_STACK_PAGES);

    // Remove from scheduler tracking
    SchedulerRemoveProcess(proc);

    // Clear signal handler slot so the PID can be safely reused without
    // the next process inheriting our handler pointers (which would
    // reference user memory no longer mapped and cause a #PF on signal).
    //
    // Signal handlers are shared across all threads in a thread group
    // (CLONE_SIGHAND semantics) and indexed by tgid. Only clear when
    // the leader (pid == tgid) is destroyed, otherwise a thread exiting
    // before the leader would wipe the still-live process's handlers.
    if (proc->pid == proc->tgid) {
        for (int s = 0; s < 64; ++s) {
            g_sigHandlers[proc->tgid][s].handler = 0;
            g_sigHandlers[proc->tgid][s].flags = 0;
            g_sigHandlers[proc->tgid][s].restorer = 0;
            g_sigHandlers[proc->tgid][s].mask = 0;
        }
    }

    // Publish the final shared-address-space release only after thread cleanup.
    if (isThread && !proc->isKernelThread)
        SchedulerReleaseThreadAddressSpace(proc);

    FreeProcessStruct(proc);
}

// ---------------------------------------------------------------------------
// ProcessFork -- create a child process that is a copy of the parent.
// ---------------------------------------------------------------------------
// Uses Copy-on-Write (COW) for writable pages: both parent and child share
// the same physical page marked read-only with PTE_COW_BIT. First write
// triggers a page fault that copies the page (handled in idt.cpp).
// Read-only pages are shared directly with PmmRefPage.
// The child inherits open file descriptors (shallow copy).
// The child's trampoline returns to user mode with RAX=0 (fork return value).

// Helper: walk a 4-level page table and copy all leaf pages.
static bool ForkCopyUserPages(PageTable srcPt, PageTable dstPt,
                              uint16_t srcPid, uint16_t dstPid,
                              volatile uint64_t& srcTlbCpuMask, const Process* mappings)
{
    static constexpr uint64_t PTE_PHYS_MASK = 0x000FFFFFFFFFF000ULL;

    auto* srcPml4 = reinterpret_cast<uint64_t*>(
        PhysToVirt(srcPt.pml4).raw());

    [[maybe_unused]] uint64_t sharedCount = 0;
    uint64_t copiedCount = 0;

    extern volatile uint64_t g_lapicTickCount;
    uint64_t forkStartTick = g_lapicTickCount;

    // Only copy user-half (PML4 entries 0..255)
    for (uint64_t i4 = 0; i4 < 256; i4++)
    {
        if (!(srcPml4[i4] & VMM_PRESENT)) continue;

        auto* srcPdpt = reinterpret_cast<uint64_t*>(
            PhysToVirt(PhysicalAddress(srcPml4[i4] & PTE_PHYS_MASK)).raw());

        for (uint64_t i3 = 0; i3 < 512; i3++)
        {
            if (!(srcPdpt[i3] & VMM_PRESENT)) continue;
            if (srcPdpt[i3] & (1ULL << 7)) continue; // 1GB huge page, skip

            auto* srcPd = reinterpret_cast<uint64_t*>(
                PhysToVirt(PhysicalAddress(srcPdpt[i3] & PTE_PHYS_MASK)).raw());

            for (uint64_t i2 = 0; i2 < 512; i2++)
            {
                if (!(srcPd[i2] & VMM_PRESENT)) continue;
                if (srcPd[i2] & (1ULL << 7)) continue; // 2MB huge page, skip

                auto* srcPt4 = reinterpret_cast<uint64_t*>(
                    PhysToVirt(PhysicalAddress(srcPd[i2] & PTE_PHYS_MASK)).raw());

                for (uint64_t i1 = 0; i1 < 512; i1++)
                {
                    if (!(srcPt4[i1] & VMM_PRESENT)) continue;

                    // Reconstruct the virtual address
                    uint64_t vaddr = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);

                    PhysicalAddress srcPhys(srcPt4[i1] & PTE_PHYS_MASK);
                    uint64_t pteFlags = srcPt4[i1] & ~PTE_PHYS_MASK;
                    bool memfdPage = false;
                    for (uint32_t m = 0; m < Process::MAX_MEMFD_MAPS; ++m) {
                        const auto& map = mappings->memfdMaps[m];
                        if (map.mfd && map.length && vaddr >= map.vaddr &&
                            vaddr - map.vaddr < map.length) {
                            memfdPage = true;
                            break;
                        }
                    }
                    if (memfdPage) {
                        // Memfd VMAs borrow object-owned pages. Their copied
                        // VMA holds an object ref; neither COW nor a per-PTE
                        // page reference belongs here. COW disconnects parent
                        // writes from the compositor and can retire the backing.
                        if (!VmmMapPage(dstPt, VirtualAddress(vaddr), srcPhys,
                                        pteFlags & ~PTE_COW_BIT, MemTag::User, dstPid))
                            return false;
                        sharedCount++;
                        continue;
                    }
                    bool wasWritable = (pteFlags & VMM_WRITABLE) != 0;
                    bool alreadyCow  = (pteFlags & PTE_COW_BIT) != 0;
                    bool privateWritable = wasWritable || alreadyCow;

                    // COW fork: share writable pages between parent and
                    // child instead of copying.  Mark both PTEs read-only
                    // with PTE_COW_BIT set.  The page fault handler (idt.cpp)
                    // copies on first write.  TlbShootdownFull after the
                    // loop flushes stale writable TLB entries on all CPUs.
                    if (privateWritable)
                    {
                        // Build child PTE: same physical page, read-only + COW.
                        // Do NOT touch the parent PTE yet — that happens
                        // atomically with the child leaf write and refcount bump
                        // inside VmmForkCommitCowShare (BRO-155).
                        uint64_t childPte = (srcPhys.raw() & PTE_PHYS_MASK)
                                          | VMM_PRESENT
                                          | (pteFlags & VMM_USER)
                                          | (pteFlags & VMM_NO_EXEC)
                                          | (pteFlags & PTE_TAG_MASK)
                                          | PTE_COW_BIT
                                          | (((uint64_t)dstPid & 0x3FF) << PTE_PID_SHIFT);

                        // BRO-176: pin the shared frame BEFORE the child mapping
                        // becomes reachable. Publishing the child's leaf PTE
                        // (VmmMapPage below) adds a second mapper, but the refcount
                        // is not bumped until the commit. In that window a
                        // concurrent teardown of the parent's address space (a
                        // sibling thread's exit_group) could unref the frame to
                        // zero and free it while the child maps it — the cross-AS
                        // COW double-free. Taking an anticipatory reference here
                        // holds the frame at >=1 across the whole share, so it can
                        // never be freed mid-share. The matching unref (unpin)
                        // runs after the commit's own ref is in place. This also
                        // strengthens BRO-155: refcount is >=2 throughout the
                        // share window, so the COW write-fault handler always
                        // observes "shared" and copies, never upgrading in place.
                        PmmRefPage(srcPhys);

                        // Create intermediate page table entries in child FIRST.
                        // VmmMapPage takes g_userPtLock internally, so it must
                        // run BEFORE the locked commit (calling it under the lock
                        // would deadlock). The leaf PTE it writes here is
                        // overwritten by the commit below.
                        uint64_t childMapFlags = VMM_USER;
                        if (pteFlags & VMM_NO_EXEC)
                            childMapFlags |= VMM_NO_EXEC;
                        if (!VmmMapPage(dstPt, VirtualAddress(vaddr), srcPhys,
                                        childMapFlags, MemTag::User, dstPid))
                        {
                            SerialPrintf("FORK: failed to map COW page at vaddr 0x%lx\n", vaddr);
                            // Parent PTE was not modified — nothing to restore.
                            // Drop the anticipatory pin taken above.
                            PmmUnrefPage(srcPhys);
                            return false;
                        }

                        // Locate the child's leaf PTE (intermediate tables now
                        // exist) and commit the COW share atomically: downgrade
                        // parent, install child leaf, bump refcount under
                        // g_userPtLock so the COW write-fault handler can't
                        // observe a half-shared state.
                        auto* dstPml4 = reinterpret_cast<uint64_t*>(
                            PhysToVirt(dstPt.pml4).raw());
                        auto* dstPdpt = reinterpret_cast<uint64_t*>(
                            PhysToVirt(PhysicalAddress(dstPml4[i4] & PTE_PHYS_MASK)).raw());
                        auto* dstPd = reinterpret_cast<uint64_t*>(
                            PhysToVirt(PhysicalAddress(dstPdpt[i3] & PTE_PHYS_MASK)).raw());
                        auto* dstPt4 = reinterpret_cast<uint64_t*>(
                            PhysToVirt(PhysicalAddress(dstPd[i2] & PTE_PHYS_MASK)).raw());

                        VmmForkCommitCowShare(&srcPt4[i1], &dstPt4[i1],
                                              childPte, srcPhys);

                        // Unpin: the commit's own ref now accounts for the child
                        // mapper, so drop the anticipatory reference. Net effect of
                        // pin + commit-ref + unpin is exactly +1 (the child), but
                        // the frame was never freeable during the share.
                        PmmUnrefPage(srcPhys);

                        copiedCount++;
                        continue;
                    }

                    // Build child PTE: same physical page and read-only flags.
                    uint64_t childPte = (srcPhys.raw() & PTE_PHYS_MASK)
                                      | VMM_PRESENT
                                      | (pteFlags & VMM_USER)
                                      | (pteFlags & VMM_NO_EXEC);
                    // Preserve tag bits
                    childPte |= (pteFlags & PTE_TAG_MASK);
                    // Set child PID
                    childPte |= (((uint64_t)dstPid & 0x3FF) << PTE_PID_SHIFT);

                    // BRO-176: ref the shared frame BEFORE publishing the child
                    // mapping. VmmMapPage below makes the child a second mapper of
                    // srcPhys; if the refcount bump came after (as it used to), a
                    // concurrent parent-AS teardown could free the frame to zero in
                    // the window while the child maps it. Referencing first holds
                    // the frame across the publish.
                    PmmRefPage(srcPhys);

                    // Map into child's page table via WalkToPtr (need intermediate tables)
                    // Use VmmMapPage to create intermediates, then override leaf PTE.
                    uint64_t childMapFlags = VMM_USER;
                    if (pteFlags & VMM_NO_EXEC)
                        childMapFlags |= VMM_NO_EXEC;

                    // We need to create intermediate page table entries.
                    // VmmMapPage will create them, then we overwrite the leaf PTE.
                    if (!VmmMapPage(dstPt, VirtualAddress(vaddr), srcPhys,
                                    childMapFlags, MemTag::User, dstPid))
                    {
                        SerialPrintf("FORK: failed to map shared page at vaddr 0x%lx\n", vaddr);
                        // Drop the reference taken above for the child that never mapped.
                        PmmUnrefPage(srcPhys);
                        return false;
                    }

                    // Now overwrite the child's leaf PTE with our carefully
                    // constructed one preserving Brook's metadata bits.
                    {
                        // Walk to the PTE we just created
                        auto* dstPml4 = reinterpret_cast<uint64_t*>(
                            PhysToVirt(dstPt.pml4).raw());
                        auto* dstPdpt = reinterpret_cast<uint64_t*>(
                            PhysToVirt(PhysicalAddress(dstPml4[i4] & PTE_PHYS_MASK)).raw());
                        auto* dstPd = reinterpret_cast<uint64_t*>(
                            PhysToVirt(PhysicalAddress(dstPdpt[i3] & PTE_PHYS_MASK)).raw());
                        auto* dstPt4 = reinterpret_cast<uint64_t*>(
                            PhysToVirt(PhysicalAddress(dstPd[i2] & PTE_PHYS_MASK)).raw());
                        dstPt4[i1] = childPte;
                    }

                    sharedCount++;
                }
            }
        }
    }

    [[maybe_unused]] uint64_t forkElapsed = g_lapicTickCount - forkStartTick;
    DbgPrintf("[PROFILE] fork_pages t=%lums pid=%u->%u cow=%lu shared=%lu elapsed=%lums\n",
              g_lapicTickCount,
              static_cast<uint32_t>(srcPid), static_cast<uint32_t>(dstPid),
              copiedCount, sharedCount, forkElapsed);

    // Flush all TLB entries for the parent's address space. Parent PTEs
    // were downgraded from writable to RO+COW, so any stale writable
    // entry on any CPU must be flushed.
    if (copiedCount > 0)
    {
        TlbShootdownFull(srcPt.pml4.raw(), srcTlbCpuMask);
    }

    return true;
}

Process* ProcessFork(Process* parent, uint64_t userRip,
                     uint64_t userRsp, uint64_t userRflags)
{
    if (!parent || parent->isKernelThread) return nullptr;
    ProcessOperationGuard operation(parent);

    auto* child = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!child) return nullptr;

    // Copy entire parent process struct as a starting point
    memcpy(child, parent, sizeof(Process));
    child->activeFileFaultVnode = nullptr;
    child->fdLock.next = child->fdLock.serving = 0;
    child->fileMapLock.next = child->fileMapLock.serving = 0;
    child->magic = PROCESS_MAGIC;
    child->incarnation = NextIncarnation();
    child->leaderIncarnation = 0;   // a fork child is its own leader

    // Allocate new PID
    child->pid = SchedulerAllocPid();
    child->parentPid = parent->tgid;
    child->pgid = parent->pgid;      // Inherit process group from parent
    child->sid = parent->sid;         // Inherit session from parent
    child->tgid = child->pid;         // Fork creates a new thread group
    child->threadLeader = child;
    child->isThread = false;
    child->clearChildTid = 0;
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
    child->asTlbCpuMask = 0;
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
    child->pageTable = VmmCreateUserPageTable();
    if (!child->pageTable)
    {
        SerialPuts("FORK: page table allocation failed\n");
        VmmFreeKernelStack(kstackAddr, KERNEL_STACK_PAGES);
        kfree(child->fds);
        SchedulerFreePid(child->pid);
        FreeProcessStruct(child);
        return nullptr;
    }

    // Threads copy the address-space leader's current VMA bookkeeping, not
    // the stale arrays/break/frontier copied when that thread was created.
    Process* mapOwner = parent->isThread && parent->threadLeader
                      ? parent->threadLeader : parent;
    child->programBreak = mapOwner->programBreak;
    child->mmapNext = mapOwner->mmapNext;
    SpinLockAcquire(&mapOwner->fileMapLock);
    memcpy(child->memfdMaps, mapOwner->memfdMaps, sizeof(child->memfdMaps));
    memcpy(child->fileMaps, mapOwner->fileMaps, sizeof(child->fileMaps));
    for (uint32_t i = 0; i < Process::MAX_MEMFD_MAPS; ++i)
        if (child->memfdMaps[i].length && child->memfdMaps[i].mfd)
            brook::MemFdHandleRef(child->memfdMaps[i].mfd);
    for (uint32_t i = 0; i < Process::MAX_FILE_MAPS; ++i)
        if (child->fileMaps[i].length && child->fileMaps[i].vnode) {
            VfsFileMapClone(child->fileMaps[i].vnode);
            __atomic_fetch_add(&child->fileMaps[i].vnode->refCount, 1, __ATOMIC_RELEASE);
        }
    SpinLockRelease(&mapOwner->fileMapLock);

    // Copy writable private pages; shared memfd pages retain object backing.
    if (!ForkCopyUserPages(parent->pageTable, child->pageTable,
                           parent->pid, child->pid,
                           *AddressSpaceTlbMaskPtr(parent), child))
    {
        SerialPuts("FORK: address space copy failed\n");
        ProcessClearLazyMappings(child);
        VmmDestroyUserPageTable(child->pageTable);
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
        ProcessClearLazyMappings(child);
        VmmDestroyUserPageTable(child->pageTable);
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

    // Inherit parent's signal handlers (POSIX fork semantics). The
    // g_sigHandlers table is indexed by pid, so we must explicitly copy;
    // otherwise the child's slot contains stale handler pointers from
    // whatever previous process had the same pid — a deterministic #PF
    // waiting for the first signal delivery (e.g. SIGCHLD on waitpid).
    for (int s = 0; s < 64; ++s)
        g_sigHandlers[child->tgid][s] = g_sigHandlers[parent->tgid][s];

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

    return child;
}

// ---------------------------------------------------------------------------
// ProcessCreateThread -- create a new thread sharing the parent's address space.
// ---------------------------------------------------------------------------

Process* ProcessCreateThread(Process* parent, uint64_t userRip,
                             uint64_t userRsp, uint64_t userRflags,
                             uint64_t tlsBase)
{
    if (!parent || parent->isKernelThread) return nullptr;

    auto* thread = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!thread) return nullptr;

    // Copy parent process struct as starting point
    memcpy(thread, parent, sizeof(Process));
    thread->activeFileFaultVnode = nullptr;
    thread->magic = PROCESS_MAGIC;
    thread->incarnation = NextIncarnation();
    // Allocate TID (threads get unique PIDs but share tgid)
    thread->pid = SchedulerAllocPid();
    if (thread->pid == 0)
    {
        FreeProcessStruct(thread);
        return nullptr;
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
    thread->asTlbCpuMask = 0;
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
    thread->pageTable = parent->pageTable;

    // Share the same program break, mmap state
    thread->programBreak = parent->programBreak;
    thread->mmapNext = parent->mmapNext;

    // Set TLS base for this thread (musl allocates per-thread TLS)
    thread->fsBase = tlsBase;
    thread->savedCtx.fsBase = tlsBase;

    // Signal state: inherit mask, clear pending
    thread->sigPending = 0;
    thread->inSignalHandler = false;
    thread->inSignalHandlerOnAltStack = false;
    thread->sigReturnPending = false;

    // Threads share the parent's tgid (set above), so they already index
    // into the same g_sigHandlers[tgid] slot — no copy needed.

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
              thread->pageTable.pml4.raw());

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
    stackSize = (stackSize + 4095) & ~uint32_t(4095);
    if (stackSize > (1u << 20)) stackSize = 1u << 20;

    // argBytes must fit in top of stack, leaving room for alignment + a
    // small amount of spare so the entry function doesn't immediately
    // clobber its own argument when it pushes stack frames.
    if (argLen > stackSize / 2) return 0;

    // Locate the thread-group leader — all user address-space bookkeeping
    // (mmapNext) lives there, just like sys_mmap does.
    Process* leader = target->threadLeader ? target->threadLeader : target;

    // Reserve a VA range in the leader's mmap area.  Use a dedicated lock
    // to avoid clashing with sys_mmap — overlap would be catastrophic.
    // We intentionally don't share the sys_mmap spinlock (different TU).
    static SpinLock s_remoteStackLock;
    uint64_t stackBase = 0;
    uint32_t pages = stackSize / 4096;
    {
        SpinLockAcquire(&s_remoteStackLock);
        stackBase = leader->mmapNext;
        if (stackBase + stackSize > USER_MMAP_END) {
            SpinLockRelease(&s_remoteStackLock);
            SerialPrintf("CRT: out of user VA (pid %u)\n", target->pid);
            return 0;
        }
        leader->mmapNext = stackBase + stackSize;
        SpinLockRelease(&s_remoteStackLock);
    }

    // Allocate + map pages as User/RW.  NX is set via the architecture
    // default for non-executable; user stack must not be executable.
    uint64_t vmmFlags = VMM_WRITABLE | VMM_USER;
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < pages; ++i) {
        PhysicalAddress phys = PmmAllocPage(MemTag::User, leader->pid);
        if (!phys) break;
        if (!VmmMapPage(target->pageTable,
                        VirtualAddress(stackBase + i * 4096),
                        phys, vmmFlags, MemTag::User, leader->pid)) {
            PmmFreePage(phys);
            break;
        }
        // Zero the page via the direct physmap (stack hygiene).
        auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        for (uint32_t b = 0; b < 4096; ++b) kp[b] = 0;
        ++mapped;
    }
    if (mapped < pages) {
        // Partial mapping — roll back what we did and fail.
        for (uint32_t i = 0; i < mapped; ++i) {
            VirtualAddress va(stackBase + i * 4096);
            PhysicalAddress p = VmmVirtToPhys(target->pageTable, va);
            VmmUnmapPage(target->pageTable, va);
            if (p) PmmFreePage(p);
        }
        SerialPrintf("CRT: stack alloc failed (%u/%u pages) for pid %u\n",
                     mapped, pages, target->pid);
        return 0;
    }

    uint64_t stackTop = stackBase + stackSize;

    // Place argBytes at the very top of the stack.  Align the arg buffer
    // to 16 bytes so the entry function gets an aligned RSP.
    uint64_t argVA = 0;
    if (argBytes && argLen > 0) {
        uint64_t argStart = (stackTop - argLen) & ~uint64_t(0xF);
        argVA = argStart;

        // Copy via the direct physmap.  The buffer may cross page
        // boundaries, so walk page-by-page.
        const auto* src = static_cast<const uint8_t*>(argBytes);
        uint32_t remaining = argLen;
        uint64_t cursor = argStart;
        while (remaining > 0) {
            uint64_t pageVA = cursor & ~uint64_t(0xFFF);
            uint64_t pageOff = cursor & 0xFFF;
            uint32_t thisPage = 4096u - static_cast<uint32_t>(pageOff);
            if (thisPage > remaining) thisPage = remaining;
            PhysicalAddress phys = VmmVirtToPhys(target->pageTable,
                                                  VirtualAddress(pageVA));
            if (!phys) {
                SerialPrintf("CRT: arg copy hit unmapped page 0x%lx pid %u\n",
                             pageVA, target->pid);
                return 0;
            }
            auto* dst = reinterpret_cast<uint8_t*>(
                PhysToVirt(phys).raw() + pageOff);
            memcpy(dst, src, thisPage);
            src += thisPage;
            cursor += thisPage;
            remaining -= thisPage;
        }
    }

    // Pick the user RSP.  Leave 16 bytes of headroom below the arg area
    // for alignment + a sentinel return address (function prologue will
    // push RBP onto this).  Linux ABI: at function entry RSP % 16 == 8.
    uint64_t userRsp = argVA ? (argVA - 16) : (stackTop - 16);
    userRsp &= ~uint64_t(0xF);
    userRsp -= 8;   // so that (rsp + 8) is 16-aligned = ABI pre-call state

    // Create the thread using the existing CLONE_VM|CLONE_FILES machinery.
    // Pass target's fs base as the TLS — the entry code must tolerate
    // whatever fs is pointing at (or re-initialise it explicitly).
    Process* thread = ProcessCreateThread(target, entry, userRsp, 0x202,
                                          target->fsBase);
    if (!thread) {
        // Roll back the stack mapping.
        for (uint32_t i = 0; i < pages; ++i) {
            VirtualAddress va(stackBase + i * 4096);
            PhysicalAddress p = VmmVirtToPhys(target->pageTable, va);
            VmmUnmapPage(target->pageTable, va);
            if (p) PmmFreePage(p);
        }
        return 0;
    }

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

uint64_t ProcessExec(Process* proc, const uint8_t* elfData, uint64_t elfSize,
                     int argc, const char* const* argv,
                     int envc, const char* const* envp,
                     uint64_t* outStackPtr)
{
    if (!proc || proc->isKernelThread) return 0;
    if (!ExecArgumentsFit(argc, argv, envc, envp)) return 0;

    // TLB shootdown replaces the previous CPU pinning workaround for BRO-133.
    // Flush stale TLB entries for the old address space on all remote CPUs
    // before we destroy the page tables and free the underlying physical pages.

    // Save the old page table and switch CR3 to the kernel's page table
    // BEFORE destroying the old one. This avoids a use-after-free: the
    // CPU's CR3 would point to a freed PML4 page otherwise.
    // This exec is committing to replace/detach the old image. No timer
    // may retain its old opaque user value across that replacement.
    PosixTimersCleanup(proc, true);
    PageTable oldPt = proc->pageTable;
    PageTable kernelPt = VmmKernelCR3();

    // When a thread exec's, it shares the page table with the leader and
    // other threads.  We must NOT destroy the shared PT — the group leader's
    // reaper will do that once all threads have exited.  Instead, just detach
    // from the thread group and create a fresh address space below.
    bool wasThread = proc->isThread;

    // Flush TLB entries for the old address space on all remote CPUs
    TlbShootdownFull(oldPt.pml4.raw(), *AddressSpaceTlbMaskPtr(proc));

    __asm__ volatile("mov %0, %%cr3" : : "r"(kernelPt.pml4.raw()) : "memory");

    // 1. Free all user-space pages and destroy old page table — but ONLY if
    //    we are the sole owner.  Threads share the PT with the leader; if we
    //    destroy it here, the leader (and siblings) lose their address space.
    if (!wasThread)
    {
        ProcessClearLazyMappings(proc);
        VmmDestroyUserPageTable(oldPt);
    }
    else
    {
        // Detach from the thread group — this thread becomes its own leader.
        proc->isThread = false;
        proc->tgid = proc->pid;
        proc->threadLeader = proc;
    }
    // Old address space is gone (or detached) — reset TLB CPU mask
    proc->tlbCpuMask = 0;
    proc->asTlbCpuMask = 0;
    // NOTE: PmmFreeByTag removed here. VmmDestroyUserPageTable already calls
    // PmmUnrefPage on every leaf PTE, which correctly frees pages at refcount=0
    // and preserves pages still referenced by the global file page cache.

    // Any user-space pointers the kernel held into the old address space are
    // now stale. If we fail later in ProcessExec, SchedulerExitCurrentProcess
    // must not dereference them. Linux does the same in flush_old_exec().
    proc->clearChildTid = 0;
    // exec() replaces the program image; a previously-registered crash
    // entry point is no longer valid (it pointed into the old VAS).
    proc->crashEntry = 0;
    proc->crashInProgress = false;

    // 2. Create fresh page table
    proc->pageTable = VmmCreateUserPageTable();
    if (!proc->pageTable)
    {
        SerialPuts("EXEC: page table allocation failed\n");
        return 0;
    }

    // 3. Load the new ELF binary
    if (!ElfLoad(elfData, elfSize, &proc->elf, proc->pageTable, proc->pid))
    {
        SerialPuts("EXEC: ELF load failed\n");
        return 0;
    }

    proc->programBreak = proc->elf.programBreakLow;
    // Keep mmap beyond both the loaded image and its reserved brk range.
    uint64_t mmapFloor = (proc->elf.programBreakHigh + 16 * 1024 * 1024 + 4095) & ~uint64_t{4095};
    proc->mmapNext = mmapFloor > USER_MMAP_BASE ? mmapFloor : USER_MMAP_BASE;

    // Load dynamic linker if the ELF specifies PT_INTERP.
    uint64_t interpEntry = 0;
    uint64_t interpBase  = 0;
    if (proc->elf.interpPath[0] != '\0')
    {
        interpEntry = LoadInterpreter(proc);
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

    for (uint64_t i = guardPages; i < totalStackPages; i++)
    {
        VirtualAddress vaddr(stackVirtBase + i * 4096);
        PhysicalAddress phys = PmmAllocPage(MemTag::User, ProcessAddressSpacePid(proc));
        if (!phys || !VmmMapPage(proc->pageTable, vaddr, phys,
                                  VMM_WRITABLE | VMM_USER,
                                  MemTag::User, ProcessAddressSpacePid(proc)))
        {
            SerialPuts("EXEC: stack allocation failed\n");
            return 0;
        }
        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        memset(p, 0, 4096);
    }
    proc->stackBase = stackVirtBase + guardPages * 4096;
    proc->stackTop  = stackVirtTop - 8;

    // 5. Build user stack with argc/argv/envp/auxv
    uint64_t userSP = SetupUserStack(proc, argc, argv, envc, envp, interpBase);
    proc->stackTop = userSP;

    // 6. Set up TLS
    SetupTLS(proc, stackVirtBase, guardPages);

    // 7. Clear framebuffer state (new program starts fresh)
    proc->fbVirtual = nullptr;
    proc->fbVirtualSize = 0;
    proc->fbVfbWidth = 0;
    proc->fbVfbHeight = 0;
    proc->fbVfbStride = 0;
    proc->fbDestX = 0;
    proc->fbDestY = 0;
    proc->fbScale = 0;
    proc->fbDirty = 0;
    proc->fbExitColor = 0;

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
    for (int s = 0; s < 64; ++s) {
        auto& sa = g_sigHandlers[proc->tgid][s];
        if (sa.handler != 1 /* SIG_IGN */) {
            sa.handler = 0;
            sa.flags = 0;
            sa.restorer = 0;
            sa.mask = 0;
        }
    }

    *outStackPtr = userSP;

    // If dynamically linked, start at the interpreter's entry point;
    // otherwise, start at the main binary's entry.
    return interpEntry ? interpEntry : proc->elf.entryPoint;
}

// ---------------------------------------------------------------------------
// Signal delivery
// ---------------------------------------------------------------------------

// Signal constants
static constexpr int SIGKILL = 9;
[[maybe_unused]] static constexpr int SIGSTOP = 19;

int ProcessSendSignal(Process* proc, int signum)
{
    if (!proc) return -3; // ESRCH
    if (signum < 1 || signum > 64) return -22; // EINVAL

    // SIGKILL and SIGSTOP cannot be caught/blocked
    if (signum == SIGKILL)
    {
        // Immediately terminate. There's a race window if the process is
        // Running on another CPU — it may execute a few more instructions
        // before the next timer tick preempts it. This is acceptable for a
        // hobby OS; a production kernel would use an IPI to force-preempt.
        proc->fbVirtual = nullptr;
        proc->fbVirtualSize = 0;
        SchedulerMarkTerminated(proc, SIGKILL);
        // BRO-173/175: reapable is decoupled from holders; the reaper requires
        // refCount==0 before freeing, so it is safe to mark reapable here.
        __atomic_store_n(&proc->reapable, true, __ATOMIC_RELEASE);
        DbgPrintf("SIGNAL: SIGKILL -> pid %u\n", proc->pid);
        return 0;
    }

    if (signum == SIGSTOP)
    {
        // Immediately stop (cannot be caught/blocked)
        SchedulerStop(proc, signum);
        DbgPrintf("SIGNAL: SIGSTOP -> pid %u stopped (default)\n", proc->pid);
        return 0;
    }

    // SIGCONT: resume stopped process (always, even if blocked/ignored)
    static constexpr int SIGCONT = 18;
    if (signum == SIGCONT)
    {
        if (proc->state == ProcessState::Stopped)
        {
            SchedulerContinue(proc);
            DbgPrintf("SIGNAL: SIGCONT -> pid %u resumed\n", proc->pid);
        }
        // Still deliver SIGCONT if a handler is registered
        uint64_t bit = 1ULL << (signum - 1);
        __atomic_or_fetch(&proc->sigPending, bit, __ATOMIC_RELEASE);
        return 0;
    }

    // SIGTSTP/SIGTTIN/SIGTTOU: if no user handler registered, stop immediately
    // (like SIGSTOP). Otherwise fall through to set pending for handler delivery.
    if (signum == 20 || signum == 21 || signum == 22)
    {
        KernelSigaction& sa = g_sigHandlers[proc->tgid][signum - 1];
        if (sa.handler == 0 || sa.handler == 1) // SIG_DFL or SIG_IGN
        {
            if (sa.handler == 1) return 0; // SIG_IGN — ignore
            // Default action: stop
            SchedulerStop(proc, signum);
            DbgPrintf("SIGNAL: sig %d -> pid %u stopped (default)\n", signum, proc->pid);
            return 0;
        }
    }

    // Drop signals whose current disposition is "ignore".  Without this the
    // bit sits in sigPending forever and HasPendingSignals() returns true,
    // causing every blocking syscall to return -EINTR — which for line-reads
    // in the shell looks like EOF and silently kills bash on e.g. SIGWINCH.
    //
    // Signals that are ignored:
    //   - explicit SIG_IGN
    //   - SIG_DFL where the POSIX default is "ignore": SIGCHLD (17),
    //     SIGURG (23), SIGWINCH (28)
    {
        KernelSigaction& sa = g_sigHandlers[proc->tgid][signum - 1];
        bool defaultIsIgnore = (signum == 17 || signum == 23 || signum == 28);
        if (sa.handler == 1 ||
            (sa.handler == 0 && defaultIsIgnore))
        {
            return 0;
        }

        // SIG_DFL where the default action is "terminate": kill the process.
        // Without this, abort()'s raise(SIGABRT) with SIG_DFL sets the bit
        // pending but never terminates — execution falls through to HLT.
        bool defaultIsTerminate = (signum == 1  || signum == 2  || signum == 3  ||
                                   signum == 4  || signum == 6  || signum == 8  ||
                                   signum == 11 || signum == 13 || signum == 14 ||
                                   signum == 15);
        if (sa.handler == 0 && defaultIsTerminate)
        {
            proc->fbVirtual = nullptr;
            proc->fbVirtualSize = 0;
            SchedulerMarkTerminated(proc, signum);
            // BRO-173/175: reapable decoupled from holders (reaper requires
            // refCount==0), so it is safe to mark reapable unconditionally.
            __atomic_store_n(&proc->reapable, true, __ATOMIC_RELEASE);
            DbgPrintf("SIGNAL: sig %d -> pid %u terminated (default action)\n",
                      signum, proc->pid);
            return 0;
        }
    }

    uint64_t bit = 1ULL << (signum - 1);
    __atomic_or_fetch(&proc->sigPending, bit, __ATOMIC_RELEASE);

    // Wake blocked processes so they can handle the signal
    if (proc->state == ProcessState::Blocked)
    {
        proc->pendingWakeup = 1;
        SchedulerUnblock(proc);
    }

    return 0;
}

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

extern "C" int64_t SyscallCheckSignals(SyscallFrame* frame, int64_t syscallResult)
{
    Process* proc = ProcessCurrent();
    if (!proc || proc->isKernelThread) return syscallResult;

    if (SchedulerKernelOperationBoundary()) {
        SchedulerYield();
        __builtin_trap(); // A terminal thread must never resume.
    }

    // ppoll/pselect6/epoll_pwait must select pending signals while their
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
        {
            uint64_t ucEnd = frame->rsp + sizeof(SignalUcontext);
            if (frame->rsp >= 0x0000800000000000ULL || ucEnd > 0x0000800000000000ULL ||
                ucEnd < frame->rsp) {
                brook::SerialPrintf("SIGRETURN: bad user stack 0x%lx pid %u\n", frame->rsp, proc->pid);
                return syscallResult;
            }
        }
        auto* uc = reinterpret_cast<SignalUcontext*>(frame->rsp);
        const SignalMcontext& mc = uc->uc_mcontext;

        // Restore signal mask
        proc->sigMask = uc->uc_sigmask;

        // Restore all registers from mcontext into the SyscallFrame
        frame->rcx    = mc.rip;      // user RIP (sysret uses RCX)
        frame->rsp    = mc.rsp;
        frame->rflags = mc.eflags;
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

        return static_cast<int64_t>(mc.rax); // restore original RAX
    }

    // --- Signal delivery ---
    if (proc->inSignalHandler) return syscallResult;

    uint64_t deliverable = (proc->sigPending | PosixTimerPending(proc)) & ~proc->sigMask;
    if (deliverable == 0) return syscallResult;

    // Find lowest pending signal
    int signum = __builtin_ctzll(deliverable) + 1;

    SignalInfo signalInfo{};
    if (!PosixTimerDequeue(proc, 1ULL << (signum - 1), &signalInfo)) {
        uint64_t previous = __atomic_fetch_and(&proc->sigPending,
            ~(1ULL << (signum - 1)), __ATOMIC_ACQ_REL);
        if (!(previous & (1ULL << (signum - 1)))) return syscallResult;
        signalInfo.si_signo = signum;
    }

    uint16_t pid = proc->tgid;
    if (pid >= MAX_PROCESSES) return syscallResult;

    KernelSigaction& sa = g_sigHandlers[pid][signum - 1];

    // SIG_DFL (0) — default action
    if (sa.handler == 0)
    {
        // Timer-origin signals preserve pending/mask semantics until dequeue.
        // Catchable signals other than the default-ignore set terminate by
        // default, including the real-time range (ordinary legacy dispatch
        // below historically omitted those defaults).
        if (signalInfo.si_code == -2 && signum != 17 && signum != 23 && signum != 28) {
            SchedulerKillThreadGroup(proc->tgid, proc, 128 + signum, signum);
            SchedulerExitCurrentProcess(128 + signum, signum);
        }
        switch (signum)
        {
        case 1:  // SIGHUP
        case 2:  // SIGINT
        case 3:  // SIGQUIT
        case 6:  // SIGABRT
        case 11: // SIGSEGV
        case 13: // SIGPIPE
        case 14: // SIGALRM
        case 15: // SIGTERM
            DbgPrintf("SIGNAL: default terminate pid %u by signal %d\n", proc->pid, signum);
            // A fatal default signal terminates the whole thread group on
            // Linux. Killing only this thread lets sibling threads run after
            // process-wide teardown has started.
            SchedulerKillThreadGroup(proc->tgid, proc, 128 + signum, signum);
            SchedulerExitCurrentProcess(128 + signum, signum);
        case 17: // SIGCHLD — default is ignore
        case 28: // SIGWINCH — ignore
            return syscallResult;
        case 20: // SIGTSTP — default is stop
        case 21: // SIGTTIN
        case 22: // SIGTTOU
        {
            DbgPrintf("SIGNAL: pid %u stopped by signal %d\n", proc->pid, signum);
            // Set Stopped state. The scheduler timer tick will deschedule
            // this process when it sees state != Running. We can't call
            // SchedulerStop/Yield from SyscallCheckSignals because we're
            // in the syscall return path with a special stack layout.
            SchedulerMarkStopped(proc, signum);
            return syscallResult;
        }
        default:
            return syscallResult;
        }
    }

    // SIG_IGN (1)
    if (sa.handler == 1)
        return syscallResult;

    // --- User handler: build SignalFrame on user stack ---
    DbgPrintf("SIGNAL: delivering signal %d to pid %u handler=0x%lx restorer=0x%lx\n",
              signum, proc->pid, sa.handler, sa.restorer);
    // Promote SIGWINCH delivery debug to SerialPrintf in release until the
    // resize-kills-terminal bug is closed.
    if (signum == 28)
        brook::SerialPrintf("SIGNAL: deliver SIGWINCH pid=%u tgid=%u handler=0x%lx restorer=0x%lx flags=0x%lx syscallResult=%ld syscall=%lu\n",
                            proc->pid, proc->tgid, sa.handler, sa.restorer, sa.flags,
                            syscallResult, proc->currentSyscallNum);

    proc->inSignalHandler = true;

    // Save current signal mask, then block signals specified by sa_mask + this signal
    proc->sigSavedMask = returnMask;
    proc->sigMask |= sa.mask | (1ULL << (signum - 1));
    // Never block SIGKILL or SIGSTOP
    proc->sigMask &= ~((1ULL << 8) | (1ULL << 18));

    // Build the signal frame — on alt stack if SA_ONSTACK and one is configured,
    // otherwise on the current user stack (skip red zone).
    uint64_t userRsp;
    bool useAlt = (sa.flags & SA_ONSTACK) && proc->sigAltstackSp != 0
                  && !(proc->sigAltstackFlags & 2 /*SS_DISABLE*/)
                  && !proc->inSignalHandlerOnAltStack;
    if (useAlt)
    {
        userRsp = proc->sigAltstackSp + proc->sigAltstackSize;
        proc->inSignalHandlerOnAltStack = true;
    }
    else
    {
        userRsp = frame->rsp;
        userRsp -= 128;                    // skip red zone
    }
    userRsp -= sizeof(SignalFrame);
    userRsp = (userRsp & ~0xFULL) - 8;    // function-entry ABI: RSP % 16 == 8

    {
        uint64_t sfEnd = userRsp + sizeof(SignalFrame);
        if (userRsp >= 0x0000800000000000ULL || sfEnd > 0x0000800000000000ULL ||
            sfEnd < userRsp) {
            brook::SerialPrintf("SIGNAL: bad user stack 0x%lx for pid %u signal %d\n",
                                userRsp, proc->pid, signum);
            proc->inSignalHandler = false;
            proc->sigMask = proc->sigSavedMask;
            return syscallResult;
        }
    }
    auto* sf = reinterpret_cast<SignalFrame*>(userRsp);

    // Clear the frame
    memset(sf, 0, sizeof(SignalFrame));

    // Return address: sa_restorer (musl's __restore_rt which calls rt_sigreturn)
    sf->pretcode = sa.restorer;

    // Fill ucontext
    sf->uc.uc_sigmask = proc->sigSavedMask;
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
    mc.cs     = 0x23;           // user code segment

    // Linux restarts selected interrupted syscalls after a signal handler
    // returns when the handler was installed with SA_RESTART. Bash relies on
    // this for SIGWINCH while blocked in terminal read(); otherwise resize
    // converts the read into EINTR and the interactive shell can exit.
    //
    // SA_RESTART applies to most "slow" syscalls: read, write, open, ioctl,
    // wait4, nanosleep, poll, futex, etc.
    // The exceptions (never restarted) are: connect, accept, recvfrom,
    // sendto, epoll_wait, epoll_pwait, pselect6 and ppoll — we don't restart those.
    //
    // We read the syscall number from the per-process snapshot rather than
    // gs:120 (per-CPU) because other processes may have overwritten gs:120
    // while this process was blocked in SchedulerBlock().
    uint64_t syscallNum = proc->currentSyscallNum;
    bool restartable = false;
    switch (syscallNum)
    {
    case 0:   // read
    case 1:   // write
    case 2:   // open
    case 3:   // close
    case 7:   // poll
    case 16:  // ioctl
    case 17:  // pread64
    case 18:  // pwrite64
    case 19:  // readv
    case 20:  // writev
    case 35:  // nanosleep
    case 61:  // wait4
    case 202: // futex
    case 230: // clock_nanosleep
        restartable = true;
        break;
    }
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
    sf->info = signalInfo;

    // Redirect execution to the signal handler
    frame->rcx = sa.handler;                           // RIP = handler
    frame->rsp = userRsp;                              // RSP = signal frame
    frame->rdi = static_cast<uint64_t>(signum);        // arg1 = signum
    if (sa.flags & SA_SIGINFO)
    {
        // SA_SIGINFO: handler(int signum, siginfo_t* info, void* ucontext)
        frame->rsi = reinterpret_cast<uint64_t>(&sf->info);
        frame->rdx = reinterpret_cast<uint64_t>(&sf->uc);
    }

    // The handler's mask remains active until rt_sigreturn restores returnMask.
    restoreMask.active = false;
    return syscallResult; // RAX value (handler ignores it, saved in ucontext)
}
