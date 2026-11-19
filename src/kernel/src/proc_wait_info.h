#pragma once
#include <stdint.h>

// Pure helpers behind /proc/<pid>/syscall, /proc/<pid>/wchan and the
// `proc blocked <pid>` debug command. They classify what a blocked task waits
// for from its syscall number, the type of its fd argument and the kernel
// wait site, without touching scheduler state (host-tested).
namespace brook {

// Recorded kernel return addresses of the latest SchedulerBlock call.
static constexpr uint32_t ProcWaitSiteDepth = 4;

// FdType values the classifier needs (kept numerically equal to fd_table.h;
// the kernel static_asserts that). -1 means "no fd argument / unknown".
namespace ProcFd {
static constexpr int None = 0, Vnode = 1, DevFramebuf = 2, DevKeyboard = 3, Pipe = 4,
    Socket = 8, DevTty = 9, EventFd = 10, DevDsp = 11, EpollFd = 12, TimerFd = 13,
    UnixSocket = 15, DevKlog = 16, DevDri = 17, DrmPrime = 18, SyncFile = 21,
    SignalFd = 22, NetlinkSocket = 24, BlockDevice = 26, DevIntelDri = 27,
    IntelPrime = 28, IntelSyncObj = 29;
}

// True when the syscall's first argument is a file descriptor whose type
// determines the wait kind (read/write family, socket I/O, ioctl).
static inline bool ProcWaitSyscallUsesFd(uint64_t nr)
{
    switch (nr) {
    case 0: case 1: case 16: case 17: case 18: case 19: case 20: // read..writev, ioctl
    case 42: case 43: case 44: case 45: case 46: case 47:        // connect..recvmsg
    case 288: case 295: case 296: case 299: case 307: case 327: case 328:
        return true;
    default: return false;
    }
}

static inline const char* ProcWaitFdKind(int fdType, bool ioctl)
{
    switch (fdType) {
    case ProcFd::Pipe: return "pipe";
    case ProcFd::Socket: case ProcFd::NetlinkSocket: return "socket";
    case ProcFd::UnixSocket: return "unix-socket";
    case ProcFd::EventFd: return "eventfd";
    case ProcFd::TimerFd: return "timerfd";
    case ProcFd::SignalFd: return "signalfd";
    case ProcFd::DevTty: case ProcFd::DevKeyboard: return "tty";
    case ProcFd::DevDsp: return "audio";
    case ProcFd::DevKlog: return "klog";
    case ProcFd::EpollFd: return "epoll";
    case ProcFd::DevDri: case ProcFd::DrmPrime: case ProcFd::SyncFile: case ProcFd::DevIntelDri: case ProcFd::IntelPrime: case ProcFd::IntelSyncObj:
        return "gpu-wait";
    case ProcFd::Vnode: case ProcFd::BlockDevice: return ioctl ? "ioctl" : "file-io";
    default: return ioctl ? "ioctl" : "io";
    }
}

// Wait kind for a task. `blocked` is state==Blocked; `site` is the first
// wait-site symbol (demangled, may be null). Lock waits win: a task blocked on
// a kernel mutex inside futex() is waiting for that mutex, not for the futex.
static inline const char* ProcWaitKind(bool blocked, bool stopped, bool kernelThread,
                                       uint64_t nr, int fdType, bool rwlock,
                                       const char* site)
{
    if (stopped) return "stopped";
    if (!blocked) return "running";
    if (rwlock) return "rwlock";
    if (site) {
        auto has = [site](const char* needle) {
            for (const char* s = site; *s; ++s) {
                const char* a = s; const char* b = needle;
                while (*a && *b && *a == *b) { ++a; ++b; }
                if (!*b) return true;
            }
            return false;
        };
        if (has("KMutexLock")) return "kmutex";
        if (has("KRwLock")) return "rwlock";
        if (has("KSemaphore")) return "semaphore";
        if (has("Intel") || has("Bdw") || has("Gpu") || has("Virgl")) {
            if (!(nr == 202 || nr == 7 || nr == 271 || nr == 232 || nr == 281 || nr == 441))
                return "gpu-wait";
        }
    }
    if (kernelThread) return "kernel";
    switch (nr) {
    case 202: return "futex";
    case 7: case 271: return "poll";
    case 23: case 270: return "select";
    case 232: case 281: case 441: return "epoll";
    case 35: case 230: return "sleep";
    case 34: case 130: case 128: return "signal";
    case 61: case 247: return "wait-child";
    case 58: return "vfork";
    default: break;
    }
    if (ProcWaitSyscallUsesFd(nr)) return ProcWaitFdKind(fdType, nr == 16);
    if (nr >= 506 && nr <= 524) return "brook-wm";
    if (nr >= 0xB00 && nr < 0xC00) return "brook-ext";
    return "other";
}

// Strip a demangled symbol to its qualified function name without the
// "brook::" namespace or the argument list: "brook::FutexWait(int)" -> "FutexWait".
static inline uint32_t ProcWaitShortSymbol(const char* name, char* out, uint32_t capacity)
{
    if (!capacity) return 0;
    const char* s = name;
    if (s[0]=='b'&&s[1]=='r'&&s[2]=='o'&&s[3]=='o'&&s[4]=='k'&&s[5]==':'&&s[6]==':') s += 7;
    uint32_t n = 0;
    while (*s && *s != '(' && n + 1 < capacity) out[n++] = *s++;
    out[n] = 0;
    return n;
}

// Linux /proc/<pid>/syscall line: "running", or
// "nr arg1 .. arg6 sp pc" (args, sp and pc in 0x-prefixed hex).
static inline uint32_t ProcFormatSyscall(char* buf, uint32_t capacity, bool running,
                                         uint64_t nr, const uint64_t args[6],
                                         uint64_t sp, uint64_t pc)
{
    uint32_t n = 0;
    auto put = [&](char c) { if (n + 1 < capacity) buf[n++] = c; };
    auto text = [&](const char* s) { while (*s) put(*s++); };
    auto dec = [&](uint64_t v) {
        char d[24]; int k = 0; do { d[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) put(d[--k]);
    };
    auto hex = [&](uint64_t v) {
        text("0x"); char d[20]; int k = 0;
        do { const unsigned x = v & 15; d[k++] = char(x < 10 ? '0' + x : 'a' + x - 10); v >>= 4; } while (v);
        while (k) put(d[--k]);
    };
    if (running) text("running");
    else {
        dec(nr);
        for (int i = 0; i < 6; ++i) { put(' '); hex(args[i]); }
        put(' '); hex(sp); put(' '); hex(pc);
    }
    put('\n');
    if (capacity) buf[n] = 0;
    return n;
}

} // namespace brook
