// Included by scheduler.cpp and the hosted lock-order regression.
static void FillFdSnapshot(FdSnapshot* out, const FdEntry& fde)
{
    out->type = static_cast<uint8_t>(fde.type);
    out->flags = fde.statusFlags;
    out->seekPos = fde.seekPos;
    FdDiagnosticPath(fde,out->path,sizeof(out->path));
}

static bool SchedulerReadFdSnapshot(uint16_t pid, uint32_t index, int fd,
                                    int* outFd, FdSnapshot* outSnap, uint64_t incarnation)
{
    for (;;) {
        const uint64_t flags = SchedLockAcquire(g_allProcLock);
        Process* process = nullptr;
        for (uint32_t i = 0; i < g_processCount; ++i) {
            Process* candidate = g_allProcesses[i];
            if (candidate && candidate->pid == pid) { process = candidate; break; }
        }
        if (!process || !process->fds ||
            (incarnation && (process->incarnation != incarnation ||
                            process->state == ProcessState::Terminated))) {
            SchedLockRelease(g_allProcLock, flags);
            return false;
        }

        // Never wait for a preemptible FD-lock owner under the process-list
        // lock. Resuming that owner can require ReleaseSchedulerReference,
        // which itself takes g_allProcLock. Retry from the PID/incarnation
        // lookup after dropping the registry lock; no borrowed pointer escapes.
        SpinLock* fdLock = ProcessFdLock(process);
        if (!SpinLockTryAcquire(fdLock)) {
            SchedLockRelease(g_allProcLock, flags);
            SchedulerYield();
            continue;
        }
        bool found = false;
        if (fd >= 0) {
            if (process->fds[fd].type != FdType::None) {
                FillFdSnapshot(outSnap, process->fds[fd]);
                found = true;
            }
        } else {
            uint32_t ordinal = 0;
            for (uint32_t slot = 0; slot < MAX_FDS; ++slot) {
                if (process->fds[slot].type == FdType::None) continue;
                if (ordinal++ != index) continue;
                *outFd = static_cast<int>(slot);
                FillFdSnapshot(outSnap, process->fds[slot]);
                found = true;
                break;
            }
        }
        SpinLockRelease(fdLock);
        SchedLockRelease(g_allProcLock, flags);
        return found;
    }
}

bool SchedulerGetFdByIndex(uint16_t pid, uint32_t index, int* outFd,
                           FdSnapshot* outSnap, uint64_t incarnation)
{
    return SchedulerReadFdSnapshot(pid, index, -1, outFd, outSnap, incarnation);
}

bool SchedulerGetFdInfo(uint16_t pid, int fd, FdSnapshot* outSnap, uint64_t incarnation)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return false;
    return SchedulerReadFdSnapshot(pid, 0, fd, nullptr, outSnap, incarnation);
}
