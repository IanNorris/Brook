// process.cpp -- Process creation, fd table, user stack setup.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process.h"
#include "scheduler.h"
#include "vfs.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "serial.h"
#include "kprintf.h"
#include "string.h"

namespace brook {

// ProcessCurrent() is now in scheduler.cpp (g_currentProcess lives there).

// ---------------------------------------------------------------------------
// File descriptor operations
// ---------------------------------------------------------------------------

int FdAlloc(Process* proc, FdType type, void* handle)
{
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
            return static_cast<int>(i);
        }
    }
    return -24; // EMFILE
}

void FdFree(Process* proc, int fd)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return;
    proc->fds[fd].type     = FdType::None;
    proc->fds[fd].handle   = nullptr;
    proc->fds[fd].refCount = 0;
    proc->fds[fd].seekPos  = 0;
}

FdEntry* FdGet(Process* proc, int fd)
{
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
static constexpr uint64_t AT_ENTRY    = 9;
static constexpr uint64_t AT_RANDOM   = 25;

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
                                int envc, const char* const* envp)
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

    // 2. Push environment strings and record pointers
    uint64_t envAddrs[16] = {};
    for (int i = 0; i < envc && i < 16; ++i)
    {
        uint64_t len = 0;
        while (envp[i][len]) ++len;
        envAddrs[i] = pushBytes(envp[i], len + 1);
    }

    // 3. Push argument strings and record pointers
    uint64_t argAddrs[16] = {};
    for (int i = 0; i < argc && i < 16; ++i)
    {
        uint64_t len = 0;
        while (argv[i][len]) ++len;
        argAddrs[i] = pushBytes(argv[i], len + 1);
    }

    // 4. Align to 16 bytes
    sp &= ~0xFULL;

    // 5. Push auxiliary vectors (in reverse order, so first entry is at lowest address)
    pushU64(0); pushU64(AT_NULL);                               // AT_NULL
    pushU64(randomAddr); pushU64(AT_RANDOM);                    // AT_RANDOM
    pushU64(proc->elf.entryPoint); pushU64(AT_ENTRY);           // AT_ENTRY
    pushU64(0); pushU64(AT_BASE);                               // AT_BASE (no interp)
    pushU64(4096); pushU64(AT_PAGESZ);                          // AT_PAGESZ
    pushU64(proc->elf.phdrNum); pushU64(AT_PHNUM);              // AT_PHNUM
    pushU64(proc->elf.phdrEntSize); pushU64(AT_PHENT);          // AT_PHENT
    pushU64(proc->elf.phdrVaddr); pushU64(AT_PHDR);             // AT_PHDR

    // 6. Push envp array (null-terminated)
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

Process* ProcessCreate(const uint8_t* elfData, uint64_t elfSize,
                       int argc, const char* const* argv,
                       int envc, const char* const* envp)
{
    auto* proc = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!proc) return nullptr;

    // Zero-initialize
    auto* raw = reinterpret_cast<uint8_t*>(proc);
    for (uint64_t i = 0; i < sizeof(Process); ++i) raw[i] = 0;

    // Initialize FPU/SSE state with safe defaults so fxrstor works correctly
    // on the first context switch to this process.
    // FCW (bytes 0-1): 0x037F = mask all FPU exceptions
    // MXCSR (bytes 24-27): 0x1F80 = mask all SSE exceptions
    proc->fxsave.data[0] = 0x7F;
    proc->fxsave.data[1] = 0x03;
    proc->fxsave.data[24] = 0x80;
    proc->fxsave.data[25] = 0x1F;

    proc->pid = SchedulerAllocPid();
    proc->state = ProcessState::Ready;

    // Default working directory
    proc->cwd[0] = '/'; proc->cwd[1] = 'b'; proc->cwd[2] = 'o';
    proc->cwd[3] = 'o'; proc->cwd[4] = 't'; proc->cwd[5] = '\0';

    // Allocate per-process kernel stack (for ring 3→0 transitions).
    VirtualAddress kstackAddr = VmmAllocPages(KERNEL_STACK_PAGES,
        VMM_WRITABLE, MemTag::KernelData, proc->pid);
    if (!kstackAddr)
    {
        SerialPuts("PROC: kernel stack allocation failed\n");
        kfree(proc);
        return nullptr;
    }
    proc->kernelStackBase = kstackAddr.raw();
    proc->kernelStackTop  = kstackAddr.raw() + KERNEL_STACK_SIZE;

    // Create per-process page table
    proc->pageTable = VmmCreateUserPageTable();
    if (!proc->pageTable)
    {
        SerialPuts("PROC: page table allocation failed\n");
        kfree(proc);
        return nullptr;
    }

    // Load ELF binary into the process's page table (no CR3 switch needed —
    // ElfLoad writes via the direct physical map).
    if (!ElfLoad(elfData, elfSize, &proc->elf, proc->pageTable, proc->pid))
    {
        SerialPuts("PROC: ELF load failed\n");
        VmmDestroyUserPageTable(proc->pageTable);
        kfree(proc);
        return nullptr;
    }

    proc->programBreak = proc->elf.programBreakLow;
    proc->mmapNext = USER_MMAP_BASE;

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
        kfree(proc);
        return nullptr;
    }
    // Guard page (first page) is left unmapped — faults on stack overflow.
    proc->stackBase = stackVirtBase + guardPages * 4096;
    proc->stackTop  = stackVirtTop - 8;

    // Set up standard file descriptors
    // fd 0 = stdin (TODO: wire to keyboard)
    // fd 1 = stdout (serial)
    // fd 2 = stderr (serial)
    proc->fds[0].type = FdType::DevKeyboard;
    proc->fds[0].refCount = 1;
    proc->fds[1].type = FdType::Vnode; // treated as serial stdout in syscall
    proc->fds[1].refCount = 1;
    proc->fds[2].type = FdType::Vnode; // treated as serial stderr in syscall
    proc->fds[2].refCount = 1;

    // Build user stack with argc/argv/envp/auxv
    uint64_t userSP = SetupUserStack(proc, argc, argv, envc, envp);

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

    SerialPrintf("PROC: created pid=%u, entry=0x%lx, stack=0x%lx, brk=0x%lx, cr3=0x%lx\n",
                 proc->pid, proc->elf.entryPoint, proc->stackTop,
                 proc->programBreak, proc->pageTable.pml4.raw());

    return proc;
}

void ProcessDestroy(Process* proc)
{
    if (!proc) return;

    // Ensure we're on the kernel page table before tearing down
    VmmSwitchPageTable(VmmKernelCR3());

    // Close all file descriptors, releasing VFS/device resources
    for (uint32_t i = 0; i < MAX_FDS; ++i)
    {
        FdEntry& fde = proc->fds[i];
        if (fde.type == FdType::None) continue;

        if (fde.type == FdType::Vnode && fde.handle)
            VfsClose(static_cast<Vnode*>(fde.handle));

        FdFree(proc, static_cast<int>(i));
    }

    // Free per-process kernel stack
    if (proc->kernelStackBase)
        VmmFreePages(VirtualAddress(proc->kernelStackBase), KERNEL_STACK_PAGES);

    // Free all VMM page allocations owned by this process
    VmmKillPid(proc->pid);

    // Free per-process page table pages
    VmmDestroyUserPageTable(proc->pageTable);

    // Remove from scheduler tracking
    SchedulerRemoveProcess(proc);

    kfree(proc);
}

} // namespace brook
