// syscall_kcmp.cpp -- kcmp(2): do two tasks share a kernel object?
//
// Linux semantics (kernel/kcmp.c): ESRCH for a missing task, EPERM unless the
// caller may ptrace-read both (PTRACE_MODE_READ_REALCREDS), EBADF for a
// missing descriptor, EINVAL for an unknown type. The result orders
// obfuscated object identities (kcmp.h). Only pointer values are compared;
// nothing is dereferenced, so a racing close/exec yields some consistent
// answer, as on Linux.

#include "kcmp.h"
#include "pid_namespace.h"
#include "fd_table.h"
#include "process.h"
#include "process_user_namespace.h"
#include "scheduler.h"
#include "sysv_sem.h"
#include "syscall_internal.h"
#include "syscall_handlers.h"

namespace brook {
namespace {

constexpr int64_t EOPNOTSUPP_ = 95;
KcmpCookies g_kcmpCookies;
uint32_t g_kcmpCookieState; // 0 none, 1 filling, 2 ready

const KcmpCookies& Cookies()
{
    if (__atomic_load_n(&g_kcmpCookieState, __ATOMIC_ACQUIRE) == 2) return g_kcmpCookies;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&g_kcmpCookieState, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        KcmpCookiesInit(g_kcmpCookies, [] {
            uint64_t value;
            return RdrandU64(&value) ? value : SoftRandU64();
        });
        __atomic_store_n(&g_kcmpCookieState, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&g_kcmpCookieState, __ATOMIC_ACQUIRE) != 2) {
#if defined(__x86_64__)
            __asm__ volatile("pause");
#endif
        }
    }
    return g_kcmpCookies;
}

// The open file description a descriptor names, or 0 for a free slot.
uint64_t FileIdentity(Process* task, uint64_t index)
{
    const uint32_t fd = static_cast<uint32_t>(index); // Linux: unsigned int
    if (fd >= MAX_FDS) return 0;
    SpinLock* lock = ProcessFdLock(task);
    SpinLockAcquire(lock);
    uint64_t identity = 0;
    if (FdEntry* table = task->fds) {
        const FdEntry& entry = table[fd];
        if (entry.type != FdType::None && !entry.closing)
            identity = entry.file ? reinterpret_cast<uintptr_t>(entry.file)
                                  : reinterpret_cast<uintptr_t>(entry.handle);
    }
    SpinLockRelease(lock);
    return identity;
}

uint64_t SighandIdentity(Process* task)
{
    // Handlers live in a per-thread-group table (ReadSignalAction(tgid)).
    const uint64_t group = task->isThread ? task->leaderIncarnation : task->incarnation;
    return group << 16 | task->tgid;
}

struct TaskRef {
    Process* task;
    explicit TaskRef(const PidNamespace* view, int32_t pid)
        : task(pid > 0 ? SchedulerFindVisibleProcessReference(view, pid) : nullptr) {}
    ~TaskRef() { if (task) ProcessUnref(task); }
    TaskRef(const TaskRef&) = delete;
    TaskRef& operator=(const TaskRef&) = delete;
};

} // namespace

int64_t KcmpTasks(Process* self, Process* first, Process* second, uint64_t type,
                  uint64_t index1, uint64_t index2)
{
    if (!first || !second) return -ESRCH;
    if (!ProcessCanPtraceRead(self, first) || !ProcessCanPtraceRead(self, second)) return -EPERM;
    uint64_t a = 0, b = 0;
    switch (type) {
    case KcmpFile:
        a = FileIdentity(first, index1);
        b = FileIdentity(second, index2);
        if (!a || !b) return -EBADF;
        break;
    case KcmpVm:
        a = reinterpret_cast<uintptr_t>(__atomic_load_n(&first->addressSpace, __ATOMIC_ACQUIRE));
        b = reinterpret_cast<uintptr_t>(__atomic_load_n(&second->addressSpace, __ATOMIC_ACQUIRE));
        break;
    case KcmpFiles:
        a = reinterpret_cast<uintptr_t>(__atomic_load_n(&first->fds, __ATOMIC_ACQUIRE));
        b = reinterpret_cast<uintptr_t>(__atomic_load_n(&second->fds, __ATOMIC_ACQUIRE));
        break;
    case KcmpFs:
        a = reinterpret_cast<uintptr_t>(__atomic_load_n(&first->fsContext, __ATOMIC_ACQUIRE));
        b = reinterpret_cast<uintptr_t>(__atomic_load_n(&second->fsContext, __ATOMIC_ACQUIRE));
        break;
    case KcmpSighand:
        a = SighandIdentity(first);
        b = SighandIdentity(second);
        break;
    case KcmpIo:
        break; // no I/O context objects: equal, like two Linux tasks without one
    case KcmpSysvsem:
        a = SysvSemUndoIdentity(first);
        b = SysvSemUndoIdentity(second);
        break;
    case KcmpEpollTfd:
        return -EOPNOTSUPP_;
    default:
        return -EINVAL;
    }
    return KcmpOrder(Cookies(), static_cast<uint32_t>(type), a, b);
}

int64_t sys_kcmp(uint64_t pid1, uint64_t pid2, uint64_t type,
                 uint64_t index1, uint64_t index2, uint64_t)
{
    Process* self = ProcessCurrent();
    if (!self) return -ESRCH;
    // Hold the caller's kernel region so a kill cannot strand the references.
    SchedulerKernelRegionEnter(self);
    int64_t result;
    {
        const auto* view = ProcessPidView(self);
        TaskRef first(view, static_cast<int32_t>(pid1)), second(view, static_cast<int32_t>(pid2));
        result = KcmpTasks(self, first.task, second.task, static_cast<uint32_t>(type), index1, index2);
    }
    SchedulerKernelRegionLeave(self);
    return result;
}

} // namespace brook
