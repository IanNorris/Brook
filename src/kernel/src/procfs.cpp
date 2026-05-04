// procfs — virtual /proc filesystem for Brook OS
//
// Provides read-only process and system information compatible with the
// Linux /proc layout that busybox ps/top expect.

#include "procfs.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include "smp.h"
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

static char HexDigit(uint8_t n)
{
    return static_cast<char>(n < 10 ? '0' + n : 'a' + (n - 10));
}

static char* U64ToHex(char* buf, uint64_t val)
{
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t n = static_cast<uint8_t>((val >> shift) & 0xF);
        if (!started && n == 0 && shift > 0) continue;
        started = true;
        *buf++ = HexDigit(n);
    }
    return buf;
}

static char* AppendStr(char* p, const char* s)
{
    while (*s) *p++ = *s++;
    return p;
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
    uint32_t cpuCount = SmpGetCpuCount();
    if (cpuCount == 0) cpuCount = 1;

    // Allocate enough for summary + per-CPU lines
    uint32_t bufSize = 512 + cpuCount * 80;
    auto* buf = static_cast<char*>(kmalloc(bufSize));
    if (!buf) return nullptr;

    // Sum actual CPU time from all processes
    ProcessSnapshot snaps[MAX_PROCESSES];
    uint32_t count = SchedulerSnapshotProcesses(snaps, MAX_PROCESSES);
    uint64_t totalUser = 0, totalSys = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        totalUser += snaps[i].userTicks;
        totalSys  += snaps[i].sysTicks;
    }

    uint64_t ticks = g_lapicTickCount;
    // Convert ms ticks to centiseconds (HZ=100)
    uint64_t user = totalUser / 10;
    uint64_t sys  = totalSys / 10;
    uint64_t idle = (ticks - totalUser - totalSys) / 10;
    if (idle > ticks / 10) idle = 0; // underflow guard

    // Summary line (aggregate across all CPUs)
    uint32_t n = ProcFmt(buf, bufSize,
        "cpu  %lu %lu %lu %lu 0 0 0 0 0 0\n",
        user, 0UL, sys, idle);

    // Per-CPU lines (split evenly for now — we don't track per-CPU time yet)
    for (uint32_t c = 0; c < cpuCount; ++c)
    {
        uint64_t perUser = user / cpuCount;
        uint64_t perSys  = sys / cpuCount;
        uint64_t perIdle = idle / cpuCount;
        n += ProcFmt(buf + n, bufSize - n,
            "cpu%u %lu %lu %lu %lu 0 0 0 0 0 0\n",
            c, perUser, 0UL, perSys, perIdle);
    }

    n += ProcFmt(buf + n, bufSize - n,
        "intr 0\n"
        "ctxt 0\n"
        "btime %lu\n"
        "processes %lu\n"
        "procs_running 1\n"
        "procs_blocked 0\n",
        0UL,  // btime
        (uint64_t)count);

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

// /proc/cpuinfo — per-CPU information including feature flags.
// Many apps (VLC, FFmpeg, glibc ifunc) read this for CPU capabilities.
static Vnode* GenCpuinfo()
{
    uint32_t cpuCount = SmpGetCpuCount();
    if (cpuCount == 0) cpuCount = 1;

    // Query CPUID for feature flags
    uint32_t eax, ebx, ecx, edx;

    // Leaf 0: vendor string
    char vendor[13] = {};
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    __builtin_memcpy(vendor + 0, &ebx, 4);
    __builtin_memcpy(vendor + 4, &edx, 4);
    __builtin_memcpy(vendor + 8, &ecx, 4);

    // Leaf 1: feature bits
    uint32_t feat_ecx = 0, feat_edx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    feat_ecx = ecx;
    feat_edx = edx;
    uint32_t family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    uint32_t model  = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
    uint32_t stepping = eax & 0xF;

    // Leaf 7: extended features (AVX2, BMI, etc.)
    uint32_t leaf7_ebx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    leaf7_ebx = ebx;

    // Build flags string
    char flags[512];
    char* fp = flags;
    auto addFlag = [&](bool cond, const char* name) {
        if (!cond) return;
        if (fp > flags) *fp++ = ' ';
        while (*name) *fp++ = *name++;
    };
    // EDX flags
    addFlag(feat_edx & (1U << 0),  "fpu");
    addFlag(feat_edx & (1U << 4),  "tsc");
    addFlag(feat_edx & (1U << 5),  "msr");
    addFlag(feat_edx & (1U << 8),  "cx8");
    addFlag(feat_edx & (1U << 15), "cmov");
    addFlag(feat_edx & (1U << 19), "clflush");
    addFlag(feat_edx & (1U << 23), "mmx");
    addFlag(feat_edx & (1U << 24), "fxsr");
    addFlag(feat_edx & (1U << 25), "sse");
    addFlag(feat_edx & (1U << 26), "sse2");
    addFlag(feat_edx & (1U << 28), "ht");
    // ECX flags
    addFlag(feat_ecx & (1U << 0),  "sse3");
    addFlag(feat_ecx & (1U << 1),  "pclmulqdq");
    addFlag(feat_ecx & (1U << 9),  "ssse3");
    addFlag(feat_ecx & (1U << 12), "fma");
    addFlag(feat_ecx & (1U << 13), "cx16");
    addFlag(feat_ecx & (1U << 19), "sse4_1");
    addFlag(feat_ecx & (1U << 20), "sse4_2");
    addFlag(feat_ecx & (1U << 22), "movbe");
    addFlag(feat_ecx & (1U << 23), "popcnt");
    addFlag(feat_ecx & (1U << 25), "aes");
    addFlag(feat_ecx & (1U << 26), "xsave");
    addFlag(feat_ecx & (1U << 28), "avx");
    addFlag(feat_ecx & (1U << 29), "f16c");
    addFlag(feat_ecx & (1U << 30), "rdrand");
    // Leaf 7 EBX
    addFlag(leaf7_ebx & (1U << 3),  "bmi1");
    addFlag(leaf7_ebx & (1U << 5),  "avx2");
    addFlag(leaf7_ebx & (1U << 8),  "bmi2");
    *fp = '\0';

    // Allocate output: ~350 bytes per CPU entry
    uint32_t perCpu = 512;
    uint32_t bufSize = cpuCount * perCpu;
    auto* buf = static_cast<char*>(kmalloc(bufSize));
    if (!buf) return nullptr;

    uint32_t n = 0;
    for (uint32_t i = 0; i < cpuCount && n < bufSize - perCpu; i++) {
        n += ProcFmt(buf + n, bufSize - n,
            "processor\t: %lu\n"
            "vendor_id\t: %s\n"
            "cpu family\t: %lu\n"
            "model\t\t: %lu\n"
            "stepping\t: %lu\n"
            "cpu MHz\t\t: 3000.000\n"
            "physical id\t: 0\n"
            "siblings\t: %lu\n"
            "core id\t\t: %lu\n"
            "cpu cores\t: %lu\n"
            "flags\t\t: %s\n"
            "bogomips\t: 6000.00\n"
            "clflush size\t: 64\n"
            "cache_alignment\t: 64\n"
            "address sizes\t: 48 bits physical, 48 bits virtual\n\n",
            (uint64_t)i, vendor, (uint64_t)family, (uint64_t)model,
            (uint64_t)stepping, (uint64_t)cpuCount, (uint64_t)i,
            (uint64_t)cpuCount, flags);
    }

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

    // Convert LAPIC ticks (~1ms each) to Linux clock ticks (HZ=100, 10ms each)
    uint64_t utime = proc.userTicks / 10;
    uint64_t stime = proc.sysTicks / 10;

    uint32_t n = ProcFmt(buf, 512,
        "%u (%s) %c %u %u %u 0 -1 0 "
        "0 0 0 0 %lu %lu 0 0 "
        "20 0 1 0 0 %lu %lu "
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
        "0 0 0 0 0 0 0 0 0 0\n",
        (uint32_t)proc.pid, proc.name, StateChar(proc.state),
        (uint32_t)proc.parentPid, (uint32_t)proc.pgid, (uint32_t)proc.sid,
        utime, stime,
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

// /proc/[pid]/statm — memory usage in pages
// Format: size resident shared text lib data dt
static Vnode* GenPidStatm(const ProcessSnapshot& proc)
{
    auto* buf = static_cast<char*>(kmalloc(128));
    if (!buf) return nullptr;

    uint64_t size = proc.programBreak > 0 ? proc.programBreak / 4096 : 0;
    uint64_t resident = (proc.stackTop - proc.stackBase) / 4096;

    uint32_t n = ProcFmt(buf, 128, "%lu %lu 0 0 0 %lu 0\n",
        size, resident, resident);

    return MakeProcVnode(buf, n);
}

// /proc/[pid]/maps — minimal Linux-style VM map.
// glibc's pthread_getattr_np() consults this for the initial thread's stack.
static Vnode* GenPidMaps(const ProcessSnapshot& proc)
{
    auto* buf = static_cast<char*>(kmalloc(256));
    if (!buf) return nullptr;

    char* p = buf;
    p = U64ToHex(p, proc.stackBase);
    *p++ = '-';
    p = U64ToHex(p, USER_STACK_TOP);
    p = AppendStr(p, " rw-p 00000000 00:00 0                          [stack]\n");
    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
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
    { "cpuinfo", GenCpuinfo },
};
static constexpr uint32_t NUM_GLOBAL = sizeof(g_globalEntries) / sizeof(g_globalEntries[0]);

// Per-PID file table
struct ProcPidEntry {
    const char* name;
    Vnode* (*gen)(const ProcessSnapshot&);
};

static ProcPidEntry g_pidEntries[] = {
    { "stat",    GenPidStat },
    { "statm",   GenPidStatm },
    { "status",  GenPidStatus },
    { "cmdline", GenPidCmdline },
    { "maps",    GenPidMaps },
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
