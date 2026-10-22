// Production sender body shared with deterministic scheduler/wakeup tests.
int ProcessSendSignal(Process* proc, int signum, const SignalInfo* info)
{
    if (!proc) return -3; // ESRCH
    if (signum < 1 || signum > 64) return -22; // EINVAL

    // SIGKILL and SIGSTOP cannot be caught/blocked
    if (signum == SIGKILL)
    {
        SchedulerRequestFatalSignal(proc, signum);
        return 0;
    }

    if (signum == SIGSTOP)
    {
        // Immediately stop (cannot be caught/blocked)
        SchedulerStop(proc, signum);
        DbgPrintf("SIGNAL: SIGSTOP -> pid %u stopped (default)\n", proc->pid);
        return 0;
    }

    // SIGCONT: resume stopped process (always, even if blocked/ignored)
    static constexpr int SIGCONT = 18;
    if (signum == SIGCONT)
    {
        const KernelSigaction sa = ReadSignalAction(proc->tgid,signum - 1);
        const bool masked = (__atomic_load_n(&proc->sigMask, __ATOMIC_ACQUIRE) &
                             (1ULL << (signum - 1))) != 0;
        // Blocked signals remain pending even with an ignored disposition:
        // sigtimedwait can consume them, or the action can change before unmask.
        const bool notify = masked || (sa.handler != 0 && sa.handler != 1);
        if (notify) QueueSignal(proc, signum, info);
        // Resume Stopped under the ready lock regardless of mask/disposition.
        // Publish the pending bit first so a resumed target can observe it.
        SchedulerContinue(proc);
        // A caught SIGCONT also interrupts ordinary waits. Serialize even if
        // the target is still Running/Ready and about to admit a block.
        if (notify) SchedulerUnblock(proc);
        return 0;
    }

    // SIGTSTP/SIGTTIN/SIGTTOU: if no user handler registered, stop immediately
    // (like SIGSTOP). Otherwise fall through to set pending for handler delivery.
    if (signum == 20 || signum == 21 || signum == 22)
    {
        const KernelSigaction sa = ReadSignalAction(proc->tgid,signum - 1);
        if (sa.handler == 0 || sa.handler == 1) // SIG_DFL or SIG_IGN
        {
            if (sa.handler == 1 && !(proc->sigMask & (1ULL << (signum-1)))) return 0;
            // Catchable default stop signals respect the target mask.
            if (proc->sigMask & (1ULL << (signum - 1))) {
                QueueSignal(proc, signum, info);
                return 0;
            }
            SchedulerStop(proc, signum);
            DbgPrintf("SIGNAL: sig %d -> pid %u stopped (default)\n", signum, proc->pid);
            return 0;
        }
    }

    // Drop signals whose current disposition is "ignore".  Without this the
    // bit sits in sigPending forever and HasPendingSignals() returns true,
    // causing every blocking syscall to return -EINTR — which for line-reads
    // in the shell looks like EOF and silently kills bash on e.g. SIGWINCH.
    //
    // Signals that are ignored:
    //   - explicit SIG_IGN
    //   - SIG_DFL where the POSIX default is "ignore": SIGCHLD (17),
    //     SIGURG (23), SIGWINCH (28)
    {
        const KernelSigaction sa = ReadSignalAction(proc->tgid,signum - 1);
        bool defaultIsIgnore = (signum == 17 || signum == 23 || signum == 28);
        if (!(proc->sigMask & (1ULL << (signum-1))) &&
            (sa.handler == 1 || (sa.handler == 0 && defaultIsIgnore)))
        {
            return 0;
        }

        // SIG_DFL where the default action is "terminate": kill the process.
        // Without this, abort()'s raise(SIGABRT) with SIG_DFL sets the bit
        // pending but never terminates — execution falls through to HLT.
        const bool masked = (proc->sigMask & (1ULL << (signum - 1))) != 0;
        if (sa.handler == 0 && DefaultSignalAction(signum) == SignalDefault::Terminate && !masked)
        {
            SchedulerRequestFatalSignal(proc, signum);
            return 0;
        }
    }

    QueueSignal(proc, signum, info);

    // Synchronous sigtimedwait also consumes masked signals. Keep its wake;
    // ordinary waits recheck their deliverable mask after a spurious wake.
    // Serialize with block admission even while Running/Ready: the signal
    // may arrive between a caller's last pending check and its block call.
    SchedulerUnblock(proc);

    return 0;
}
