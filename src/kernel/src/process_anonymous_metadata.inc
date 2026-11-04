// Same-group proc FD stat snapshots; included after ProcessOperationGuard.
int ProcessReadAnonymousFdMetadata(Process* viewer, Process* target, uint32_t fd, VnodeStat* out)
{
    if (!viewer || !target || !out) return -13;
    ProcessOperationGuard operation(ProcessCurrent());
    const uint64_t viewerGroup = viewer->isThread ? viewer->leaderIncarnation : viewer->incarnation;
    const uint64_t targetGroup = target->isThread ? target->leaderIncarnation : target->incarnation;
    FdType type = FdType::None;
    uint8_t flags = 0;
    void* memfd = nullptr;
    auto* lock = ProcessFdLock(target);
    SpinLockAcquire(lock);
    if (!target->fds || fd >= MAX_FDS || target->fds[fd].type == FdType::None || target->fds[fd].closing) {
        SpinLockRelease(lock);
        return -2;
    }
    const auto& entry = target->fds[fd];
    type = entry.type; flags = entry.flags;
    const bool sameGroup = viewer->tgid == target->tgid && viewerGroup == targetGroup;
    switch (type) {
        case FdType::MemFd:
            if (!sameGroup) { SpinLockRelease(lock); return -13; }
            if (!entry.handle) { SpinLockRelease(lock); return -2; }
            memfd = entry.handle; MemFdHandleRef(memfd); break;
        case FdType::Pipe: case FdType::UnixSocket: case FdType::EventFd:
        case FdType::EpollFd: case FdType::TimerFd: case FdType::SignalFd:
        case FdType::SyncFile: case FdType::DevKeyboard: case FdType::DevNull:
        case FdType::DevUrandom: case FdType::DevDsp: case FdType::DevTty:
        case FdType::DebugSerial: case FdType::DebugConsole: case FdType::DevFramebuf: break;
        default: SpinLockRelease(lock); return 0;
    }
    if (!sameGroup) { SpinLockRelease(lock); return -13; }
    SpinLockRelease(lock);
    if (memfd) { MemFdHandleStat(memfd, out); MemFdHandleUnref(memfd); return 1; }
    *out = {}; out->dev = 0; out->nlink = 0; out->exactDescriptorMetadata = true;
    if (type == FdType::Pipe && !(flags & 0x04)) out->mode = 0010666;
    else if (type == FdType::UnixSocket) out->mode = 0140666;
    else if (type == FdType::EventFd || type == FdType::EpollFd || type == FdType::TimerFd ||
             type == FdType::SignalFd || type == FdType::SyncFile) out->mode = 0100666;
    else {
        out->mode = 0020666;
        out->rdev = type == FdType::Pipe || type == FdType::DevTty ? 0x8800 :
                    type == FdType::DevFramebuf ? 0x1d00 : type == FdType::DevNull ? 0x0103 :
                    type == FdType::DevUrandom ? 0x0109 : type == FdType::DevDsp ? 0x0e03 :
                    type == FdType::DebugSerial ? 0x0440 : 0x0400;
    }
    return 1;
}
