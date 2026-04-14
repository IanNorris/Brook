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
#include "serial_writer.h"
#include "pipe.h"
#include "memory/heap.h"
#include "compositor.h"

// Forward declaration
extern "C" __attribute__((naked)) void ReturnToKernel();

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
            static uint32_t s_ttyLog = 0;
            if (s_ttyLog < 80 && count > 0 && count < 200)
            {
                s_ttyLog++;
                // Show first few bytes with hex for non-printable
                char preview[65];
                uint32_t pLen = 0;
                for (uint32_t i = 0; i < count && i < 16 && pLen < 60; i++)
                {
                    uint8_t ch = static_cast<uint8_t>(buf[i]);
                    if (ch >= ' ' && ch <= '~')
                        preview[pLen++] = static_cast<char>(ch);
                    else
                    {
                        preview[pLen++] = '\\';
                        preview[pLen++] = 'x';
                        preview[pLen++] = "0123456789abcdef"[ch >> 4];
                        preview[pLen++] = "0123456789abcdef"[ch & 0xF];
                    }
                }
                preview[pLen] = '\0';
                SerialPrintf("TTY_W: fd=%lu n=%lu [%s]\n", fd, count, preview);
            }
            if (count > 0)
            {
                SerialWriterEnqueue(buf, static_cast<uint32_t>(count));
                // Also render to framebuffer TTY so text is visible on-screen
                for (uint64_t i = 0; i < count; i++)
                    TtyPutChar(buf[i]);
            }
            return static_cast<int64_t>(count);
        }
        // Fall through to FD-table-based write below
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
    if (fde->type == FdType::DevNull)
        return static_cast<int64_t>(count);

    // Write to /dev/tty (DevKeyboard) — route to serial + TTY framebuffer
    if (fde->type == FdType::DevKeyboard)
    {
        const char* buf = reinterpret_cast<const char*>(bufAddr);
        static uint32_t s_kbLog = 0;
        if (s_kbLog < 40 && count > 0 && count < 200)
        {
            s_kbLog++;
            char preview[17];
            uint32_t pLen = count < 16 ? (uint32_t)count : 16;
            for (uint32_t i = 0; i < pLen; i++)
                preview[i] = (buf[i] >= ' ' && buf[i] <= '~') ? buf[i] : '.';
            preview[pLen] = '\0';
            SerialPrintf("KB_W: fd=%lu n=%lu [%s]\n", fd, count, preview);
        }
        if (count > 0)
        {
            SerialWriterEnqueue(buf, static_cast<uint32_t>(count));
            for (uint64_t i = 0; i < count; i++)
                TtyPutChar(buf[i]);
        }
        return static_cast<int64_t>(count);
    }

    // Write to pipe — block until at least some bytes are written
    if (fde->type == FdType::Pipe && fde->handle)
    {
        auto* pipe = static_cast<PipeBuffer*>(fde->handle);
        const char* src = reinterpret_cast<const char*>(bufAddr);
        uint64_t written = 0;

        while (written < count)
        {
            // If no readers, return EPIPE
            if (__atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0)
                return written > 0 ? static_cast<int64_t>(written) : -EPIPE;

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
                break;  // Return partial writes immediately
            }
            // Buffer full — block until reader drains some
            Process* self = ProcessCurrent();
            pipe->writerWaiter = self;
            SchedulerBlock(self);
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
                if (!InputPollEvent(&ev)) break;
                uint8_t sc = ev.scanCode;
                if (ev.type == InputEventType::KeyRelease)
                    sc |= 0x80;
                buf[bytesRead++] = sc;
            }
            return static_cast<int64_t>(bytesRead);
        }

        // Non-canonical (raw) terminal mode: return ASCII chars as they arrive.
        // Shells like ash use this for line editing.
        if (!proc->ttyCanonical)
        {
            uint64_t bytesRead = 0;
            while (bytesRead < count)
            {
                InputEvent ev;
                if (!InputPollEvent(&ev))
                {
                    if (bytesRead > 0) break; // return what we have
                    // Register as waiter BEFORE re-checking, to close the
                    // race where a key arrives between the poll and block.
                    InputAddWaiter(proc);
                    // Re-check after registration — if data arrived between
                    // the first poll and AddWaiter, consume it immediately.
                    if (InputPollEvent(&ev))
                    {
                        InputRemoveWaiter(proc);
                        goto got_event_nc;
                    }
                    SchedulerBlock(proc);
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

        for (;;)
        {
            InputEvent ev;
            if (!InputPollEvent(&ev))
            {
                // Register as waiter BEFORE re-checking to close the race.
                InputAddWaiter(proc);
                if (InputPollEvent(&ev))
                {
                    InputRemoveWaiter(proc);
                    goto got_event_cooked;
                }
                SchedulerBlock(proc);
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
                SerialWriterEnqueue("^C\n", 3);
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
                SerialWriterEnqueue("\n", 1);
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
                    SerialWriterEnqueue("\b \b", 3); // erase character
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
                    SerialWriterEnqueue("\t", 1);
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
                    SerialWriterEnqueue(&c, 1); // echo
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

            // Block until writer puts data or writer closes
            Process* self = ProcessCurrent();
            pipe->readerWaiter = self;
            // Use timed wakeup to recheck writer count periodically
            // (avoids permanent deadlock if close notification was missed)
            extern volatile uint64_t g_lapicTickCount;
            self->wakeupTick = g_lapicTickCount + 10; // recheck every ~10ms
            SchedulerBlock(self);
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

    // /dev/tty — controlling terminal (returns keyboard for read, serial for write)
    if (StrEq(path, "/dev/tty") || StrEq(path, "/dev/console"))
    {
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

    // Synthetic memory files: /etc/passwd, /etc/group, /proc/self/...
    {
        struct SyntheticFile { const char* path; const char* content; };
        static const SyntheticFile syntheticFiles[] = {
            { "/etc/passwd", "root:x:0:0:root:/:/boot/BIN/BASH\n" },
            { "/etc/group",  "root:x:0:\n" },
            { "/etc/shells", "/boot/BIN/BASH\n" },
            { "/etc/hostname", "brook\n" },
            { "/etc/nsswitch.conf", "passwd: files\ngroup: files\n" },
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
    }

    // /proc/self/exe → return ENOENT for now (readlink handles it)
    if (StrEq(lookupPath, "/proc/self/exe"))
        return -ENOENT;

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
        DbgPrintf("sys_open: not found: '%s' (flags=0x%x)\n", lookupPath, vfsFlags);
        return -ENOENT;
    }

    int fd = FdAlloc(proc, FdType::Vnode, vn);
    if (fd < 0)
    {
        VfsClose(vn);
        return -EMFILE;
    }
    proc->fds[fd].statusFlags = static_cast<uint32_t>(flags);

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

    // Determine virtual address.
    auto pickAddr = [&]() -> uint64_t {
        if (flags & MAP_FIXED) {
            if (addr == 0) return 0; // Fail
            // Unmap any existing pages in the range.
            for (uint64_t i = 0; i < pages; i++) {
                VirtualAddress va(addr + i * 4096);
                PhysicalAddress existing = VmmVirtToPhys(proc->pageTable, va);
                if (existing) {
                    VmmUnmapPage(proc->pageTable, va);
                    PmmFreePage(existing);
                }
            }
            return addr;
        }
        // Non-fixed: use addr as hint if valid, otherwise allocate.
        uint64_t base = proc->mmapNext;
        if (addr >= USER_MMAP_BASE && addr + pages * 4096 <= USER_MMAP_END) {
            // Check if hint range is free.
            bool free = true;
            for (uint64_t i = 0; i < pages && free; i++)
                if (VmmVirtToPhys(proc->pageTable, VirtualAddress(addr + i * 4096)))
                    free = false;
            if (free) base = addr;
        }
        if (base + pages * 4096 > USER_MMAP_END) return 0;
        if (base >= proc->mmapNext)
            proc->mmapNext = base + pages * 4096;
        return base;
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
        if (!allocAt(vaddr, MemTag::User)) return -ENOMEM;

        // Zero via direct map (safe even for PROT_NONE / read-only pages).
        for (uint64_t p = 0; p < pages; ++p)
            zeroUserPage(vaddr + p * 4096);

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

        DbgPrintf("sys_mmap: fb mapped %lu pages at virt 0x%lx (%s, vfb=%ux%u)\n",
                     pages, vaddr,
                     useVirtFb ? "virtual" : "physical",
                     proc->fbVfbWidth, proc->fbVfbHeight);
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

    // Read file data into a kernel-side buffer then copy through direct map
    uint64_t bytesLeft = length;
    uint64_t dstOff = 0;
    while (bytesLeft > 0)
    {
        uint64_t chunk = (bytesLeft > 4096) ? 4096 : bytesLeft;
        uint64_t userVA = vaddr + dstOff;
        PhysicalAddress phys = VmmVirtToPhys(proc->pageTable,
                                             VirtualAddress(userVA));
        if (!phys) break;
        auto* kp = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
        VfsRead(vn, kp, chunk, &readOff);
        dstOff += chunk;
        bytesLeft -= chunk;
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
        if (!phys) continue; // Page not mapped — skip (Linux does this)

        // Remap the page with the new flags.
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

    for (uint64_t i = 0; i < pages; ++i)
    {
        VirtualAddress va(addr + i * 4096);
        PhysicalAddress phys = VmmVirtToPhys(proc->pageTable, va);
        if (phys)
        {
            VmmUnmapPage(proc->pageTable, va);
            PmmFreePage(phys);
        }
    }

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

    DbgPrintf("FORK: pid=%u forked child pid=%u\n", parent->pid, child->pid);
    return static_cast<int64_t>(child->pid);
}

static int64_t sys_clone(uint64_t flags, uint64_t newStack, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    // clone with a new stack: use fork but override the child's RSP.
    // musl's __clone pushes the arg+fn onto newStack before the syscall,
    // then the child does pop %rdi; call *%r9 using that stack.
    (void)flags;

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

    Process* child = ProcessFork(parent, userRip, userRsp, userRflags);
    if (!child) return -ENOMEM;

    child->forkRbx = savedRbx;  child->forkRbp = savedRbp;
    child->forkR12 = savedR12;  child->forkR13 = savedR13;
    child->forkR14 = savedR14;  child->forkR15 = savedR15;
    child->forkRdi = savedRdi;  child->forkRsi = savedRsi;
    child->forkRdx = savedRdx;  child->forkR8  = savedR8;
    child->forkR9  = savedR9;   child->forkR10 = savedR10;

    SchedulerAddProcess(child);

    SerialPrintf("FORK: parent pid=%u -> child pid=%u '%s', rip=0x%lx rsp=0x%lx\n",
                 parent->pid, child->pid, child->name, userRip, userRsp);
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
// sys_wait4 (61)
// ---------------------------------------------------------------------------
// Wait for a child process to terminate. Supports pid=-1 (any child)
// or a specific child PID. WNOHANG (options & 1) returns 0 if no child
// has exited yet instead of blocking.

static constexpr uint64_t WNOHANG = 1;

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

    // Spin until a terminated child is found (or return immediately for WNOHANG)
    for (;;)
    {
        Process* child = SchedulerFindTerminatedChild(parent->pid, targetPid);
        if (child)
        {
            int32_t childPid = child->pid;
            int32_t childStatus = child->exitStatus;

            if (statusAddr)
            {
                // Linux wait status encoding: (status & 0xFF) << 8 for normal exit
                auto* wstatus = reinterpret_cast<int32_t*>(statusAddr);
                *wstatus = (childStatus & 0xFF) << 8;
            }

            SchedulerReapChild(child);
            return static_cast<int64_t>(childPid);
        }

        if (options & WNOHANG)
            return 0;

        // Block until a child exits and wakes us. Use a short timed
        // wakeup because the child may already be Terminated but not yet
        // reapable (DrainPostSwitch hasn't run to set the flag).
        extern volatile uint64_t g_lapicTickCount;
        parent->wakeupTick = g_lapicTickCount + 5;
        SchedulerBlock(parent);
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

    constexpr uint64_t MAX_ELF_SIZE = 2 * 1024 * 1024;
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

    SerialPrintf("sys_execve: loaded '%s' (%lu bytes) for pid %u\n",
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

    // Update process name to reflect new binary
    const char* baseName = lookupPath;
    for (const char* p = lookupPath; *p; ++p)
        if (*p == '/') baseName = p + 1;
    uint32_t ni = 0;
    while (baseName[ni] && ni < 30)
    {
        proc->name[ni] = baseName[ni];
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
            auto* cc = reinterpret_cast<uint8_t*>(&t[4]);
            cc[0] = 0;
            __builtin_memset(&cc[1], 0, 19);
            cc[1] = 0x03;  // VINTR
            cc[2] = 0x1C;  // VQUIT
            cc[3] = 0x7F;  // VERASE
            cc[4] = 0x15;  // VKILL
            cc[5] = 0x04;  // VEOF
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
            *pgrp = cur ? static_cast<int>(cur->pid) : 1;
            return 0;
        }

        // TIOCSPGRP, TIOCSCTTY
        if (cmd == 0x5410 || cmd == 0x540E)
            return 0;

        // TIOCGWINSZ
        if (cmd == 0x5413)
        {
            auto* ws = reinterpret_cast<uint16_t*>(arg);
            ws[0] = 25; ws[1] = 80; ws[2] = 0; ws[3] = 0;
            return 0;
        }

        return 0;
    }

    // tcgetattr/tcsetattr arrive as ioctl on stdin (fd 0)
    // TCGETS = 0x5401, TCSETS/TCSETSW/TCSETSF = 0x5402-0x5404
    // Also handle any fd that is a TTY device (e.g. fd 63 from /dev/tty dup)
    bool isTtyFd = (fd <= 2) || (fde->type == FdType::DevKeyboard) ||
                   (fde->type == FdType::Vnode && !fde->handle);
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
        auto* cc = reinterpret_cast<uint8_t*>(&t[4]);
        cc[0] = 0;
        __builtin_memset(&cc[1], 0, 19);
        cc[1] = 0x03;  // VINTR = Ctrl+C
        cc[2] = 0x1C;  // VQUIT = Ctrl+backslash
        cc[3] = 0x7F;  // VERASE = DEL
        cc[4] = 0x15;  // VKILL = Ctrl+U
        cc[5] = 0x04;  // VEOF = Ctrl+D
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
        *pgrp = cur ? static_cast<int>(cur->pid) : 1;
        return 0;
    }

    // TIOCSPGRP = 0x5410 — set foreground process group
    if (isTtyFd && cmd == 0x5410)
        return 0;

    // TIOCSCTTY = 0x540E — set controlling terminal
    if (isTtyFd && cmd == 0x540E)
        return 0;

    // TIOCGWINSZ = 0x5413 — terminal window size
    if (isTtyFd && cmd == 0x5413)
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

    if (fde->type == FdType::DevKeyboard || fde->type == FdType::DevNull) {
        auto* raw = reinterpret_cast<uint8_t*>(st);
        for (uint64_t i = 0; i < sizeof(LinuxStat); ++i) raw[i] = 0;
        st->st_mode = 0020666; // S_IFCHR
        st->st_rdev = (fde->type == FdType::DevNull) ? 0x0103 : 0x0400;
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

struct KernelSigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

static KernelSigaction g_sigHandlers[MAX_PROCESSES][64];

static int64_t sys_rt_sigaction(uint64_t signum, uint64_t actAddr, uint64_t oldactAddr,
                                 uint64_t sigsetsize, uint64_t, uint64_t)
{
    if (signum < 1 || signum > 64) return -EINVAL;
    if (sigsetsize != 8) return -EINVAL;

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    uint32_t idx = static_cast<uint32_t>(signum) - 1;
    uint16_t pid = proc->pid;
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
    (void)how; (void)setAddr;
    if (oldAddr && sigsetsize > 0) {
        auto* p = reinterpret_cast<uint8_t*>(oldAddr);
        for (uint64_t i = 0; i < sigsetsize; ++i) p[i] = 0;
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
            fds[i].revents = fds[i].events; ready++;
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

    return -EINVAL; // no symlinks in our FS
}

static int64_t sys_readlinkat(uint64_t dirfd, uint64_t pathAddr, uint64_t bufAddr,
                               uint64_t bufsiz, uint64_t, uint64_t)
{
    (void)dirfd;
    return sys_readlink(pathAddr, bufAddr, bufsiz, 0, 0, 0);
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

static int64_t sys_chdir(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return 0; // pretend success — we only have root dir
}

static int64_t sys_fchdir(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
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
// sys_sigaltstack (131) — stub
// ---------------------------------------------------------------------------

static int64_t sys_sigaltstack(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t)
{
    return 0; // pretend success
}

// ---------------------------------------------------------------------------
// sys_rt_sigreturn (15) — stub
// ---------------------------------------------------------------------------

static int64_t sys_rt_sigreturn(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    return 0;
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

static int64_t sys_tgkill(uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    return 0; // ignore signals for now
}

static int64_t sys_tkill(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    return 0;
}

// ---------------------------------------------------------------------------
// sys_kill (62) — stub
// ---------------------------------------------------------------------------

static int64_t sys_kill(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    return 0; // ignore
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
// sys_select (23) — minimal implementation
// ---------------------------------------------------------------------------

static int64_t sys_select(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                           uint64_t exceptfdsAddr, uint64_t timeoutAddr, uint64_t)
{
    (void)exceptfdsAddr;
    (void)timeoutAddr;

    // For simplicity, report all FDs as ready
    // This is sufficient for many programs that just use select as a timeout
    int ready = 0;
    if (readfdsAddr)
    {
        auto* rfds = reinterpret_cast<uint64_t*>(readfdsAddr);
        for (uint64_t fd = 0; fd < nfds && fd < 64; fd++)
        {
            if (rfds[fd / 64] & (1ULL << (fd % 64)))
                ready++;
        }
    }
    if (writefdsAddr)
    {
        auto* wfds = reinterpret_cast<uint64_t*>(writefdsAddr);
        for (uint64_t fd = 0; fd < nfds && fd < 64; fd++)
        {
            if (wfds[fd / 64] & (1ULL << (fd % 64)))
                ready++;
        }
    }
    return ready ? ready : 1; // at least 1 to avoid blocking
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
    if (!vn) return -ENOENT;
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
// sys_futex (202) — fast userspace mutex (minimal stub)
// ---------------------------------------------------------------------------

static constexpr int FUTEX_WAIT         = 0;
static constexpr int FUTEX_WAKE         = 1;
static constexpr int FUTEX_PRIVATE_FLAG = 128;

static int64_t sys_futex(uint64_t uaddrVal, uint64_t opVal, uint64_t val,
                          uint64_t, uint64_t, uint64_t)
{
    int op = static_cast<int>(opVal) & ~FUTEX_PRIVATE_FLAG;

    if (op == FUTEX_WAKE) {
        // Wake up to `val` waiters — since we have no wait queue, return 0
        (void)val;
        return 0;
    }

    if (op == FUTEX_WAIT) {
        // Check if *uaddr == val; if not, return EAGAIN
        auto* uaddr = reinterpret_cast<volatile uint32_t*>(uaddrVal);
        if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != static_cast<uint32_t>(val))
            return -EAGAIN;
        // For now, just yield and return — proper blocking requires a wait queue
        SchedulerYield();
        return -EAGAIN;
    }

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
    g_syscallTable[SYS_PIPE]            = sys_pipe;
    g_syscallTable[SYS_DUP]             = sys_dup;
    g_syscallTable[SYS_DUP2]            = sys_dup2;
    g_syscallTable[SYS_CLONE]           = sys_clone;
    g_syscallTable[SYS_FORK]            = sys_fork;
    g_syscallTable[SYS_VFORK]           = sys_vfork;
    g_syscallTable[SYS_EXECVE]          = sys_execve;
    g_syscallTable[SYS_EXIT]            = sys_exit;
    g_syscallTable[SYS_WAIT4]           = sys_wait4;
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
    g_syscallTable[SYS_GETPPID]         = sys_getppid;
    g_syscallTable[SYS_GETGROUPS]       = sys_getgroups;
    g_syscallTable[SYS_SETGROUPS]       = sys_setgroups;
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
    g_syscallTable[SYS_STATFS]          = sys_statfs;
    g_syscallTable[SYS_FSTATFS]         = sys_fstatfs;
    g_syscallTable[SYS_GETTID]          = sys_gettid;
    g_syscallTable[SYS_TKILL]           = sys_tkill;
    g_syscallTable[SYS_TGKILL]          = sys_tgkill;
    g_syscallTable[SYS_READLINKAT]      = sys_readlinkat;
    g_syscallTable[SYS_PPOLL]           = sys_ppoll;
    g_syscallTable[SYS_PIPE2]           = sys_pipe2;
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
    case 32: return "dup";        case 33: return "dup2";
    case 35: return "nanosleep";  case 39: return "getpid";
    case 40: return "sendfile";   case 56: return "clone";
    case 57: return "fork";       case 58: return "vfork";
    case 59: return "execve";     case 60: return "exit";
    case 61: return "wait4";      case 62: return "kill";
    case 63: return "uname";      case 72: return "fcntl";
    case 79: return "getcwd";     case 80: return "chdir";
    case 81: return "fchdir";     case 82: return "rename";
    case 83: return "mkdir";      case 87: return "unlink";
    case 89: return "readlink";   case 95: return "umask";
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
    case 131: return "sigaltstack"; case 134: return "statfs";
    case 135: return "fstatfs";   case 157: return "prctl";
    case 158: return "arch_prctl"; case 186: return "gettid";
    case 200: return "tkill";     case 217: return "getdents64";
    case 218: return "set_tid_address"; case 228: return "clock_gettime";
    case 230: return "clock_nanosleep"; case 231: return "exit_group";
    case 234: return "tgkill";    case 257: return "openat";
    case 262: return "newfstatat"; case 267: return "readlinkat";
    case 270: return "pselect6";  case 271: return "ppoll";     case 273: return "set_robust_list";
    case 289: return "prlimit64"; case 292: return "dup3";
    case 293: return "pipe2";     case 302: return "rseq";
    case 318: return "getrandom"; case 334: return "faccessat";
    case 439: return "faccessat2";
    default: return nullptr;
    }
}

static int64_t SyscallDispatchTraced(uint64_t num, uint64_t a0, uint64_t a1,
                                      uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    Process* proc = ProcessCurrent();
    const char* name = SyscallName(num);

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

    int64_t ret = g_syscallTable[num](a0, a1, a2, a3, a4, a5);

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
    return g_syscallTable[num](a0, a1, a2, a3, a4, a5);
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
