// Pending signals always belong to the reading/polling task, not the creator.
// The open description shares only its mask; dup/fork/SCM_RIGHTS retain it.
SignalFd* SignalFdCreate(uint64_t mask) {
    auto* fd=static_cast<SignalFd*>(kmalloc(sizeof(SignalFd)));
    if(fd)*fd={1,SignalFdMask(mask)};return fd;
}
void SignalFdRef(SignalFd* fd){if(fd)__atomic_add_fetch(&fd->references,1,__ATOMIC_RELAXED);}
void SignalFdUnref(SignalFd* fd){if(fd && __atomic_fetch_sub(&fd->references,1,__ATOMIC_ACQ_REL)==1)kfree(fd);}
void SignalFdSetMask(SignalFd* fd,uint64_t mask){__atomic_store_n(&fd->mask,SignalFdMask(mask),__ATOMIC_RELEASE);}
uint32_t SignalFdReady(Process* p,SignalFd* fd,uint32_t events) {
    if(!p || !fd)return 0;
    const auto pending=ProcessPendingSignals(p)|PosixTimerPending(p);
    return (pending & __atomic_load_n(&fd->mask,__ATOMIC_ACQUIRE))?(events&1):0;
}
int64_t SignalFdRead(Process* p,SignalFd* fd,uint64_t destination,uint64_t count,bool nonblocking) {
    if(count<sizeof(SignalFdInfo))return -22;
    struct Region {
        Process* p;
        explicit Region(Process* task):p(task){SchedulerKernelRegionEnter(p);}
        ~Region(){SchedulerKernelRegionLeave(p);}
    } region(p);
    uint64_t copied=0;
    while(count-copied>=sizeof(SignalFdInfo)) {
        if(__atomic_load_n(&p->terminalPending,__ATOMIC_ACQUIRE))return copied?int64_t(copied):-4;
        const auto mask=__atomic_load_n(&fd->mask,__ATOMIC_ACQUIRE);
        const auto pending=ProcessPendingSignals(p)|PosixTimerPending(p);
        const auto matched=pending&mask;
        if(matched) {
            const int signo=__builtin_ctzll(matched)+1;SignalInfo info{};
            if(!PosixTimerDequeue(p,1ULL<<(signo-1),&info) && !ProcessDequeueSignal(p,signo,&info))continue;
            const auto record=SignalFdEncode(info.si_signo,info.si_errno,info.si_code,info._data);
            // Like Linux, a fault can consume this signal after earlier records.
            if(!CopyToUser(destination+copied,&record,sizeof(record)))return copied?int64_t(copied):-14;
            copied+=sizeof(record);continue;
        }
        if(copied)return int64_t(copied);
        if(nonblocking)return -11;
        if(pending & ~__atomic_load_n(&p->sigMask,__ATOMIC_ACQUIRE) & ~mask)return -4;
        // Signal delivery already wakes the target, including masked signals.
        // The existing poll/epoll safety recheck also covers shared-mask updates.
        p->wakeupTick=ApicTickCount()+10;SchedulerBlockInterruptible(p);
    }
    return int64_t(copied);
}
