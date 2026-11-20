#include "seccomp.h"
#include "syscall_internal.h"
#include "user_copy.h"

namespace brook {
namespace {
constexpr uint64_t SetModeFilter = 1;
constexpr uint64_t GetActionAvailable = 2;
constexpr uint64_t ThreadSync = 1;
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
    if (flags & ~(ThreadSync | ThreadSyncEsrch)) return -EINVAL;
    Process* process = ProcessCurrent();
    if (!process || process->isKernelThread) return -ESRCH;
    KernelOperationGuard operation(process);
    FilterProgram input{};
    if (!CopyFromUser(&input, program, sizeof(input))) return -EFAULT;
    if (!input.length || input.length > SeccompMaxFilterInstructions) return -EINVAL;
    if (!ProcessGetNoNewPrivs(process)) return -AccessDenied;

    const auto bytes = input.length * sizeof(SeccompInstruction);
    auto* filter = static_cast<SeccompFilter*>(kmalloc(sizeof(SeccompFilter) + bytes));
    if (!filter) return -ENOMEM;
    filter->previous = nullptr;
    filter->references = 1;
    filter->count = input.length;
    filter->pathInstructions = 0;
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

uint32_t SeccompEvaluate(const SeccompFilter* filter, const SeccompData& data)
{
    uint32_t verdict = SeccompAllow;
    for (; filter; filter = filter->previous)
        verdict = SeccompSelectAction(verdict, SeccompExecute(filter->code, filter->count, data));
    return verdict;
}

int64_t SeccompDenied(Process* process, uint64_t syscall, uint32_t verdict)
{
    const auto data = static_cast<uint16_t>(verdict);
    switch (verdict & SeccompActionMask) {
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
