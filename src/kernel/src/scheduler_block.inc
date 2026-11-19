// Shared production blocking path. The hosted test supplies IRQ/context
// shims and a deterministic hook before admission; it executes this body.
static inline uint64_t BlockSaveIrq(){
#ifdef BROOK_SCHED_BLOCK_HOST_TEST
    return BlockHostSaveIrq();
#else
    uint64_t flags;__asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");return flags;
#endif
}
static inline void BlockRestoreIrq(uint64_t flags){
#ifdef BROOK_SCHED_BLOCK_HOST_TEST
    BlockHostRestoreIrq(flags);
#else
    if(flags&0x200)__asm__ volatile("sti" ::: "memory");
#endif
}

// Diagnostics for /proc/<pid>/wchan and `proc blocked`: when the wait began
// and the kernel return addresses above SchedulerBlock (frame-pointer walk,
// bounded to the current kernel stack). Written only by the blocking task.
static inline void BlockRecordWaitSite(Process* proc, const void* frame)
{
#ifdef BROOK_SCHED_BLOCK_HOST_TEST
    (void)proc;(void)frame;
#else
    if (proc != g_perCpu[ThisCpu()].currentProcess) return;
    __atomic_store_n(&proc->blockedSinceMs, ApicTickCount(), __ATOMIC_RELAXED);
    uint64_t rbp = reinterpret_cast<uint64_t>(frame);
    const uint64_t base = proc->kernelStackBase, top = proc->kernelStackTop;
    uint32_t depth = 0;
    while (depth < ProcWaitSiteDepth && rbp && !(rbp & 7) && rbp >= base && rbp + 16 <= top) {
        const auto* f = reinterpret_cast<const uint64_t*>(rbp);
        const uint64_t ret = f[1];
        if (ret < 0xffffffff80000000ULL) break;
        proc->blockSites[depth++] = ret;
        if (f[0] <= rbp) break;
        rbp = f[0];
    }
    for (uint32_t i = depth; i < ProcWaitSiteDepth; ++i) proc->blockSites[i] = 0;
#endif
}

static void SchedulerBlockImpl(Process* proc, bool interruptible)
{
    // UAF guard — same rationale as in DoSwitch.
    {
#ifndef BROOK_SCHED_BLOCK_HOST_TEST
        uint64_t v = (uint64_t)proc;
        if (v == 0 || (v >> 47) != 0x1FFFFULL)
        {
            KernelPanic("SCHED: SchedulerBlock proc %p not canonical kernel-half\n", proc);
        }
#endif
        if (proc->magic != PROCESS_MAGIC)
        {
            KernelPanic("SCHED: SchedulerBlock proc %p has bad magic 0x%lx — UAF?\n",
                        proc, proc->magic);
        }
    }

    struct WaitRegistration {
        Process* proc;
        bool active;
        ~WaitRegistration() {
            if (!active) return;
            const uint64_t flags = SchedLockAcquire(g_readyLock);
            proc->interruptibleWait = false;
            SchedLockRelease(g_readyLock, flags);
        }
    } registration{proc, interruptible};

    // Disable interrupts across the entire block+yield sequence to prevent
    // SchedulerTimerTick from firing between setting Blocked and yielding,
    // which would overwrite the Blocked state with Ready.
    const uint64_t flags = BlockSaveIrq();
#ifdef BROOK_SCHED_BLOCK_HOST_TEST
    BlockHostBeforeReadyLock(proc);
#endif

    uint64_t rlf3 = SchedLockAcquire(g_readyLock);

    // Registration and cancellation check are atomic with terminal publication.
    // If publication won before registration, return for caller-owned cleanup;
    // if it wins after, SchedulerMarkTerminated wakes this registered wait.
    if (interruptible) {
        proc->interruptibleWait = true;
        if (proc->terminalPending) {
            SchedLockRelease(g_readyLock, rlf3);
            BlockRestoreIrq(flags);
            return;
        }
    }

    // Terminal state is irreversible. A remote exit_group can win while this
    // syscall is preparing to sleep; retire on our own stack-switch path,
    // rather than reviving it as Blocked or returning to user code.
    if (proc->state == ProcessState::Terminated)
    {
        SchedLockRelease(g_readyLock, rlf3);
        if (proc == g_perCpu[ThisCpu()].currentProcess)
        {
            SchedulerYield();
            KernelPanic("SCHED: terminated pid=%u resumed from block", proc->pid);
        }
        BlockRestoreIrq(flags);
        return;
    }

    // A remote stop can win while this syscall is preparing to block. Keep
    // Stopped until SIGCONT instead of overwriting it with Blocked (which loses
    // the continued event and can leave waitid(WCONTINUED) asleep forever).
    if (proc->state == ProcessState::Stopped) {
        SchedLockRelease(g_readyLock, rlf3);
        if (proc == g_perCpu[ThisCpu()].currentProcess) SchedulerYield();
        BlockRestoreIrq(flags);
        return;
    }

    if (interruptible) {
        const uint64_t pending = ProcessPendingSignals(proc) |
                                 PosixTimerPending(proc);
        const uint64_t mask = __atomic_load_n(&proc->sigMask, __ATOMIC_ACQUIRE);
        // A caller (notably futex enqueue) may have cleared the wake latch
        // after its last signal check. Check pending signals under the same
        // lock used by SchedulerUnblock, before admitting an interruptible wait.
        // Stopped/Terminated handling above must win over ordinary signals.
        // Uninterruptible kernel mutex waits still require ownership grants.
        if (pending & ~mask) {
            SchedLockRelease(g_readyLock, rlf3);
            BlockRestoreIrq(flags);
            return;
        }
    }

    // Check for a pending wakeup that raced with us (e.g. KMutexUnlock
    // calling SchedulerUnblock before we got here).  If set, the waker
    // already transferred mutex ownership; we should NOT block.
    if (__atomic_load_n(&proc->pendingWakeup, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&proc->pendingWakeup, 0, __ATOMIC_RELEASE);
        SchedTrace(proc, STR_BLOCK_SKIP);
        SchedLockRelease(g_readyLock, rlf3);
        BlockRestoreIrq(flags);
        return;
    }

    proc->state = ProcessState::Blocked;
    SchedTrace(proc, STR_BLOCK);
    ReadyQueueRemoveLocked(proc);
    g_schedOps->VoluntaryYield(g_schedState, proc->pid);
    SchedLockRelease(g_readyLock, rlf3);

    uint32_t cpu = ThisCpu();
    if (proc == g_perCpu[cpu].currentProcess)
        SchedulerYield();

    // Restore interrupts after yield (the context_switch + iretq path
    // will re-enable interrupts for us, but if we didn't yield we need
    // to restore).
    BlockRestoreIrq(flags);
}

__attribute__((noinline)) void SchedulerBlock(Process* proc)
{
    BlockRecordWaitSite(proc, __builtin_frame_address(0));
    SchedulerBlockImpl(proc, false);
}

__attribute__((noinline)) void SchedulerBlockInterruptible(Process* proc)
{
    BlockRecordWaitSite(proc, __builtin_frame_address(0));
    // Keep even otherwise-unprotected waits alive until their caller unwinds.
    // Leave retains the latch until syscall return; it is not a terminal boundary.
    SchedulerKernelRegionEnter(proc);
    SchedulerBlockImpl(proc, true);
    SchedulerKernelRegionLeave(proc);
}

