// Publish sender information and the pending bit together. Dequeue clears both
// under this short IRQ lock; no allocation or scheduler locks are taken here.
static IrqSpinLock g_signalSenderLock;
// SIGCHLD needs immutable exit/status information even after waitpid reaps the
// child. One coalesced record per task, guarded with the pending bit, no ABI field.
struct PendingChildSignal {uint64_t incarnation;SignalInfo info;bool pending;};
static PendingChildSignal g_childSignals[MAX_PROCESSES]{};
static PendingChildSignal g_groupChildSignals[MAX_PROCESSES]{};
static uint32_t ChildGroup(Process* p){return p->tgid?p->tgid:p->pid;}
static uint64_t ChildGroupIncarnation(Process* p){return p->isThread?p->leaderIncarnation:p->incarnation;}
static PendingChildSignal* SharedChildSignal(Process* p) {
    const auto group=ChildGroup(p);if(group>=MAX_PROCESSES)return nullptr;
    auto* record=&g_groupChildSignals[group];
    return record->pending && record->incarnation==ChildGroupIncarnation(p)?record:nullptr;
}
uint64_t ProcessPendingSignals(Process* p) {
    auto flags=IrqSpinLockAcquire(&g_signalSenderLock);
    uint64_t pending=__atomic_load_n(&p->sigPending,__ATOMIC_ACQUIRE);
    if(SharedChildSignal(p))pending|=1ULL<<16;
    IrqSpinLockRelease(&g_signalSenderLock,flags);return pending;
}
// Caller holds scheduler ready/registry locks. Signal readers share this lock,
// so neither readers nor parent notification can observe a mixed exec identity.
void ProcessCommitExecIdentity(Process* p) {
    auto flags=IrqSpinLockAcquire(&g_signalSenderLock);
    const auto oldGroup=ChildGroup(p);const auto oldIncarnation=ChildGroupIncarnation(p);
    p->isThread=false;
#ifdef BROOK_SIGNAL_IDENTITY_TEST
    SignalIdentityTransitionTest();
#endif
    p->tgid=p->pid;p->threadLeader=p;p->leaderIncarnation=0;
    if(oldGroup<MAX_PROCESSES && ChildGroup(p)<MAX_PROCESSES) {
        auto& old=g_groupChildSignals[oldGroup];
        if(old.pending && old.incarnation==oldIncarnation) {
            auto saved=old;old={};saved.incarnation=p->incarnation;
            g_groupChildSignals[ChildGroup(p)]=saved;
        }
    }
    IrqSpinLockRelease(&g_signalSenderLock,flags);
}

static void QueueSignal(Process* proc, int signum, const SignalInfo* info)
{
    uint64_t flags = IrqSpinLockAcquire(&g_signalSenderLock);
    uint64_t bit = 1ULL << (signum - 1);
    if (!(__atomic_load_n(&proc->sigPending, __ATOMIC_ACQUIRE) & bit)) {
        auto& sender = proc->sigSenders[signum - 1];
        sender = {};
        sender.code = info ? info->si_code : 128; // SI_KERNEL
        if(signum==17 && proc->pid<MAX_PROCESSES) {
            auto& child=g_childSignals[proc->pid];child={};child.incarnation=proc->incarnation;
            if(info)child.info=*info;
            else {child.info.si_signo=17;child.info.si_code=128;}
        }
        if (info) {
            memcpy(&sender.pid, info->_data, sizeof(sender.pid));
            memcpy(&sender.uid, info->_data + 4, sizeof(sender.uid));
        }
    }
    __atomic_or_fetch(&proc->sigPending, bit, __ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_signalSenderLock, flags);
}

bool ProcessDequeueSignal(Process* proc, int signum, SignalInfo* info)
{
    uint64_t flags = IrqSpinLockAcquire(&g_signalSenderLock);
    uint64_t bit = 1ULL << (signum - 1);
    uint64_t previous = __atomic_fetch_and(&proc->sigPending, ~bit, __ATOMIC_ACQ_REL);
    auto& sender = proc->sigSenders[signum - 1];
    if (info) {
        *info = {};
        info->si_signo = signum;
        info->si_code = sender.code;
        memcpy(info->_data, &sender.pid, sizeof(sender.pid));
        memcpy(info->_data + 4, &sender.uid, sizeof(sender.uid));
        if((previous&bit) && signum==17 && proc->pid<MAX_PROCESSES &&
           g_childSignals[proc->pid].incarnation==proc->incarnation)
            *info=g_childSignals[proc->pid].info;
    }
    sender = {};
    bool consumed=(previous&bit)!=0;
    if(!consumed && signum==17)if(auto* shared=SharedChildSignal(proc)) {
        if(info)*info=shared->info;shared->pending=false;consumed=true;
    }
    IrqSpinLockRelease(&g_signalSenderLock, flags);
    return consumed;
}

void ProcessQueueChildSignal(Process* parent,Process* child)
{
    const uint32_t pid=child->pid,uid=child->uid;
    int code=1,status=(child->waitStatus>>8)&255;
    if(child->state==ProcessState::Stopped){code=5;status=child->stopSignal;}
    else if(child->state!=ProcessState::Terminated && child->continuedPending){code=6;status=18;}
    else if(child->waitStatus&127){code=(child->waitStatus&128)?3:2;status=child->waitStatus&127;}
    SignalInfo info{};info.si_signo=17;info.si_code=code;
    memcpy(info._data,&pid,4);memcpy(info._data+4,&uid,4);memcpy(info._data+8,&status,4);
    const auto group=ChildGroup(parent);if(group>=MAX_PROCESSES)return;
    auto flags=IrqSpinLockAcquire(&g_signalSenderLock);
    auto& record=g_groupChildSignals[group];
    const auto incarnation=ChildGroupIncarnation(parent);
    if(!record.pending || record.incarnation!=incarnation)record={incarnation,info,true};
    IrqSpinLockRelease(&g_signalSenderLock,flags);
}
