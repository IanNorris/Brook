// syscall_sched.cpp -- scheduler syscalls: yield, affinity, policy, priority, membarrier.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "apic.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_handlers.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_sched_yield (24)
// ---------------------------------------------------------------------------

int64_t sys_sched_yield(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    SchedulerYield();
    return 0;
}

// Linux x86-64 membarrier(324). Only private expedited barriers and their
// registration are advertised; sync-core, rseq and global commands stay EINVAL.
int64_t sys_membarrier(uint64_t command,uint64_t flags,uint64_t,
                       uint64_t,uint64_t,uint64_t)
{
    constexpr uint32_t expedited=1u<<3, registration=1u<<4;
    // Linux arguments are int/unsigned int: ignore upper register halves.
    if(static_cast<uint32_t>(flags))return -EINVAL;
    const uint32_t cmd=static_cast<uint32_t>(command);
    if(!cmd)return expedited|registration;
    auto* proc=SchedulerCurrentProcess();
    auto* space=proc ? proc->addressSpace : nullptr;
    if(!space || space->kernel)return -EINVAL;
    if(cmd==registration) {
        __atomic_store_n(&space->membarrierPrivateRegistered,1u,__ATOMIC_RELEASE);
        return 0;
    }
    if(cmd!=expedited)return -EINVAL;
    if(!__atomic_load_n(&space->membarrierPrivateRegistered,__ATOMIC_ACQUIRE))
        return -EPERM;
    return ApicMemoryBarrierAllCpus() ? 0 : -ETIMEDOUT;
}

// ---------------------------------------------------------------------------
// sys_sched_getaffinity (204) — get CPU affinity mask
// ---------------------------------------------------------------------------

int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
                               uint64_t maskAddr, uint64_t, uint64_t, uint64_t)
{
    (void)pid;
    if (cpusetsize < 8) return -EINVAL;
    // Report all 8 CPUs available
    const uint64_t mask = 0xFF; // 8 CPUs
    if (!CopyToUser(maskAddr, &mask, sizeof(mask))) return -EFAULT;
    return 8; // size of mask in bytes
}

// ---------------------------------------------------------------------------
// sys_sched_setaffinity (203) — set CPU affinity mask (stub: succeed)
// ---------------------------------------------------------------------------

int64_t sys_sched_setaffinity(uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t)
{
    return 0; // no-op
}

// ---------------------------------------------------------------------------
// sys_sched_setparam (142) / sys_sched_getparam (143)
// sys_sched_setscheduler (144) / sys_sched_getscheduler (145)
// sys_sched_get_priority_max (146) / sys_sched_get_priority_min (147)
// ---------------------------------------------------------------------------

int64_t sys_sched_setparam(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    return 0; // pretend success
}

int64_t sys_sched_getparam(uint64_t, uint64_t paramAddr, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    // Return sched_priority = 0 (default for SCHED_OTHER). The old check
    // tested readability and then wrote, so a read-only page was written
    // and a bad pointer reported success.
    if (!paramAddr) return -EINVAL;
    const int32_t priority = 0;
    return CopyToUser(paramAddr, &priority, sizeof(priority)) ? 0 : -EFAULT;
}

int64_t sys_sched_setscheduler(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t)
{
    return 0; // pretend success
}

int64_t sys_sched_getscheduler(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t)
{
    return 0; // SCHED_OTHER
}

int64_t sys_sched_get_priority_max(uint64_t policy, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t)
{
    // Linux returns: SCHED_OTHER=0, SCHED_FIFO/RR=99
    if (policy == 1 || policy == 2) return 99; // SCHED_FIFO / SCHED_RR
    return 0; // SCHED_OTHER / SCHED_BATCH / SCHED_IDLE
}

int64_t sys_sched_get_priority_min(uint64_t policy, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t)
{
    if (policy == 1 || policy == 2) return 1; // SCHED_FIFO / SCHED_RR
    return 0;
}

int64_t sys_getpriority(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    // Linux's successful syscall result is 20 - nice, not libc's signed nice.
    // All tasks currently expose fixed nice 0; selectors and nice changes
    // remain compatibility stubs, separate from the scheduler priority hint.
    return 20;
}

int64_t sys_setpriority(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    return 0; // priority changes are currently ignored
}

} // namespace brook
