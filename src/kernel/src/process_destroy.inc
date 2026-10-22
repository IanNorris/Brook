// Shared with the hosted lifetime regression; resource backends are external.
static void DestroyProcessResources(Process* proc, bool published)
{
    if (!proc) return;

    PosixTimersCleanup(proc, false);
    SysvSemExit(proc);
    bool isThread = proc->isThread;

    // Filesystem operations finish cooperatively before terminal publication;
    // there are no abandoned mutexes or backend mutations to force-release.
    ProcessReleaseFileFault(proc);

    // Threads don't own FDs — the leader does. Kernel threads never had
    // an fd table allocated (proc->fds is NULL), so skip the close loop
    // for them as well; otherwise ProcessCloseAllFds would deref NULL.
    if (!isThread && !proc->isKernelThread && proc->fds)
        ProcessCloseAllFds(proc);

    // Free the shared fd table once the leader (or fork root) is destroyed.
    // Threads get their fds pointer cleared so the leader still sees the
    // live table until its own ProcessDestroy runs.
    if (!isThread && proc->fds)
    {
        kfree(proc->fds);
        proc->fds = nullptr;
    }
    else if (isThread)
    {
        proc->fds = nullptr;
    }

    // Unpublished births have never registered windows or created a GPU context.
    // Do not retire borrowed/copied parent handles during their rollback.
    // Remove compositor/window references before retiring graphics backing.
    // This finalizer runs on the cleanup worker, outside scheduler IRQ context.
    if (published && !isThread)
    {
        CompositorUnregisterProcess(proc);
        WmDestroyWindowForProcess(proc);
        // Release the app's GPU (virgl) context, if it created one.
        if (proc->gpuAppCtx > 0)
        {
            const brook::GpuAppOps* ga = brook::GpuAppGet();
            if (ga && ga->CtxDestroy) ga->CtxDestroy(proc->gpuAppCtx);
            proc->gpuAppCtx = 0;
        }
    }

    // Address-space ownership, not PID-tag sweeps, retires mappings. Final
    // AS release is queued independently and cannot touch a reused PID's pages.

    // Free per-process kernel stack (each thread has its own).  This must be
    // after teardown work that may still need helper calls using this proc.
    if (proc->kernelStackBase)
        VmmFreeKernelStack(VirtualAddress(proc->kernelStackBase), KERNEL_STACK_PAGES);

    // Clear signal handler slot so the PID can be safely reused without
    // the next process inheriting our handler pointers (which would
    // reference user memory no longer mapped and cause a #PF on signal).
    //
    // Signal handlers are shared across all threads in a thread group
    // (CLONE_SIGHAND semantics) and indexed by tgid. Only clear when
    // the leader (pid == tgid) is destroyed, otherwise a thread exiting
    // before the leader would wipe the still-live process's handlers.
    if (proc->pid == proc->tgid) {
        ResetSignalActions(proc->tgid);
    }

    // Release the PID only after clearing its signal slots and exact resources.
    if (published) SchedulerRemoveProcess(proc);
    else SchedulerFreePid(proc->pid);

    // Unpublished births never incremented their leader's membership count.
    if (published && isThread && !proc->isKernelThread)
        SchedulerReleaseThreadAddressSpace(proc);

    FreeProcessStruct(proc);
}

void ProcessDestroyNow(Process* proc) { DestroyProcessResources(proc, true); }

void ProcessDiscardUnpublished(Process* proc)
{
    // Only a fully constructed birth that has never reached SchedulerAddProcess.
    // Its PID reservation and owned resources exist; scheduler membership does not.
    DestroyProcessResources(proc, false);
}
