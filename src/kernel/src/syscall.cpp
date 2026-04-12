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
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "vfs.h"
#include "tty.h"
#include "input.h"

// Forward declaration
extern "C" __attribute__((naked)) void ReturnToKernel();

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
        "push %%rbp\n\t"               // [4]
        "mov %%rsp, %%rbp\n\t"
        "push %%rdx\n\t"               // [5]
        "push %%rsi\n\t"               // [6]
        "push %%rdi\n\t"               // [7]
        "push %%r8\n\t"                // [8]
        "push %%r9\n\t"                // [9]
        "push %%r10\n\t"               // [10] preserve R10
        "push %%r11\n\t"               // [11]
        "push %%r12\n\t"               // [12]
        "push %%r14\n\t"               // [13]
        "push %%r15\n\t"               // [14] — 14 pushes = 112 bytes, aligned
        "mov %%r10, %%rcx\n\t"
        "cmp $400, %%rax\n\t"
        "jae .Lsyscall_invalid\n\t"
        "mov %%gs:16, %%r12\n\t"
        "sti\n\t"
        "call *(%%r12, %%rax, 8)\n\t"
        "cli\n\t"
        "jmp .Lsyscall_return\n\t"
        ".Lsyscall_invalid:\n\t"
        "mov $-38, %%rax\n\t"
        ".Lsyscall_return:\n\t"
        "pop %%r15\n\t"                // [14]
        "pop %%r14\n\t"                // [13]
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
static constexpr int64_t EBADF   = 9;
static constexpr int64_t ENOMEM  = 12;
static constexpr int64_t EFAULT  = 14;
static constexpr int64_t ENODEV  = 19;
static constexpr int64_t EINVAL  = 22;
static constexpr int64_t EMFILE  = 24;
static constexpr int64_t ERANGE  = 34;
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
        SerialLock();
        for (uint64_t i = 0; i < count; ++i)
        {
            SerialPutChar(buf[i]);
            TtyPutChar(buf[i]);
        }
        SerialUnlock();
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

    // Write to /dev/fb0 signals a frame is complete (sets dirty flag).
    if (fde->type == FdType::DevFramebuf)
    {
        proc->fbDirty = 1;
        return static_cast<int64_t>(count);
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

    if (fde->type == FdType::DevKeyboard)
    {
        // Non-blocking read of raw scancodes from input subsystem
        auto* buf = reinterpret_cast<uint8_t*>(bufAddr);
        uint64_t bytesRead = 0;
        while (bytesRead < count)
        {
            InputEvent ev;
            if (!InputPollEvent(&ev)) break;

            // Encode as PS/2-style scancode: bit 7 = release
            uint8_t sc = ev.scanCode;
            if (ev.type == InputEventType::KeyRelease)
                sc |= 0x80;
            buf[bytesRead++] = sc;
        }
        return static_cast<int64_t>(bytesRead);
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
    if (!path) return -EFAULT;

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

    // Device paths
    if (StrEq(path, "/dev/fb0"))
    {
        int fd = FdAlloc(proc, FdType::DevFramebuf, nullptr);
        if (fd < 0) return -EMFILE;
        SerialPrintf("sys_open: /dev/fb0 → fd %d\n", fd);
        return fd;
    }

    if (StrEq(path, "keyboard") || StrEq(path, "/dev/keyboard"))
    {
        int fd = FdAlloc(proc, FdType::DevKeyboard, nullptr);
        if (fd < 0) return -EMFILE;
        SerialPrintf("sys_open: keyboard → fd %d\n", fd);
        return fd;
    }

    Vnode* vn = VfsOpen(lookupPath, static_cast<uint32_t>(flags));
    if (!vn)
    {
        SerialPrintf("sys_open: not found: %s\n", lookupPath);
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
    {
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
        SerialPrintf("sys_brk: query → 0x%lx\n", proc->programBreak);
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

        // Zero via user address (process CR3 is active during syscall)
        auto* p = reinterpret_cast<uint8_t*>(addr);
        for (uint64_t b = 0; b < 4096; ++b) p[b] = 0;
    }

    SerialPrintf("sys_brk: 0x%lx → 0x%lx\n", proc->programBreak, newBreak);
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

    // Helper: allocate user-space virtual pages backed by physical memory.
    auto allocUserPages = [&](MemTag tag) -> uint64_t {
        uint64_t vaddr = proc->mmapNext;
        if (vaddr + pages * 4096 > USER_MMAP_END) return 0;
        proc->mmapNext = vaddr + pages * 4096;

        for (uint64_t i = 0; i < pages; i++)
        {
            PhysicalAddress phys = PmmAllocPage(tag, proc->pid);
            if (!phys) return 0;
            if (!VmmMapPage(proc->pageTable, VirtualAddress(vaddr + i * 4096), phys,
                            VMM_WRITABLE | VMM_USER | VMM_NO_EXEC,
                            tag, proc->pid))
            {
                PmmFreePage(phys);
                return 0;
            }
        }
        return vaddr;
    };

    if (flags & MAP_ANONYMOUS)
    {
        uint64_t vaddr = allocUserPages(MemTag::User);
        if (!vaddr) return -ENOMEM;

        auto* p = reinterpret_cast<uint8_t*>(vaddr);
        for (uint64_t b = 0; b < pages * 4096; ++b) p[b] = 0;

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

        // Reserve user virtual address range
        uint64_t vaddr = proc->mmapNext;
        if (vaddr + pages * 4096 > USER_MMAP_END) return -ENOMEM;
        proc->mmapNext = vaddr + pages * 4096;

        // Map pages — if using a virtual FB, resolve each page's physical
        // address individually (VmmAllocPages may not be contiguous).
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

        SerialPrintf("sys_mmap: fb mapped %lu pages at virt 0x%lx (%s, vfb=%ux%u)\n",
                     pages, vaddr,
                     useVirtFb ? "virtual" : "physical",
                     proc->fbVfbWidth, proc->fbVfbHeight);
        return static_cast<int64_t>(vaddr);
    }

    // File-backed mmap
    if (fde->type != FdType::Vnode || !fde->handle)
        return -EBADF;

    uint64_t vaddr = allocUserPages(MemTag::User);
    if (!vaddr) return -ENOMEM;

    // Zero then read file data
    auto* p = reinterpret_cast<uint8_t*>(vaddr);
    for (uint64_t b = 0; b < pages * 4096; ++b) p[b] = 0;

    auto* vn = static_cast<Vnode*>(fde->handle);
    uint64_t readOff = offset;
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
        if (proc)
        {
            proc->fsBase = addr;
            proc->savedCtx.fsBase = addr;
        }

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
extern volatile uint64_t g_lapicTickCount; // defined in apic.cpp

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

static int64_t sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg,
                          uint64_t, uint64_t, uint64_t)
{
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

        // If process has a virtual framebuffer, report its dimensions instead.
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
            return 0; // pretend success

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
        return 0;
    }

    // tcgetattr/tcsetattr arrive as ioctl on stdin (fd 0)
    // TCGETS = 0x5401, TCSETS/TCSETSW/TCSETSF = 0x5402-0x5404
    if (fd <= 2 && cmd >= 0x5401 && cmd <= 0x5404)
        return 0; // stub success

    // TIOCGWINSZ = 0x5413 — terminal window size
    if (fd <= 2 && cmd == 0x5413)
    {
        struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };
        auto* ws = reinterpret_cast<winsize*>(arg);
        ws->ws_row = 25;
        ws->ws_col = 80;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
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

    st->st_ino = 1;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_size = static_cast<int64_t>(vs.size);
    st->st_blocks = (st->st_size + 511) / 512;

    if (vs.isDir)
        st->st_mode = 0040755; // S_IFDIR | rwxr-xr-x
    else
        st->st_mode = 0100644; // S_IFREG | rw-r--r--
}

static int64_t sys_stat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    const char* path = reinterpret_cast<const char*>(pathAddr);
    auto* st = reinterpret_cast<LinuxStat*>(statAddr);

    VnodeStat vs; vs.size = 0; vs.isDir = false;
    if (VfsStatPath(path, &vs) < 0) return -ENOENT;

    FillStat(st, vs);
    return 0;
}

static int64_t sys_lstat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return sys_stat(pathAddr, statAddr, 0, 0, 0, 0);
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
        VnodeStat vs; vs.size = 0; vs.isDir = false;
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

    return -EBADF;
}

static int64_t sys_newfstatat(uint64_t dirfd, uint64_t pathAddr, uint64_t statAddr,
                               uint64_t flags, uint64_t, uint64_t)
{
    (void)dirfd; (void)flags;
    const char* path = reinterpret_cast<const char*>(pathAddr);
    if (!path || path[0] == '\0')
        return sys_fstat(dirfd, statAddr, 0, 0, 0, 0);
    return sys_stat(pathAddr, statAddr, 0, 0, 0, 0);
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

// ---------------------------------------------------------------------------
// Signal stubs: rt_sigaction (13), rt_sigprocmask (14)
// ---------------------------------------------------------------------------

static int64_t sys_rt_sigaction(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t) { return 0; }

static int64_t sys_rt_sigprocmask(uint64_t, uint64_t, uint64_t oldAddr,
                                   uint64_t sigsetsize, uint64_t, uint64_t)
{
    if (oldAddr && sigsetsize > 0) {
        auto* p = reinterpret_cast<uint8_t*>(oldAddr);
        for (uint64_t i = 0; i < sigsetsize; ++i) p[i] = 0;
    }
    return 0;
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
// sys_openat (257) -- delegate to sys_open
// ---------------------------------------------------------------------------

static int64_t sys_openat(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                           uint64_t mode, uint64_t, uint64_t)
{
    (void)dirfd;
    return sys_open(pathAddr, flags, mode, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_getcwd (79)
// ---------------------------------------------------------------------------

static int64_t sys_getcwd(uint64_t bufAddr, uint64_t size, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    if (size < 2) return -ERANGE;
    auto* buf = reinterpret_cast<char*>(bufAddr);
    buf[0] = '/';
    buf[1] = '\0';
    return static_cast<int64_t>(bufAddr);
}

// ---------------------------------------------------------------------------
// sys_fcntl (72) -- stub
// ---------------------------------------------------------------------------

static int64_t sys_fcntl(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return 0; // pretend success
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
    g_syscallTable[SYS_STAT]            = sys_stat;
    g_syscallTable[SYS_FSTAT]           = sys_fstat;
    g_syscallTable[SYS_LSTAT]           = sys_lstat;
    g_syscallTable[SYS_LSEEK]           = sys_lseek;
    g_syscallTable[SYS_MMAP]            = sys_mmap;
    g_syscallTable[SYS_MPROTECT]        = sys_mprotect;
    g_syscallTable[SYS_MUNMAP]          = sys_munmap;
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
    g_syscallTable[SYS_EXIT]            = sys_exit;
    g_syscallTable[SYS_UNAME]           = sys_uname;
    g_syscallTable[SYS_FCNTL]           = sys_fcntl;
    g_syscallTable[SYS_GETCWD]          = sys_getcwd;
    g_syscallTable[SYS_GETTIMEOFDAY]    = sys_gettimeofday;
    g_syscallTable[SYS_GETUID]          = sys_getuid;
    g_syscallTable[SYS_GETGID]          = sys_getgid;
    g_syscallTable[SYS_SETUID]          = sys_setuid;
    g_syscallTable[SYS_SETGID]          = sys_setgid;
    g_syscallTable[SYS_GETEUID]         = sys_geteuid;
    g_syscallTable[SYS_GETEGID]         = sys_getegid;
    g_syscallTable[SYS_ARCH_PRCTL]      = sys_arch_prctl;
    g_syscallTable[SYS_GETDENTS64]      = sys_getdents64;
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
