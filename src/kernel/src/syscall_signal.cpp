// syscall_signal.cpp -- signal syscalls: sigaction/mask/pending/suspend/timedwait, kill family, sigaltstack, sigreturn, signalfd.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process_user_namespace.h"

#include "posix_timer.h"
#include "signal_fd.h"
#include "signal_altstack.h"
#include "timeout_value.h"
#include "string.h"
#include "tsc_clock.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_signal.h"
#include "syscall_time.h"
#include "syscall_handlers.h"

namespace brook {

// Temporary masks survive the blocking C handler until signal selection on
// syscall return. A PID can be reused after a killed waiter, so pair each slot
// with its process incarnation. Only that process installs/consumes its slot;
// this state is neither shared by threads nor inherited by fork/exec.
struct WaitSignalMask {
    uint64_t incarnation;
    uint64_t original;
    bool active;
};
static WaitSignalMask g_waitSignalMasks[MAX_PROCESSES] = {};

int InstallWaitSignalMask(uint64_t address, uint64_t size)
{
    if (!address) return 0;
    if (size != sizeof(uint64_t)) return -EINVAL;
    if (!UserBufferReadable(address, sizeof(uint64_t))) return -EFAULT;
    Process* proc = ProcessCurrent();
    if (!proc || proc->pid >= MAX_PROCESSES) return -ESRCH;
    uint64_t mask = *reinterpret_cast<const uint64_t*>(address);
    // SIGKILL and SIGSTOP cannot be blocked, including through these APIs.
    mask &= ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));
    auto& saved = g_waitSignalMasks[proc->pid];
    saved.incarnation = proc->incarnation;
    saved.original = proc->sigMask;
    saved.active = true;
    proc->sigMask = mask;
    return 0;
}

// Consumed by SyscallCheckSignals before choosing a signal under the temporary
// mask. The signal frame must restore this original mask via rt_sigreturn.
extern "C" bool SyscallTakeWaitSignalMask(Process* proc, uint64_t* original)
{
    if (!proc || proc->pid >= MAX_PROCESSES) return false;
    auto& saved = g_waitSignalMasks[proc->pid];
    bool active = saved.active && saved.incarnation == proc->incarnation;
    saved.active = false;
    if (active) *original = saved.original;
    return active;
}

// ---------------------------------------------------------------------------
// sys_alarm (37) — deliver SIGALRM after seconds
// ---------------------------------------------------------------------------

int64_t sys_alarm(uint64_t seconds, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    uint64_t prevRemaining = 0;
    if (proc->alarmTick != 0)
    {
        // Calculate remaining seconds from previous alarm
        uint64_t now = ApicTickCount();
        if (proc->alarmTick > now)
            prevRemaining = (proc->alarmTick - now + 999) / 1000; // round up
    }

    if (seconds == 0)
        proc->alarmTick = 0; // Cancel alarm
    else
        proc->alarmTick = ApicTickCount() + seconds * 1000;

    return static_cast<int64_t>(prevRemaining);
}

// ---------------------------------------------------------------------------
// sys_pause (34) — wait for a signal
// ---------------------------------------------------------------------------

int64_t sys_pause(uint64_t, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    // Block until a signal is delivered
    while (!HasPendingSignals())
    {
        proc->wakeupTick = ApicTickCount() + 100; // recheck every 100ms
        SchedulerBlockInterruptible(proc);
    }

    return -EINTR; // pause always returns EINTR
}

// ---------------------------------------------------------------------------
// sys_rt_sigsuspend (130) — temporarily replace signal mask and wait
// ---------------------------------------------------------------------------

int64_t sys_rt_sigsuspend(uint64_t maskAddr, uint64_t sigsetsize, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (sigsetsize != 8) return -EINVAL;
    if (!maskAddr) return -EFAULT;
    int error = InstallWaitSignalMask(maskAddr, sigsetsize);
    if (error) return error;

    // Block until a signal that isn't blocked is pending
    while (!HasPendingSignals())
    {
        proc->wakeupTick = ApicTickCount() + 100;
        SchedulerBlockInterruptible(proc);
    }

    // Select and deliver the signal under the temporary mask. The shared
    // syscall-return path saves the original mask in the handler's ucontext;
    // restoring it here can block SIGCHLD again before its handler runs.
    return -EINTR;
}

// ---------------------------------------------------------------------------
// sys_rt_sigtimedwait (128) — wait synchronously for a selected signal
// ---------------------------------------------------------------------------

int64_t sys_rt_sigtimedwait(uint64_t setAddr, uint64_t infoAddr,
                            uint64_t timeoutAddr, uint64_t sigsetsize,
                            uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (sigsetsize != 8) return -EINVAL;
    if (!UserBufferReadable(setAddr, sizeof(uint64_t))) return -EFAULT;
    // Linux dequeues first and only then copies siginfo to the user buffer.
    // A bad output pointer therefore consumes the selected signal.
    uint64_t waitSet = *reinterpret_cast<const uint64_t*>(setAddr);
    waitSet &= ~((1ULL << 8) | (1ULL << 18));
    uint64_t deadline = 0;
    if (timeoutAddr) {
        if (!UserBufferReadable(timeoutAddr, sizeof(timespec))) return -EFAULT;
        timespec ts = *reinterpret_cast<const timespec*>(timeoutAddr);
        uint64_t duration;
        if (!TimeoutNanoseconds(ts.tv_sec, ts.tv_nsec, &duration)) return -EINVAL;
        deadline = TimeoutAddSafe(ClockNowNs(1), duration);
    }
    for (;;) {
        // A fatal group exit must unwind this synchronous wait even though
        // its signal mask excludes ordinary asynchronous delivery. The syscall
        // return boundary publishes termination before any return to user code.
        if (__atomic_load_n(&proc->terminalPending, __ATOMIC_ACQUIRE))
            return -EINTR;
        uint64_t pending = ProcessPendingSignals(proc) |
                           PosixTimerPending(proc);
        uint64_t matched = pending & waitSet;
        if (matched) {
            int signum = __builtin_ctzll(matched) + 1;
            SignalInfo info{};
            if (!PosixTimerDequeue(proc, 1ULL << (signum - 1), &info)) {
                if (!ProcessDequeueSignal(proc, signum, &info)) continue;
            }
            ProcessSignalInfoToUser(proc,&info);
            if (infoAddr && !CopyToUser(infoAddr, &info, sizeof(info))) return -EFAULT;
            return signum;
        }
        if (pending & ~proc->sigMask & ~waitSet) return -EINTR;
        uint64_t now = ClockNowNs(1);
        if (timeoutAddr && now >= deadline) return -EAGAIN;
        uint64_t sleepMs = timeoutAddr ? (deadline - now) / 1000000ULL +
            ((deadline - now) % 1000000ULL != 0) : 100;
        if (sleepMs > 100) sleepMs = 100;
        proc->wakeupTick = ApicTickCount() + sleepMs;
        SchedulerBlockInterruptible(proc);
    }
}

// ---------------------------------------------------------------------------
// Signal: rt_sigaction (13), rt_sigprocmask (14)
// ---------------------------------------------------------------------------

int64_t sys_rt_sigaction(uint64_t signum, uint64_t actAddr, uint64_t oldactAddr,
                          uint64_t sigsetsize, uint64_t, uint64_t)
{
    if (signum < 1 || signum > 64) return -EINVAL;
    if (sigsetsize != 8) return -EINVAL;

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    uint32_t idx = static_cast<uint32_t>(signum) - 1;
    uint16_t pid = proc->tgid;
    if (pid >= MAX_PROCESSES) return -EINVAL;

    KernelSigaction action{};
    if(actAddr && !CopyFromUser(&action,actAddr,sizeof(action)))return -EFAULT;
    if(actAddr && (signum==9 || signum==19))return -EINVAL;
    action.mask &= ~((1ULL<<8)|(1ULL<<18));
    const KernelSigaction previous=ExchangeSignalAction(pid,idx,actAddr?&action:nullptr);
    return !oldactAddr || CopyToUser(oldactAddr,&previous,sizeof(previous))?0:-EFAULT;
}

int64_t sys_rt_sigpending(uint64_t setAddr, uint64_t sigsetsize,
                         uint64_t, uint64_t, uint64_t, uint64_t)
{
    // Unlike rt_sigprocmask, Linux permits a partial (or zero-length) set.
    if (sigsetsize > sizeof(uint64_t)) return -EINVAL;
    if (sigsetsize == 0) return 0;
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    // Observe without consuming. Ordinary signals currently have per-task
    // storage; unioning sibling queues would leak thread-directed signals.
    uint64_t pending = (ProcessPendingSignals(proc) |
                        PosixTimerPending(proc)) & proc->sigMask;
    return CopyToUser(setAddr, &pending, sigsetsize) ? 0 : -EFAULT;
}

int64_t sys_rt_sigprocmask(uint64_t how, uint64_t setAddr, uint64_t oldAddr,
                            uint64_t sigsetsize, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (sigsetsize != 8) return -EINVAL;

    // Return old mask
    if (oldAddr)
    {
        if (!UserBufferWritable(oldAddr, sizeof(uint64_t))) return -EFAULT;
        *reinterpret_cast<uint64_t*>(oldAddr) = proc->sigMask;
    }

    // Modify mask
    if (setAddr)
    {
        if (!UserBufferReadable(setAddr, sizeof(uint64_t))) return -EFAULT;
        uint64_t newSet = *reinterpret_cast<const uint64_t*>(setAddr);
        // Can't block SIGKILL (9) or SIGSTOP (19)
        uint64_t unblockable = (1ULL << 8) | (1ULL << 18);
        newSet &= ~unblockable;

        switch (how)
        {
        case 0: // SIG_BLOCK
            proc->sigMask |= newSet;
            break;
        case 1: // SIG_UNBLOCK
            proc->sigMask &= ~newSet;
            break;
        case 2: // SIG_SETMASK
            proc->sigMask = newSet;
            break;
        default:
            return -EINVAL;
        }
    }
    return 0;
}

int64_t sys_signalfd4(uint64_t descriptor,uint64_t maskAddress,uint64_t size,uint64_t flags,uint64_t,uint64_t)
{
    if(size!=8 || (flags&~uint64_t(0x800|0x80000)))return -EINVAL;
    uint64_t mask=0;if(!CopyFromUser(&mask,maskAddress,sizeof(mask)))return -EFAULT;
    Process* p=ProcessCurrent();
    if(int(descriptor)!=-1) {
        FdRef ref(p,int(descriptor));if(!ref)return -EBADF;
        if(ref->type!=FdType::SignalFd || !ref->handle)return -EINVAL;
        SignalFdSetMask(static_cast<SignalFd*>(ref->handle),mask);return int(descriptor);
    }
    KernelOperationGuard guard(p);
    auto* object=SignalFdCreate(mask);if(!object)return -ENOMEM;
    FdEntry entry{};entry.type=FdType::SignalFd;entry.handle=object;
    entry.statusFlags=flags&0x800;entry.fdFlags=(flags&0x80000)?1:0;
    int fd=FdTablePublishOwned(p->fds,ProcessFdLock(p),entry,0,-1,nullptr,nullptr);
    if(fd<0){SignalFdUnref(object);return -EMFILE;}return fd;
}
int64_t sys_signalfd(uint64_t descriptor,uint64_t mask,uint64_t size,uint64_t,uint64_t,uint64_t)
{ return sys_signalfd4(descriptor,mask,size,0,0,0); }

// Linux sigaltstack constants
static constexpr int SS_ONSTACK  = 1;
static constexpr int SS_DISABLE  = 2;

struct linux_stack_t {
    uint64_t ss_sp;
    int32_t  ss_flags;
    uint32_t _pad;
    uint64_t ss_size;
};

int64_t sys_sigaltstack(uint64_t ssAddr, uint64_t oldSsAddr, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    const linux_stack_t old{proc->sigAltstackSp,
        SignalAltstackFlags(proc->sigAltstackSp,proc->sigAltstackSize,
                            proc->sigAltstackFlags,proc->syscallUserRsp),
        0,proc->sigAltstackSize};
    linux_stack_t ss{};
    // Snapshot input before output: ss and old_ss may alias.
    if(ssAddr && !CopyFromUser(&ss,ssAddr,sizeof(ss)))return -EFAULT;
    if (ssAddr)
    {
        if(old.ss_flags==SS_ONSTACK)return -EPERM;
        // Linux accepts SS_ONSTACK as an enabled configuration, but never
        // stores it as execution state. SS_AUTODISARM is not supported yet.
        if(ss.ss_flags!=0 && ss.ss_flags!=SS_DISABLE && ss.ss_flags!=SS_ONSTACK)
            return -EINVAL;
        if(ss.ss_flags!=SS_DISABLE && (ss.ss_size<2048 || ss.ss_size>0xffffffffULL))
            return -ENOMEM;
        if (ss.ss_flags == SS_DISABLE)
        {
            proc->sigAltstackSp    = 0;
            proc->sigAltstackSize  = 0;
            proc->sigAltstackFlags = SS_DISABLE;
        }
        else
        {
            proc->sigAltstackSp    = ss.ss_sp;
            proc->sigAltstackSize  = static_cast<uint32_t>(ss.ss_size);
            proc->sigAltstackFlags = 0;
        }
    }

    return !oldSsAddr || CopyToUser(oldSsAddr,&old,sizeof(old)) ? 0 : -EFAULT;
}

// ---------------------------------------------------------------------------
// sys_rt_sigreturn (15) — restore context saved by signal delivery
// ---------------------------------------------------------------------------
// Sets a flag so SyscallCheckSignals (called on asm return path) restores
// the full register context from the SignalFrame ucontext on the user stack.

int64_t sys_rt_sigreturn(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    proc->sigReturnPending = true;
    DbgPrintf("SIGRETURN: pid %u requesting context restore (cur user RIP=0x%lx)\n",
              proc->pid,
              []{ uint64_t r; __asm__ volatile("movq %%gs:48, %0" : "=r"(r)); return r; }());

    return 0; // Return value doesn't matter — SyscallCheckSignals overwrites RAX
}

// Thread-directed signals retain their Linux sender metadata. In particular,
// NPTL validates SI_TKILL and the sender TGID before applying setxid requests.
static int64_t SendThreadSignal(uint64_t tgid, uint64_t tid, uint64_t sig, bool checkGroup)
{
    int32_t group = static_cast<int32_t>(tgid), thread = static_cast<int32_t>(tid);
    int32_t signal = static_cast<int32_t>(sig);
    if (thread <= 0 || (checkGroup && group <= 0) || signal < 0 || signal > 64)
        return -EINVAL;
    if (thread > 0xffff || (checkGroup && group > 0xffff)) return -ESRCH;
    Process* self = ProcessCurrent();
    SchedulerKernelRegionEnter(self);
    Process* target = SchedulerFindProcessReference(static_cast<uint16_t>(thread));
    int result = -ESRCH;
    if (target && (!checkGroup || target->tgid == group)) {
        result = ProcessCanSignal(self,target) || (signal==18 && self && self->sid==target->sid)?0:-EPERM;
        if (!result && signal) {
            SignalInfo info{};
            info.si_signo = signal;
            info.si_code = -6; // SI_TKILL
            int32_t sender = self ? self->tgid : 0;
            uint32_t uid = self ? ProcessReadIds(self, false).real : 0;
            memcpy(info._data, &sender, sizeof(sender));
            memcpy(info._data + 4, &uid, sizeof(uid));
            // SA_ONSTACK without an enabled alternate stack uses the normal
            // stack. Silently discarding it deadlocks libc's setxid protocol.
            result = ProcessSendSignal(target, signal, &info);
        }
    }
    if (target) ProcessUnref(target);
    SchedulerKernelRegionLeave(self);
    return result;
}

int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig,
                    uint64_t, uint64_t, uint64_t)
{
    return SendThreadSignal(tgid, tid, sig, true);
}

int64_t sys_tkill(uint64_t tid, uint64_t sig, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    return SendThreadSignal(0, tid, sig, false);
}

// ---------------------------------------------------------------------------
// sys_kill (62)
// ---------------------------------------------------------------------------

int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t,
                  uint64_t, uint64_t, uint64_t)
{
    // Linux pid_t is a signed 32-bit syscall argument. Do not alias a large
    // positive PID onto an unrelated 16-bit Brook process identity.
    int32_t requested = static_cast<int32_t>(pid);
    if (requested <= 0) return -ENOSYS; // process-group selectors not implemented
    if (requested > 0xffff) return -ESRCH;
    int32_t signal = static_cast<int32_t>(sig);
    if (signal < 0 || signal > 64) return -EINVAL;
    Process* self = ProcessCurrent();
    SchedulerKernelRegionEnter(self);
    Process* target = SchedulerFindProcessReference(static_cast<uint16_t>(requested));
    if (!target) {
        SchedulerKernelRegionLeave(self);
        return -ESRCH;
    }
    if(!ProcessCanSignal(self,target) && !(signal==18 && self && self->sid==target->sid)) {ProcessUnref(target);SchedulerKernelRegionLeave(self);return -EPERM;}
    SignalInfo info{};info.si_signo=signal;info.si_code=0; // SI_USER
    const int32_t sender=self?self->tgid:0;
    const uint32_t senderUid=self?ProcessReadIds(self,false).real:0;
    memcpy(info._data,&sender,4);memcpy(info._data+4,&senderUid,4);
    int result = signal ? ProcessSendSignal(target, signal, &info) : 0;
    ProcessUnref(target);
    SchedulerKernelRegionLeave(self);
    return result;
}

} // namespace brook
