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
    proc->runningOnCpu = -1;
    proc->schedPriority = 2;  // SCHED_PRIORITY_NORMAL

    // Default working directory
    proc->cwd[0] = '/'; proc->cwd[1] = 'b'; proc->cwd[2] = 'o';
    proc->cwd[3] = 'o'; proc->cwd[4] = 't'; proc->cwd[5] = '\0';

    // Allocate per-process kernel stack with guard pages (for ring 3→0 transitions).
    VirtualAddress kstackAddr = VmmAllocKernelStack(KERNEL_STACK_PAGES,
        MemTag::KernelData, proc->pid);
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

    DbgPrintf("PROC: created pid=%u, entry=0x%lx, stack=0x%lx, brk=0x%lx, cr3=0x%lx\n",
                 proc->pid, proc->elf.entryPoint, proc->stackTop,
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

    // Initialize FPU/SSE state
    proc->fxsave.data[0] = 0x7F;
    proc->fxsave.data[1] = 0x03;
    proc->fxsave.data[24] = 0x80;
    proc->fxsave.data[25] = 0x1F;

    proc->pid = SchedulerAllocPid();
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
        kfree(proc);
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

void ProcessDestroy(Process* proc)
{
    if (!proc) return;

    // NOTE: All page table teardown uses the DMAP (direct physical map) for
    // safe access regardless of the current CR3. No CR3 switch needed.

    // Close all file descriptors, releasing VFS/device resources
    for (uint32_t i = 0; i < MAX_FDS; ++i)
    {
        FdEntry& fde = proc->fds[i];
        if (fde.type == FdType::None) continue;

        if (fde.type == FdType::Vnode && fde.handle)
            VfsClose(static_cast<Vnode*>(fde.handle));

        FdFree(proc, static_cast<int>(i));
    }

    // Free per-process kernel stack (includes guard page accounting)
    if (proc->kernelStackBase)
        VmmFreeKernelStack(VirtualAddress(proc->kernelStackBase), KERNEL_STACK_PAGES);

    // Free per-process page table pages FIRST — must happen before PmmKillPid
    // frees the data pages, because freed pages can be immediately reallocated
    // by other CPUs. Walking the page table after data pages are freed is safe
    // (FreeTableLevel only frees intermediate table pages, not leaf data pages),
    // but freed-and-reallocated pages would have wrong descriptors.
    if (!proc->isKernelThread)
        VmmDestroyUserPageTable(proc->pageTable);

    // Now free all VMM page allocations and PMM-tracked pages for this process.
    VmmKillPid(proc->pid);

    // Remove from scheduler tracking
    SchedulerRemoveProcess(proc);

    kfree(proc);
}

// ---------------------------------------------------------------------------
// ProcessFork -- create a child process that is a copy of the parent.
// ---------------------------------------------------------------------------
// Copies the entire user-space address space (full copy, no CoW).
// The child inherits open file descriptors (shallow copy).
// The child's trampoline returns to user mode with RAX=0 (fork return value).

// Helper: walk a 4-level page table and copy all leaf pages.
static bool ForkCopyUserPages(PageTable srcPt, PageTable dstPt, uint16_t dstPid)
{
    static constexpr uint64_t PTE_PHYS_MASK = 0x000FFFFFFFFFF000ULL;

    auto* srcPml4 = reinterpret_cast<uint64_t*>(
        PhysToVirt(srcPt.pml4).raw());

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
                    // Sign-extend if needed (not needed for user half, bits 47:0)

                    PhysicalAddress srcPhys(srcPt4[i1] & PTE_PHYS_MASK);
                    uint64_t flags = srcPt4[i1] & 0xFFF; // low 12 bits = flags

                    // Determine page flags for the child mapping
                    uint64_t mapFlags = 0;
                    if (flags & (1ULL << 1)) mapFlags |= VMM_WRITABLE;
                    if (flags & (1ULL << 2)) mapFlags |= VMM_USER;

                    // Allocate a new physical page for the child
                    PhysicalAddress dstPhys = PmmAllocPage(MemTag::User, dstPid);
                    if (!dstPhys)
                    {
                        SerialPrintf("FORK: OOM copying page at vaddr 0x%lx\n", vaddr);
                        return false;
                    }

                    // Copy page contents via direct map
                    auto* src = reinterpret_cast<uint8_t*>(PhysToVirt(srcPhys).raw());
                    auto* dst = reinterpret_cast<uint8_t*>(PhysToVirt(dstPhys).raw());
                    for (uint64_t b = 0; b < 4096; b += 8)
                        *reinterpret_cast<uint64_t*>(dst + b) =
                            *reinterpret_cast<uint64_t*>(src + b);

                    // Map into child's page table
                    if (!VmmMapPage(dstPt, VirtualAddress(vaddr), dstPhys,
                                    mapFlags, MemTag::User, dstPid))
                    {
                        SerialPrintf("FORK: failed to map page at vaddr 0x%lx\n", vaddr);
                        PmmFreePage(dstPhys);
                        return false;
                    }
                }
            }
        }
    }

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

    // Allocate new PID
    child->pid = SchedulerAllocPid();
    child->parentPid = parent->pid;
    if (child->pid == 0)
    {
        SerialPuts("FORK: PID allocation failed\n");
        kfree(child);
        return nullptr;
    }

    // Reset scheduler state
    child->state = ProcessState::Ready;
    child->runningOnCpu = -1;
    child->reapable = false;
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
        kfree(child);
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
        kfree(child);
        return nullptr;
    }

    // Copy all user-space pages from parent to child (full copy)
    if (!ForkCopyUserPages(parent->pageTable, child->pageTable, child->pid))
    {
        SerialPuts("FORK: address space copy failed\n");
        VmmDestroyUserPageTable(child->pageTable);
        VmmFreeKernelStack(kstackAddr, KERNEL_STACK_PAGES);
        kfree(child);
        return nullptr;
    }

    // Duplicate file descriptors (shallow copy — share VFS handles)
    // fd 0/1/2 (keyboard/serial) don't need refcounting.
    // VFS Vnodes: we don't increment refcount currently (single-owner model).
    // This is acceptable for now — child gets independent seek positions.
    for (uint32_t i = 0; i < MAX_FDS; i++)
    {
        if (parent->fds[i].type == FdType::Vnode && parent->fds[i].handle)
        {
            // Re-open the same file for the child to get independent state
            // For now, just copy the fd entry (shares the Vnode pointer)
            child->fds[i] = parent->fds[i];
        }
    }

    // Update child's TLS fsBase to point to the new address space's TLS
    // (same virtual address, but backed by the child's physical pages)
    // No change needed — the virtual address is the same and the page table
    // now maps it to the child's copy.

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

    SerialPrintf("FORK: parent pid=%u -> child pid=%u '%s', rip=0x%lx rsp=0x%lx\n",
                 parent->pid, child->pid, child->name, userRip, userRsp);

    return child;
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

    // 1. Free all user-space pages and destroy old page table.
    // Only free User-tagged pages — keep kernel stack (KernelData tag) intact.
    VmmDestroyUserPageTable(proc->pageTable);
    PmmFreeByTag(proc->pid, MemTag::User);

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
    uint64_t userSP = SetupUserStack(proc, argc, argv, envc, envp);
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

    // 8. Close O_CLOEXEC fds (we don't track this flag yet, so keep all open)
    // For now, FDs 0/1/2 are preserved (stdin/stdout/stderr).

    *outStackPtr = userSP;
    return proc->elf.entryPoint;
}

} // namespace brook
