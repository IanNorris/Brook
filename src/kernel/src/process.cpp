// process.cpp -- Process creation, fd table, user stack setup.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process.h"
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
#include "ext2_vfs.h"
#include "string.h"
#include "spinlock.h"

namespace brook {

// ProcessCurrent() is now in scheduler.cpp (g_currentProcess lives there).

// Helper: free a Process* and clear its magic so any stale pointer to this
// page that survives in the scheduler / wakeup lists fails its magic-check
// at next deref instead of corrupting the run.
static inline void FreeProcessStruct(Process* p)
{
    if (p) p->magic = 0;
    kfree(p);
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
    auto* raw = reinterpret_cast<uint8_t*>(table);
    for (uint64_t i = 0; i < sizeof(FdEntry) * MAX_FDS; ++i) raw[i] = 0;
    return table;
}

int FdAlloc(Process* proc, FdType type, void* handle)
{
    if (!proc || !proc->fds) return -24; // EMFILE
    // fd 0-2 are reserved (stdin/stdout/stderr), start searching from 3
    for (uint32_t i = 0; i < MAX_FDS; ++i)
    {
        if (proc->fds[i].type == FdType::None)
        {
            proc->fds[i].type     = type;
            proc->fds[i].flags    = 0;
            proc->fds[i].refCount = 1;
            proc->fds[i].handle   = handle;
            proc->fds[i].seekPos  = 0;
            proc->fds[i].dirPath[0] = '\0';
            return static_cast<int>(i);
        }
    }
    SerialPrintf("FD: pid %u exhausted %u fds\n", proc->pid, MAX_FDS);
    return -24; // EMFILE
}

void FdFree(Process* proc, int fd)
{
    if (!proc || !proc->fds) return;
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return;
    proc->fds[fd].type     = FdType::None;
    proc->fds[fd].handle   = nullptr;
    proc->fds[fd].refCount = 0;
    proc->fds[fd].seekPos  = 0;
}

FdEntry* FdGet(Process* proc, int fd)
{
    if (!proc || !proc->fds) return nullptr;
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return nullptr;
    if (proc->fds[fd].type == FdType::None) return nullptr;
    return &proc->fds[fd];
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

    // 1. Push 16 random bytes for AT_RANDOM
    uint64_t randSeed = 0xDEADBEEFCAFEBABEULL;
    randSeed = SimpleRand(randSeed);
    pushU64(randSeed);
    randSeed = SimpleRand(randSeed);
    uint64_t randomAddr = pushU64(randSeed);

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
    static constexpr int MAX_ENV = 64;
    uint64_t envAddrs[MAX_ENV] = {};
    for (int i = 0; i < envc && i < MAX_ENV; ++i)
    {
        uint64_t len = 0;
        while (envp[i][len]) ++len;
        envAddrs[i] = pushBytes(envp[i], len + 1);
    }

    // 3. Push argument strings and record pointers
    uint64_t argAddrs[32] = {};
    for (int i = 0; i < argc && i < 32; ++i)
    {
        uint64_t len = 0;
        while (argv[i][len]) ++len;
        argAddrs[i] = pushBytes(argv[i], len + 1);
    }

    // 4. Align to 16 bytes
    sp &= ~0xFULL;

    // Compute total 8-byte slots to be pushed below, and pad so final RSP
    // is 16-byte aligned (Linux ABI: RSP mod 16 == 0 at process entry).
    //   auxv: 17 entries × 2 slots = 34
    //   envp: (envc + 1) slots (including NULL terminator)
    //   argv: (argc + 1) slots (including NULL terminator)
    //   argc: 1 slot
    int envcClamped = (envc > MAX_ENV) ? MAX_ENV : envc;
    int totalSlots = 34 + (envcClamped + 1) + (argc + 1) + 1;
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

    // 6. Push envp array (null-terminated). envcClamped already capped to MAX_ENV.
    pushU64(0); // envp[envc] = NULL
    for (int i = envcClamped - 1; i >= 0; --i)
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

    DbgPrintf("INTERP: loading '%s' for pid %u\n",
                 proc->elf.interpPath, proc->pid);

    // Try the exact path first.
    Vnode* vn = VfsOpen(proc->elf.interpPath);

    // Fallback: try /boot prefix.
    if (!vn)
    {
        char bootPath[256] = "/boot";
        uint32_t bi = 5;
        const char* p = proc->elf.interpPath;
        while (*p && bi + 1 < sizeof(bootPath)) bootPath[bi++] = *p++;
        bootPath[bi] = '\0';
        vn = VfsOpen(bootPath);
    }

    // Fallback: extract filename and look in /boot/lib/.
    if (!vn)
    {
        const char* fname = proc->elf.interpPath;
        for (const char* p = proc->elf.interpPath; *p; ++p)
            if (*p == '/') fname = p + 1;

        char libPath[256] = "/boot/lib/";
        uint32_t li = 10;
        while (*fname && li + 1 < sizeof(libPath)) libPath[li++] = *fname++;
        libPath[li] = '\0';

        SerialPrintf("INTERP: trying '%s'\n", libPath);
        vn = VfsOpen(libPath);
    }

    if (!vn)
    {
        SerialPrintf("INTERP: failed to open '%s'\n", proc->elf.interpPath);
        return 0;
    }

    VnodeStat st;
    if (VfsStat(vn, &st) != 0 || st.size == 0)
    {
        SerialPrintf("INTERP: failed to stat '%s'\n", proc->elf.interpPath);
        VfsClose(vn);
        return 0;
    }

    auto* buf = static_cast<uint8_t*>(kmalloc(st.size));
    if (!buf)
    {
        SerialPrintf("INTERP: OOM for %lu bytes\n", st.size);
        VfsClose(vn);
        return 0;
    }

    uint64_t off = 0;
    int rd = VfsRead(vn, buf, st.size, &off);
    VfsClose(vn);

    if (rd < 0 || static_cast<uint64_t>(rd) != st.size)
    {
        SerialPrintf("INTERP: read error (%d/%lu)\n", rd, st.size);
        kfree(buf);
        return 0;
    }

    uint64_t entry = ElfLoadAt(buf, st.size, INTERP_LOAD_BASE,
                               proc->pageTable, proc->pid);
    kfree(buf);

    if (!entry)
        SerialPrintf("INTERP: ElfLoadAt failed for '%s'\n", proc->elf.interpPath);

    return entry;
}

Process* ProcessCreate(const uint8_t* elfData, uint64_t elfSize,
                       int argc, const char* const* argv,
                       int envc, const char* const* envp,
                       const FdEntry* stdFds)
{
    auto* proc = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!proc) return nullptr;

    // Zero-initialize
    auto* raw = reinterpret_cast<uint8_t*>(proc);
    for (uint64_t i = 0; i < sizeof(Process); ++i) raw[i] = 0;
    proc->magic = PROCESS_MAGIC;

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
    proc->schedPriority = 2;  // SCHED_PRIORITY_NORMAL

    // Allocate shared fd table (shared across CLONE_FILES threads).
    proc->fds = AllocFdTable();
    if (!proc->fds)
    {
        SerialPuts("PROC: fd table allocation failed\n");
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
        FreeProcessStruct(proc);
        return nullptr;
    }

    // Load ELF binary into the process's page table (no CR3 switch needed —
    // ElfLoad writes via the direct physical map).
    if (!ElfLoad(elfData, elfSize, &proc->elf, proc->pageTable, proc->pid))
    {
        SerialPuts("PROC: ELF load failed\n");
        VmmDestroyUserPageTable(proc->pageTable);
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

    proc->programBreak = proc->elf.programBreakLow;
    proc->mmapNext = USER_MMAP_BASE;

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
        PhysicalAddress phys = PmmAllocPage(MemTag::User, proc->pid);
        if (!phys || !VmmMapPage(proc->pageTable, vaddr, phys,
                                  VMM_WRITABLE | VMM_USER,
                                  MemTag::User, proc->pid))
        {
            stackOk = false;
            break;
        }
        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        for (uint64_t b = 0; b < 4096; b++) p[b] = 0;
    }
    if (!stackOk)
    {
        SerialPuts("PROC: stack allocation failed\n");
        PmmKillPid(proc->pid);
        VmmDestroyUserPageTable(proc->pageTable);
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
        proc->fds[0].refCount = 1;
        proc->fds[1].type = FdType::Vnode; // treated as serial stdout in syscall
        proc->fds[1].refCount = 1;
        proc->fds[1].statusFlags = 1; // O_WRONLY
        proc->fds[2].type = FdType::Vnode; // treated as serial stderr in syscall
        proc->fds[2].refCount = 1;
        proc->fds[2].statusFlags = 2; // O_RDWR
    }
    // fd 3 = debug serial (hardcoded in sys_write); reserve it
    proc->fds[3].type = FdType::DevNull;
    proc->fds[3].refCount = 1;
    proc->fds[3].statusFlags = 1; // O_WRONLY

    // Default TTY mode: canonical with echo
    proc->ttyCanonical = true;
    proc->ttyEcho = true;
    proc->straceEnabled = false;

    // Build user stack with argc/argv/envp/auxv
    uint64_t userSP = SetupUserStack(proc, argc, argv, envc, envp, interpBase);

    // Store the final SP for SwitchToUserMode
    proc->stackTop = userSP;

    // Set up TLS if the ELF has a PT_TLS segment
    if (proc->elf.tlsTotalSize > 0)
    {
        // Allocate TLS block at a user-space virtual address.
        // Place it just below the stack guard page.
        uint64_t tlsPages = (proc->elf.tlsTotalSize + 64 + 4095) / 4096;
        uint64_t tlsBase = stackVirtBase - guardPages * 4096 - tlsPages * 4096;

        // Helper: translate user vaddr to kernel pointer via direct map
        auto tlsToKernel = [&](uint64_t userAddr) -> uint8_t* {
            PhysicalAddress phys = VmmVirtToPhys(proc->pageTable, VirtualAddress(userAddr));
            return phys ? reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw()) : nullptr;
        };

        bool tlsOk = true;
        for (uint64_t i = 0; i < tlsPages; i++)
        {
            VirtualAddress vaddr(tlsBase + i * 4096);
            PhysicalAddress phys = PmmAllocPage(MemTag::User, proc->pid);
            if (!phys || !VmmMapPage(proc->pageTable, vaddr, phys,
                                      VMM_WRITABLE | VMM_USER,
                                      MemTag::User, proc->pid))
            {
                tlsOk = false;
                break;
            }
            auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
            for (uint64_t b = 0; b < 4096; b++) p[b] = 0;
        }

        if (tlsOk)
        {
            // Copy initial TLS data via direct map.
            // tlsInitData points to user vaddr in the loaded ELF image.
            if (proc->elf.tlsInitData && proc->elf.tlsInitSize > 0)
            {
                for (uint64_t i = 0; i < proc->elf.tlsInitSize; ++i)
                {
                    uint8_t* src = tlsToKernel(
                        reinterpret_cast<uint64_t>(proc->elf.tlsInitData) + i);
                    uint8_t* dst = tlsToKernel(tlsBase + i);
                    if (src && dst) *dst = *src;
                }
            }

            // TCB pointer: variant II (x86-64), FS:0 = pointer to self
            uint64_t tcbAddr = tlsBase + proc->elf.tlsTotalSize;
            tcbAddr = (tcbAddr + 15) & ~15ULL;

            // Write self-pointer via direct map
            auto* tcbSlot = reinterpret_cast<uint64_t*>(tlsToKernel(tcbAddr));
            if (tcbSlot) *tcbSlot = tcbAddr; // Self-pointer (user vaddr)

            // Stack canary at offset 40 (0x28) from FS base
            uint64_t canary = 0x57a0000012345678ULL;
            if (tcbAddr + 48 < tlsBase + tlsPages * 4096)
            {
                auto* canarySlot = reinterpret_cast<uint64_t*>(
                    tlsToKernel(tcbAddr + 0x28));
                if (canarySlot) *canarySlot = canary;
            }

            proc->fsBase = tcbAddr;
        }
    }

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
    for (uint64_t i = 0; i < sizeof(Process); ++i) raw[i] = 0;
    proc->magic = PROCESS_MAGIC;

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
    FdEntry& fde = proc->fds[i];

    if (fde.type == FdType::Vnode && fde.handle)
    {
        auto* vn = static_cast<Vnode*>(fde.handle);
        uint32_t prev = __atomic_fetch_sub(&vn->refCount, 1, __ATOMIC_ACQ_REL);
        if (prev <= 1)
            VfsClose(vn);
    }

    if (fde.type == FdType::Socket && fde.handle)
    {
        int sockIdx = static_cast<int>(
            reinterpret_cast<uintptr_t>(fde.handle)) - 1;
        brook::SockUnref(sockIdx);
    }

    if (fde.type == FdType::Pipe && fde.handle)
    {
        auto* pipe = static_cast<PipeBuffer*>(fde.handle);
        if (fde.flags & 1) // write end
        {
            __atomic_fetch_sub(&pipe->writers, 1, __ATOMIC_RELEASE);
            Process* reader = pipe->readerWaiter;
            if (reader)
            {
                pipe->readerWaiter = nullptr;
                __atomic_store_n(&reader->pendingWakeup, 1, __ATOMIC_RELEASE);
                SchedulerUnblock(reader);
            }
        }
        else // read end
        {
            __atomic_fetch_sub(&pipe->readers, 1, __ATOMIC_RELEASE);
            Process* writer = pipe->writerWaiter;
            if (writer)
            {
                pipe->writerWaiter = nullptr;
                __atomic_store_n(&writer->pendingWakeup, 1, __ATOMIC_RELEASE);
                SchedulerUnblock(writer);
            }
        }

        if (__atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0 &&
            __atomic_load_n(&pipe->writers, __ATOMIC_ACQUIRE) == 0)
        {
            kfree(pipe);
        }
    }

    FdFree(proc, static_cast<int>(i));
}

// Close all FDs with FD_CLOEXEC set — called by ProcessExec (execve semantics).
void ProcessCloseCloexecFds(Process* proc)
{
    if (!proc) return;
    for (uint32_t i = 0; i < MAX_FDS; ++i)
    {
        FdEntry& fde = proc->fds[i];
        if (fde.type == FdType::None) continue;
        if (!(fde.fdFlags & 1)) continue; // not FD_CLOEXEC
        CloseFdEntry(proc, i);
    }
}

void ProcessCloseAllFds(Process* proc)
{
    if (!proc) return;

    for (uint32_t i = 0; i < MAX_FDS; ++i)
    {
        FdEntry& fde = proc->fds[i];
        if (fde.type == FdType::None) continue;
        CloseFdEntry(proc, i);
    }
}

void ProcessDestroy(Process* proc)
{
    if (!proc) return;

    bool isThread = proc->isThread;

    // Release any kernel mutexes held by this process (prevents deadlock)
    Ext2ForceUnlockForPid(proc->pid);

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

    // Free per-process kernel stack (each thread has its own)
    if (proc->kernelStackBase)
        VmmFreeKernelStack(VirtualAddress(proc->kernelStackBase), KERNEL_STACK_PAGES);

    // Threads share the page table with the leader — don't destroy it
    if (!proc->isKernelThread && !isThread)
    {
        // Unmap MemFd mmap ranges first — their physical pages belong to
        // kernel heap buffers, not to this process. VmmDestroyUserPageTable
        // would otherwise PmmUnrefPage them and potentially free live heap.
        for (uint32_t i = 0; i < Process::MAX_MEMFD_MAPS; i++) {
            auto& m = proc->memfdMaps[i];
            if (m.length == 0) continue;
            uint64_t pages = (m.length + 4095) / 4096;
            for (uint64_t p = 0; p < pages; p++)
                VmmUnmapPage(proc->pageTable, VirtualAddress(m.vaddr + p * 4096));
            if (m.mfd) brook::MemFdHandleUnref(m.mfd);
            m.vaddr = 0; m.length = 0; m.mfd = nullptr;
        }
        VmmDestroyUserPageTable(proc->pageTable);
    }

    // Only the leader owns compositor/window state
    if (!isThread)
    {
        CompositorUnregisterProcess(proc);
        WmDestroyWindowForProcess(proc);
        if (proc->fbVfbWidth > 0)
            CompositorWaitFrame();
    }

    // Only free user pages for the leader process
    if (!isThread)
        VmmKillPid(proc->pid);

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

    FreeProcessStruct(proc);
}

// ---------------------------------------------------------------------------
// ProcessFork -- create a child process that is a copy of the parent.
// ---------------------------------------------------------------------------
// Copies the entire user-space address space (full copy, no CoW).
// The child inherits open file descriptors (shallow copy).
// The child's trampoline returns to user mode with RAX=0 (fork return value).

// Helper: walk a 4-level page table and copy all leaf pages.
static bool ForkCopyUserPages(PageTable srcPt, PageTable dstPt,
                              uint16_t srcPid, uint16_t dstPid)
{
    static constexpr uint64_t PTE_PHYS_MASK = 0x000FFFFFFFFFF000ULL;

    auto* srcPml4 = reinterpret_cast<uint64_t*>(
        PhysToVirt(srcPt.pml4).raw());

    [[maybe_unused]] uint64_t sharedCount = 0;

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
                    bool wasWritable = (pteFlags & VMM_WRITABLE) != 0;
                    bool alreadyCow  = (pteFlags & PTE_COW_BIT) != 0;
                    bool needsCow    = wasWritable || alreadyCow;

                    if (wasWritable)
                    {
                        // First COW: clear writable in parent, set COW bit
                        srcPt4[i1] = (srcPt4[i1] & ~VMM_WRITABLE) | PTE_COW_BIT;
                    }

                    // Build child PTE: same physical page, same flags
                    // (read-only if COW, original flags if already RO)
                    uint64_t childPte = (srcPhys.raw() & PTE_PHYS_MASK)
                                      | VMM_PRESENT
                                      | (pteFlags & VMM_USER)
                                      | (pteFlags & VMM_NO_EXEC);
                    if (needsCow)
                        childPte |= PTE_COW_BIT; // mark as COW in child
                    else if (pteFlags & VMM_WRITABLE)
                        childPte |= VMM_WRITABLE; // genuinely read-write non-COW
                    // Preserve tag bits
                    childPte |= (pteFlags & PTE_TAG_MASK);
                    // Set child PID
                    childPte |= (((uint64_t)dstPid & 0x3FF) << PTE_PID_SHIFT);

                    // Map into child's page table via WalkToPtr (need intermediate tables)
                    // Use VmmMapPage to create intermediates, then override leaf PTE.
                    uint64_t childMapFlags = VMM_USER;
                    if (!needsCow && (pteFlags & VMM_WRITABLE))
                        childMapFlags |= VMM_WRITABLE;
                    if (pteFlags & VMM_NO_EXEC)
                        childMapFlags |= VMM_NO_EXEC;

                    // We need to create intermediate page table entries.
                    // VmmMapPage will create them, then we overwrite the leaf PTE.
                    if (!VmmMapPage(dstPt, VirtualAddress(vaddr), srcPhys,
                                    childMapFlags, MemTag::User, dstPid))
                    {
                        SerialPrintf("FORK: failed to map COW page at vaddr 0x%lx\n", vaddr);
                        return false;
                    }

                    // Now overwrite the child's leaf PTE with our carefully constructed one
                    // (VmmMapPage set it, but we need COW bit + read-only)
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

                    // Increment physical page refcount for sharing
                    PmmRefPage(srcPhys);

                    sharedCount++;
                }
            }
        }
    }

    // Flush TLB for parent (we changed its PTEs to read-only)
    // Full TLB flush by reloading CR3
    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    DbgPrintf("FORK: COW shared %lu pages (parent PID %u -> child PID %u)\n",
                 sharedCount, static_cast<uint32_t>(srcPid), static_cast<uint32_t>(dstPid));
    return true;
}

Process* ProcessFork(Process* parent, uint64_t userRip,
                     uint64_t userRsp, uint64_t userRflags)
{
    if (!parent || parent->isKernelThread) return nullptr;

    auto* child = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!child) return nullptr;

    // Copy entire parent process struct as a starting point
    auto* rawDst = reinterpret_cast<uint8_t*>(child);
    auto* rawSrc = reinterpret_cast<const uint8_t*>(parent);
    for (uint64_t i = 0; i < sizeof(Process); i++) rawDst[i] = rawSrc[i];
    child->magic = PROCESS_MAGIC;

    // Allocate new PID
    child->pid = SchedulerAllocPid();
    child->parentPid = parent->pid;
    child->pgid = parent->pgid;      // Inherit process group from parent
    child->sid = parent->sid;         // Inherit session from parent
    child->tgid = child->pid;         // Fork creates a new thread group
    child->threadLeader = child;
    child->isThread = false;
    child->clearChildTid = 0;
    child->parentSetTid = 0;
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
        FreeProcessStruct(child);
        return nullptr;
    }

    // Reset scheduler state
    child->state = ProcessState::Ready;
    child->runningOnCpu = -1;
    child->reapable = false;
    child->compositorRegistered = false;
    child->schedNext = nullptr;
    child->schedPrev = nullptr;
    child->inReadyQueue = 0;
    child->wakeupTick = 0;
    child->syncNext = nullptr;
    child->pendingWakeup = 0;

    // Set fork-child trampoline state
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
        FreeProcessStruct(child);
        return nullptr;
    }

    // COW-share all user-space pages from parent to child
    if (!ForkCopyUserPages(parent->pageTable, child->pageTable,
                           parent->pid, child->pid))
    {
        SerialPuts("FORK: address space copy failed\n");
        VmmDestroyUserPageTable(child->pageTable);
        VmmFreeKernelStack(kstackAddr, KERNEL_STACK_PAGES);
        FreeProcessStruct(child);
        return nullptr;
    }

    // Duplicate file descriptors (shallow copy — share VFS handles)
    // fd 0/1/2 (keyboard/serial) don't need refcounting.
    // VFS Vnodes: we don't increment refcount currently (single-owner model).
    // This is acceptable for now — child gets independent seek positions.
    for (uint32_t i = 0; i < MAX_FDS; i++)
    {
        if (parent->fds[i].type != FdType::None)
        {
            child->fds[i] = parent->fds[i];

            // Increment pipe reader/writer counts for the child's copy
            if (parent->fds[i].type == FdType::Pipe && parent->fds[i].handle)
            {
                auto* pipe = static_cast<PipeBuffer*>(parent->fds[i].handle);
                if (parent->fds[i].flags & 1)
                    __atomic_fetch_add(&pipe->writers, 1, __ATOMIC_RELEASE);
                else
                    __atomic_fetch_add(&pipe->readers, 1, __ATOMIC_RELEASE);
            }

            // Increment Vnode refcount for the child's copy
            if (parent->fds[i].type == FdType::Vnode && parent->fds[i].handle)
            {
                auto* vn = static_cast<Vnode*>(parent->fds[i].handle);
                __atomic_fetch_add(&vn->refCount, 1, __ATOMIC_RELEASE);
            }

            // Increment socket refcount for the child's copy
            if (parent->fds[i].type == FdType::Socket && parent->fds[i].handle)
            {
                int sockIdx = static_cast<int>(
                    reinterpret_cast<uintptr_t>(parent->fds[i].handle)) - 1;
                brook::SockRef(sockIdx);
            }

            // Increment memfd refcount for the child's copy (shared buffer).
            if (parent->fds[i].type == FdType::MemFd && parent->fds[i].handle)
                brook::MemFdHandleRef(parent->fds[i].handle);

            // Increment unix socket refcount for the child's copy
            if (parent->fds[i].type == FdType::UnixSocket && parent->fds[i].handle)
                UnixSocketHandleRef(parent->fds[i].handle);
        }
    }

    // Update child's TLS fsBase to point to the new address space's TLS
    // (same virtual address, but backed by the child's physical pages)
    // No change needed — the virtual address is the same and the page table
    // now maps it to the child's copy.

    // Inherit terminal mode from parent
    child->ttyCanonical = parent->ttyCanonical;
    child->ttyEcho = parent->ttyEcho;
    child->straceEnabled = parent->straceEnabled;

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
    auto* rawDst = reinterpret_cast<uint8_t*>(thread);
    auto* rawSrc = reinterpret_cast<const uint8_t*>(parent);
    for (uint64_t i = 0; i < sizeof(Process); i++) rawDst[i] = rawSrc[i];
    thread->magic = PROCESS_MAGIC;

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
    thread->reapable = false;
    thread->compositorRegistered = false;
    thread->schedNext = nullptr;
    thread->schedPrev = nullptr;
    thread->inReadyQueue = 0;
    thread->wakeupTick = 0;
    thread->syncNext = nullptr;
    thread->pendingWakeup = 0;

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
    for (uint32_t i = 0; i < 512; ++i) thread->fxsave.data[i] = 0;
    thread->fxsave.data[0] = 0x7F;
    thread->fxsave.data[1] = 0x03;
    thread->fxsave.data[24] = 0x80;
    thread->fxsave.data[25] = 0x1F;

    // Allocate per-thread kernel stack
    VirtualAddress kstackAddr = VmmAllocKernelStack(KERNEL_STACK_PAGES,
        MemTag::KernelData, thread->pid);
    if (!kstackAddr)
    {
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

    // Threads get a unique pid (TID) but share the process group's signal
    // handlers. The g_sigHandlers table is per-pid, so copy the parent's
    // slot into the thread's slot. Without this the thread's slot contains
    // whatever stale data was left by a previous process with the same pid.
    for (int s = 0; s < 64; ++s)
        g_sigHandlers[thread->tgid][s] = g_sigHandlers[parent->tgid][s];

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

    DbgPrintf("THREAD: parent pid=%u -> thread tid=%u tgid=%u, rip=0x%lx rsp=0x%lx tls=0x%lx\n",
                 parent->pid, thread->pid, thread->tgid, userRip, userRsp, tlsBase);

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
        uint64_t lf = SpinLockAcquire(&s_remoteStackLock);
        stackBase = leader->mmapNext;
        if (stackBase + stackSize > USER_MMAP_END) {
            SpinLockRelease(&s_remoteStackLock, lf);
            SerialPrintf("CRT: out of user VA (pid %u)\n", target->pid);
            return 0;
        }
        leader->mmapNext = stackBase + stackSize;
        SpinLockRelease(&s_remoteStackLock, lf);
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
            for (uint32_t b = 0; b < thisPage; ++b) dst[b] = src[b];
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

    // Save the old page table and switch CR3 to the kernel's page table
    // BEFORE destroying the old one. This avoids a use-after-free: the
    // CPU's CR3 would point to a freed PML4 page otherwise.
    PageTable oldPt = proc->pageTable;
    PageTable kernelPt = VmmKernelCR3();
    __asm__ volatile("mov %0, %%cr3" : : "r"(kernelPt.pml4.raw()) : "memory");

    // 1. Free all user-space pages and destroy old page table.
    VmmDestroyUserPageTable(oldPt);
    PmmFreeByTag(proc->pid, MemTag::User);

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
    proc->mmapNext = USER_MMAP_BASE;

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
        PhysicalAddress phys = PmmAllocPage(MemTag::User, proc->pid);
        if (!phys || !VmmMapPage(proc->pageTable, vaddr, phys,
                                  VMM_WRITABLE | VMM_USER,
                                  MemTag::User, proc->pid))
        {
            SerialPuts("EXEC: stack allocation failed\n");
            return 0;
        }
        auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        for (uint64_t b = 0; b < 4096; b++) p[b] = 0;
    }
    proc->stackBase = stackVirtBase + guardPages * 4096;
    proc->stackTop  = stackVirtTop - 8;

    // 5. Build user stack with argc/argv/envp/auxv
    uint64_t userSP = SetupUserStack(proc, argc, argv, envc, envp, interpBase);
    proc->stackTop = userSP;

    // 6. Set up TLS
    proc->fsBase = 0;
    if (proc->elf.tlsTotalSize > 0)
    {
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
            PhysicalAddress phys = PmmAllocPage(MemTag::User, proc->pid);
            if (!phys || !VmmMapPage(proc->pageTable, vaddr, phys,
                                      VMM_WRITABLE | VMM_USER,
                                      MemTag::User, proc->pid))
            {
                tlsOk = false;
                break;
            }
            auto* p = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
            for (uint64_t b = 0; b < 4096; b++) p[b] = 0;
        }

        if (tlsOk)
        {
            if (proc->elf.tlsInitData && proc->elf.tlsInitSize > 0)
            {
                for (uint64_t i = 0; i < proc->elf.tlsInitSize; ++i)
                {
                    uint8_t* src = tlsToKernel(
                        reinterpret_cast<uint64_t>(proc->elf.tlsInitData) + i);
                    uint8_t* dst = tlsToKernel(tlsBase + i);
                    if (src && dst) *dst = *src;
                }
            }

            uint64_t tcbAddr = tlsBase + proc->elf.tlsTotalSize;
            tcbAddr = (tcbAddr + 15) & ~15ULL;

            auto* tcbSlot = reinterpret_cast<uint64_t*>(tlsToKernel(tcbAddr));
            if (tcbSlot) *tcbSlot = tcbAddr;

            uint64_t canary = 0x57a0000012345678ULL;
            if (tcbAddr + 48 < tlsBase + tlsPages * 4096)
            {
                auto* canarySlot = reinterpret_cast<uint64_t*>(
                    tlsToKernel(tcbAddr + 0x28));
                if (canarySlot) *canarySlot = canary;
            }

            proc->fsBase = tcbAddr;
        }
    }

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
        // Immediately terminate
        proc->exitStatus = 128 + SIGKILL;
        proc->fbVirtual = nullptr;
        proc->fbVirtualSize = 0;
        proc->state = ProcessState::Terminated;
        if (!__atomic_load_n(&proc->compositorRegistered, __ATOMIC_ACQUIRE))
            proc->reapable = true;
        DbgPrintf("SIGNAL: SIGKILL -> pid %u\n", proc->pid);
        return 0;
    }

    if (signum == SIGSTOP)
    {
        // Immediately stop (cannot be caught/blocked)
        proc->state = ProcessState::Stopped;
        DbgPrintf("SIGNAL: SIGSTOP -> pid %u stopped (default)\n", proc->pid);
        return 0;
    }

    // SIGCONT: resume stopped process (always, even if blocked/ignored)
    static constexpr int SIGCONT = 18;
    if (signum == SIGCONT)
    {
        if (proc->state == ProcessState::Stopped)
        {
            proc->stopReported = false;
            SchedulerUnblock(proc);
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
            proc->state = ProcessState::Stopped;
            DbgPrintf("SIGNAL: sig %d -> pid %u stopped (default)\n", signum, proc->pid);
            // Wake parent for waitpid WUNTRACED
            Process* parent = ProcessFindByPid(proc->parentPid);
            if (parent)
                ProcessSendSignal(parent, 17); // SIGCHLD
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

extern "C" int64_t SyscallCheckSignals(SyscallFrame* frame, int64_t syscallResult)
{
    Process* proc = ProcessCurrent();
    if (!proc || proc->isKernelThread) return syscallResult;

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

        return static_cast<int64_t>(mc.rax); // restore original RAX
    }

    // --- Signal delivery ---
    if (proc->inSignalHandler) return syscallResult;

    uint64_t deliverable = proc->sigPending & ~proc->sigMask;
    if (deliverable == 0) return syscallResult;

    // Find lowest pending signal
    int signum = __builtin_ctzll(deliverable) + 1;

    // Clear from pending
    __atomic_and_fetch(&proc->sigPending, ~(1ULL << (signum - 1)), __ATOMIC_RELEASE);

    uint16_t pid = proc->tgid;
    if (pid >= MAX_PROCESSES) return syscallResult;

    KernelSigaction& sa = g_sigHandlers[pid][signum - 1];

    // SIG_DFL (0) — default action
    if (sa.handler == 0)
    {
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
            // Use SchedulerExitCurrentProcess instead of setting state+returning.
            // The old approach returned to user space with state=Terminated; the
            // compositor could then mark the process reapable and call ProcessDestroy
            // (freeing the kernel stack) while the process was still live and about
            // to re-enter the syscall dispatcher — causing a use-after-free of the
            // kernel stack and a double fault.
            SchedulerExitCurrentProcess(128 + signum);
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
            proc->state = ProcessState::Stopped;
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

    proc->inSignalHandler = true;

    // Save current signal mask, then block signals specified by sa_mask + this signal
    proc->sigSavedMask = proc->sigMask;
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
    userRsp &= ~0xFULL;                   // 16-byte align

    auto* sf = reinterpret_cast<SignalFrame*>(userRsp);

    // Clear the frame
    for (uint64_t i = 0; i < sizeof(SignalFrame) / 8; i++)
        reinterpret_cast<uint64_t*>(sf)[i] = 0;

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

    // Fill siginfo
    sf->info.si_signo = signum;
    sf->info.si_code  = 0;      // SI_USER

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

    return syscallResult; // RAX value (handler ignores it, saved in ucontext)
}
