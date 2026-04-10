// process.cpp -- Process creation, fd table, user stack setup.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "serial.h"
#include "kprintf.h"
#include "string.h"

namespace brook {

// ---------------------------------------------------------------------------
// Global current process (single-process for now)
// ---------------------------------------------------------------------------

static Process* g_currentProcess = nullptr;

Process* ProcessCurrent()
{
    return g_currentProcess;
}

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

    // Helper: push bytes onto the stack
    auto pushBytes = [&](const void* data, uint64_t len) -> uint64_t {
        sp -= len;
        auto* dst = reinterpret_cast<uint8_t*>(sp);
        const auto* src = reinterpret_cast<const uint8_t*>(data);
        for (uint64_t i = 0; i < len; ++i) dst[i] = src[i];
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
bool ElfLoad(const uint8_t* data, uint64_t size, ElfBinary* out);

Process* ProcessCreate(const uint8_t* elfData, uint64_t elfSize,
                       int argc, const char* const* argv,
                       int envc, const char* const* envp)
{
    auto* proc = static_cast<Process*>(kmalloc(sizeof(Process)));
    if (!proc) return nullptr;

    // Zero-initialize
    auto* raw = reinterpret_cast<uint8_t*>(proc);
    for (uint64_t i = 0; i < sizeof(Process); ++i) raw[i] = 0;

    proc->pid = 1; // First user process

    // Load ELF binary
    if (!ElfLoad(elfData, elfSize, &proc->elf))
    {
        SerialPuts("PROC: ELF load failed\n");
        kfree(proc);
        return nullptr;
    }

    proc->programBreak = proc->elf.programBreakLow;

    // Allocate user stack
    uint64_t stackPages = USER_STACK_SIZE / 4096;
    uint64_t guardPages = 1;
    uint64_t stackBase = VmmAllocPages(stackPages + guardPages,
                                        VMM_WRITABLE | VMM_USER,
                                        MemTag::User, proc->pid);
    if (!stackBase)
    {
        SerialPuts("PROC: stack allocation failed\n");
        kfree(proc);
        return nullptr;
    }

    // Guard page at the bottom
    VmmUnmapPage(stackBase);

    proc->stackBase = stackBase + guardPages * 4096;
    proc->stackTop  = stackBase + (stackPages + guardPages) * 4096 - 8;

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
        // Allocate TLS block: variant II (x86-64), FS:0 points to the TCB
        // at the END of the TLS block.
        uint64_t tlsPages = (proc->elf.tlsTotalSize + 64 + 4095) / 4096;
        uint64_t tlsBase = VmmAllocPages(tlsPages, VMM_WRITABLE | VMM_USER,
                                          MemTag::User, proc->pid);
        if (tlsBase)
        {
            uint8_t* tlsMem = reinterpret_cast<uint8_t*>(tlsBase);
            // Zero the whole region
            for (uint64_t i = 0; i < tlsPages * 4096; ++i)
                tlsMem[i] = 0;

            // Copy initial TLS data
            if (proc->elf.tlsInitData && proc->elf.tlsInitSize > 0)
            {
                for (uint64_t i = 0; i < proc->elf.tlsInitSize; ++i)
                    tlsMem[i] = proc->elf.tlsInitData[i];
            }

            // TCB pointer: variant II, FS:0 = pointer to self
            // Place TCB at end of TLS block
            uint64_t tcbAddr = tlsBase + proc->elf.tlsTotalSize;
            // Align up to 16 bytes
            tcbAddr = (tcbAddr + 15) & ~15ULL;
            auto* tcb = reinterpret_cast<uint64_t*>(tcbAddr);
            tcb[0] = tcbAddr; // Self-pointer (required by musl/glibc)

            // Stack canary at offset 40 (0x28) from FS base
            uint64_t canary = 0x57a0000012345678ULL;
            if (tcbAddr + 48 < tlsBase + tlsPages * 4096)
            {
                auto* canarySlot = reinterpret_cast<uint64_t*>(tcbAddr + 0x28);
                *canarySlot = canary;
            }

            proc->fsBase = tcbAddr;
        }
    }

    g_currentProcess = proc;

    SerialPrintf("PROC: created pid=%u, entry=0x%lx, stack=0x%lx, brk=0x%lx\n",
                 proc->pid, proc->elf.entryPoint, proc->stackTop,
                 proc->programBreak);

    return proc;
}

void ProcessDestroy(Process* proc)
{
    if (!proc) return;

    // Close all file descriptors, releasing VFS/device resources
    for (uint32_t i = 0; i < MAX_FDS; ++i)
    {
        FdEntry& fde = proc->fds[i];
        if (fde.type == FdType::None) continue;

        if (fde.type == FdType::Vnode && fde.handle)
            VfsClose(static_cast<Vnode*>(fde.handle));

        FdFree(proc, static_cast<int>(i));
    }

    // Free all VMM page allocations owned by this process
    VmmKillPid(proc->pid);

    if (g_currentProcess == proc)
        g_currentProcess = nullptr;

    kfree(proc);
}

} // namespace brook
