// procfs — virtual /proc filesystem for Brook OS
//
// Provides read-only process and system information compatible with the
// Linux /proc layout that busybox ps/top expect.

#include "procfs.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "serial.h"

namespace brook {

extern volatile uint64_t g_lapicTickCount;

// ---- Helpers ----

static uint32_t ProcStrLen(const char* s)
{
    uint32_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void ProcStrCopy(char* dst, const char* src, uint32_t maxLen)
{
    uint32_t i = 0;
    while (i + 1 < maxLen && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

// Minimal snprintf-like formatting into a buffer. Returns bytes written (excl NUL).
static uint32_t ProcFmt(char* buf, uint32_t bufSize, const char* fmt, ...);

// Integer to decimal string. Returns pointer past last digit.
static char* U64ToDec(char* buf, uint64_t val)
{
    if (val == 0) { *buf++ = '0'; return buf; }
    char tmp[20];
    int n = 0;
    while (val > 0) { tmp[n++] = '0' + (val % 10); val /= 10; }
    for (int i = n - 1; i >= 0; --i) *buf++ = tmp[i];
    return buf;
}

static char* I64ToDec(char* buf, int64_t val)
{
    if (val < 0) { *buf++ = '-'; val = -val; }
    return U64ToDec(buf, static_cast<uint64_t>(val));
}

// Simple format: %u, %lu, %d, %ld, %s, %c — enough for /proc files.
static uint32_t ProcFmt(char* buf, uint32_t bufSize, const char* fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    char* p = buf;
    char* end = buf + bufSize - 1;
    while (*fmt && p < end)
    {
        if (*fmt != '%') { *p++ = *fmt++; continue; }
        fmt++; // skip %
        bool isLong = false;
        if (*fmt == 'l') { isLong = true; fmt++; }
        switch (*fmt)
        {
        case 'u': {
            uint64_t v = isLong ? __builtin_va_arg(ap, uint64_t) : __builtin_va_arg(ap, uint32_t);
            char tmp[20]; char* t = U64ToDec(tmp, v);
            for (char* s = tmp; s < t && p < end; ) *p++ = *s++;
            break;
        }
        case 'd': {
            int64_t v = isLong ? __builtin_va_arg(ap, int64_t) : __builtin_va_arg(ap, int32_t);
            char tmp[21]; char* t = I64ToDec(tmp, v);
            for (char* s = tmp; s < t && p < end; ) *p++ = *s++;
            break;
        }
        case 's': {
            const char* s = __builtin_va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s && p < end) *p++ = *s++;
            break;
        }
        case 'c': {
            int c = __builtin_va_arg(ap, int);
            *p++ = static_cast<char>(c);
            break;
        }
        default:
            if (p < end) *p++ = '%';
            if (p < end) *p++ = *fmt;
            break;
        }
        fmt++;
    }
    *p = '\0';
    __builtin_va_end(ap);
    return static_cast<uint32_t>(p - buf);
}

// ---- Procfs vnode types ----

// A procfs vnode holds a dynamically generated buffer.
struct ProcPriv {
    char*    data;
    uint32_t size;
    bool     isDir;
};

// VnodeOps for procfs files
static int ProcFileOpen(Vnode*, int) { return 0; }

static int ProcFileRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (!pp || !pp->data) return 0;
    if (*offset >= pp->size) return 0;
    uint64_t avail = pp->size - *offset;
    if (len > avail) len = avail;
    auto* src = reinterpret_cast<const uint8_t*>(pp->data + *offset);
    auto* dst = static_cast<uint8_t*>(buf);
    for (uint64_t i = 0; i < len; ++i) dst[i] = src[i];
    *offset += len;
    return static_cast<int>(len);
}

static int ProcFileWrite(Vnode*, const void*, uint64_t, uint64_t*) { return -1; }

static void ProcFileClose(Vnode* vn)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (pp) { kfree(pp->data); kfree(pp); }
}

static int ProcFileStat(Vnode* vn, VnodeStat* st)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    st->size = pp ? pp->size : 0;
    st->isDir = false;
    return 0;
}

static VnodeOps g_procFileOps = {
    ProcFileOpen, ProcFileRead, ProcFileWrite, nullptr, ProcFileClose, ProcFileStat
};

// VnodeOps for procfs directories (readdir support)
static int ProcDirRead(Vnode*, void*, uint64_t, uint64_t*) { return -1; }
static void ProcDirClose(Vnode* vn)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (pp) { kfree(pp->data); kfree(pp); }
}

// Forward declare readdir
static int ProcRootReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie);
static int ProcPidReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie);

static int ProcDirStat(Vnode* vn, VnodeStat* st)
{
    st->size = 0;
    st->isDir = true;
    return 0;
}

static VnodeOps g_procRootDirOps = {
    ProcFileOpen, ProcDirRead, ProcFileWrite, ProcRootReaddir, ProcDirClose, ProcDirStat
};

static VnodeOps g_procPidDirOps = {
    ProcFileOpen, ProcDirRead, ProcFileWrite, ProcPidReaddir, ProcDirClose, ProcDirStat
};

// ---- Helper: create a procfs vnode from a buffer ----

static Vnode* MakeProcVnode(char* data, uint32_t size, bool isDir = false, VnodeOps* ops = nullptr)
{
    auto* pp = static_cast<ProcPriv*>(kmalloc(sizeof(ProcPriv)));
    if (!pp) { kfree(data); return nullptr; }
    pp->data = data;
    pp->size = size;
    pp->isDir = isDir;

    auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
    if (!vn) { kfree(data); kfree(pp); return nullptr; }
    vn->ops = ops ? ops : &g_procFileOps;
    vn->type = isDir ? VnodeType::Dir : VnodeType::File;
    vn->priv = pp;
    vn->refCount = 1;
    return vn;
}

// ---- Content generators ----

static const char* StateStr(ProcessState s)
{
    switch (s) {
        case ProcessState::Ready:      return "R";
        case ProcessState::Running:    return "R";
        case ProcessState::Blocked:    return "S";
        case ProcessState::Stopped:    return "T";
        case ProcessState::Terminated: return "Z";
        default: return "?";
    }
}

static char StateChar(ProcessState s)
{
    switch (s) {
        case ProcessState::Ready:      return 'R';
        case ProcessState::Running:    return 'R';
        case ProcessState::Blocked:    return 'S';
        case ProcessState::Stopped:    return 'T';
        case ProcessState::Terminated: return 'Z';
        default: return '?';
    }
}

// /proc/stat — CPU time statistics
static Vnode* GenStat()
{
    auto* buf = static_cast<char*>(kmalloc(1024));
    if (!buf) return nullptr;

    uint64_t ticks = g_lapicTickCount;
    // Approximate: 1 tick ≈ 1ms, report as jiffies (centiseconds)
    uint64_t user = ticks / 20;    // rough approximation
    uint64_t sys  = ticks / 20;
    uint64_t idle = ticks / 10;

    uint32_t n = ProcFmt(buf, 1024,
        "cpu  %lu %lu %lu %lu 0 0 0 0 0 0\n"
        "cpu0 %lu %lu %lu %lu 0 0 0 0 0 0\n"
        "intr 0\n"
        "ctxt 0\n"
        "btime %lu\n"
        "processes %lu\n"
        "procs_running 1\n"
        "procs_blocked 0\n",
        user, 0UL, sys, idle,
        user, 0UL, sys, idle,
        0UL,  // btime
        ticks / 1000);  // rough process count

    return MakeProcVnode(buf, n);
}

// /proc/meminfo
static Vnode* GenMeminfo()
{
    auto* buf = static_cast<char*>(kmalloc(1024));
    if (!buf) return nullptr;

    uint64_t totalPages = PmmGetTotalPageCount();
    uint64_t freePages  = PmmGetFreePageCount();
    uint64_t totalKB = totalPages * 4;  // 4KB pages
    uint64_t freeKB  = freePages * 4;

    uint32_t n = ProcFmt(buf, 1024,
        "MemTotal:       %lu kB\n"
        "MemFree:        %lu kB\n"
        "MemAvailable:   %lu kB\n"
        "Buffers:        0 kB\n"
        "Cached:         0 kB\n"
        "SwapCached:     0 kB\n"
        "SwapTotal:      0 kB\n"
        "SwapFree:       0 kB\n",
        totalKB, freeKB, freeKB);

    return MakeProcVnode(buf, n);
}

// /proc/uptime — seconds idle_seconds
static Vnode* GenUptime()
{
    auto* buf = static_cast<char*>(kmalloc(64));
    if (!buf) return nullptr;

    uint64_t ticks = g_lapicTickCount;
    uint64_t secs = ticks / 1000;
    uint64_t frac = (ticks % 1000) / 10;  // centiseconds

    uint32_t n = ProcFmt(buf, 64, "%lu.%lu %lu.%lu\n",
        secs, frac, secs, frac);
    return MakeProcVnode(buf, n);
}

// /proc/version
static Vnode* GenVersion()
{
    auto* buf = static_cast<char*>(kmalloc(128));
    if (!buf) return nullptr;

    uint32_t n = ProcFmt(buf, 128,
        "Brook OS version 0.1 (clang) #1 SMP\n");
    return MakeProcVnode(buf, n);
}

// /proc/loadavg
static Vnode* GenLoadavg()
{
    auto* buf = static_cast<char*>(kmalloc(64));
    if (!buf) return nullptr;

    uint32_t n = ProcFmt(buf, 64, "0.00 0.00 0.00 1/1 1\n");
    return MakeProcVnode(buf, n);
}

// /proc/[pid]/stat — single-line process stat
static Vnode* GenPidStat(const ProcessSnapshot& proc)
{
    auto* buf = static_cast<char*>(kmalloc(512));
    if (!buf) return nullptr;

    // Format: pid (name) state ppid pgid sid tty_nr tpgid flags
    //         minflt cminflt majflt cmajflt utime stime cutime cstime
    //         priority nice num_threads itrealvalue starttime vsize rss ...
    uint64_t vsize = proc.programBreak > 0 ? proc.programBreak : 0;
    uint64_t rss = (proc.stackTop - proc.stackBase) / 4096;  // rough RSS in pages

    uint32_t n = ProcFmt(buf, 512,
        "%u (%s) %c %u %u %u 0 -1 0 "
        "0 0 0 0 0 0 0 0 "
        "20 0 1 0 0 %lu %lu "
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
        (uint32_t)proc.pid, proc.name, StateChar(proc.state),
        (uint32_t)proc.parentPid, (uint32_t)proc.pgid, (uint32_t)proc.sid,
        vsize, rss);

    return MakeProcVnode(buf, n);
}

// /proc/[pid]/status — human-readable
static Vnode* GenPidStatus(const ProcessSnapshot& proc)
{
    auto* buf = static_cast<char*>(kmalloc(512));
    if (!buf) return nullptr;

    uint64_t vsize = proc.programBreak > 0 ? proc.programBreak : 0;

    uint32_t n = ProcFmt(buf, 512,
        "Name:\t%s\n"
        "State:\t%s (%s)\n"
        "Tgid:\t%u\n"
        "Pid:\t%u\n"
        "PPid:\t%u\n"
        "VmSize:\t%lu kB\n"
        "VmRSS:\t%lu kB\n"
        "Threads:\t1\n",
        proc.name,
        StateStr(proc.state), proc.name,
        (uint32_t)proc.pid,
        (uint32_t)proc.pid,
        (uint32_t)proc.parentPid,
        vsize / 1024,
        (proc.stackTop - proc.stackBase) / 1024);

    return MakeProcVnode(buf, n);
}

// /proc/[pid]/cmdline — NUL-terminated command name
static Vnode* GenPidCmdline(const ProcessSnapshot& proc)
{
    uint32_t len = ProcStrLen(proc.name);
    auto* buf = static_cast<char*>(kmalloc(len + 1));
    if (!buf) return nullptr;
    ProcStrCopy(buf, proc.name, len + 1);
    return MakeProcVnode(buf, len + 1);  // include NUL terminator
}

// ---- Path parsing helpers ----

static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

static uint32_t ParseUint(const char* s, const char** end)
{
    uint32_t val = 0;
    while (IsDigit(*s)) { val = val * 10 + (*s - '0'); s++; }
    if (end) *end = s;
    return val;
}

// Check if a path component is a numeric PID
static bool IsPidPath(const char* path, uint32_t* pid, const char** rest)
{
    if (!path || !IsDigit(path[0])) return false;
    const char* end;
    *pid = ParseUint(path, &end);
    if (*end == '\0') { *rest = ""; return true; }
    if (*end == '/') { *rest = end + 1; return true; }
    return false;
}

// Find a process snapshot by PID
static bool FindProcess(uint32_t pid, ProcessSnapshot* out)
{
    ProcessSnapshot snaps[MAX_PROCESSES];
    uint32_t count = SchedulerSnapshotProcesses(snaps, MAX_PROCESSES);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (snaps[i].pid == pid) { *out = snaps[i]; return true; }
    }
    return false;
}

// ---- Global file table ----

struct ProcGlobalEntry {
    const char* name;
    Vnode* (*gen)();
};

static ProcGlobalEntry g_globalEntries[] = {
    { "stat",    GenStat },
    { "meminfo", GenMeminfo },
    { "uptime",  GenUptime },
    { "version", GenVersion },
    { "loadavg", GenLoadavg },
};
static constexpr uint32_t NUM_GLOBAL = sizeof(g_globalEntries) / sizeof(g_globalEntries[0]);

// Per-PID file table
struct ProcPidEntry {
    const char* name;
    Vnode* (*gen)(const ProcessSnapshot&);
};

static ProcPidEntry g_pidEntries[] = {
    { "stat",    GenPidStat },
    { "status",  GenPidStatus },
    { "cmdline", GenPidCmdline },
};
static constexpr uint32_t NUM_PID_ENTRIES = sizeof(g_pidEntries) / sizeof(g_pidEntries[0]);

static bool StrEqProc(const char* a, const char* b)
{
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

// ---- Public API ----

void ProcFsInit()
{
    SerialPuts("procfs: initialised\n");
}

Vnode* ProcFsOpen(const char* relPath, int /*flags*/)
{
    if (!relPath || !relPath[0]) return nullptr;

    // Handle "self" → current process PID
    char resolved[64];
    if (relPath[0] == 's' && relPath[1] == 'e' && relPath[2] == 'l' && relPath[3] == 'f')
    {
        Process* cur = SchedulerCurrentProcess();
        if (!cur) return nullptr;
        char* p = U64ToDec(resolved, cur->pid);
        const char* rest = relPath + 4;
        if (*rest == '/') { while (*rest && p < resolved + 60) *p++ = *rest++; }
        *p = '\0';
        relPath = resolved;
    }

    // Check global files
    for (uint32_t i = 0; i < NUM_GLOBAL; ++i)
    {
        if (StrEqProc(relPath, g_globalEntries[i].name))
            return g_globalEntries[i].gen();
    }

    // Check pid paths: "PID" (directory) or "PID/file"
    uint32_t pid;
    const char* rest;
    if (IsPidPath(relPath, &pid, &rest))
    {
        ProcessSnapshot snap = {};
        if (!FindProcess(pid, &snap)) return nullptr;

        // "PID" with no subpath → open directory
        if (!rest[0]) return ProcFsOpenDir(relPath);

        // "PID/file"
        for (uint32_t i = 0; i < NUM_PID_ENTRIES; ++i)
        {
            if (StrEqProc(rest, g_pidEntries[i].name))
                return g_pidEntries[i].gen(snap);
        }
    }

    return nullptr;
}

int ProcFsStatPath(const char* relPath, VnodeStat* st)
{
    if (!relPath || !relPath[0]) {
        st->size = 0; st->isDir = true; return 0;
    }

    // Handle "self" → current process PID
    char resolved[64];
    if (relPath[0] == 's' && relPath[1] == 'e' && relPath[2] == 'l' && relPath[3] == 'f')
    {
        Process* cur = SchedulerCurrentProcess();
        if (!cur) return -1;
        char* p = U64ToDec(resolved, cur->pid);
        const char* rest = relPath + 4;
        if (*rest == '/') { while (*rest && p < resolved + 60) *p++ = *rest++; }
        *p = '\0';
        relPath = resolved;
    }

    // Global files
    for (uint32_t i = 0; i < NUM_GLOBAL; ++i)
    {
        if (StrEqProc(relPath, g_globalEntries[i].name))
        {
            st->size = 0;  // dynamic content — size unknown
            st->isDir = false;
            return 0;
        }
    }

    // PID paths
    uint32_t pid;
    const char* rest;
    if (IsPidPath(relPath, &pid, &rest))
    {
        ProcessSnapshot snap;
        if (!FindProcess(pid, &snap)) return -1;

        if (!rest[0]) {
            st->size = 0; st->isDir = true; return 0;
        }

        for (uint32_t i = 0; i < NUM_PID_ENTRIES; ++i)
        {
            if (StrEqProc(rest, g_pidEntries[i].name))
            {
                st->size = 0;
                st->isDir = false;
                return 0;
            }
        }
    }

    return -1;  // not found
}

Vnode* ProcFsOpenDir(const char* relPath)
{
    // Determine if this is /proc root or /proc/PID
    uint32_t pid = 0;
    const char* rest = nullptr;
    bool isPidDir = relPath && relPath[0] && IsPidPath(relPath, &pid, &rest) && (!rest || !rest[0]);

    if (isPidDir)
    {
        // Check process exists
        ProcessSnapshot snap;
        if (!FindProcess(pid, &snap)) return nullptr;

        // Store PID in data buffer for readdir
        auto* buf = static_cast<char*>(kmalloc(8));
        if (!buf) return nullptr;
        char* p = U64ToDec(buf, pid);
        *p = '\0';
        return MakeProcVnode(buf, static_cast<uint32_t>(p - buf), true, &g_procPidDirOps);
    }

    // /proc root directory
    return MakeProcVnode(nullptr, 0, true, &g_procRootDirOps);
}

// ---- Readdir implementations ----

// /proc root: list global files + PID directories
static int ProcRootReaddir(Vnode* /*vn*/, DirEntry* out, uint32_t* cookie)
{
    uint32_t idx = *cookie;

    // First: global files
    if (idx < NUM_GLOBAL)
    {
        ProcStrCopy(out->name, g_globalEntries[idx].name, sizeof(out->name));
        out->size = 0;
        out->isDir = false;
        *cookie = idx + 1;
        return 1;
    }

    // Then: PID directories
    uint32_t pidIdx = idx - NUM_GLOBAL;
    ProcessSnapshot snaps[MAX_PROCESSES];
    uint32_t count = SchedulerSnapshotProcesses(snaps, MAX_PROCESSES);

    if (pidIdx >= count) return 0;  // end of directory

    char pidStr[8];
    char* p = U64ToDec(pidStr, snaps[pidIdx].pid);
    *p = '\0';
    ProcStrCopy(out->name, pidStr, sizeof(out->name));
    out->size = 0;
    out->isDir = true;
    *cookie = idx + 1;
    return 1;
}

// /proc/PID directory: list per-process files
static int ProcPidReaddir(Vnode* /*vn*/, DirEntry* out, uint32_t* cookie)
{
    uint32_t idx = *cookie;
    if (idx >= NUM_PID_ENTRIES) return 0;

    ProcStrCopy(out->name, g_pidEntries[idx].name, sizeof(out->name));
    out->size = 0;
    out->isDir = false;
    *cookie = idx + 1;
    return 1;
}

} // namespace brook
