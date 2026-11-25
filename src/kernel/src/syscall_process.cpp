// syscall_process.cpp -- process lifecycle syscalls: fork/clone/exit/wait, ids, process groups, arch_prctl, prctl.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process_namespaces.h"
#include "process_fs.h"
#include "process_user_namespace.h"

#include "posix_timer.h"
#include "sysv_sem.h"
#include "cpu.h"
#include "string.h"
#include "tsc_clock.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_handlers.h"
#include "seccomp.h"
#include "pid_namespace.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_arch_prctl (158) -- TLS setup
// ---------------------------------------------------------------------------

static constexpr uint64_t ARCH_SET_GS    = 0x1001;
static constexpr uint64_t ARCH_GET_GS    = 0x1004;
static constexpr uint64_t ARCH_SET_FS    = 0x1002;
static constexpr uint64_t ARCH_GET_FS    = 0x1003;
static constexpr uint64_t ARCH_CET_STATUS = 0x3001;

int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t,
                        uint64_t, uint64_t, uint64_t)
{
    switch (code)
    {
    case ARCH_SET_GS:
    case ARCH_SET_FS:
    {
        // Restrict bases to the canonical user half, even if not mapped yet.
        // High GS would also defeat the interrupt-entry kernel GS discriminator.
        if (!UserSegmentBaseValid(addr)) return -EPERM;
        Process* proc = ProcessCurrent();
        if (!proc) return -EINVAL;
        uint64_t flags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        if (code == ARCH_SET_GS) {
            proc->savedCtx.gsBase = addr;
            // Kernel GS is active here. Change only the parked user base.
            WriteMsr(MSR_KERNEL_GS_BASE, addr);
        } else {
            proc->fsBase = addr;
            proc->savedCtx.fsBase = addr;
            WriteMsr(0xC0000100U, addr);
        }
        __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
        return 0;
    }
    case ARCH_GET_GS:
    {
        const uint64_t gsBase = ReadMsr(MSR_KERNEL_GS_BASE);
        return CopyToUser(addr, &gsBase, sizeof(gsBase)) ? 0 : -EFAULT;
    }
    case ARCH_GET_FS:
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100U));
        uint64_t fsBase = (static_cast<uint64_t>(hi) << 32) | lo;
        return CopyToUser(addr, &fsBase, sizeof(fsBase)) ? 0 : -EFAULT;
    }
    case ARCH_CET_STATUS:
        return -EINVAL; // CET not supported
    default:
        return -EINVAL;
    }
}

// ---------------------------------------------------------------------------
// sys_exit (60) / sys_exit_group (231)
// ---------------------------------------------------------------------------

int64_t sys_exit(uint64_t status, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t)
{
    {
        Process* p = ProcessCurrent();
        (void)p;
        DbgPrintf("sys_exit: '%s' (pid %u, tgid %u) exited with status %lu\n",
                  p ? p->name : "?", p ? p->pid : 0, p ? p->tgid : 0, status);
    }

    // Linux sys_exit terminates only the calling thread, including the
    // leader. SchedulerExitCurrentProcess retains shared FDs/address space
    // while live peers exist; asLiveThreads prevents premature leader reap.
    Process* proc = ProcessCurrent();

    // Wake vfork parent if child exits without exec
    if (proc) ProcessVforkRelease(proc);

    SchedulerExitCurrentProcess(static_cast<int>(status));
    // never reached
    return 0;
}

void ProcessVforkRelease(Process* child)
{
    Process* parent = __atomic_exchange_n(&child->vforkParent, nullptr, __ATOMIC_ACQ_REL);
    if (!parent) return;
    __atomic_store_n(&parent->vforkWaiting, 0, __ATOMIC_RELEASE);
    SchedulerUnblock(parent);
    ProcessUnref(parent);
}

int64_t sys_exit_group(uint64_t status, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    DbgPrintf("sys_exit_group: '%s' tgid %u exiting with status %lu\n",
              proc ? proc->name : "?", proc ? proc->tgid : 0, status);

    // Kill all other threads in this thread group so they don't linger
    // and cause use-after-free on shared resources after the leader exits.
    if (proc)
        SchedulerKillThreadGroup(proc->tgid, proc, static_cast<int>(status));

    // Wake vfork parent if child exits without exec
    if (proc) ProcessVforkRelease(proc);

    SchedulerExitCurrentProcess(static_cast<int>(status));
    // never reached
    return 0;
}

// ---------------------------------------------------------------------------
// sys_getpid (39)
// ---------------------------------------------------------------------------

int64_t sys_getpid(uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return 1;
    // For threads, getpid returns the thread group leader's PID (tgid)
    return ProcessVisibleTgid(proc, ProcessPidView(proc));
}

// ---------------------------------------------------------------------------
// sys_getppid (110)
// ---------------------------------------------------------------------------

int64_t sys_getppid(uint64_t, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? SchedulerVisibleParentTid(proc, ProcessPidView(proc)) : 0;
}

// ---------------------------------------------------------------------------
// sys_fork (57) / sys_clone (56) / sys_vfork (58)
// ---------------------------------------------------------------------------
// The fork syscall needs access to the user-mode return address (RCX) and
// RFLAGS (R11) that were saved on the kernel stack by BrookSyscallDispatcher.
// These are at known offsets from the current kernel stack frame.

int64_t sys_fork(uint64_t, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t)
{
    Process* parent = ProcessCurrent();
    if (!parent) return -ENOSYS;

    Process* child = ProcessFork(parent, parent->syscallUserRip,
                                 parent->syscallUserRsp,
                                 parent->syscallUserRflags);
    if (!child) return -ENOMEM;
    if (const int error = ProcessPidPrepareChild(child, parent, false)) {
        ProcessDiscardUnpublished(child);
        return error;
    }

    // Write captured register values into the child process struct.
    child->forkRbx = parent->syscallUserRbx;
    child->forkRbp = parent->syscallUserRbp;
    child->forkR12 = parent->syscallUserR12;
    child->forkR13 = parent->syscallUserR13;
    child->forkR14 = parent->syscallUserR14;
    child->forkR15 = parent->syscallUserR15;
    child->forkRdi = parent->syscallUserRdi;
    child->forkRsi = parent->syscallUserRsi;
    child->forkRdx = parent->syscallUserRdx;
    child->forkR8  = parent->syscallUserR8;
    child->forkR9  = parent->syscallUserR9;
    child->forkR10 = parent->syscallUserR10;

    const uint32_t visibleTid = ProcessVisibleTid(child, ProcessPidView(parent));
    [[maybe_unused]] const uint16_t globalPid = child->pid, globalTgid = child->tgid;
    if (!SchedulerAddProcessWithSeccomp(child, parent)) {
        ProcessDiscardUnpublished(child);
        return -ENOMEM;
    }

    DbgPrintf("FORK: parent pid=%u tgid=%u -> child pid=%u tgid=%u\n",
              parent->pid, parent->tgid, globalPid, globalTgid);
    return visibleTid;
}

int64_t sys_clone(uint64_t flags, uint64_t newStack, uint64_t parentTidAddr,
                   uint64_t childTidAddr, uint64_t tlsAddr, uint64_t)
{
    // Clone flags used by musl pthread_create:
    //   CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
    //   CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID
    static constexpr uint64_t CLONE_FS             = 0x00000200;
    static constexpr uint64_t CLONE_THREAD         = 0x00010000;
    static constexpr uint64_t CLONE_SETTLS          = 0x00080000;
    static constexpr uint64_t CLONE_PARENT_SETTID   = 0x00100000;
    static constexpr uint64_t CLONE_CHILD_CLEARTID  = 0x00200000;
    static constexpr uint64_t CLONE_CHILD_SETTID    = 0x01000000;
    static constexpr uint64_t CLONE_VFORK           = 0x00004000;
    static constexpr uint64_t CLONE_CLEAR_SIGHAND   = 0x100000000ULL;
    static constexpr uint64_t CLONE_NEWNS           = 0x00020000ULL;

    // Other isolation domains must not fall through to an ordinary fork.
    constexpr uint64_t unsupportedNamespaces = 0x02000000ULL;
    constexpr uint64_t CLONE_NEWUSER=0x10000000ULL;
    if(flags&unsupportedNamespaces)return -EINVAL;
    Process* parent = ProcessCurrent();
    if (!parent) return -ENOSYS;
    constexpr uint64_t supportedNamespaces=CLONE_NEWNS|CLONE_NEWUSER|CloneNewRuntimeNamespaces|CloneNewPid;
    if(flags&supportedNamespaces) {
        // Bounded fork-style namespace creation. Do not silently ignore shared
        // VM/FS/thread, tid, pidfd, TLS, vfork or unknown requested semantics.
        if((flags&~(supportedNamespaces|uint64_t{0xff})) || (flags&0xff)!=17)return -EINVAL;
        if(!(flags&CLONE_NEWUSER)) {
            auto* owner=ProcessUserNamespaceAcquire(parent);
            bool allowed=ProcessCapable(parent,owner,CapSysAdmin);UserNamespaceRelease(owner);
            if(!allowed)return -EPERM;
        }
    }

    if ((flags & CLONE_THREAD) && (flags & CLONE_PARENT_SETTID) &&
        !UserBufferWritable(parentTidAddr, sizeof(int32_t))) return -EFAULT;
    // context_switch loads the FS base with wrmsr; a non-canonical or kernel
    // base would #GP in the kernel. Same rule as arch_prctl(ARCH_SET_FS).
    if ((flags & CLONE_SETTLS) && !UserSegmentBaseValid(tlsAddr)) return -EPERM;

    uint64_t userRip = parent->syscallUserRip;
    uint64_t userRsp = parent->syscallUserRsp;
    uint64_t userRflags = parent->syscallUserRflags;

    // If caller provided a new stack, use it for the child.
    if (newStack)
        userRsp = newStack;

    Process* child;

    if (flags & CLONE_THREAD)
    {
        // Thread creation: share address space
        uint64_t tls = (flags & CLONE_SETTLS) ? tlsAddr : parent->fsBase;
        child = ProcessCreateThread(parent, userRip, userRsp, userRflags, tls,(flags & CLONE_FS)!=0);
        if (!child) return -ENOMEM;

    }
    else
    {
        // Fork: copy address space
        child = ProcessFork(parent, userRip, userRsp, userRflags,(flags & CLONE_FS)!=0);
        if (!child) return -ENOMEM;
    }
    // CLONE_CHILD_CLEARTID: cleared and woken when the child exits, for
    // threads and forks alike (glibc's _Fork relies on both TID flags).
    if (flags & CLONE_CHILD_CLEARTID)
        child->clearChildTid = childTidAddr;
    // CLONE_CHILD_SETTID: stored in the child's own memory by the child
    // itself (ForkChildTrampoline) before it first returns to user mode.
    if (flags & CLONE_CHILD_SETTID)
        child->setChildTid = childTidAddr;

    if(flags&CLONE_NEWUSER) {
        auto* context=ProcessFsAcquire(child);
        const bool hasNamespaceRoot=FsContextHasNamespaceRoot(context);
        FsContextRelease(context);
        if(!hasNamespaceRoot) {ProcessDiscardUnpublished(child);return -EPERM;}
        if(int error=ProcessUserNamespaceCreate(child)) {ProcessDiscardUnpublished(child);return error;}
    }
    if(flags&CLONE_NEWNS) {
        if(int error=ProcessFsIsolateMounts(child)) {ProcessDiscardUnpublished(child);return error;}
    }
    if(const auto domains=flags&CloneNewRuntimeNamespaces) {
        if(int error=ProcessNamespacesIsolate(child,domains)) {ProcessDiscardUnpublished(child);return error;}
    }
    if (const int error = ProcessPidPrepareChild(child, parent, (flags & CloneNewPid) != 0)) {
        ProcessDiscardUnpublished(child);
        return error;
    }
    const uint32_t visibleTid = ProcessVisibleTid(child, ProcessPidView(parent));
    if (int error=SysvSemClone(parent,child,(flags & 0x00040000)!=0)) {
        SysvSemExit(child);
        ProcessDiscardUnpublished(child);
        return error;
    }
    if (flags & CLONE_PARENT_SETTID) {
        // Threads checked the address above. For a fork, Linux stores into
        // the parent only and ignores a failed store.
        const int32_t tid = static_cast<int32_t>(visibleTid);
        const bool stored = CopyToUser(parentTidAddr, &tid, sizeof(tid));
        if (!stored && (flags & CLONE_THREAD)) {
            SysvSemExit(child);
            ProcessDiscardUnpublished(child);
            return -EFAULT;
        }
    }

    child->forkRbx = parent->syscallUserRbx;  child->forkRbp = parent->syscallUserRbp;
    child->forkR12 = parent->syscallUserR12;  child->forkR13 = parent->syscallUserR13;
    child->forkR14 = parent->syscallUserR14;  child->forkR15 = parent->syscallUserR15;
    child->forkRdi = parent->syscallUserRdi;  child->forkRsi = parent->syscallUserRsi;
    child->forkRdx = parent->syscallUserRdx;  child->forkR8  = parent->syscallUserR8;
    child->forkR9  = parent->syscallUserR9;   child->forkR10 = parent->syscallUserR10;

    // CLONE_CLEAR_SIGHAND: reset all signal handlers to SIG_DFL in the child.
    // Used by Ladybird's WebContent spawn so inherited handlers don't fire
    // before exec replaces the address space.
    if ((flags & CLONE_CLEAR_SIGHAND) && !(flags & CLONE_THREAD))
    {
        uint32_t childPid = child->pid;
        if (childPid < MAX_PROCESSES)ResetSignalActions(childPid);
    }

    // CLONE_VFORK: publish the (referenced) parent before the child can run,
    // so an exec or exit that happens first still releases it.
    const bool vfork = (flags & CLONE_VFORK) && !(flags & CLONE_THREAD);
    if (vfork) {
        ProcessRef(parent);
        __atomic_store_n(&parent->vforkWaiting, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&child->vforkParent, parent, __ATOMIC_RELEASE);
    }

    const uint16_t globalPid = child->pid;
    [[maybe_unused]] const uint16_t globalTgid = child->tgid;
    if (!SchedulerAddProcessWithSeccomp(child, parent)) {
        if (vfork) ProcessVforkRelease(child);
        ProcessDiscardUnpublished(child);
        return -ENOMEM;
    }

    {
        if (!(flags & CLONE_THREAD))
            SerialPrintf("[PROFILE] fork t=%lums parent_pid=%u child_pid=%u\n",
                         ApicTickCount(), parent->pid, globalPid);
    }

    DbgPrintf("CLONE: parent '%s' pid=%u tgid=%u -> child pid=%u tgid=%u flags=0x%lx %s\n",
              parent->name, parent->pid, parent->tgid, globalPid, globalTgid, flags,
              (flags & CLONE_THREAD) ? "THREAD" : "FORK");

    // CLONE_VFORK: parent blocks until child calls exec or exit.
    // SchedulerBlock returns on any wake, so wait for the release itself. A
    // release between the check and the block leaves a pending wakeup, so
    // the block returns at once. Termination of the parent ends the wait.
    if (vfork)
    {
        while (__atomic_load_n(&parent->vforkWaiting, __ATOMIC_ACQUIRE) &&
               !__atomic_load_n(&parent->terminalPending, __ATOMIC_ACQUIRE))
            SchedulerBlock(parent);
    }

    return visibleTid;
}

int64_t sys_vfork(uint64_t, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    // vfork: fork + parent blocks until child exec/exit.
    // Implemented via CLONE_VFORK in sys_clone.
    static constexpr uint64_t CLONE_VFORK = 0x00004000;
    return sys_clone(CLONE_VFORK, 0, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// sys_clone3 (435)
// ---------------------------------------------------------------------------
// Extended clone with struct clone_args. We extract fields and delegate
// to sys_clone so all thread/fork logic stays in one place.
// glibc and musl try clone3 first and fall back to clone on ENOSYS;
// implementing it avoids that fallback and silences the UNIMPL noise.

int64_t sys_unshare(uint64_t flags,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)
{
    if(!flags)return 0;
    constexpr uint64_t CloneNewUser=0x10000000;
    if (flags != CloneNewUser && flags != CloneNewPid && (flags & ~CloneNewRuntimeNamespaces)) return -EINVAL;
    auto* process=ProcessCurrent();
    if(!process)return -ESRCH;
    if(flags==CloneNewUser) {
        // Only the running task can create a peer in a single-threaded group.
        // Copy CLONE_FS state before gaining authority in the child namespace.
        if(process->isThread || __atomic_load_n(&process->asLiveThreads,__ATOMIC_ACQUIRE))
            return -EINVAL;
        int error=0;
        auto* context=ProcessFsCopy(process,&error);
        if(!context)return error;
        // A chroot must not be able to recover capabilities in a new nested
        // user namespace after applying its filesystem boundary.
        if(!FsContextHasNamespaceRoot(context)) {FsContextRelease(context);return -EPERM;}
        error=ProcessUserNamespaceCreate(process);
        if(error)FsContextRelease(context);
        else ProcessFsReplaceOwned(process,context);
        return error;
    }
    auto* owner=ProcessUserNamespaceAcquire(process);
    const bool allowed=ProcessCapable(process,owner,CapSysAdmin);
    UserNamespaceRelease(owner);
    if (!allowed) return -EPERM;
    if (flags == CloneNewPid) {
        if (process->isThread || __atomic_load_n(&process->asLiveThreads,__ATOMIC_ACQUIRE)) return -EINVAL;
        return ProcessPidUnshare(process);
    }
    return ProcessNamespacesIsolate(process, flags);
}

struct CloneArgs {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

int64_t sys_clone3(uint64_t argsPtr, uint64_t size, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    if (!argsPtr || size < offsetof(CloneArgs, tls) + sizeof(uint64_t))
        return -EINVAL;
    if (!UserBufferReadable(argsPtr, size))
        return -EFAULT;

    CloneArgs copied{};
    if (!CopyFromUser(&copied, argsPtr, size < sizeof(copied) ? size : sizeof(copied)))
        return -EFAULT;
    const CloneArgs* ca = &copied;
    const uint64_t cloneFlags=ca->flags;
    // CLONE_NEWTIME occupies an exit-signal bit in legacy clone, but it is an
    // independent unsupported namespace request in clone3.
    if(cloneFlags&0x80)return -EINVAL;

    // Stack: clone3 passes base+size; clone expects the top (stack grows down)
    uint64_t stackTop = ca->stack;
    if (stackTop && ca->stack_size)
        stackTop = ca->stack + ca->stack_size;

    // Delegate to sys_clone with the same argument layout:
    //   flags, newStack, parentTidAddr, childTidAddr, tlsAddr
    if(cloneFlags&(0x00020000|0x10000000|CloneNewRuntimeNamespaces|CloneNewPid)) {
        for(uint64_t i=sizeof(CloneArgs);i<size;++i) {
            uint8_t byte=0;
            if(!CopyFromUser(&byte,argsPtr+i,1))return -EFAULT;
            if(byte)return -E2BIG;
        }
        if((cloneFlags&0xff) || ca->exit_signal!=17 || ca->pidfd || ca->set_tid || ca->set_tid_size || ca->cgroup)return -EINVAL;
        if(ca->stack && ca->stack_size>~uint64_t{0}-ca->stack)return -EINVAL;
        return sys_clone(cloneFlags|ca->exit_signal,stackTop,ca->parent_tid,ca->child_tid,ca->tls,0);
    }
    return sys_clone(cloneFlags, stackTop, ca->parent_tid, ca->child_tid, ca->tls, 0);
}

// ---------------------------------------------------------------------------
// sys_wait4 (61)
// ---------------------------------------------------------------------------
// Wait for a child process to terminate. Supports pid=-1 (any child)
// or a specific child PID. WNOHANG (options & 1) returns 0 if no child
// has exited yet instead of blocking.

static constexpr uint32_t WNOHANG = 1;
static constexpr uint32_t WUNTRACED = 2;
static constexpr uint32_t WEXITED = 4;
static constexpr uint32_t WCONTINUED = 8;
static constexpr uint32_t WNOWAIT = 0x01000000;

struct WaitSiginfo {
    int32_t signo, error, code, reserved;
    int32_t pid;
    uint32_t uid;
    int32_t status, reserved2;
    int64_t userTime, systemTime;
    uint8_t padding[80];
};
static_assert(sizeof(WaitSiginfo) == 128, "Linux siginfo size");
static_assert(__builtin_offsetof(WaitSiginfo, pid) == 16, "Linux SIGCHLD pid");
static_assert(__builtin_offsetof(WaitSiginfo, status) == 24, "Linux SIGCHLD status");

// Like Linux, the child is already reaped when the outputs are copied, so a
// raced unmap reports -EFAULT but cannot undo the wait.
static bool CopyWaitUsage(uint64_t address, const ChildWaitSnapshot& event)
{
    if (!address) return true;
    int64_t usage[18]{}; // Linux x86-64 rusage (144 bytes), tracked CPU ticks.
    usage[0] = event.userTicks / 1000;
    usage[1] = (event.userTicks % 1000) * 1000;
    usage[2] = event.sysTicks / 1000;
    usage[3] = (event.sysTicks % 1000) * 1000;
    return CopyToUser(address, usage, sizeof(usage));
}

static bool ChildWaitInterrupted(Process* caller)
{
    if (__atomic_load_n(&caller->terminalPending, __ATOMIC_ACQUIRE)) return true;
    uint64_t pending = (ProcessPendingSignals(caller) |
                        PosixTimerPending(caller)) & ~caller->sigMask;
    constexpr uint64_t childBit = 1ULL << (17 - 1);
    if (pending & ~childBit) return true;
    if (pending & childBit) {
        // Default/ignored SIGCHLD is a wait wakeup. A caught SIGCHLD must reach
        // its handler through normal syscall signal delivery instead of vanishing.
        if (ReadSignalAction(caller->tgid,16).handler > 1) return true;
        ProcessDequeueSignal(caller, 17, nullptr);
    }
    return false;
}

static int WaitChildEvent(Process* caller, int selector, uint32_t id,
                          uint32_t options, ChildWaitSnapshot* event)
{
    for (;;) {
        int result = SchedulerWaitChild(caller, selector, id, options, event);
        if (result != 0 || (options & WNOHANG)) return result;
        if (ChildWaitInterrupted(caller)) return -EINTR;
        // Exit publication sets pendingWakeup on every parent-group waiter;
        // SchedulerBlock consumes it even if notification preceded the block.
        caller->wakeupTick = ApicTickCount() + 1000;
        SchedulerBlock(caller);
        if (ChildWaitInterrupted(caller)) return -EINTR;
    }
}

int64_t sys_wait4(uint64_t pidArg, uint64_t statusAddr, uint64_t rawOptions,
                  uint64_t usageAddr, uint64_t, uint64_t)
{
    Process* parent = ProcessCurrent();
    if (!parent) return -ESRCH;
    uint32_t options = static_cast<uint32_t>(rawOptions);
    if (options & ~(WNOHANG | WUNTRACED | WCONTINUED)) return -EINVAL;
    if (statusAddr && !UserBufferWritable(statusAddr, sizeof(int32_t))) return -EFAULT;
    if (usageAddr && !UserBufferWritable(usageAddr, 144)) return -EFAULT;
    int32_t pid = static_cast<int32_t>(pidArg);
    if (pid == static_cast<int32_t>(0x80000000U)) return -ESRCH;
    int selector = pid == -1 ? 0 : pid > 0 ? 1 : pid < -1 ? 2 : 3;
    uint32_t id = pid < -1 ? static_cast<uint32_t>(-pid) : static_cast<uint32_t>(pid);
    ChildWaitSnapshot event{};
    int result = WaitChildEvent(parent, selector, id, options | WEXITED, &event);
    if (result <= 0) return result;
    const int32_t status = event.status;
    if (statusAddr && !CopyToUser(statusAddr, &status, sizeof(status))) return -EFAULT;
    if (!CopyWaitUsage(usageAddr, event)) return -EFAULT;
    return event.pid;
}

int64_t sys_waitid(uint64_t rawSelector, uint64_t rawId, uint64_t infoAddr,
                   uint64_t rawOptions, uint64_t usageAddr, uint64_t)
{
    Process* parent = ProcessCurrent();
    if (!parent) return -ESRCH;
    uint32_t selector = static_cast<uint32_t>(rawSelector);
    uint32_t id = static_cast<uint32_t>(rawId);
    uint32_t options = static_cast<uint32_t>(rawOptions);
    if (selector > 2 || (selector == 1 && static_cast<int32_t>(id) <= 0) ||
        (selector == 2 && static_cast<int32_t>(id) < 0) ||
        (options & ~(WNOHANG | WUNTRACED | WEXITED | WCONTINUED | WNOWAIT)) ||
        !(options & (WUNTRACED | WEXITED | WCONTINUED))) return -EINVAL;
    if (infoAddr && !UserBufferWritable(infoAddr, sizeof(WaitSiginfo))) return -EFAULT;
    if (usageAddr && !UserBufferWritable(usageAddr, 144)) return -EFAULT;
    if (selector == 2 && !id) selector = 3; // P_PGID with zero selects the caller’s group.
    ChildWaitSnapshot event{};
    int result = WaitChildEvent(parent, static_cast<int>(selector), id, options, &event);
    if (result < 0) return result;
    WaitSiginfo info{};
    if (result) {
        info.signo = 17;
        info.code = event.code;
        info.pid = event.pid;
        info.uid = ProcessReportId(parent,false,event.uid);
        info.status = event.code == 1 ? (event.status >> 8) & 0xff :
                      event.code == 5 ? (event.status >> 8) & 0xff :
                      event.code == 6 ? 18 : event.status & 0x7f;
        info.userTime = event.userTicks / 10; // Linux USER_HZ = 100.
        info.systemTime = event.sysTicks / 10;
        if (!CopyWaitUsage(usageAddr, event)) return -EFAULT;
    }
    if (infoAddr && !CopyToUser(infoAddr, &info, sizeof(info))) return -EFAULT;
    return 0;
}

// ---------------------------------------------------------------------------
// sys_set_tid_address (218)
// ---------------------------------------------------------------------------

int64_t sys_set_tid_address(uint64_t tidptr, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (proc)
        proc->clearChildTid = tidptr;
    return proc ? ProcessVisibleTid(proc, ProcessPidView(proc)) : 1;
}

// ---------------------------------------------------------------------------
// sys_getpgrp (111) / sys_getpgid (121) / sys_setpgid (109) / sys_getsid (124)
// ---------------------------------------------------------------------------

int64_t sys_getpgrp(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? SchedulerGetProcessGroup(proc, 0, false) : 1;
}

int64_t sys_getpgid(uint64_t pid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? SchedulerGetProcessGroup(proc, static_cast<uint32_t>(pid), false) : pid ? -ESRCH : 1;
}

int64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (static_cast<int32_t>(pgid) < 0) return -EINVAL;
    return SchedulerSetProcessGroup(proc, static_cast<uint32_t>(pid), static_cast<uint32_t>(pgid));
}

int64_t sys_getsid(uint64_t pid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? SchedulerGetProcessGroup(proc, static_cast<uint32_t>(pid), true) : pid ? -ESRCH : 1;
}

int64_t sys_setsid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? SchedulerCreateSession(proc) : -EPERM;
}

// ---------------------------------------------------------------------------
// sys_gettid (186) / sys_tgkill (234) / sys_tkill (200)
// ---------------------------------------------------------------------------

int64_t sys_gettid(uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? ProcessVisibleTid(proc, ProcessPidView(proc)) : 1;
}

// ---------------------------------------------------------------------------
// sys_prctl (157) — process control
// ---------------------------------------------------------------------------

int64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t)
{
    if (option==3 || option==4) { // PR_GET/SET_DUMPABLE
        auto* proc=ProcessCurrent();
        if (!proc || !proc->addressSpace) return -ESRCH;
        if (option==3) return __atomic_load_n(&proc->addressSpace->dumpable,__ATOMIC_ACQUIRE)?1:0;
        if (arg2>1) return -EINVAL;
        __atomic_store_n(&proc->addressSpace->dumpable,arg2!=0,__ATOMIC_RELEASE);
        return 0;
    }
    if(option==36 || option==37) { // PR_SET/GET_CHILD_SUBREAPER
        const int result=SchedulerChildSubreaperControl(ProcessCurrent(),option==36,arg2!=0);
        if(result<0)return result;
        if(option==36)return 0;
        return CopyToUser(arg2,&result,sizeof(result))?0:-EFAULT;
    }
    if (option == 38) { // PR_SET_NO_NEW_PRIVS
        if (arg2 != 1 || arg3 || arg4 || arg5) return -EINVAL;
        auto* proc = ProcessCurrent();
        if (!proc) return -ESRCH;
        ProcessSetNoNewPrivs(proc);
        return 0;
    }
    if (option == 39) { // PR_GET_NO_NEW_PRIVS
        if (arg2 || arg3 || arg4 || arg5) return -EINVAL;
        auto* proc = ProcessCurrent();
        if (!proc) return -ESRCH;
        return ProcessGetNoNewPrivs(proc) ? 1 : 0;
    }
    if(option==23 || option==24) {
        if(arg3 || arg4 || arg5)return -EINVAL;
        auto* process=ProcessCurrent();return process?ProcessCapabilityControl(process,unsigned(option),arg2):-ESRCH;
    }
    // Linux ignores unused PR_GET/SET_SECCOMP arguments. Bubblewrap passes
    // only the arguments it needs through the variadic prctl wrapper.
    if (option == 21 || option == 22) return SeccompPrctl(option, arg2, arg3);
    // Remaining capability/secure-exec operations are explicit errors.
    switch (option) {
        case 7: case 8:   // GET/SET_KEEPCAPS
        case 23: case 24: // CAPBSET_READ/DROP
        case 27: case 28: // GET/SET_SECUREBITS
        case 47:         // CAP_AMBIENT
            return -EINVAL;
        default: break;
    }
    // Most prctl options are not relevant for Brook.
    // Return success for harmless ones, EINVAL for unknown.
    return 0;
}

} // namespace brook
