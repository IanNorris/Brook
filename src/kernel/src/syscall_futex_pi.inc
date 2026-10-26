// Included after mapping/key and target-AS helpers in syscall.cpp. No mapping
// fault, sleeping lock acquisition or scheduler wake may occur under futexLock.
static PiWaitGraph<MAX_PROCESSES> g_piGraph;
static Process* g_piPins[MAX_PROCESSES];
static uint8_t g_piPublishedRank[MAX_PROCESSES];
struct PiWaitAddress { uint64_t address, operation; };
static PiWaitAddress g_piWaitAddress[MAX_PROCESSES];
static constexpr uint32_t PiTidMask=0x3fffffff,PiWaiters=0x80000000,PiDied=0x40000000;

static PiIdentity PiId(const Process* proc) { return {proc->pid,proc->incarnation}; }
static bool PiFutexQueued(uint64_t domain,uint64_t offset) {return g_piGraph.HasKey({domain,offset});}

static bool PiPin(Process* proc)
{
    if(!proc || proc->pid>=MAX_PROCESSES)return false;
    auto*& slot=g_piPins[proc->pid];
    if(slot)return slot==proc && slot->incarnation==proc->incarnation;
    ProcessRef(proc);slot=proc;g_piPublishedRank[proc->pid]=255;
    return true;
}

// Each referenced task has one process pin, independent of the number of
// graph edges. Withdraw scheduler donation before releasing the last pin.
// readyLock never acquires futexLock; process cancellation runs outside it.
static void PiSyncPins()
{
    for(unsigned pid=0;pid<MAX_PROCESSES;++pid) {
        auto* proc=g_piPins[pid];if(!proc)continue;
        const auto rank=g_piGraph.Donation(PiId(proc));
        if(rank!=g_piPublishedRank[pid]) {
            SchedulerSetPiDonation(proc,rank);g_piPublishedRank[pid]=rank;
        }
        if(!g_piGraph.TaskAt(pid).references) {g_piPins[pid]=nullptr;ProcessUnref(proc);}
    }
}
static bool PiOrdinaryQueued(PiKey key)
{
    for(auto* node=g_futexBuckets[FutexHash(key.domain,key.offset)];node;node=node->next)
        if(node->owner==key.domain && node->uaddr==key.offset)return true;
    return false;
}
static int PiGraphError(PiGraphResult result)
{
    if(result==PiGraphResult::Deadlock)return -35; // EDEADLK
    if(result==PiGraphResult::Busy)return -EAGAIN;
    return -EINVAL;
}

static void PiFutexCancelProcess(Process* proc)
{
    if(!proc)return;
    const auto flags=IrqSpinLockAcquire(&g_futexLock);
    const auto identity=PiId(proc);
    (void)g_piGraph.Cancel(identity,-EINTR);
    (void)g_piGraph.Consume(identity);
    PiSyncPins();
    IrqSpinLockRelease(&g_futexLock,flags);
}

// Must run with a mapping transaction held, after writable prefault. Access the
// physical alias: cleanup may be executing in a different address space.
static uint32_t* PiMappedWord(AddressSpaceState* space,uint64_t address)
{
    const auto* range=MappingRangeFind(space->ranges,address);
    if(!range || range->kind==MappingKind::Device || (range->protection&3)!=3 ||
       range->end-address<4)return nullptr;
    const auto leaf=VmmReadLeaf(space->pageTable,VirtualAddress(address));
    if((leaf&(VMM_PRESENT|VMM_USER|VMM_WRITABLE))!=(VMM_PRESENT|VMM_USER|VMM_WRITABLE))return nullptr;
    const auto physical=VmmVirtToPhys(space->pageTable,VirtualAddress(address));
    return physical?reinterpret_cast<uint32_t*>(PhysToVirt(physical).raw()):nullptr;
}

static int64_t PiFutexOperation(uint64_t address,uint64_t operation,int op,uint64_t timeout)
{
    auto* proc=ProcessCurrent();if(!proc || !proc->addressSpace)return -ENOSYS;
    if((operation&FUTEX_CLOCK_REALTIME) && op!=FUTEX_LOCK_PI2)return -ENOSYS;
    if(address&3)return -EINVAL;
    if(!UserBufferWritable(address,4) || !UserBufferReadable(address,4))return -EFAULT;
    FutexWaitDeadline deadline{};
    if(op!=FUTEX_UNLOCK_PI && op!=FUTEX_TRYLOCK_PI && timeout) {
        timespec request{};uint64_t value=0;
        if(!CopyFromUser(&request,timeout,sizeof(request)))return -EFAULT;
        if(!FutexTimeoutNanoseconds(request.tv_sec,request.tv_nsec,&value))return -EINVAL;
        const unsigned clock=op==FUTEX_LOCK_PI || (operation&FUTEX_CLOCK_REALTIME)?0:1;
        if(!FutexMakeDeadline(false,value,ClockNowNs(clock),clock,&deadline))return -EINVAL;
    }
    PiKey key{};
    Process* wake=nullptr;
    int64_t result=0;
    bool queued=false;
    bool restoreIrqs=false;
    // Retry transient owner-exit and first-contender races outside every
    // publication lock. Never expose EAGAIN/EINVAL for valid blocking PI lock
    // admission: pthread callers expect either ownership or a defined error.
    for(;;) {
        if(!RobustEnsurePage(proc,proc->addressSpace,address,3))return -EFAULT;
        {
            MappingTransaction mapping(proc,proc->addressSpace);
            if(!mapping.LockFaultRange(address))return -ENOMEM;
            auto* word=PiMappedWord(proc->addressSpace,address);if(!word)return -EFAULT;
            if(int error=FutexMappedKey(proc,address,operation,
                MappingRangeFind(proc->addressSpace->ranges,address),key.domain,key.offset))return error;
            const auto flags=IrqSpinLockAcquire(&g_futexLock);
            uint32_t observed=__atomic_load_n(word,__ATOMIC_ACQUIRE);
            if(PiOrdinaryQueued(key))result=-EINVAL;
            else if(op==FUTEX_UNLOCK_PI) {
                if((observed&PiTidMask)!=proc->pid)result=-EPERM;
                else {
                    const auto* next=g_piGraph.Next(key,PiId(proc));
                    const uint32_t replacement=next?(PiWaiters|next->waiter.pid):0;
                    if(!next && g_piGraph.HasKey(key))result=-EINVAL;
                    else if(!__atomic_compare_exchange_n(word,&observed,replacement,false,
                                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))result=-EAGAIN;
                    else if(next) {
                        const auto successor=next->waiter;
                        wake=g_piPins[successor.pid];ProcessRef(wake);
                        (void)g_piGraph.Handoff(key,PiId(proc),successor);
                        PiSyncPins();
                    }
                }
            } else if((observed&PiTidMask)==proc->pid)result=-35;
            else if(!(observed&PiTidMask)) {
                if(g_piGraph.HasKey(key))result=(observed&PiDied)?-EAGAIN:-EINVAL;
                else {
                    const uint32_t replacement=(observed&PiDied)|proc->pid;
                    result=__atomic_compare_exchange_n(word,&observed,replacement,false,
                        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)?0:-EAGAIN;
                }
            } else if(op==FUTEX_TRYLOCK_PI)result=-EAGAIN;
            else if(deadline.expired(ClockNowNs(deadline.clockId)))result=-ETIMEDOUT;
            else {
                const uint32_t tid=observed&PiTidMask;
                // The registry pin precedes publishing this identity into the
                // graph. A terminating owner is retried after its exit cleanup.
                auto* owner=tid<MAX_PROCESSES?SchedulerFindProcessReference(static_cast<uint16_t>(tid)):nullptr;
                if(!owner)result=-ESRCH;
                else if(owner->isKernelThread)result=-EPERM;
                else if(__atomic_load_n(&owner->robustExitState,__ATOMIC_ACQUIRE)>=3)result=-ESRCH;
                else if(owner->state==ProcessState::Terminated ||
                        __atomic_load_n(&owner->robustExitState,__ATOMIC_ACQUIRE)>=2)result=-EAGAIN;
                else {
                    uint32_t expected=observed;
                    if(!__atomic_compare_exchange_n(word,&expected,observed|PiWaiters,false,
                        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))result=-EAGAIN;
                    else if(!PiPin(proc) || !PiPin(owner))result=-EINVAL;
                    else {
                        const auto admitted=g_piGraph.Enqueue(key,PiId(proc),proc->schedPriority,
                            PiId(owner),owner->schedPriority);
                        if(admitted!=PiGraphResult::Ok)result=PiGraphError(admitted);
                        else {
                            g_piWaitAddress[proc->pid]={address,operation};
                            __atomic_store_n(&proc->pendingWakeup,0,__ATOMIC_RELEASE);
                            proc->wakeupTick=deadline.wakeupTick(ClockNowNs(deadline.clockId),ApicTickCount());
                            queued=true;
                        }
                    }
                    PiSyncPins();
                }
                ProcessUnref(owner);
            }
            // Transfer the wake's pin out while keeping IRQs off until both
            // mapping locks and the temporary reference have been released.
            restoreIrqs=(flags&0x200)!=0;
            IrqSpinLockRelease(&g_futexLock,wake?(flags&~uint64_t{0x200}):flags);
        }
        if(wake) {
            WakeProcess(wake);SchedulerReleaseWaitReference(wake);wake=nullptr;
            if(restoreIrqs) __asm__ volatile("sti" ::: "memory");
        }
        if(queued || result!=-EAGAIN || op==FUTEX_TRYLOCK_PI || op==FUTEX_UNLOCK_PI)break;
        if(HasPendingSignals())return SignalRestartNoIntr;
        if(deadline.expired(ClockNowNs(deadline.clockId)))return -ETIMEDOUT;
        SchedulerSleepMs(1);
    }
    if(!queued)return result;
    for(;;) {
        SchedulerBlockInterruptible(proc);
        const bool interrupted=HasPendingSignals() || proc->terminalPending;
        const bool expired=deadline.expired(ClockNowNs(deadline.clockId));
        const auto flags=IrqSpinLockAcquire(&g_futexLock);
        if(interrupted || expired)(void)g_piGraph.Cancel(PiId(proc),interrupted?SignalRestartNoIntr:-ETIMEDOUT);
        int completion=0;
        if(g_piGraph.Consume(PiId(proc),&completion)) {
            PiSyncPins();IrqSpinLockRelease(&g_futexLock,flags);return completion;
        }
        // Spurious wakeups never report ownership. The existing node remains
        // linked across timer-hint rearming, closing the lost-wakeup interval.
        proc->wakeupTick=deadline.wakeupTick(ClockNowNs(deadline.clockId),ApicTickCount());
        PiSyncPins();IrqSpinLockRelease(&g_futexLock,flags);
    }
}

// Exit/exec cleanup runs in a protected worker before target AS retirement.
// Each attempt pins a waiter AS and resolves its word without holding the
// graph lock; key, identity and selected successor are then revalidated.
static void PiFutexOwnerExit(Process* target)
{
    auto* actor=ProcessCurrent();if(!actor || !target)return;
    for(unsigned attempt=0;attempt<MAX_PROCESSES;++attempt) {
        Process* waiter=nullptr;PiKey key{};PiWaitAddress user{};
        {
            const auto flags=IrqSpinLockAcquire(&g_futexLock);
            const auto* owned=g_piGraph.FirstOwnedBy(PiId(target));
            if(owned) {
                key=owned->key;
                const auto* next=g_piGraph.Next(key,PiId(target));
                waiter=g_piPins[next->waiter.pid];ProcessRef(waiter);user=g_piWaitAddress[waiter->pid];
            }
            IrqSpinLockRelease(&g_futexLock,flags);
        }
        if(!waiter)break;
        bool exists=false;
        auto* space=SchedulerReferenceProcessAddressSpace(waiter->pid,waiter->incarnation,&exists);
        if(!space) {
            const auto flags=IrqSpinLockAcquire(&g_futexLock);
            const auto* node=g_piGraph.Get(PiId(waiter));
            const bool wake=node && node->state==PiWaitState::Waiting && node->owner==PiId(target) &&
                g_piGraph.Cancel(PiId(waiter),-EFAULT);
            PiSyncPins();IrqSpinLockRelease(&g_futexLock,flags);
            if(wake)WakeProcess(waiter);
            ProcessUnref(waiter);continue;
        }
        bool readable=space && RobustEnsurePage(actor,space,user.address,3);
        bool wake=false;
        {
            MappingTransaction mapping(actor,space);
            readable=readable && mapping.LockFaultRange(user.address);
            auto* word=readable?PiMappedWord(space,user.address):nullptr;
            uint64_t domain=0,offset=0;
            readable=word && !FutexMappedKey(waiter,user.address,user.operation,
                MappingRangeFind(space->ranges,user.address),domain,offset) && key==PiKey{domain,offset};
            const auto flags=IrqSpinLockAcquire(&g_futexLock);
            const auto* next=g_piGraph.Next(key,PiId(target));
            if(next && next->waiter==PiId(waiter)) {
                uint32_t observed=readable?__atomic_load_n(word,__ATOMIC_ACQUIRE):0;
                const bool owned=(observed&PiTidMask)==target->pid;
                const bool robustDied=!(observed&PiTidMask) && (observed&PiDied);
                if(!readable || (!owned && !robustDied)) {
                    wake=g_piGraph.Cancel(PiId(waiter),readable?-EINVAL:-EFAULT);
                } else {
                    const uint32_t replacement=PiDied|PiWaiters|waiter->pid;
                    if(__atomic_compare_exchange_n(word,&observed,replacement,false,
                        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
                        wake=g_piGraph.Handoff(key,PiId(target),PiId(waiter))==PiGraphResult::Ok;
                    }
                }
                PiSyncPins();
            }
            IrqSpinLockRelease(&g_futexLock,flags);
        }
        if(wake)WakeProcess(waiter);
        AddressSpaceUnref(space);ProcessUnref(waiter);
    }
    // A malicious/churning user word can defeat every bounded CAS attempt.
    // Retiring the owner must still release all graph pins and waiters; never
    // leave an unserviceable dependency after robustExitState becomes complete.
    for(unsigned remaining=0;remaining<MAX_PROCESSES;++remaining) {
        const auto flags=IrqSpinLockAcquire(&g_futexLock);
        const auto* node=g_piGraph.FirstOwnedBy(PiId(target));
        if(!node) {IrqSpinLockRelease(&g_futexLock,flags);break;}
        auto* waiter=g_piPins[node->waiter.pid];ProcessRef(waiter);
        (void)g_piGraph.Cancel(PiId(waiter),-EINVAL);PiSyncPins();
        IrqSpinLockRelease(&g_futexLock,flags);
        WakeProcess(waiter);ProcessUnref(waiter);
    }
}
