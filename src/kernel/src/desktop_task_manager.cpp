#include "apic.h"
#include "brook_task_manager.h"
#include "desktop_task_policy.h"
#include "intel_bdw_buffer.h"
#include "intel_bdw_scanout.h"
#include "memory/heap.h"
#include "pid_namespace.h"
#include "process_resident.h"
#include "process_user_namespace.h"
#include "scheduler.h"
#include "smp.h"
#include "string.h"
#include "syscall_handlers.h"
#include "syscall_internal.h"
#include "user_access_scope.h"
#include "user_copy.h"

namespace brook {

int DesktopEndTask(Process* self, uint32_t pid, uint64_t incarnation);

namespace {

struct Workspace {
    static constexpr uint32_t GpuOwners = MAX_PROCESSES * 2;
    DebugTaskSnapshot tasks[MAX_PROCESSES];
    BrookTaskManagerRow rows[MAX_PROCESSES];
    BdwBufferOwnerUsage gpu[GpuOwners];
    uint64_t liveGroups[MAX_PROCESSES];
    uint64_t representativeIdentity[MAX_PROCESSES];
    uint16_t representativePid[MAX_PROCESSES];
    uint16_t groupRows[MAX_PROCESSES]; // row index plus one, zero means absent
};

bool g_sampling = false;
uint64_t g_lastSample = 0;
constexpr uint32_t DesktopUid = 1000;
static_assert(MAX_PROCESSES <= BROOK_TASK_MANAGER_MAX_ROWS);
static_assert(MAX_PROCESSES < 65535);

struct TableSample {
    Workspace* work;
    ~TableSample() {
        if (work) kfree(work);
        __atomic_store_n(&g_sampling, false, __ATOMIC_RELEASE);
    }
};

uint32_t CollectGroups(Workspace& work, uint32_t count, uint32_t callerPid, uint32_t uid)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& task = work.tasks[i];
        const auto group = task.tgid;
        if (!group || group >= MAX_PROCESSES || !task.groupIncarnation ||
            task.state == ProcessState::Terminated) continue;

        uint16_t& index = work.groupRows[group];
        const bool first = index == 0;
        if (first) index = ++total;
        auto& row = work.rows[index - 1];
        row.cpu_ms += task.userTicks + task.sysTicks;
        ++row.threads;
        if (!first && task.pid != group) continue;

        row.pid = group;
        row.incarnation = task.groupIncarnation;
        row.uid = task.euid;
        memcpy(row.name, task.name, sizeof(row.name));
        row.flags = task.kernel || task.systemService || !task.uid || !task.euid ?
                    BROOK_TASK_MANAGER_SYSTEM : 0;
        if (task.pid == group && DesktopTaskMayEnd(uid, callerPid, group, task.uid, task.euid, row.flags))
            row.flags |= BROOK_TASK_MANAGER_CAN_END;
        work.representativePid[index - 1] = task.pid;
        work.representativeIdentity[index - 1] = task.incarnation;
        work.liveGroups[group] = task.groupIncarnation;
    }
    return total;
}

void MeasureResources(Workspace& work, uint32_t count)
{
    const bool gpuTracked = BdwIntelUserNodeEnabled();
    if (gpuTracked) {
        const auto totals = BdwGpuBufferMemorySnapshot(work.gpu, Workspace::GpuOwners,
                                                        work.liveGroups, MAX_PROCESSES);
        if (!totals.unlistedOwnersBuffers) {
            for (uint32_t i = 0; i < count; ++i)
                work.rows[i].flags |= BROOK_TASK_MANAGER_GPU_MEMORY_KNOWN;
        }
        for (const auto& owner : work.gpu) {
            if (!owner.bytes || owner.exited || owner.pid >= MAX_PROCESSES) continue;
            const auto index = work.groupRows[owner.pid];
            if (!index || index > count) continue;
            auto& row = work.rows[index - 1];
            if (row.incarnation == owner.incarnation) row.gpu_memory_bytes = owner.bytes;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        ResidentPages resident{};
        if (!ProcessMeasureResident(work.representativePid[i], work.representativeIdentity[i], resident)) {
            work.rows[i].resident_bytes = resident.Total() * 4096;
            work.rows[i].flags |= BROOK_TASK_MANAGER_RESIDENT_KNOWN;
        }
    }
}

int Snapshot(Process* actor, uint64_t headerPtr, uint64_t rowsPtr, uint64_t capacity)
{
    if (!capacity || capacity > BROOK_TASK_MANAGER_MAX_ROWS) return -EINVAL;
    UserAccessScope user(actor);
    if (!UserBufferWritable(headerPtr, sizeof(BrookTaskManagerSnapshot)) ||
        !UserBufferWritable(rowsPtr, capacity * sizeof(BrookTaskManagerRow))) return -EFAULT;

    if (__atomic_exchange_n(&g_sampling, true, __ATOMIC_ACQUIRE)) return -EAGAIN;
    TableSample sample{nullptr};
    const uint64_t now = ApicTickCount();
    if (g_lastSample && now - g_lastSample < 250) return -EAGAIN;
    g_lastSample = now;
    sample.work = static_cast<Workspace*>(kmalloc(sizeof(Workspace)));
    if (!sample.work) return -ENOMEM;
    auto& work = *sample.work;
    memset(&work, 0, sizeof(work));

    uint32_t tasks = 0;
    uint64_t begin = 0, end = 0;
    const auto captured = SchedulerSnapshotDebugTasks(work.tasks, MAX_PROCESSES, &tasks, &begin, &end);
    if (captured != tasks) return -75; // EOVERFLOW
    const auto total = CollectGroups(work, captured, actor->tgid ? actor->tgid : actor->pid, DesktopUid);
    const uint32_t rows = capacity < total ? uint32_t(capacity) : total;
    MeasureResources(work, rows);

    const BrookTaskManagerSnapshot header{BROOK_TASK_MANAGER_VERSION, rows, total,
                                         SmpGetCpuCount(), end};
    if ((rows && !CopyToUser(rowsPtr, work.rows, rows * sizeof(BrookTaskManagerRow))) ||
        !CopyToUser(headerPtr, &header, sizeof(header))) return -EFAULT;
    return 0;
}

}

int64_t sys_brook_task_manager(uint64_t op, uint64_t version, uint64_t a, uint64_t b, uint64_t c, uint64_t reserved)
{
    auto* actor = ProcessCurrent();
    if (!actor) return -ESRCH;
    if (version != BROOK_TASK_MANAGER_VERSION || reserved) return -EINVAL;
    KernelOperationGuard operation(actor);
    auto authority = ProcessUserAuthorityAcquire(actor);
    const bool allowed = !authority.ns && !ProcessPidView(actor) && authority.uid == DesktopUid;
    UserNamespaceRelease(authority.ns);
    if (!allowed) return -EPERM;
    if (op == BROOK_TASK_MANAGER_SNAPSHOT) return Snapshot(actor, a, b, c);
    if (op == BROOK_TASK_MANAGER_END_TASK && a < MAX_PROCESSES && b && !c)
        return DesktopEndTask(actor, uint32_t(a), b);
    return -EINVAL;
}

}
