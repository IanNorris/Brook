// syscall.cpp -- SYSCALL/SYSRET dispatcher, syscall table, and user-mode entry.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "syscall.h"
#include "cpu.h"
#include "gdt.h"
#include "serial.h"
#include "panic.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"

// Forward declaration
extern "C" __attribute__((naked)) void ReturnToKernel();

// ---------------------------------------------------------------------------
// SYSCALL dispatcher -- naked assembly, pointed to by LSTAR MSR.
// ---------------------------------------------------------------------------

extern "C" __attribute__((naked, used)) void BrookSyscallDispatcher()
{
    __asm__ volatile(
        "push %%rcx\n\t"
        "swapgs\n\t"
        "mov %%rsp, %%rcx\n\t"
        "mov %%gs:8, %%rsp\n\t"
        "and $~0xF, %%rsp\n\t"
        "push %%rcx\n\t"
        "push %%r11\n\t"
        "push %%rbp\n\t"
        "mov %%rsp, %%rbp\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "mov %%r10, %%rcx\n\t"
        "cmp $400, %%rax\n\t"
        "jae .Lsyscall_invalid\n\t"
        "mov %%gs:16, %%r12\n\t"
        "call *(%%r12, %%rax, 8)\n\t"
        "jmp .Lsyscall_return\n\t"
        ".Lsyscall_invalid:\n\t"
        "mov $-38, %%rax\n\t"
        ".Lsyscall_return:\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rbp\n\t"
        "popfq\n\t"
        "pop %%rsp\n\t"
        "swapgs\n\t"
        "pop %%rcx\n\t"
        ".byte 0x48\n\t"
        "sysret\n\t"
        ::: "memory"
    );
}

namespace brook {

// ---------------------------------------------------------------------------
// Error codes (Linux)
// ---------------------------------------------------------------------------

static constexpr int64_t ENOENT  = 2;
static constexpr int64_t EBADF   = 9;
static constexpr int64_t ENOMEM  = 12;
static constexpr int64_t EFAULT  = 14;
static constexpr int64_t EINVAL  = 22;
static constexpr int64_t EMFILE  = 24;
static constexpr int64_t ENOSYS  = 38;

// ---------------------------------------------------------------------------
// sys_write (1)
// ---------------------------------------------------------------------------

static int64_t sys_write(uint64_t fd, uint64_t bufAddr, uint64_t count,
                          uint64_t, uint64_t, uint64_t)
{
    if (fd == 1 || fd == 2)
    {
        const char* buf = reinterpret_cast<const char*>(bufAddr);
        for (uint64_t i = 0; i < count; ++i)
            SerialPutChar(buf[i]);
        return static_cast<int64_t>(count);
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
        if (ret > 0) fde->seekPos += static_cast<uint64_t>(ret);
        return ret;
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
        if (ret > 0) fde->seekPos += static_cast<uint64_t>(ret);
        return ret;
    }

    return -EBADF;
}

// ---------------------------------------------------------------------------
// sys_open (2)
// ---------------------------------------------------------------------------

static int64_t sys_open(uint64_t pathAddr, uint64_t flags, uint64_t mode,
                         uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path) return -EFAULT;

    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    // Special device paths
    // TODO: proper /dev mount, for now hardcode
    // /dev/fb0 and /dev/keyboard handled via FdType

    Vnode* vn = VfsOpen(path, static_cast<uint32_t>(flags));
    if (!vn)
    {
        SerialPrintf("sys_open: not found: %s\n", path);
        return -ENOENT;
    }

    int fd = FdAlloc(proc, FdType::Vnode, vn);
    if (fd < 0)
    {
        VfsClose(vn);
        return -EMFILE;
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
        VfsClose(static_cast<Vnode*>(fde->handle));

    FdFree(proc, static_cast<int>(fd));
    return 0;
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
        // TODO: need file size from VFS
        return -EINVAL;
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
        return static_cast<int64_t>(proc->programBreak);

    // Validate within program break limits
    if (newBreak < proc->elf.programBreakLow)
        return static_cast<int64_t>(proc->programBreak);
    if (newBreak > proc->elf.programBreakHigh)
        return static_cast<int64_t>(proc->programBreak);

    // Map any new pages needed between old and new break
    uint64_t oldPage = (proc->programBreak + 4095) & ~4095ULL;
    uint64_t newPage = (newBreak + 4095) & ~4095ULL;

    for (uint64_t addr = oldPage; addr < newPage; addr += 4096)
    {
        // Check if page is already mapped (might be from ELF loader)
        uint64_t phys = VmmVirtToPhys(addr);
        if (phys == 0)
        {
            phys = PmmAllocPage(MemTag::User, proc->pid);
            if (!phys) return static_cast<int64_t>(proc->programBreak);
            if (!VmmMapPage(addr, phys, VMM_WRITABLE | VMM_USER, MemTag::User, proc->pid))
                return static_cast<int64_t>(proc->programBreak);

            // Zero the page
            auto* p = reinterpret_cast<uint8_t*>(addr);
            for (uint64_t b = 0; b < 4096; ++b) p[b] = 0;
        }
    }

    proc->programBreak = newBreak;
    return static_cast<int64_t>(newBreak);
}

// ---------------------------------------------------------------------------
// sys_mmap (9)
// ---------------------------------------------------------------------------

static constexpr uint64_t MAP_ANONYMOUS = 0x20;

static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset)
{
    if (length == 0) return -EINVAL;

    Process* proc = ProcessCurrent();
    if (!proc) return -ENOMEM;

    uint64_t pages = (length + 4095) / 4096;

    if (flags & MAP_ANONYMOUS)
    {
        // Allocate anonymous memory
        uint64_t vaddr = VmmAllocPages(pages, VMM_WRITABLE | VMM_USER,
                                        MemTag::User, proc->pid);
        if (!vaddr) return -ENOMEM;

        // Zero it
        auto* p = reinterpret_cast<uint8_t*>(vaddr);
        for (uint64_t b = 0; b < pages * 4096; ++b) p[b] = 0;

        return static_cast<int64_t>(vaddr);
    }

    // File-backed mmap (for WAD loading etc.)
    FdEntry* fde = FdGet(proc, static_cast<int>(fd));
    if (!fde || fde->type != FdType::Vnode || !fde->handle)
        return -EBADF;

    // Allocate pages and read file into them
    uint64_t vaddr = VmmAllocPages(pages, VMM_WRITABLE | VMM_USER,
                                    MemTag::User, proc->pid);
    if (!vaddr) return -ENOMEM;

    // Zero first
    auto* p = reinterpret_cast<uint8_t*>(vaddr);
    for (uint64_t b = 0; b < pages * 4096; ++b) p[b] = 0;

    // Read file data
    // TODO: proper seek + read at offset
    auto* vn = static_cast<Vnode*>(fde->handle);
    uint64_t readOff = 0;
    VfsRead(vn, reinterpret_cast<void*>(vaddr), static_cast<uint32_t>(length), &readOff);

    return static_cast<int64_t>(vaddr);
}

// ---------------------------------------------------------------------------
// sys_mprotect (10) -- stub
// ---------------------------------------------------------------------------

static int64_t sys_mprotect(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t)
{
    return 0; // pretend success
}

// ---------------------------------------------------------------------------
// sys_munmap (11) -- stub
// ---------------------------------------------------------------------------

static int64_t sys_munmap(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    // TODO: actually free pages
    return 0;
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
        if (proc) proc->fsBase = addr;

        SerialPrintf("arch_prctl: SET_FS 0x%lx\n", addr);
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
    ReturnToKernel();
    return 0;
}

// ---------------------------------------------------------------------------
// sys_writev (20)
// ---------------------------------------------------------------------------

struct iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

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
    return proc ? proc->pid : 1;
}

// ---------------------------------------------------------------------------
// sys_set_tid_address (218)
// ---------------------------------------------------------------------------

static int64_t sys_set_tid_address(uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t)
{
    return 1; // Return "tid" = 1
}

// ---------------------------------------------------------------------------
// sys_clock_gettime (228) / sys_gettimeofday (96)
// ---------------------------------------------------------------------------

// Simple monotonic counter based on LAPIC timer interrupts.
// The LAPIC fires every 1ms, so we track a global tick count.
extern uint64_t g_lapicTickCount; // defined in apic.cpp

struct timespec {
    int64_t  tv_sec;
    int64_t  tv_nsec;
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

static int64_t sys_clock_gettime(uint64_t clockid, uint64_t tsAddr, uint64_t,
                                  uint64_t, uint64_t, uint64_t)
{
    auto* ts = reinterpret_cast<timespec*>(tsAddr);
    if (!ts) return -EFAULT;

    uint64_t ms = g_lapicTickCount;
    ts->tv_sec  = static_cast<int64_t>(ms / 1000);
    ts->tv_nsec = static_cast<int64_t>((ms % 1000) * 1000000);
    return 0;
}

static int64_t sys_gettimeofday(uint64_t tvAddr, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    auto* tv = reinterpret_cast<timeval*>(tvAddr);
    if (!tv) return -EFAULT;

    uint64_t ms = g_lapicTickCount;
    tv->tv_sec  = static_cast<int64_t>(ms / 1000);
    tv->tv_usec = static_cast<int64_t>((ms % 1000) * 1000);
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

    uint64_t target = g_lapicTickCount + sleepMs;
    while (g_lapicTickCount < target)
        __asm__ volatile("pause");

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
// sys_access (21)
// ---------------------------------------------------------------------------

static int64_t sys_access(uint64_t pathAddr, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path) return -EFAULT;

    // Try to open the file to check existence
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return -ENOENT;
    VfsClose(vn);
    return 0;
}

// ---------------------------------------------------------------------------
// sys_ioctl (16) -- stub
// ---------------------------------------------------------------------------

static int64_t sys_ioctl(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return -ENOSYS; // TODO: implement for framebuffer/keyboard
}

// ---------------------------------------------------------------------------
// sys_prlimit64 (302)
// ---------------------------------------------------------------------------

static int64_t sys_prlimit64(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    return -ENOSYS;
}

// ---------------------------------------------------------------------------
// sys_getrandom (318)
// ---------------------------------------------------------------------------

static int64_t sys_getrandom(uint64_t bufAddr, uint64_t count, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    auto* buf = reinterpret_cast<uint8_t*>(bufAddr);
    // Deterministic PRNG (sufficient for stack canary init)
    uint64_t state = 0xB4005E4D12340001ULL;
    for (uint64_t i = 0; i < count; ++i)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        buf[i] = static_cast<uint8_t>(state);
    }
    return static_cast<int64_t>(count);
}

// ---------------------------------------------------------------------------
// sys_openat (257) -- delegate to sys_open for now
// ---------------------------------------------------------------------------

static int64_t sys_openat(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                           uint64_t mode, uint64_t, uint64_t)
{
    // Ignore dirfd, treat path as absolute
    return sys_open(pathAddr, flags, mode, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_newfstatat (262) / sys_fstat (5) -- stub
// ---------------------------------------------------------------------------

static int64_t sys_fstat(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return -ENOSYS;
}

static int64_t sys_newfstatat(uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t)
{
    return -ENOSYS;
}

// ---------------------------------------------------------------------------
// sys_not_implemented
// ---------------------------------------------------------------------------

static int64_t sys_not_implemented(uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t)
{
    return -ENOSYS;
}

// ---------------------------------------------------------------------------
// Syscall table
// ---------------------------------------------------------------------------

static SyscallFn g_syscallTable[SYSCALL_MAX];

void SyscallTableInit()
{
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        g_syscallTable[i] = sys_not_implemented;

    g_syscallTable[SYS_READ]            = sys_read;
    g_syscallTable[SYS_WRITE]           = sys_write;
    g_syscallTable[SYS_OPEN]            = sys_open;
    g_syscallTable[SYS_CLOSE]           = sys_close;
    g_syscallTable[SYS_FSTAT]           = sys_fstat;
    g_syscallTable[SYS_LSEEK]           = sys_lseek;
    g_syscallTable[SYS_MMAP]            = sys_mmap;
    g_syscallTable[SYS_MPROTECT]        = sys_mprotect;
    g_syscallTable[SYS_MUNMAP]          = sys_munmap;
    g_syscallTable[SYS_BRK]             = sys_brk;
    g_syscallTable[SYS_IOCTL]           = sys_ioctl;
    g_syscallTable[SYS_WRITEV]          = sys_writev;
    g_syscallTable[SYS_ACCESS]          = sys_access;
    g_syscallTable[SYS_NANOSLEEP]       = sys_nanosleep;
    g_syscallTable[SYS_GETPID]          = sys_getpid;
    g_syscallTable[SYS_EXIT]            = sys_exit;
    g_syscallTable[SYS_UNAME]           = sys_uname;
    g_syscallTable[SYS_GETTIMEOFDAY]    = sys_gettimeofday;
    g_syscallTable[SYS_ARCH_PRCTL]      = sys_arch_prctl;
    g_syscallTable[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    g_syscallTable[SYS_CLOCK_GETTIME]   = sys_clock_gettime;
    g_syscallTable[SYS_CLOCK_NANOSLEEP] = sys_clock_nanosleep;
    g_syscallTable[SYS_EXIT_GROUP]      = sys_exit;
    g_syscallTable[SYS_OPENAT]          = sys_openat;
    g_syscallTable[SYS_NEWFSTATAT]      = sys_newfstatat;
    g_syscallTable[SYS_PRLIMIT64]       = sys_prlimit64;
    g_syscallTable[SYS_GETRANDOM]       = sys_getrandom;

    uint32_t count = 0;
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        if (g_syscallTable[i] != sys_not_implemented) ++count;

    SerialPrintf("SYSCALL: table initialised (%u entries, %u implemented)\n",
                 static_cast<unsigned>(SYSCALL_MAX), count);
}

SyscallFn* SyscallGetTable()
{
    return g_syscallTable;
}

// ---------------------------------------------------------------------------
// Entry point address (for LSTAR MSR)
// ---------------------------------------------------------------------------

uint64_t SyscallGetEntryPoint()
{
    return reinterpret_cast<uint64_t>(&BrookSyscallDispatcher);
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
