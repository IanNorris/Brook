#include "process_user_namespace.h"
#include "process_fs.h"
#include "mount_namespace.h"
#include "mount_info_format.h"
#include "spinlock.h"
#include "vfs_object.h"
#include "apic.h"
// procfs — virtual /proc filesystem for Brook OS
//
// Provides read-only process and system information compatible with the
// Linux /proc layout that busybox ps/top expect.

#include "procfs.h"
#include "proc_inode.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include "smp.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "memory/user_mapping.h"
#include "exec_limits.h"
#include "user_copy.h"
#include "serial.h"
#include "net.h"
#include "module.h"
#include "virtio_blk.h"
#include "string.h"

namespace brook {


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

static char* AppendU64(char* p, uint64_t val)
{
    char tmp[21];
    char* end = U64ToDec(tmp, val);
    *end = '\0';
    return AppendStr(p, tmp);
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
    uint16_t pid;
    uint16_t tgid;
    uint64_t incarnation;
    uint64_t groupIncarnation;
    bool taskRoot;
    bool taskMember;
    UserNamespace* userNamespace;
    UserAuthority openedAuthority;
    uint8_t mapKind; // 1 UID, 2 GID, 3 setgroups; independent namespace lease
    bool sysKernelDirectory;
    bool commandLine;
    uint64_t reportedOwner;
};

static int ProcReportedInode(VfsObject*,uint64_t*);
static int ProcOpenedIdentity(Vnode*,VnodeStat*);

// VnodeOps for procfs files
static int ProcFileOpen(Vnode*, int) { return 0; }

static int ProcFileRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (pp && pp->commandLine) {
        struct ReadOperation {
            Process* actor=ProcessCurrent();
            ReadOperation() { if(actor)SchedulerKernelRegionEnter(actor); }
            ~ReadOperation() { if(actor)SchedulerKernelRegionLeave(actor); }
        } operation;
        if (!len) return 0;
        bool exists=false;
        auto* space=SchedulerReferenceProcessAddressSpace(pp->pid,pp->incarnation,&exists);
        if (!space) return exists?0:-3; // zombie/kernel EOF, departed incarnation ESRCH
        const uint32_t capacity=len>EXEC_ARG_BYTES?EXEC_ARG_BYTES:static_cast<uint32_t>(len);
        auto* staged=static_cast<uint8_t*>(kmalloc(capacity));
        if (!staged) { AddressSpaceUnref(space);return -12; }
        const uint32_t copied=UserMappingReadArguments(space,*offset,staged,capacity);
        AddressSpaceUnref(space);
        // Destination can fault; no foreign AS gate or frame reference is held.
        uint32_t delivered=copied;
        if (reinterpret_cast<uint64_t>(buf)<0x0000800000000000ULL)
            delivered=CopyToUserBytes(reinterpret_cast<uint64_t>(buf),staged,copied);
        else memcpy(buf,staged,copied);
        kfree(staged);*offset+=delivered;
        return delivered?static_cast<int>(delivered):copied?-14:0;
    }
    if (!pp || !pp->data) return 0;
    if (*offset >= pp->size) return 0;
    uint64_t avail = pp->size - *offset;
    if (len > avail) len = avail;
    auto* src = reinterpret_cast<const uint8_t*>(pp->data + *offset);
    auto* dst = static_cast<uint8_t*>(buf);
    memcpy(dst, src, len);
    *offset += len;
    return static_cast<int>(len);
}

static int ProcFileWrite(Vnode*, const void*, uint64_t, uint64_t*) { return -1; }

static void ProcFileClose(Vnode* vn)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (pp) { UserNamespaceRelease(pp->userNamespace);UserNamespaceRelease(pp->openedAuthority.ns);kfree(pp->data); kfree(pp); }
}

static int ProcFileStat(Vnode* vn, VnodeStat* st)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    st->size = pp ? pp->size : 0;
    st->isDir = false;st->mode=0100444;st->dev=0x70726f63;
    st->nlink=1;
    if(vn->backendObject)if(int error=ProcOpenedIdentity(vn,st))return error;
    return 0;
}

static VnodeOps g_procFileOps = {
    ProcFileOpen, ProcFileRead, ProcFileWrite, nullptr, ProcFileClose, ProcFileStat, nullptr
};

// VnodeOps for procfs directories (readdir support)
static int ProcDirRead(Vnode*, void*, uint64_t, uint64_t*) { return -1; }
static void ProcDirClose(Vnode* vn)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (pp) { UserNamespaceRelease(pp->userNamespace);UserNamespaceRelease(pp->openedAuthority.ns);kfree(pp->data); kfree(pp); }
}

// Forward declare readdir
static int ProcRootReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie);
static int ProcPidReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie);
static int ProcFdReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie);
static Vnode* GenPidFdLink(uint16_t pid, int fd, uint64_t incarnation = 0);
static int ProcTaskReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie);

static int ProcDirStat(Vnode* vn, VnodeStat* st)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (pp && pp->pid)
    {
        ProcessSnapshot snap;
        if (!SchedulerSnapshotProcess(pp->pid, &snap) || snap.incarnation != pp->incarnation ||
            (pp->taskMember && snap.state == ProcessState::Terminated)) return -1;
        st->ino = (pp->incarnation << 20) | (uint64_t{pp->pid} << 4) | (pp->taskRoot ? 2 : 1);
        st->nlink = pp->taskRoot ? 2 + SchedulerCountTasks(pp->tgid, pp->groupIncarnation) : 2;
    }
    else st->nlink = 2;
    st->size = 0;
    st->isDir = true;st->mode=0040555;st->dev=0x70726f63;
    if(vn->backendObject)if(int error=ProcOpenedIdentity(vn,st))return error;
    return 0;
}

static VnodeOps g_procFdDirOps = {
    ProcFileOpen, ProcDirRead, ProcFileWrite, ProcFdReaddir, ProcDirClose, ProcDirStat, nullptr
};

static VnodeOps g_procRootDirOps = {
    ProcFileOpen, ProcDirRead, ProcFileWrite, ProcRootReaddir, ProcDirClose, ProcDirStat, nullptr
};

static VnodeOps g_procPidDirOps = {
    ProcFileOpen, ProcDirRead, ProcFileWrite, ProcPidReaddir, ProcDirClose, ProcDirStat, nullptr
};

static VnodeOps g_procTaskDirOps = {
    ProcFileOpen, ProcDirRead, ProcFileWrite, ProcTaskReaddir, ProcDirClose, ProcDirStat, nullptr
};

// ---- Helper: create a procfs vnode from a buffer ----

static Vnode* MakeProcVnode(char* data, uint32_t size, bool isDir = false, VnodeOps* ops = nullptr)
{
    auto* pp = static_cast<ProcPriv*>(kmalloc(sizeof(ProcPriv)));
    if (!pp) { kfree(data); return nullptr; }
    *pp = {};
    pp->data = data;
    pp->size = size;
    pp->isDir = isDir;

    auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
    if (!vn) { kfree(data); kfree(pp); return nullptr; }
    vn->streamPipe=nullptr;vn->streamOwner=nullptr;vn->streamRelease=nullptr;
        vn->directoryPath = nullptr;
    vn->mountOwner = nullptr;
    vn->backendObject = nullptr;
    vn->ops = ops ? ops : &g_procFileOps;
    vn->type = isDir ? VnodeType::Dir : VnodeType::File;
    vn->priv = pp;
    vn->refCount = 1; vn->cacheId = 0;
    return vn;
}


static int ProcMapRead(Vnode* vn,void* data,uint64_t length,uint64_t* offset) {
    auto* pp=static_cast<ProcPriv*>(vn->priv);char text[80];
    uint32_t size=ProcessUserMapRead(pp->userNamespace,pp->openedAuthority.ns,pp->mapKind==2,pp->mapKind==3,text,sizeof(text));
    if(*offset>=size)return 0;if(length>size-*offset)length=size-*offset;
    memcpy(data,text+*offset,length);*offset+=length;return int(length);
}
static int ProcMapWrite(Vnode* vn,const void* data,uint64_t length,uint64_t* offset) {
    if(*offset || !length || length>255)return -22;
    char text[256];memcpy(text,data,length);
    auto* pp=static_cast<ProcPriv*>(vn->priv);
    int error=ProcessUserMapWrite(pp->userNamespace,pp->openedAuthority,text,length,pp->mapKind==2,pp->mapKind==3);
    if(error)return error;*offset+=length;return int(length);
}
static int ProcMapStat(Vnode* vn,VnodeStat* st) {
    int result=ProcFileStat(vn,st);auto* pp=static_cast<ProcPriv*>(vn->priv);
    st->mode=0100644;st->uid=pp->userNamespace?pp->userNamespace->owner:0;return result;
}
static VnodeOps g_procMapOps={ProcFileOpen,ProcMapRead,ProcMapWrite,nullptr,ProcFileClose,ProcMapStat,nullptr};

// Read-only reporting of Brook's fixed unmapped-ID policy. These values are
// shared with credential conversion, not mutable sysctl compatibility stubs.
static Vnode* GenOverflowId()
{
    char* text=static_cast<char*>(kmalloc(16));if(!text)return nullptr;
    char* end=U64ToDec(text,UserIdOverflow);*end++='\n';*end=0;
    return MakeProcVnode(text,uint32_t(end-text));
}
static int ProcSysReaddir(Vnode* node,DirEntry* out,uint32_t* cookie)
{
    const bool kernel=static_cast<ProcPriv*>(node->priv)->sysKernelDirectory;
    const char* names[]={"overflowuid","overflowgid"};
    if(*cookie>=(kernel?2u:1u))return 0;
    *out={};ProcStrCopy(out->name,kernel?names[*cookie]:"kernel",sizeof(out->name));
    out->isDir=!kernel;++*cookie;return 1;
}
static VnodeOps g_procSysDirOps={ProcFileOpen,ProcDirRead,ProcFileWrite,ProcSysReaddir,ProcDirClose,ProcDirStat,nullptr};
static Vnode* MakeProcSysDirectory(bool kernel)
{
    auto* node=MakeProcVnode(nullptr,0,true,&g_procSysDirOps);
    if(node)static_cast<ProcPriv*>(node->priv)->sysKernelDirectory=kernel;
    return node;
}
static bool ProcOverflowPath(const char* path)
{
    return !strcmp(path,"sys/kernel/overflowuid") || !strcmp(path,"sys/kernel/overflowgid");
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

    // Sum per-CPU counters for both busy (user+system) and idle.
    // Using per-CPU counters avoids a 153KB stack allocation for process snapshots.
    uint64_t totalBusy = 0, totalIdle = 0;
    for (uint32_t c = 0; c < cpuCount; ++c)
    {
        uint64_t cpuBusy = 0, cpuIdle = 0;
        SchedulerGetCpuTicks(c, cpuBusy, cpuIdle);
        totalBusy += cpuBusy;
        totalIdle += cpuIdle;
    }

    // Convert ms ticks to centiseconds (HZ=100)
    uint64_t user = totalBusy / 10;
    uint64_t idle = totalIdle / 10;

    // Summary line (aggregate across all CPUs)
    uint32_t n = ProcFmt(buf, bufSize,
        "cpu  %lu %lu %lu %lu 0 0 0 0 0 0\n",
        user, 0UL, 0UL, idle);

    // Per-CPU lines with actual per-CPU tick counters
    for (uint32_t c = 0; c < cpuCount; ++c)
    {
        uint64_t cpuBusy = 0, cpuIdle = 0;
        SchedulerGetCpuTicks(c, cpuBusy, cpuIdle);
        // Convert ms ticks to centiseconds (HZ=100)
        uint64_t perUser = cpuBusy / 10;
        uint64_t perIdle = cpuIdle / 10;
        n += ProcFmt(buf + n, bufSize - n,
            "cpu%u %lu %lu %lu %lu 0 0 0 0 0 0\n",
            c, perUser, 0UL, 0UL, perIdle);
    }

    n += ProcFmt(buf + n, bufSize - n,
        "intr 0\n"
        "ctxt 0\n"
        "btime %lu\n"
        "processes %lu\n"
        "procs_running 1\n"
        "procs_blocked 0\n",
        0UL,  // btime
        SchedulerGetTotalForks());

    return MakeProcVnode(buf, n);
}

// /proc/meminfo
static Vnode* GenMeminfo()
{
    auto* buf = static_cast<char*>(kmalloc(1024));
    if (!buf) return nullptr;

    uint64_t totalPages = PmmGetRamPageCount();
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

    uint64_t ticks = ApicTickCount();
    uint64_t secs = ticks / 1000;
    uint64_t frac = (ticks % 1000) / 10;  // centiseconds

    // Idle time: sum idle ticks across all CPUs
    uint64_t totalIdle = 0;
    uint32_t cpuCount = SmpGetCpuCount();
    for (uint32_t i = 0; i < cpuCount; ++i)
    {
        uint64_t busy, idle;
        SchedulerGetCpuTicks(i, busy, idle);
        totalIdle += idle;
    }
    uint64_t idleSecs = totalIdle / 1000;
    uint64_t idleFrac = (totalIdle % 1000) / 10;

    // ProcFmt has no width/padding modifiers. Emit both centisecond digits
    // explicitly: printing 9 as ".9" would turn 0.09 seconds into 0.9.
    uint32_t n = ProcFmt(buf, 64, "%lu.%c%c %lu.%c%c\n",
        secs, static_cast<int>('0' + frac / 10), static_cast<int>('0' + frac % 10),
        idleSecs, static_cast<int>('0' + idleFrac / 10), static_cast<int>('0' + idleFrac % 10));
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

    uint32_t total = 0, running = 0;
    SchedulerGetProcessCounts(total, running);

    uint32_t avg1, avg5, avg15;
    SchedulerGetLoadAvg(avg1, avg5, avg15);

    // Format: 1min 5min 15min running/total lastpid
    // Values are fixed-point * 1000, so 1500 = "1.50"
    char* p = buf;
    p = AppendU64(p, avg1 / 1000); *p++ = '.';
    uint32_t frac1 = (avg1 % 1000) / 10;
    if (frac1 < 10) *p++ = '0';
    p = AppendU64(p, frac1);
    *p++ = ' ';
    p = AppendU64(p, avg5 / 1000); *p++ = '.';
    uint32_t frac5 = (avg5 % 1000) / 10;
    if (frac5 < 10) *p++ = '0';
    p = AppendU64(p, frac5);
    *p++ = ' ';
    p = AppendU64(p, avg15 / 1000); *p++ = '.';
    uint32_t frac15 = (avg15 % 1000) / 10;
    if (frac15 < 10) *p++ = '0';
    p = AppendU64(p, frac15);
    *p++ = ' ';
    p = AppendU64(p, running);
    *p++ = '/';
    p = AppendU64(p, total);
    *p++ = ' ';
    p = AppendU64(p, SchedulerGetTotalForks());
    *p++ = '\n';
    *p = '\0';

    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
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
    char* fEnd = flags + sizeof(flags) - 1;
    auto addFlag = [&](bool cond, const char* name) {
        if (!cond) return;
        if (fp > flags && fp < fEnd) *fp++ = ' ';
        while (*name && fp < fEnd) *fp++ = *name++;
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
        "20 0 %u 0 0 %lu %lu "
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
        "0 0 0 0 0 0 0 0 0 0\n",
        (uint32_t)proc.pid, proc.name, StateChar(proc.state),
        (uint32_t)proc.parentPid, (uint32_t)proc.pgid, (uint32_t)proc.sid,
        utime, stime, proc.threadCount,
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
        "Pgid:\t%u\n"
        "Sid:\t%u\n"
        "Uid:\t%u\t%u\t%u\t%u\n"
        "Gid:\t%u\t%u\t%u\t%u\n"
        "VmSize:\t%lu kB\n"
        "VmRSS:\t%lu kB\n"
        "Threads:\t%u\n",
        proc.name,
        StateStr(proc.state), proc.name,
        (uint32_t)proc.tgid,
        (uint32_t)proc.pid,
        (uint32_t)proc.parentPid,
        (uint32_t)proc.pgid,
        (uint32_t)proc.sid,
        ProcessReportId(ProcessCurrent(),false,proc.uid),ProcessReportId(ProcessCurrent(),false,proc.euid),
        ProcessReportId(ProcessCurrent(),false,proc.suid),ProcessReportId(ProcessCurrent(),false,proc.euid),
        ProcessReportId(ProcessCurrent(),true,proc.gid),ProcessReportId(ProcessCurrent(),true,proc.egid),
        ProcessReportId(ProcessCurrent(),true,proc.sgid),ProcessReportId(ProcessCurrent(),true,proc.egid),
        vsize / 1024,
        (proc.stackTop - proc.stackBase) / 1024, proc.threadCount);

    return MakeProcVnode(buf, n);
}

// /proc/[pid]/cmdline — live NUL-separated arguments, tied to PID incarnation.
static Vnode* GenPidCmdline(const ProcessSnapshot& proc)
{
    auto* node=MakeProcVnode(nullptr,0);
    if (node) {
        auto* pp=static_cast<ProcPriv*>(node->priv);
        pp->commandLine=true;pp->pid=proc.pid;pp->incarnation=proc.incarnation;
    }
    return node;
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

// /proc/[pid]/maps — Linux-style VM map showing stack and heap regions.
// glibc's pthread_getattr_np() consults this for the initial thread's stack.
static Vnode* GenPidMaps(const ProcessSnapshot& proc)
{
    auto* buf = static_cast<char*>(kmalloc(512));
    if (!buf) return nullptr;

    char* p = buf;

    // Heap region (program break)
    if (proc.programBreak > 0)
    {
        // The heap starts at the end of loaded ELF segments.
        // We approximate the heap start as programBreak rounded down to a reasonable base.
        uint64_t heapEnd = proc.programBreak;
        uint64_t heapStart = heapEnd > (64 * 1024) ? heapEnd - (64 * 1024) : 0x400000;
        p = U64ToHex(p, heapStart);
        *p++ = '-';
        p = U64ToHex(p, heapEnd);
        p = AppendStr(p, " rw-p 00000000 00:00 0                          [heap]\n");
    }

    // Stack region
    p = U64ToHex(p, proc.stackBase);
    *p++ = '-';
    p = U64ToHex(p, USER_STACK_TOP);
    p = AppendStr(p, " rw-p 00000000 00:00 0                          [stack]\n");
    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// /proc/[pid]/exe — symlink-like content: the executable path
static Vnode* GenPidExe(const ProcessSnapshot& proc)
{
    uint32_t len = ProcStrLen(proc.exePath);
    if (len == 0) return nullptr;
    auto* buf = static_cast<char*>(kmalloc(len + 2));
    if (!buf) return nullptr;
    ProcStrCopy(buf, proc.exePath, len + 1);
    buf[len] = '\n';
    return MakeProcVnode(buf, len + 1);
}

// /proc/[pid]/cwd — current working directory
static Vnode* GenPidCwd(const ProcessSnapshot& proc)
{
    Process* live=SchedulerFindProcessReference(proc.pid);
    if(!live)return nullptr;
    char cwd[512]{};
    int error=live->incarnation==proc.incarnation?ProcessFsGetcwdFromVfsCallback(live,cwd,sizeof(cwd)):-2;
    ProcessUnref(live);
    if(error)return nullptr;
    uint32_t len=ProcStrLen(cwd);
    auto* buf=static_cast<char*>(kmalloc(len+2));
    if(!buf)return nullptr;
    ProcStrCopy(buf,cwd,len+1);
    buf[len] = '\n';
    return MakeProcVnode(buf, len + 1);
}

// Proc magic targets are capabilities, not names. Export only inside the
// caller's thread group until a cross-process inspection policy is available.
static int ProcAcquireFsTarget(uint16_t pid,uint64_t incarnation,uint16_t tgid,uint64_t group,FsSnapshot* out)
{
    *out={};Process* self=ProcessCurrent();ProcessSnapshot caller{};
    if(!self || !SchedulerSnapshotProcess(self->pid,&caller) || caller.tgid!=tgid ||
       caller.groupIncarnation!=group)return -13;
    Process* live=SchedulerFindProcessReference(pid);if(!live)return -2;
    int error=live->incarnation==incarnation && live->tgid==tgid &&
              (live->isThread?live->leaderIncarnation:live->incarnation)==group?ProcessFsSnapshot(live,out):-2;
    ProcessUnref(live);return error;
}
// Legacy proc content generation is reporting only; normal following uses the
// retained magic-link target below. Called within a VFS namespace transaction.
static Vnode* GenPidRoot(const ProcessSnapshot& proc)
{
    FsSnapshot fs{};int error=ProcAcquireFsTarget(proc.pid,proc.incarnation,proc.tgid,proc.groupIncarnation,&fs);
    char root[512]{};
    if(!error)error=VfsPathDirectoryPathFromCallback(fs.root,root,sizeof(root));
    FsSnapshotRelease(&fs);if(error)return nullptr;
    uint32_t length=ProcStrLen(root);auto* buffer=static_cast<char*>(kmalloc(length+2));
    if(!buffer)return nullptr;
    memcpy(buffer,root,length);buffer[length]='\n';buffer[length+1]=0;
    return MakeProcVnode(buffer,length+1);
}

// Per-task mount graph reporting uses the same restricted target policy as
// retained proc-root handles. No backend callbacks or nested VFS transaction.
static Vnode* GenPidMountInfo(const ProcessSnapshot& proc)
{
    VfsMountInfo* rows=nullptr;uint32_t count=0;int error=-11;
    for(unsigned attempt=0;attempt<4 && error==-11;++attempt) {
        FsSnapshot fs{};error=ProcAcquireFsTarget(proc.pid,proc.incarnation,proc.tgid,proc.groupIncarnation,&fs);
        if(!error)error=VfsMountNamespaceInfo(fs.mountNamespace,fs.root,&rows,&count);
        FsSnapshotRelease(&fs);
    }
    if(error)return nullptr;
    // Four-byte escaping is worst case for every cached path and type field.
    constexpr uint32_t PerRow=4*(sizeof(VfsMountInfo::root)+sizeof(VfsMountInfo::mountPoint)+2*sizeof(VfsMountInfo::fsType))+128;
    if(count>(~uint32_t{0}-1)/PerRow) {VfsMountInfoFree(rows);return nullptr;}
    uint32_t capacity=count*PerRow+1;
    auto* buffer=static_cast<char*>(kmalloc(capacity));
    if(!buffer) {VfsMountInfoFree(rows);return nullptr;}
    uint32_t bytes=0;error=FormatMountInfo(rows,count,buffer,capacity,&bytes);VfsMountInfoFree(rows);
    if(error) {kfree(buffer);return nullptr;}
    return MakeProcVnode(buffer,bytes);
}

// /proc/[pid]/limits — resource limits (Linux-compatible format)
static Vnode* GenPidLimits(const ProcessSnapshot& /*proc*/)
{
    auto* buf = static_cast<char*>(kmalloc(1024));
    if (!buf) return nullptr;

    char* p = buf;
    p = AppendStr(p, "Limit                     Soft Limit           Hard Limit           Units     \n");
    p = AppendStr(p, "Max open files            ");
    p = AppendU64(p, MAX_FDS);
    p = AppendStr(p, "                  ");
    p = AppendU64(p, MAX_FDS);
    p = AppendStr(p, "                  files     \n");
    p = AppendStr(p, "Max stack size            ");
    p = AppendU64(p, USER_STACK_SIZE);
    p = AppendStr(p, "             ");
    p = AppendU64(p, USER_STACK_SIZE);
    p = AppendStr(p, "             bytes     \n");
    p = AppendStr(p, "Max data size             ");
    p = AppendU64(p, PROGRAM_BREAK_SIZE);
    p = AppendStr(p, "            ");
    p = AppendU64(p, PROGRAM_BREAK_SIZE);
    p = AppendStr(p, "            bytes     \n");
    p = AppendStr(p, "Max processes             ");
    p = AppendU64(p, MAX_PROCESSES);
    p = AppendStr(p, "                  ");
    p = AppendU64(p, MAX_PROCESSES);
    p = AppendStr(p, "                  processes \n");
    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// ---- Path parsing helpers ----

static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

static uint32_t ParseUint(const char* s, const char** end)
{
    uint32_t val = 0;
    while (IsDigit(*s)) {
        if (val > (65535u - static_cast<uint32_t>(*s - '0')) / 10) {
            while (IsDigit(*s)) ++s;
            if (end) *end = s;
            return 65536;
        }
        val = val * 10 + (*s++ - '0');
    }
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
    return pid > 0 && pid <= 65535 && SchedulerSnapshotProcess(static_cast<uint16_t>(pid), out);
}

// ---- /proc/net/dev — network interface statistics ----

static Vnode* GenNetDev()
{
    auto* buf = static_cast<char*>(kmalloc(1024));
    if (!buf) return nullptr;

    char* p = buf;
    p = AppendStr(p, "Inter-|   Receive                            "
                     "|  Transmit\n");
    p = AppendStr(p, " face |bytes    packets errs drop fifo frame "
                     "|bytes    packets errs drop fifo colls carrier\n");

    uint32_t count = NetIfCount();
    for (uint32_t i = 0; i < count; ++i)
    {
        NetIf* nif = NetIfAt(i);
        if (!nif) continue;
        // "  eth0: rxBytes rxPkts 0 0 0 0  txBytes txPkts 0 0 0 0 0"
        p = AppendStr(p, "  eth");
        p = U64ToDec(p, i);
        p = AppendStr(p, ": ");
        p = U64ToDec(p, nif->rxBytes);
        *p++ = ' ';
        p = U64ToDec(p, nif->rxPackets);
        p = AppendStr(p, " 0 0 0 0  ");
        p = U64ToDec(p, nif->txBytes);
        *p++ = ' ';
        p = U64ToDec(p, nif->txPackets);
        p = AppendStr(p, " 0 0 0 0 0\n");
    }

    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// ---- /proc/net/tcp — TCP socket table (Linux-compatible format) ----

static const char* TcpStateStr(TcpState s)
{
    switch (s) {
    case TcpState::Established: return "01";
    case TcpState::SynSent:     return "02";
    case TcpState::SynRecv:     return "03";
    case TcpState::FinWait1:    return "04";
    case TcpState::FinWait2:    return "05";
    case TcpState::TimeWait:    return "06";
    case TcpState::Closed:      return "07";
    case TcpState::CloseWait:   return "08";
    case TcpState::LastAck:     return "09";
    case TcpState::Listen:      return "0A";
    default:                    return "00";
    }
}

static char* AppendHex32(char* p, uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; --i) *p++ = hex[(v >> (i * 4)) & 0xF];
    return p;
}

static char* AppendHex16(char* p, uint16_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 3; i >= 0; --i) *p++ = hex[(v >> (i * 4)) & 0xF];
    return p;
}

static Vnode* GenNetTcp()
{
    // Linux /proc/net/tcp format (one line per socket):
    //  sl  local_address rem_address   st tx_queue rx_queue
    uint32_t maxSock = NetMaxSockets();
    uint32_t bufSize = 256 + maxSock * 192;
    auto* buf = static_cast<char*>(kmalloc(bufSize));
    if (!buf) return nullptr;

    char* p = buf;
    p = AppendStr(p, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");

    uint32_t sl = 0;
    for (uint32_t i = 0; i < maxSock; ++i)
    {
        SocketSnapshot snap = NetSnapshotSocket(i);
        if (!snap.used) continue;
        if (snap.type != 1 /* SOCK_STREAM */) continue;

        if (static_cast<uint32_t>(p - buf) + 192 >= bufSize) break;

        p = AppendStr(p, "  ");
        if (sl < 10) *p++ = ' ';
        if (sl < 100) *p++ = ' ';
        if (sl < 1000) *p++ = ' ';
        p = U64ToDec(p, sl);
        p = AppendStr(p, ": ");

        // local_address (IP:port in hex)
        p = AppendHex32(p, snap.localIp);
        *p++ = ':';
        p = AppendHex16(p, ntohs(snap.localPort));
        *p++ = ' ';

        // rem_address
        p = AppendHex32(p, snap.remoteIp);
        *p++ = ':';
        p = AppendHex16(p, ntohs(snap.remotePort));
        *p++ = ' ';

        // state
        const char* st = TcpStateStr(snap.tcpState);
        *p++ = st[0]; *p++ = st[1];
        *p++ = ' ';

        // tx_queue:rx_queue
        p = AppendHex32(p, snap.txQueued);
        *p++ = ':';
        p = AppendHex32(p, snap.rxCount);
        p = AppendStr(p, " 00:00000000 00000000 ");
        p = U64ToDec(p, ProcessReportId(ProcessCurrent(), false, snap.identity.uid));
        p = AppendStr(p, " 0 ");
        p = U64ToDec(p, snap.identity.inode);
        *p++ = '\n';

        ++sl;
    }

    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// ---- /proc/net/udp — UDP socket table ----

static Vnode* GenNetUdp()
{
    uint32_t maxSock = NetMaxSockets();
    uint32_t bufSize = 256 + maxSock * 192;
    auto* buf = static_cast<char*>(kmalloc(bufSize));
    if (!buf) return nullptr;

    char* p = buf;
    p = AppendStr(p, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");

    uint32_t sl = 0;
    for (uint32_t i = 0; i < maxSock; ++i)
    {
        SocketSnapshot snap = NetSnapshotSocket(i);
        if (!snap.used) continue;
        if (snap.type != 2 /* SOCK_DGRAM */) continue;

        if (static_cast<uint32_t>(p - buf) + 192 >= bufSize) break;

        p = AppendStr(p, "  ");
        if (sl < 10) *p++ = ' ';
        if (sl < 100) *p++ = ' ';
        if (sl < 1000) *p++ = ' ';
        p = U64ToDec(p, sl);
        p = AppendStr(p, ": ");

        p = AppendHex32(p, snap.localIp);
        *p++ = ':';
        p = AppendHex16(p, ntohs(snap.localPort));
        *p++ = ' ';

        p = AppendHex32(p, snap.remoteIp);
        *p++ = ':';
        p = AppendHex16(p, ntohs(snap.remotePort));
        p = AppendStr(p, " 07 "); // UDP is always "07" (Closed equivalent)

        p = AppendHex32(p, 0);
        *p++ = ':';
        p = AppendHex32(p, snap.rxCount);
        p = AppendStr(p, " 00:00000000 00000000 ");
        p = U64ToDec(p, ProcessReportId(ProcessCurrent(), false, snap.identity.uid));
        p = AppendStr(p, " 0 ");
        p = U64ToDec(p, snap.identity.inode);
        *p++ = '\n';

        ++sl;
    }

    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// ---- /proc/net/sockstat — socket count summary ----

static Vnode* GenNetSockstat()
{
    uint32_t maxSock = NetMaxSockets();
    uint32_t tcpInUse = 0, udpInUse = 0, rawInUse = 0;
    for (uint32_t i = 0; i < maxSock; ++i)
    {
        SocketSnapshot snap = NetSnapshotSocket(i);
        if (!snap.used) continue;
        if (snap.type == 1)      ++tcpInUse;  // SOCK_STREAM
        else if (snap.type == 2) ++udpInUse;  // SOCK_DGRAM
        else                     ++rawInUse;
    }

    auto* buf = static_cast<char*>(kmalloc(256));
    if (!buf) return nullptr;

    char* p = buf;
    p = AppendStr(p, "sockets: used ");
    p = AppendU64(p, tcpInUse + udpInUse + rawInUse);
    p = AppendStr(p, "\nTCP: inuse ");
    p = AppendU64(p, tcpInUse);
    p = AppendStr(p, "\nUDP: inuse ");
    p = AppendU64(p, udpInUse);
    p = AppendStr(p, "\nRAW: inuse ");
    p = AppendU64(p, rawInUse);
    *p++ = '\n';
    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// ---- /proc/modules — loaded kernel modules ----

static Vnode* GenModules()
{
    uint32_t maxSlots = ModuleMaxSlots();
    uint32_t bufSize = 128 + maxSlots * 128;
    auto* buf = static_cast<char*>(kmalloc(bufSize));
    if (!buf) return nullptr;

    char* p = buf;
    for (uint32_t i = 0; i < maxSlots; ++i)
    {
        ModuleSnapshot snap = ModuleSnapshotAt(i);
        if (!snap.active || !snap.name[0]) continue;
        if (static_cast<uint32_t>(p - buf) + 128 >= bufSize) break;
        // Linux format: name size refcount deps state
        p = AppendStr(p, snap.name);
        *p++ = ' ';
        p = U64ToDec(p, snap.sizeBytes);
        p = AppendStr(p, snap.resident ? " 1 - Live\n" : " 0 - Live\n");
    }

    if (p == buf) {
        // No modules loaded
        *p = '\0';
    } else {
        *p = '\0';
    }
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// ---- /proc/mounts — mounted filesystems ----

static Vnode* GenMounts()
{
    FsSnapshot fs{};auto* process=ProcessCurrent();
    int error=ProcessFsSnapshot(process,&fs);
    if(error && process && !process->isKernelThread)return nullptr;
    auto* ns=error?VfsMountNamespaceInitial():fs.mountNamespace;
    fs.mountNamespace=nullptr;FsSnapshotRelease(&fs);
    uint32_t maxSlots = VfsMountNamespaceCount(ns);
    constexpr uint32_t PerRow=sizeof(VfsMountSnapshot::mountPoint)+2*sizeof(VfsMountSnapshot::fsType)+32;
    if(maxSlots>(~uint32_t{0}-128)/PerRow) {VfsMountNamespaceRelease(ns);return nullptr;}
    uint32_t bufSize = 128 + maxSlots * PerRow;
    auto* buf = static_cast<char*>(kmalloc(bufSize));
    if (!buf) {VfsMountNamespaceRelease(ns);return nullptr;}

    char* p = buf;
    for (uint32_t i = 0; i < maxSlots; ++i)
    {
        VfsMountSnapshot snap = VfsMountNamespaceSnapshotAt(ns,i);
        if (!snap.used) continue;
        if (static_cast<uint32_t>(p - buf) + PerRow >= bufSize) break;
        // Linux format: device mountpoint type options dump pass
        p = AppendStr(p, snap.fsType);
        *p++ = ' ';
        p = AppendStr(p, snap.mountPoint);
        *p++ = ' ';
        p = AppendStr(p, snap.fsType);
        p = AppendStr(p, " rw 0 0\n");
    }
    *p = '\0';
    VfsMountNamespaceRelease(ns);
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// ---- /proc/filesystems — registered filesystem types ----

static Vnode* GenFilesystems()
{
    uint32_t maxSlots = VfsFsMaxSlots();
    uint32_t bufSize = 128 + maxSlots * 32;
    auto* buf = static_cast<char*>(kmalloc(bufSize));
    if (!buf) return nullptr;

    char* p = buf;
    for (uint32_t i = 0; i < maxSlots; ++i)
    {
        VfsFsSnapshot snap = VfsFsSnapshotAt(i);
        if (!snap.used) continue;
        if (static_cast<uint32_t>(p - buf) + 32 >= bufSize) break;
        // nodev prefix for virtual filesystems
        bool isVirtual = (snap.name[0] == 'p' && snap.name[1] == 'r' && snap.name[2] == 'o' &&
                          snap.name[3] == 'c');
        if (isVirtual)
            p = AppendStr(p, "nodev\t");
        else
            p = AppendStr(p, "\t");
        p = AppendStr(p, snap.name);
        *p++ = '\n';
    }
    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// ---- /proc/diskstats — block device I/O statistics ----

static Vnode* GenDiskstats()
{
    // Format: major minor name reads rd_merged rd_sectors rd_ms writes wr_merged wr_sectors wr_ms io_cur io_ms weighted_ms
    // We use a simplified form with real read/write counts and sectors.
    uint32_t bufSize = 512;
    auto* buf = static_cast<char*>(kmalloc(bufSize));
    if (!buf) return nullptr;

    char* p = buf;
    // Iterate virtio block devices (virtio0, virtio1, ...)
    for (uint32_t i = 0; i < 16; ++i)
    {
        char devName[16];
        devName[0] = 'v'; devName[1] = 'i'; devName[2] = 'r'; devName[3] = 't';
        devName[4] = 'i'; devName[5] = 'o'; 
        if (i < 10) { devName[6] = '0' + i; devName[7] = '\0'; }
        else { devName[6] = '0' + (i / 10); devName[7] = '0' + (i % 10); devName[8] = '\0'; }

        Device* dev = DeviceFind(devName);
        if (!dev) continue;

        uint64_t rdOps, wrOps, rdBytes, wrBytes;
        brook::VirtioBlkGetStats(dev, rdOps, wrOps, rdBytes, wrBytes);

        uint64_t rdSectors = rdBytes / 512;
        uint64_t wrSectors = wrBytes / 512;

        if (static_cast<uint32_t>(p - buf) + 128 >= bufSize) break;

        // major=253 minor=i  (virtio convention)
        p = AppendStr(p, " 253    ");
        p = AppendU64(p, i);
        *p++ = ' ';
        p = AppendStr(p, devName);
        *p++ = ' ';
        p = AppendU64(p, rdOps);
        p = AppendStr(p, " 0 ");
        p = AppendU64(p, rdSectors);
        p = AppendStr(p, " 0 ");
        p = AppendU64(p, wrOps);
        p = AppendStr(p, " 0 ");
        p = AppendU64(p, wrSectors);
        p = AppendStr(p, " 0 0 0 0\n");
    }
    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

// /proc/blkprobe — virtio-blk cold-read latency probe (BRO-165).
// One line per device with cumulative-since-boot completion-wait metrics plus
// derived averages. Read before and after a workload and diff the raw columns;
// the avg/path columns are convenience views of the same counters.
static Vnode* GenBlkprobe()
{
    uint32_t bufSize = 4096;
    auto* buf = static_cast<char*>(kmalloc(bufSize));
    if (!buf) return nullptr;

    char* p = buf;
    p = AppendStr(p, "dev waits reqs waitNsTotal waitNsMax spinIters legacy batch sg avgReqPerWait avgWaitNs avgSpin flushSubmitted flushErrors\n");

    for (uint32_t i = 0; i < 16; ++i)
    {
        char devName[16];
        devName[0] = 'v'; devName[1] = 'i'; devName[2] = 'r'; devName[3] = 't';
        devName[4] = 'i'; devName[5] = 'o';
        if (i < 10) { devName[6] = '0' + i; devName[7] = '\0'; }
        else { devName[6] = '0' + (i / 10); devName[7] = '0' + (i % 10); devName[8] = '\0'; }

        Device* dev = DeviceFind(devName);
        if (!dev) continue;

        brook::VirtioBlkProbeStats ps;
        brook::VirtioBlkGetProbe(dev, ps);

        if (static_cast<uint32_t>(p - buf) + 384 >= bufSize) break;

        uint64_t avgReq  = ps.waitCount ? ps.reqSubmitted   / ps.waitCount : 0;
        uint64_t avgWait = ps.waitCount ? ps.waitNsTotal    / ps.waitCount : 0;
        uint64_t avgSpin = ps.waitCount ? ps.spinItersTotal / ps.waitCount : 0;

        p = AppendStr(p, devName); *p++ = ' ';
        p = AppendU64(p, ps.waitCount);      *p++ = ' ';
        p = AppendU64(p, ps.reqSubmitted);   *p++ = ' ';
        p = AppendU64(p, ps.waitNsTotal);    *p++ = ' ';
        p = AppendU64(p, ps.waitNsMax);      *p++ = ' ';
        p = AppendU64(p, ps.spinItersTotal); *p++ = ' ';
        p = AppendU64(p, ps.pathLegacy);     *p++ = ' ';
        p = AppendU64(p, ps.pathBatch);      *p++ = ' ';
        p = AppendU64(p, ps.pathSG);         *p++ = ' ';
        p = AppendU64(p, avgReq);            *p++ = ' ';
        p = AppendU64(p, avgWait);           *p++ = ' ';
        p = AppendU64(p, avgSpin);           *p++ = ' ';
        p = AppendU64(p, ps.flushSubmitted); *p++ = ' ';
        p = AppendU64(p, ps.flushErrors);    *p++ = '\n';
    }
    *p = '\0';
    return MakeProcVnode(buf, static_cast<uint32_t>(p - buf));
}

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
    { "modules", GenModules },
    { "mounts",  GenMounts },
    { "diskstats", GenDiskstats },
    { "blkprobe", GenBlkprobe },
    { "filesystems", GenFilesystems },
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
    { "exe",     GenPidExe },
    { "cwd",     GenPidCwd },
    { "limits",  GenPidLimits },
    { "root",    GenPidRoot },
    { "mountinfo", GenPidMountInfo },
};
static constexpr uint32_t NUM_PID_ENTRIES = sizeof(g_pidEntries) / sizeof(g_pidEntries[0]);

static bool StrEqProc(const char* a, const char* b)
{
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

// Resolve dynamic aliases at lookup time. /proc/self is the thread group;
// /proc/thread-self identifies the caller's individual task.
static bool ResolveProcPath(const char* path, char* out, uint32_t capacity)
{
    if (!path) path = "";
    while (*path == '/') ++path;
    char* p = out;
    const bool self = strncmp(path, "self", 4) == 0 && (!path[4] || path[4] == '/');
    const bool threadSelf = strncmp(path, "thread-self", 11) == 0 && (!path[11] || path[11] == '/');
    if (self || threadSelf)
    {
        Process* cur = SchedulerCurrentProcess();
        if (!cur) return false;
        p = U64ToDec(p, cur->tgid ? cur->tgid : cur->pid);
        if (threadSelf) { p = AppendStr(p, "/task/"); p = U64ToDec(p, cur->pid); }
        path += self ? 4 : 11;
    }
    while (*path) { if (p + 1 >= out + capacity) return false; *p++ = *path++; }
    while (p > out && p[-1] == '/') --p;
    *p = 0;
    return true;
}

struct ProcLookup {
    ProcessSnapshot snap;
    const char* rest;
    bool taskRoot, taskMember;
};

static bool LookupProcPid(const char* path, ProcLookup* result)
{
    *result = {};
    uint32_t pid;
    if (!IsPidPath(path, &pid, &result->rest) || !FindProcess(pid, &result->snap)) return false;
    const char* rest = result->rest;
    if (strncmp(rest, "task", 4) != 0 || (rest[4] && rest[4] != '/')) return true;
    if (!rest[4]) { result->taskRoot = true; result->rest = ""; return true; }
    uint32_t tid;
    const char* suffix;
    if (!IsPidPath(rest + 5, &tid, &suffix) || !tid || tid > 65535) return false;
    ProcessSnapshot task;
    if (!SchedulerSnapshotTask(result->snap.tgid, result->snap.groupIncarnation,
                               static_cast<uint16_t>(tid), 0, &task)) return false;
    result->snap = task; result->rest = suffix; result->taskMember = true;
    return true;
}

static bool ProcFdNumber(const char* rest, int* fd)
{
    if (strncmp(rest, "fd/", 3) != 0 || !IsDigit(rest[3])) return false;
    const char* end;
    uint32_t value = ParseUint(rest + 3, &end);
    if (*end || value >= MAX_FDS) return false;
    *fd = static_cast<int>(value);
    return true;
}

static Vnode* MakePidDirectory(const ProcLookup& path, VnodeOps* ops)
{
    Vnode* vn = MakeProcVnode(nullptr, 0, true, ops);
    if (!vn) return nullptr;
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    pp->pid = path.snap.pid; pp->tgid = path.snap.tgid;
    pp->incarnation = path.snap.incarnation; pp->groupIncarnation = path.snap.groupIncarnation;
    pp->taskRoot = path.taskRoot; pp->taskMember = path.taskMember;
    return vn;
}

static Vnode* MakeProcNsDirectory(const ProcessSnapshot&);

void ProcFsInit() { SerialPuts("procfs: initialised\n"); }

static uint8_t ProcMapKind(const char* name) {
    return StrEqProc(name,"uid_map")?1:StrEqProc(name,"gid_map")?2:StrEqProc(name,"setgroups")?3:0;
}
static Vnode* MakeUserMapVnode(UserNamespace* ns,uint8_t kind) {
    auto opened=ProcessUserAuthorityAcquire(ProcessCurrent());
    auto* vn=MakeProcVnode(nullptr,0,false,&g_procMapOps);
    if(!vn) {UserNamespaceRelease(opened.ns);return nullptr;}
    UserNamespaceRetain(ns);
    auto* pp=static_cast<ProcPriv*>(vn->priv);pp->userNamespace=ns;pp->openedAuthority=opened;pp->mapKind=kind;return vn;
}
static Vnode* GenPidUserMap(const ProcessSnapshot& snap,uint8_t kind) {
    auto* target=SchedulerFindProcessReference(snap.pid);if(!target)return nullptr;
    UserNamespace* ns=nullptr;bool valid=target->incarnation==snap.incarnation;
    if(valid)ns=ProcessUserNamespaceAcquire(target);ProcessUnref(target);
    if(!valid)return nullptr;
    auto* vn=MakeUserMapVnode(ns,kind);UserNamespaceRelease(ns);return vn;
}

Vnode* ProcFsOpen(const char* relPath, int /*flags*/)
{
    char resolved[256];
    if (!ResolveProcPath(relPath, resolved, sizeof(resolved))) return nullptr;
    if (!resolved[0]) return ProcFsOpenDir(resolved);
    for (uint32_t i = 0; i < NUM_GLOBAL; ++i)
        if (StrEqProc(resolved, g_globalEntries[i].name)) return g_globalEntries[i].gen();
    if(ProcOverflowPath(resolved))return GenOverflowId();
    if(StrEqProc(resolved,"sys") || StrEqProc(resolved,"sys/kernel"))return ProcFsOpenDir(resolved);
    if (StrEqProc(resolved, "net/dev")) return GenNetDev();
    if (StrEqProc(resolved, "net/tcp")) return GenNetTcp();
    if (StrEqProc(resolved, "net/udp")) return GenNetUdp();
    if (StrEqProc(resolved, "net/sockstat")) return GenNetSockstat();
    ProcLookup path;
    if (!LookupProcPid(resolved, &path)) return nullptr;
    if (!path.rest[0] || StrEqProc(path.rest, "fd") || StrEqProc(path.rest,"ns")) return ProcFsOpenDir(resolved);
    for (uint32_t i = 0; i < NUM_PID_ENTRIES; ++i)
        if (StrEqProc(path.rest, g_pidEntries[i].name)) return g_pidEntries[i].gen(path.snap);
    if(auto kind=ProcMapKind(path.rest))return GenPidUserMap(path.snap,kind);
    int fd;
    if (ProcFdNumber(path.rest, &fd)) return GenPidFdLink(path.snap.pid, fd, path.snap.incarnation);
    return nullptr;
}

int ProcFsStatPath(const char* relPath, VnodeStat* st)
{
    char resolved[256];
    if (!ResolveProcPath(relPath, resolved, sizeof(resolved))) return -1;
    if(ProcOverflowPath(resolved)) {st->size=6;st->mode=0100444;st->isDir=false;return 0;}
    if (!resolved[0] || StrEqProc(resolved, "net") || StrEqProc(resolved,"sys") || StrEqProc(resolved,"sys/kernel"))
    { st->size = 0; st->isDir = true; st->nlink = 2; return 0; }
    for (uint32_t i = 0; i < NUM_GLOBAL; ++i)
        if (StrEqProc(resolved, g_globalEntries[i].name))
        { st->size = 0; st->isDir = false; return 0; }
    if (StrEqProc(resolved, "net/dev") || StrEqProc(resolved, "net/tcp") ||
        StrEqProc(resolved, "net/udp") || StrEqProc(resolved, "net/sockstat"))
    { st->size = 0; st->isDir = false; return 0; }
    ProcLookup path;
    if (!LookupProcPid(resolved, &path)) return -1;
    if (!path.rest[0] || StrEqProc(path.rest, "fd") || StrEqProc(path.rest,"ns"))
    {
        st->size = 0; st->isDir = true;
        st->nlink = path.taskRoot ? 2 + path.snap.threadCount : 2;
        st->ino = (path.snap.incarnation << 20) | (uint64_t{path.snap.pid} << 4) | (path.taskRoot ? 2 : 1);
        return 0;
    }
    for (uint32_t i = 0; i < NUM_PID_ENTRIES; ++i)
        if (StrEqProc(path.rest, g_pidEntries[i].name))
        { st->size = 0; st->isDir = false; return 0; }
    if(ProcMapKind(path.rest)) {st->isDir=false;st->mode=0100644;st->size=0;return 0;}
    int fd;
    FdSnapshot snap;
    if (ProcFdNumber(path.rest, &fd) && SchedulerGetFdInfo(path.snap.pid, fd, &snap, path.snap.incarnation))
    { st->size = 0; st->isDir = false; st->isSymlink = true; return 0; }
    return -1;
}

Vnode* ProcFsOpenDir(const char* relPath)
{
    char resolved[256];
    if (!ResolveProcPath(relPath, resolved, sizeof(resolved))) return nullptr;
    if (!resolved[0]) return MakeProcVnode(nullptr, 0, true, &g_procRootDirOps);
    if(StrEqProc(resolved,"sys"))return MakeProcSysDirectory(false);
    if(StrEqProc(resolved,"sys/kernel"))return MakeProcSysDirectory(true);
    ProcLookup path;
    if (!LookupProcPid(resolved, &path)) return nullptr;
    if (path.taskRoot) return MakePidDirectory(path, &g_procTaskDirOps);
    if (!path.rest[0]) return MakePidDirectory(path, &g_procPidDirOps);
    if (StrEqProc(path.rest, "fd")) return MakePidDirectory(path, &g_procFdDirOps);
    if (StrEqProc(path.rest, "ns")) return MakeProcNsDirectory(path.snap);
    return nullptr;
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

    if(idx==NUM_GLOBAL) {
        *out={};ProcStrCopy(out->name,"sys",sizeof(out->name));out->isDir=true;*cookie=idx+1;return 1;
    }
    // Then: PID directories — use indexed lookup to avoid large heap allocation
    uint32_t pidIdx = idx - NUM_GLOBAL - 1;
    uint16_t pid = 0;
    if (!SchedulerGetPidByIndex(pidIdx, &pid)) return 0;

    char pidStr[8];
    char* p = U64ToDec(pidStr, pid);
    *p = '\0';
    ProcStrCopy(out->name, pidStr, sizeof(out->name));
    out->size = 0;
    out->isDir = true;
    *cookie = idx + 1;
    return 1;
}

// /proc/PID directory: list per-process files
static int ProcPidReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    VnodeStat st{};
    if (ProcDirStat(vn, &st) != 0) return 0;
    uint32_t idx = *cookie;
    if (idx < NUM_PID_ENTRIES)
    {
        ProcStrCopy(out->name, g_pidEntries[idx].name, sizeof(out->name));
        out->size = 0;
        out->isDir = false;
        *cookie = idx + 1;
        return 1;
    }
    if(idx<NUM_PID_ENTRIES+4) {
        const char* names[]={"uid_map","gid_map","setgroups","ns"};
        ProcStrCopy(out->name,names[idx-NUM_PID_ENTRIES],sizeof(out->name));out->size=0;out->isDir=idx==NUM_PID_ENTRIES+3;*cookie=idx+1;return 1;
    }
    idx-=4;
    // After regular entries, emit "fd" directory
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (idx == NUM_PID_ENTRIES || (idx == NUM_PID_ENTRIES + 1 && !pp->taskMember))
    {
        ProcStrCopy(out->name, idx == NUM_PID_ENTRIES ? "fd" : "task", sizeof(out->name));
        out->size = 0;
        out->isDir = true;
        *cookie = idx + 5;
        return 1;
    }
    return 0;
}

// /proc/PID/fd/ directory: list open file descriptors
static int ProcFdReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (!pp || !pp->pid) return 0;
    int fdNum = 0;
    FdSnapshot snap = {};
    if (!SchedulerGetFdByIndex(pp->pid, *cookie, &fdNum, &snap, pp->incarnation))
        return 0;

    char fdStr[12];
    char* p = U64ToDec(fdStr, static_cast<uint64_t>(fdNum));
    *p = '\0';
    ProcStrCopy(out->name, fdStr, sizeof(out->name));
    out->size = 0;
    out->isDir = false;
    *cookie = *cookie + 1;
    return 1;
}

// A TID cursor avoids skipping stable members when an earlier task exits.
// An open directory never switches to a newly reused TGID incarnation.
static int ProcTaskReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    auto* pp = static_cast<ProcPriv*>(vn->priv);
    if (!pp) return 0;
    if (*cookie < 2)
    {
        if (!SchedulerCountTasks(pp->tgid, pp->groupIncarnation)) return 0;
        ProcStrCopy(out->name, *cookie == 0 ? "." : "..", sizeof(out->name));
        out->size = 0; out->isDir = true; ++*cookie;
        return 1;
    }
    ProcessSnapshot task;
    if (!SchedulerSnapshotTask(pp->tgid, pp->groupIncarnation, 0, *cookie - 2, &task)) return 0;
    char* end = U64ToDec(out->name, task.pid); *end = 0;
    out->size = 0; out->isDir = true;
    *cookie = uint32_t{task.pid} + 3;
    return 1;
}

// Generate content for /proc/PID/fd/N — symlink-like target path
static Vnode* GenPidFdLink(uint16_t pid, int fd, uint64_t incarnation)
{
    FdSnapshot snap = {};
    if (!SchedulerGetFdInfo(pid, fd, &snap, incarnation)) return nullptr;

    char* buf = static_cast<char*>(kmalloc(128));
    if (!buf) return nullptr;

    // Copy the path from the snapshot
    uint32_t len = 0;
    for (; len < 127 && snap.path[len]; ++len)
        buf[len] = snap.path[len];
    buf[len++] = '\n';
    buf[len] = '\0';
    return MakeProcVnode(buf, len);
}

#include "procfs_namespaces.inc"
#include "procfs_objects.inc"

} // namespace brook
