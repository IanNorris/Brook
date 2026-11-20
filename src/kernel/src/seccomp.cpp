#include "seccomp.h"
#include "apic.h"
#include "klog.h"
#include "process_user_namespace.h"
#include "syscall_internal.h"
#include "user_copy.h"

namespace brook {
namespace {
constexpr uint64_t SetModeFilter = 1;
constexpr uint64_t GetActionAvailable = 2;
constexpr uint64_t ThreadSync = 1;
constexpr uint64_t FilterLog = 2;
// There is no seccomp-specific speculation toggle on Brook. Allow programs
// to request the same opt-out accepted on Linux with a no-op arch hook.
constexpr uint64_t SpecAllow = 4;
constexpr uint64_t ThreadSyncEsrch = 16;
constexpr uint64_t ModeFilter = 2;
constexpr int AccessDenied = 13;
constexpr int OperationNotSupported = 95;

struct FilterProgram {
    uint16_t length;
    uint16_t padding[3];
    uint64_t instructions;
};
static_assert(sizeof(FilterProgram) == 16);

int64_t InstallFilter(uint64_t flags, uint64_t program)
{
    if (flags & ~(ThreadSync | FilterLog | SpecAllow | ThreadSyncEsrch)) return -EINVAL;
    Process* process = ProcessCurrent();
    if (!process || process->isKernelThread) return -ESRCH;
    KernelOperationGuard operation(process);
    FilterProgram input{};
    if (!CopyFromUser(&input, program, sizeof(input))) return -EFAULT;
    if (!input.length || input.length > SeccompMaxFilterInstructions) return -EINVAL;
    if (!ProcessGetNoNewPrivs(process)) {
        auto* userNamespace = ProcessUserNamespaceAcquire(process);
        const bool privileged = ProcessCapable(process, userNamespace, CapSysAdmin);
        UserNamespaceRelease(userNamespace);
        if (!privileged) return -AccessDenied;
    }

    const auto bytes = input.length * sizeof(SeccompInstruction);
    auto* filter = static_cast<SeccompFilter*>(kmalloc(sizeof(SeccompFilter) + bytes));
    if (!filter) return -ENOMEM;
    filter->previous = nullptr;
    filter->references = 1;
    filter->count = input.length;
    filter->pathInstructions = 0;
    filter->logRequested = (flags & FilterLog) != 0;
    int64_t result;
    if (!CopyFromUser(filter->code, input.instructions, bytes)) result = -EFAULT;
    else if (!SeccompVerify(filter->code, input.length)) result = -EINVAL;
    else {
        result = SchedulerAttachSeccomp(process, filter, (flags & ThreadSync) != 0);
        if (result > 0 && (flags & ThreadSyncEsrch)) result = -ESRCH;
    }
    SeccompRelease(filter);
    return result;
}
}

void SeccompRetain(SeccompFilter* filter)
{
    if (filter) __atomic_add_fetch(&filter->references, 1, __ATOMIC_RELAXED);
}

void SeccompRelease(SeccompFilter* filter)
{
    while (filter && __atomic_sub_fetch(&filter->references, 1, __ATOMIC_ACQ_REL) == 0) {
        SeccompFilter* previous = filter->previous;
        kfree(filter);
        filter = previous;
    }
}

uint32_t SeccompEvaluate(const SeccompFilter* filter, const SeccompData& data, bool* audit)
{
    uint32_t verdict = SeccompAllow;
    bool logRequested = false;
    for (; filter; filter = filter->previous) {
        const auto selected = SeccompSelectAction(verdict, SeccompExecute(filter->code, filter->count, data));
        if (selected != verdict) logRequested = filter->logRequested;
        verdict = selected;
    }
    verdict = SeccompEnforceAction(verdict);
    const uint32_t action = verdict & SeccompActionMask;
    *audit = action == SeccompLog || action == SeccompKillProcess || action == SeccompKillThread ||
        (logRequested && action != SeccompAllow);
    return verdict;
}

void SeccompAudit(const Process* process, uint64_t syscall, uint32_t verdict)
{
    // Installing filters is unprivileged. Allow useful audit samples without
    // letting a default-LOG policy tie disk/console work to every syscall.
    static uint64_t nextAuditMs;
    const uint64_t now = ApicTickCount();
    uint64_t deadline = __atomic_load_n(&nextAuditMs, __ATOMIC_RELAXED);
    if (now < deadline || !__atomic_compare_exchange_n(&nextAuditMs, &deadline, now + 100,
            false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    KLogCat(LogCat::Syscall, LogLevel::Info, "seccomp: pid=%u syscall=%u action=0x%x",
            process->pid, static_cast<uint32_t>(syscall), verdict & SeccompActionMask);
}

int64_t SeccompDenied(Process* process, uint64_t syscall, uint32_t verdict)
{
    const auto data = static_cast<uint16_t>(verdict);
    switch (verdict & SeccompActionMask) {
    case SeccompTrace:
    case SeccompUserNotify: return -ENOSYS; // no tracer or notification listener
    case SeccompErrno: return -int64_t(data > 4095 ? 4095 : data);
    case SeccompTrap:
        process->seccompTrap = SeccompTrap | data;
        // Linux restores the original syscall number to RAX for the signal
        // handler. SyscallCheckSignals supplies the positive SYS_SECCOMP code.
        return static_cast<int64_t>(syscall);
    case SeccompKillThread:
        SchedulerExitCurrentProcess(128 + SeccompSignal, SeccompSignal);
        break;
    default:
        SchedulerKillThreadGroup(process->tgid, process, 128 + SeccompSignal, SeccompSignal);
        SchedulerExitCurrentProcess(128 + SeccompSignal, SeccompSignal);
        break;
    }
    __builtin_unreachable();
}

int64_t SeccompPrctl(uint64_t option, uint64_t mode, uint64_t program)
{
    if (option == 21) {
        Process* process = ProcessCurrent();
        return process && SeccompLoad(process) ? ModeFilter : 0;
    }
    if (mode != ModeFilter) return -EINVAL;
    return InstallFilter(0, program);
}

int64_t SysSeccomp(uint64_t operation, uint64_t flags, uint64_t argument,
                  uint64_t, uint64_t, uint64_t)
{
    switch (operation) {
    case SetModeFilter: return InstallFilter(flags, argument);
    case GetActionAvailable: {
        if (flags) return -EINVAL;
        uint32_t action = 0;
        if (!CopyFromUser(&action, argument, sizeof(action))) return -EFAULT;
        return SeccompSupportedAction(action) ? 0 : -OperationNotSupported;
    }
    default: return -EINVAL;
    }
}
}
