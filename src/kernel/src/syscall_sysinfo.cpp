// syscall_sysinfo.cpp -- system information syscalls: uname, sysinfo, getrusage, times, rlimits, getrandom.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "pid_namespace.h"
#include "core_autodump.h"
#include "process_user_namespace.h"
#include "process_namespaces.h"
#include "memory/physical_memory.h"
#include "tsc_clock.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_handlers.h"
#include "strong_random.h"
#include "../../shared/brook_random.h"

namespace brook {

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

int64_t sys_uname(uint64_t bufAddr, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    utsname value{};auto* buf=&value;
    // This syscall describes the Linux ABI personality. Unix libraries such
    // as MonoGame select native loaders using an exact "Linux" comparison.
    StrCopyN(buf->sysname,    "Linux",     65);
    ProcessUtsRead(ProcessCurrent(),buf->nodename,buf->domainname);
    StrCopyN(buf->release,    "6.0.0",     65);
    StrCopyN(buf->version,    "#1 Brook",  65);
    StrCopyN(buf->machine,    "x86_64",    65);
    return CopyToUser(bufAddr,&value,sizeof(value))?0:-EFAULT;
}

static int64_t SetUtsName(uint64_t address,uint64_t length,bool domain)
{
    if(length>=UtsNameBytes)return -EINVAL;
    char name[UtsNameBytes]{};
    if(length && !CopyFromUser(name,address,length))return -EFAULT;
    return ProcessUtsWrite(ProcessCurrent(),domain,name,static_cast<unsigned>(length));
}

int64_t sys_sethostname(uint64_t address,uint64_t length,uint64_t,uint64_t,uint64_t,uint64_t)
{
    return SetUtsName(address,length,false);
}

int64_t sys_setdomainname(uint64_t address,uint64_t length,uint64_t,uint64_t,uint64_t,uint64_t)
{
    return SetUtsName(address,length,true);
}

// ---------------------------------------------------------------------------
// sys_prlimit64 (302)
// ---------------------------------------------------------------------------

struct rlimit64 { uint64_t rlim_cur; uint64_t rlim_max; };
static constexpr uint64_t RLIM_INFINITY = ~0ULL;

// Both Linux query entry points describe the same defaults. In particular,
// the stack's 8 MiB soft limit must not become a data/heap limit when glibc
// implements getrlimit through prlimit64 (V8 uses RLIMIT_DATA for sizing).
static rlimit64 ReportedResourceLimit(uint64_t resource)
{
    if (resource == 7) return {MAX_FDS, MAX_FDS}; // RLIMIT_NOFILE
    if (resource == 3) return {8 * 1024 * 1024, RLIM_INFINITY}; // RLIMIT_STACK
    return {RLIM_INFINITY, RLIM_INFINITY};
}

static int64_t CoreResourceLimit(uint64_t pid,uint64_t newlimitAddr,uint64_t oldlimitAddr)
{
    if (static_cast<int32_t>(pid) < 0) return -ESRCH;
    CoreResourceLimits requested{},previous{};
    if(newlimitAddr && !CopyFromUser(&requested,newlimitAddr,sizeof(requested)))return -EFAULT;
    auto* caller=ProcessCurrent();
    if(!caller)return -ESRCH;
    const bool privileged=ProcessCapable(caller,nullptr,24); // CAP_SYS_RESOURCE in initial namespace
    const int result=SchedulerCoreResourceLimit(caller,static_cast<uint32_t>(pid),
                                                newlimitAddr?&requested:nullptr,previous,privileged);
    if(result)return result;
    if(oldlimitAddr && !CopyToUser(oldlimitAddr,&previous,sizeof(previous)))return -EFAULT;
    return 0;
}

int64_t sys_setrlimit(uint64_t resource,uint64_t newlimitAddr,uint64_t,uint64_t,uint64_t,uint64_t)
{
    if(resource!=4)return -38;
    if(!newlimitAddr)return -EFAULT;
    return CoreResourceLimit(0,newlimitAddr,0);
}

int64_t sys_brook_core_dump(uint64_t pid,uint64_t flags,uint64_t,uint64_t,uint64_t,uint64_t)
{
    if(!pid || pid>65535 || flags)return -EINVAL;
    auto* caller = ProcessCurrent();
    if (ProcessPidView(caller) || !ProcessCapable(caller,nullptr,19)) return -EPERM; // CAP_SYS_PTRACE in initial namespace
    return SchedulerCoreAutoQueue(static_cast<uint16_t>(pid));
}

int64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t newlimitAddr,
                        uint64_t oldlimitAddr, uint64_t, uint64_t)
{
    if(resource==4)return CoreResourceLimit(pid,newlimitAddr,oldlimitAddr);
    (void)newlimitAddr;
    auto* caller = ProcessCurrent();
    if (pid && ProcessPidView(caller)) {
        const int32_t tid = static_cast<int32_t>(pid);
        auto* target = tid > 0 ? SchedulerFindVisibleProcessReference(ProcessPidView(caller), tid) : nullptr;
        if (!target) return -ESRCH;
        ProcessUnref(target);
    }

    if (oldlimitAddr)
    {
        const rlimit64 old = ReportedResourceLimit(resource);
        if (!CopyToUser(oldlimitAddr, &old, sizeof(old))) return -EFAULT;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sys_getrandom (318) — RDRAND-backed random number generation
// ---------------------------------------------------------------------------

int64_t sys_getrandom(uint64_t bufAddr, uint64_t count, uint64_t flags,
                        uint64_t, uint64_t, uint64_t)
{
    if (count > 256) count = 256; // cap per-call to avoid long stalls
    uint8_t buf[256];
    if (flags & BROOK_GRND_STRONG_ONLY) {
        if (flags != BROOK_GRND_STRONG_ONLY) return -EINVAL;
        if (!StrongRandomBytes(buf, static_cast<uint32_t>(count))) return -EAGAIN;
        if (!count) return 0;
        return CopyToUser(bufAddr, buf, count) ? static_cast<int64_t>(count) : -EFAULT;
    }

    uint64_t filled = 0;
    while (filled < count)
    {
        uint64_t rnd;
        if (!RdrandU64(&rnd))
            rnd = SoftRandU64();

        uint64_t remaining = count - filled;
        uint64_t chunk = (remaining < 8) ? remaining : 8;
        for (uint64_t b = 0; b < chunk; b++)
        {
            buf[filled++] = static_cast<uint8_t>(rnd >> (b * 8));
        }
    }
    const uint64_t copied=CopyToUserBytes(bufAddr,buf,count);
    return copied || !count ? static_cast<int64_t>(copied) : -EFAULT;
}

// ---------------------------------------------------------------------------
// sys_getrusage (98) — stub
// ---------------------------------------------------------------------------

int64_t sys_getrusage(uint64_t who, uint64_t usageAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t)
{
    // Built in kernel memory, copied out once (144-byte struct rusage).
    int64_t ru[18] = {};
    if (!UserBufferWritable(usageAddr, sizeof(ru))) return -EFAULT;

    Process* proc = ProcessCurrent();
    if (!proc) return CopyToUser(usageAddr, ru, sizeof(ru)) ? 0 : -EFAULT;

    // struct rusage: first two fields are struct timeval (utime, stime)
    // timeval = { int64_t tv_sec; int64_t tv_usec; }

    uint64_t uticks = proc->userTicks;
    uint64_t sticks = proc->sysTicks;

    if (who == 0) // RUSAGE_SELF
    {
        ru[0] = static_cast<int64_t>(uticks / 1000);        // utime.tv_sec
        ru[1] = static_cast<int64_t>((uticks % 1000) * 1000); // utime.tv_usec
        ru[2] = static_cast<int64_t>(sticks / 1000);        // stime.tv_sec
        ru[3] = static_cast<int64_t>((sticks % 1000) * 1000); // stime.tv_usec
    }
    // RUSAGE_CHILDREN (-1) — we include reaped ticks
    else if (who == static_cast<uint64_t>(-1ULL))
    {
        uint64_t reapedUser = 0, reapedSys = 0;
        SchedulerGetReapedTicks(reapedUser, reapedSys);
        ru[0] = static_cast<int64_t>(reapedUser / 1000);
        ru[1] = static_cast<int64_t>((reapedUser % 1000) * 1000);
        ru[2] = static_cast<int64_t>(reapedSys / 1000);
        ru[3] = static_cast<int64_t>((reapedSys % 1000) * 1000);
    }
    return CopyToUser(usageAddr, ru, sizeof(ru)) ? 0 : -EFAULT;
}

// ---------------------------------------------------------------------------
// sys_sysinfo (99) — basic system info
// ---------------------------------------------------------------------------

int64_t sys_sysinfo(uint64_t infoAddr, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t)
{
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
    sysinfo_s staged{};
    auto* info = &staged;

    info->uptime = static_cast<int64_t>(ApicTickCount() / 1000);

    uint64_t totalPages = PmmGetRamPageCount();
    uint64_t freePages  = PmmGetFreePageCount();
    info->totalram = totalPages * 4096;
    info->freeram  = freePages  * 4096;

    // Count running processes and set load averages
    uint32_t total = 0, running = 0;
    SchedulerGetProcessCounts(total, running);
    info->procs = static_cast<uint16_t>(total);

    // Load averages: Linux uses SI_LOAD_SHIFT=16, values are fixed-point << 16
    uint32_t avg1, avg5, avg15;
    SchedulerGetLoadAvg(avg1, avg5, avg15);
    // Our values are * 1000; Linux expects * 65536. Convert: (val * 65536) / 1000
    info->loads[0] = static_cast<uint64_t>(avg1) * 65536 / 1000;
    info->loads[1] = static_cast<uint64_t>(avg5) * 65536 / 1000;
    info->loads[2] = static_cast<uint64_t>(avg15) * 65536 / 1000;

    info->mem_unit = 1;
    return CopyToUser(infoAddr, &staged, sizeof(staged)) ? 0 : -EFAULT;
}

// ---------------------------------------------------------------------------
// sys_times (100) — process times
// ---------------------------------------------------------------------------

int64_t sys_times(uint64_t bufAddr, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    // struct tms { clock_t tms_utime, tms_stime, tms_cutime, tms_cstime; }
    // clock_t is int64_t on x86_64, 4 fields = 32 bytes
    if (bufAddr)
    {
        int64_t tms[4] = {};

        Process* proc = ProcessCurrent();
        if (proc)
        {
            tms[0] = static_cast<int64_t>(proc->userTicks); // tms_utime
            tms[1] = static_cast<int64_t>(proc->sysTicks);  // tms_stime
        }
        else
        {
            tms[0] = 0;
            tms[1] = 0;
        }
        // Children times (accumulated from reaped processes)
        uint64_t reapedUser = 0, reapedSys = 0;
        SchedulerGetReapedTicks(reapedUser, reapedSys);
        tms[2] = static_cast<int64_t>(reapedUser); // tms_cutime
        tms[3] = static_cast<int64_t>(reapedSys);  // tms_cstime
        if (!CopyToUser(bufAddr, tms, sizeof(tms))) return -EFAULT;
    }

    // Return value: clock ticks since boot
    return static_cast<int64_t>(ApicTickCount());
}

// ---------------------------------------------------------------------------
// sys_getrlimit (97) — return resource limits consistent with prlimit64
// ---------------------------------------------------------------------------

int64_t sys_getrlimit(uint64_t resource, uint64_t rlimAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t)
{
    if(resource==4)return rlimAddr?CoreResourceLimit(0,0,rlimAddr):-EFAULT;
    const rlimit64 limit = ReportedResourceLimit(resource);
    return CopyToUser(rlimAddr, &limit, sizeof(limit)) ? 0 : -EFAULT;
}

} // namespace brook
