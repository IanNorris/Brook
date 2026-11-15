// syscall_strace.cpp -- syscall tracing (strace) and its controls.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "serial.h"
#include "process.h"
#include "syscall_strace.h"
#include "syscall_dispatch.h"

namespace brook {

// ---------------------------------------------------------------------------
// Strace — syscall tracing facility
// ---------------------------------------------------------------------------

// Category bitmask for strace filtering
enum StraceCategory : uint32_t {
    SC_NET    = 1 << 0,  // socket, connect, sendto, recvfrom, etc.
    SC_FILE   = 1 << 1,  // open, read, write, close, stat, etc.
    SC_MEM    = 1 << 2,  // mmap, munmap, mprotect, brk
    SC_PROC   = 1 << 3,  // fork, clone, execve, exit, wait
    SC_SIG    = 1 << 4,  // rt_sigaction, rt_sigprocmask, kill, etc.
    SC_IO     = 1 << 5,  // ioctl, poll, pselect, ppoll, epoll
    SC_OTHER  = 1 << 6,  // everything else
};

uint32_t SyscallCategory(uint64_t num)
{
    switch (num) {
    // Network
    case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48:
    case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    case 299: case 307:
        return SC_NET;  // socket..shutdown, recvmmsg, sendmmsg
    // File
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 8:
    case 17: case 18: case 19: case 20: case 21:
    case 32: case 33: case 72: case 73: case 76: case 77:
    case 79: case 80: case 82: case 83: case 84: case 85: case 86: case 87: case 88:
    case 89: case 90: case 91: case 92: case 93: case 94:
    case 217: case 257: case 258: case 260: case 262: case 263: case 264:
    case 265: case 266: case 267: case 268: case 280: case 285:
    case 292: case 293: case 316: case 332: case 439:
        return SC_FILE;
    // Memory
    case 9: case 10: case 11: case 12: case 25: case 26: case 27: case 28:
        return SC_MEM;  // mmap..mremap, madvise
    // Process
    case 56: case 57: case 58: case 59: case 60: case 61: case 62:
    case 231: case 234: case 247: case 435:
        return SC_PROC; // clone..wait4, exit_group, tgkill, clone3
    // Signal
    case 13: case 14: case 15: case 34: case 127: case 128: case 129: case 130: case 131:
    case 200:
        return SC_SIG;  // rt_sigaction..rt_sigreturn, kill, tkill
    // I/O control
    case 7: case 16: case 23: case 270: case 271:
    case 213: case 214: case 232: case 233:
    case 290:
        return SC_IO;   // poll, ioctl, select, pselect6, ppoll, epoll_*
    default:
        return SC_OTHER;
    }
}

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
    case 25: return "mremap";
    case 26: return "msync";
    case 32: return "dup";        case 33: return "dup2";
    case 35: return "nanosleep";  case 39: return "getpid";
    case 40: return "sendfile";   case 41: return "socket";
    case 42: return "connect";    case 43: return "accept";
    case 44: return "sendto";     case 45: return "recvfrom";
    case 46: return "sendmsg";    case 47: return "recvmsg";
    case 48: return "shutdown";   case 49: return "bind";
    case 50: return "listen";     case 51: return "getsockname";
    case 52: return "getpeername"; case 53: return "socketpair";
    case 54: return "setsockopt"; case 55: return "getsockopt";
    case 56: return "clone";
    case 57: return "fork";       case 58: return "vfork";
    case 59: return "execve";     case 60: return "exit";
    case 247: return "waitid";
    case 61: return "wait4";      case 62: return "kill";
    case 63: return "uname";      case 72: return "fcntl";
    case 74: return "fsync";      case 75: return "fdatasync";
    case 79: return "getcwd";     case 80: return "chdir";
    case 81: return "fchdir";     case 82: return "rename";
    case 83: return "mkdir";      case 84: return "rmdir";      case 87: return "unlink";
    case 85: return "creat";       case 88: return "symlink";    case 89: return "readlink";   case 95: return "umask";
    case 96: return "gettimeofday"; case 97: return "getrlimit";
    case 98: return "getrusage";  case 99: return "sysinfo";
    case 102: return "getuid";    case 104: return "getgid";
    case 105: return "setuid";    case 106: return "setgid";
    case 107: return "geteuid";   case 108: return "getegid";
    case 109: return "setpgid";   case 110: return "getppid";
    case 111: return "getpgrp";   case 112: return "setsid";
    case 115: return "getgroups"; case 116: return "setgroups";
    case 113: return "setreuid"; case 114: return "setregid";
    case 117: return "setresuid"; case 118: return "getresuid";
    case 119: return "setresgid"; case 120: return "getresgid";
    case 121: return "getpgid";   case 124: return "getsid";
    case 222: return "timer_create"; case 223: return "timer_settime";
    case 224: return "timer_gettime"; case 225: return "timer_getoverrun";
    case 226: return "timer_delete";
    case 127: return "rt_sigpending";
    case 128: return "rt_sigtimedwait"; case 131: return "sigaltstack"; case 130: return "rt_sigsuspend";
    case 134: return "statfs";
    case 34: return "pause"; case 37: return "alarm";
    case 135: return "fstatfs";   case 157: return "prctl";
    case 158: return "arch_prctl"; case 186: return "gettid";
    case 200: return "tkill";     case 201: return "time";       case 217: return "getdents64";
    case 218: return "set_tid_address"; case 228: return "clock_gettime";
    case 230: return "clock_nanosleep"; case 231: return "exit_group";
    case 234: return "tgkill";    case 257: return "openat";
    case 262: return "newfstatat"; case 266: return "symlinkat"; case 267: return "readlinkat";
    case 270: return "pselect6";  case 271: return "ppoll";     case 273: return "set_robust_list"; case 274: return "get_robust_list";
    case 292: return "dup3";
    case 290: return "eventfd2";  case 293: return "pipe2";     case 302: return "prlimit64";
    case 299: return "recvmmsg";  case 307: return "sendmmsg";
    case 318: return "getrandom"; case 334: return "rseq";
    case 439: return "faccessat2";
    case 73: return "flock";      case 76: return "truncate";
    case 77: return "ftruncate";  case 86: return "link";
    case 90: return "chmod";      case 91: return "fchmod";
    case 92: return "chown";      case 93: return "fchown";
    case 94: return "lchown";
    case 142: return "sched_setparam"; case 143: return "sched_getparam";
    case 144: return "sched_setscheduler"; case 145: return "sched_getscheduler";
    case 146: return "sched_get_priority_max"; case 147: return "sched_get_priority_min";
    case 203: return "sched_setaffinity"; case 204: return "sched_getaffinity";
    case 258: return "mkdirat";   case 260: return "fchownat";
    case 263: return "unlinkat";  case 264: return "renameat";
    case 265: return "linkat";    case 268: return "fchmodat";
    case 280: return "utimensat"; case 285: return "fallocate";
    case 316: return "renameat2"; case 332: return "statx";
    case 435: return "clone3";
    case 18: return "pwrite64"; case 27: return "mincore";
    case 28: return "madvise";
    default: return nullptr;
    }
}

int64_t SyscallDispatchTraced(uint64_t num, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    Process* proc = ProcessCurrent();
    const char* name = SyscallName(num);

    // Rate-limit: suppress repeated identical error syscalls to prevent
    // stack/serial flood (e.g. writes to a closed fd in a tight loop).
    static uint64_t s_lastNum   = ~0ULL;
    static uint64_t s_lastA0    = 0;
    static int64_t  s_lastRet   = 0;
    static uint32_t s_suppressed = 0;
    static constexpr uint32_t SUPPRESS_THRESHOLD = 3;

    SyscallFn fn = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
    if (!fn) {
        SerialPrintf("[syscall] FATAL-AVOIDED: pid=%u (%s) num=%lu has null/oob entry "
                     "(a0=%lx a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx) -> -ENOSYS\n",
                     proc ? proc->pid : 0, proc ? proc->name : "?",
                     num, a0, a1, a2, a3, a4, a5);
        return -38; // -ENOSYS
    }

    // Successful exec and exit never return to the post-dispatch logger. Emit
    // their attempted operation here; failed exec still gets its error below.
    if (!proc->straceErrorsOnly) {
        if (num == SYS_EXIT || num == SYS_EXIT_GROUP)
            SerialPrintf("[strace:%u] %s(%lu) [noreturn]\n", proc->pid, name, a0);
        else if (num == SYS_EXECVE)
            SerialPrintf("[strace:%u] %s [attempt]\n", proc->pid, name);
    }
    int64_t ret = fn(a0, a1, a2, a3, a4, a5);

    // In errors-only mode, skip successful syscalls entirely
    bool isError = (ret < 0 && ret > -4096);
    if (proc->straceErrorsOnly && !isError) {
        // Always log blocking syscalls from the main thread for debugging
        if (proc->pid == proc->tgid &&
            (num == 202 /*futex*/ || num == 232 /*epoll_wait*/ ||
             num == 271 /*ppoll*/ || num == 7 /*poll*/ ||
             num == 231 /*exit_group*/ || num == 60 /*exit*/))
        {
            SerialPrintf("[strace-main:%u] %s(%lx,%lx,%lx) = %ld\n",
                         proc->pid, name ? name : "?", a0, a1, a2, ret);
        }
        return ret;
    }
    bool isRepeat = (num == s_lastNum && a0 == s_lastA0 && ret == s_lastRet && isError);

    if (isRepeat) {
        s_suppressed++;
        if (s_suppressed == SUPPRESS_THRESHOLD)
            SerialPrintf("[strace:%u] ... suppressing repeated %s(%lu) = -%lu\n",
                         proc->pid, name ? name : "?", a0, -ret);
        if (s_suppressed >= SUPPRESS_THRESHOLD)
            return ret;
    } else {
        if (s_suppressed > SUPPRESS_THRESHOLD)
            SerialPrintf("[strace:%u] ... (%u identical errors suppressed)\n",
                         proc->pid, s_suppressed - SUPPRESS_THRESHOLD);
        s_suppressed = 0;
    }

    s_lastNum = num;
    s_lastA0  = a0;
    s_lastRet = ret;

    // One SerialPrintf holds the serial lock for the complete record. A split
    // prefix/result lets another CPU's syscall or debug output steal the result
    // attribution, making allocation lifetimes impossible to reconstruct.
    if(SyscallCategory(num)==SC_MEM) {
        if(num==SYS_MMAP) {
            if(isError)SerialPrintf("[strace:%u] mmap(0x%lx, %lu, 0x%lx, 0x%lx, %ld, 0x%lx) = %ld (err) comm=\"%s\"\n",proc->pid,a0,a1,a2,a3,(int64_t)a4,a5,ret,proc->name);
            else SerialPrintf("[strace:%u] mmap(0x%lx, %lu, 0x%lx, 0x%lx, %ld, 0x%lx) = 0x%lx comm=\"%s\"\n",proc->pid,a0,a1,a2,a3,(int64_t)a4,a5,(uint64_t)ret,proc->name);
        } else if(num==SYS_BRK) {
            SerialPrintf("[strace:%u] brk(0x%lx, 0x%lx, 0x%lx) = 0x%lx comm=\"%s\"\n",proc->pid,a0,a1,a2,(uint64_t)ret,proc->name);
        } else {
            SerialPrintf("[strace:%u] %s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx) = %ld comm=\"%s\"\n",proc->pid,name?name:"memory",a0,a1,a2,a3,a4,a5,ret,proc->name);
        }
        return ret;
    }

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
    } else if (num == SYS_WAITID) {
        SerialPrintf("[strace:%u] waitid(%lu, %lu, 0x%lx, 0x%lx, 0x%lx)",
                     proc->pid, a0, a1, a2, a3, a4);
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

    // Log return value
    if (ret < 0 && ret > -4096)
        SerialPrintf(" = -%lu (err)\n", -ret);
    else if (num == SYS_MMAP || num == SYS_BRK)
        SerialPrintf(" = 0x%lx\n", static_cast<uint64_t>(ret));
    else
        SerialPrintf(" = %ld\n", ret);

    return ret;
}

} // namespace brook

// ---------------------------------------------------------------------------
// Strace control functions
// ---------------------------------------------------------------------------

bool StraceEnablePid(uint32_t pid, bool enable, uint32_t filter)
{
    using namespace brook;
    Process* p = ProcessFindByPid(static_cast<uint16_t>(pid));
    if (!p) return false;
    p->straceEnabled = enable;
    p->straceFilter = filter;
    return true;
}

int StraceEnableName(const char* name, bool enable, uint32_t filter)
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
                p->straceFilter = filter;
                count++;
            }
        }
    }
    return count;
}

void StraceEnableAll(bool enable, uint32_t filter)
{
    using namespace brook;
    for (uint16_t pid = 1; pid < 256; pid++) {
        Process* p = ProcessFindByPid(pid);
        if (p) {
            p->straceEnabled = enable;
            p->straceFilter = filter;
        }
    }
}
