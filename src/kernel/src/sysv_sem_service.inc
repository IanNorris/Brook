// Included by sysv_sem.cpp and the host test. All shared state is under semLock.
// No heap, user-copy, scheduler lock or sleep while that lock is held.
namespace {
constexpr unsigned SemMaxSets=32000, SemMaxPerSet=32767, SemMaxOperations=500;
struct SemSet {
    SemSet* next;
    int id;
    SysvSemDs ds;
    uint16_t* values;
    int32_t* pids;
};
struct SemUndoRecord {
    SemUndoRecord* next;
    int id;
    unsigned count;
    int32_t* adjustments;
};
struct SemUndoGroup { unsigned references; SemUndoRecord* records; };
struct SemAlias { uint64_t incarnation; SemUndoGroup* group; };
struct SemWaiter {
    SemWaiter* next;
    Process* process;
    int id;
    uint16_t number;
    bool zero, notified;
    uint64_t serial;
};
IrqSpinLock semLock{};
SemSet* semSets=nullptr;
unsigned semSetCount=0;
uint32_t semNextId=1;
SemUndoGroup semGroups[MAX_PROCESSES]{};
SemAlias semAliases[MAX_PROCESSES]{};
SemUndoRecord* semGarbage=nullptr;
SemWaiter* semWaiters=nullptr;
uint64_t semWaitSerial=0;
struct SemCredentials {
    uint32_t uid,gid;bool privileged;
    SupplementaryGroups* groups;
    explicit SemCredentials(Process* p):uid(p->euid),gid(p->egid),groups(ProcessGroupsAcquire(p)){privileged=ProcessCapable(p,nullptr,CapSysAdmin);}
    ~SemCredentials(){ProcessGroupsRelease(groups);}
    bool member(uint32_t g) const {
        if(g==gid)return true;
        if(groups)for(unsigned i=0;i<groups->count;++i)if(groups->ids[i]==g)return true;
        return false;
    }
    bool permitted(const SemSet* s,unsigned request) const {
        if(privileged)return true;
        const auto& p=s->ds.permission;
        unsigned bits=p.mode;
        if(uid==p.uid || uid==p.cuid)bits>>=6;
        else if(member(p.gid)||member(p.cgid))bits>>=3;
        return (bits&request)==request;
    }
    bool owner(const SemSet* s) const {
        return privileged || uid==s->ds.permission.uid || uid==s->ds.permission.cuid;
    }
};
SemSet* SemLookup(int id) {
    for(auto* s=semSets;s;s=s->next)if(s->id==id)return s;
    return nullptr;
}
SemSet* SemKey(int32_t key) {
    if(!key)return nullptr;
    for(auto* s=semSets;s;s=s->next)if(s->ds.permission.key==key)return s;
    return nullptr;
}
SemUndoGroup* SemGroup(Process* p,bool create) {
    if(p->pid>=MAX_PROCESSES)return nullptr;
    auto& a=semAliases[p->pid];
    if(a.group)return a.incarnation==p->incarnation?a.group:nullptr;
    if(!create)return nullptr;
    for(auto& g:semGroups)if(!g.references) {
        g.references=1;g.records=nullptr;a={p->incarnation,&g};return &g;
    }
    return nullptr;
}
SemUndoRecord* SemRecord(SemUndoGroup* g,int id) {
    if(g)for(auto* r=g->records;r;r=r->next)if(r->id==id)return r;
    return nullptr;
}
void SemDrainGarbage() {
    auto f=IrqSpinLockAcquire(&semLock);
    auto* r=semGarbage;semGarbage=nullptr;
    IrqSpinLockRelease(&semLock,f);
    while(r){auto* next=r->next;kfree(r);r=next;}
}
// Capture a finite registration frontier. New/rearmed waiters are not chased.
// Pin only the Process, never a stack waiter, across each unlocked wake.
void SemNotify() {
    auto f=IrqSpinLockAcquire(&semLock);
    const uint64_t cutoff=semWaitSerial;
    for(;;) {
        Process* target=nullptr;
        for(auto* w=semWaiters;w;w=w->next)if(w->serial<=cutoff && !w->notified) {
            w->notified=true;target=w->process;ProcessRef(target);break;
        }
        // Do not restore IF while a notifier owns a temporary process hold.
        IrqSpinLockRelease(&semLock,f&~0x200ULL);
        if(!target){if(f&0x200)__asm__ volatile("sti" ::: "memory");return;}
        SchedulerUnblock(target);ProcessUnref(target);
        if(f&0x200)__asm__ volatile("sti" ::: "memory");
        f=IrqSpinLockAcquire(&semLock);
    }
}
void SemClearUndo(int id,int number) {
    for(auto& g:semGroups)for(auto* r=g.records;r;r=r->next)if(r->id==id)
        for(unsigned i=0;i<r->count;++i)if(number<0 || i==unsigned(number))r->adjustments[i]=0;
}
struct SemOperationRegion {
    Process* p;
    explicit SemOperationRegion(Process* process):p(process){SchedulerKernelRegionEnter(p);}
    ~SemOperationRegion(){SchedulerKernelRegionLeave(p);}
};
}
int SysvSemGet(Process* p,int32_t key,int count,int flags) {
    if(!p || count<0 || unsigned(count)>SemMaxPerSet)return -22;
    SemOperationRegion region(p);SemCredentials credentials(p);SemDrainGarbage();
    SemSet* candidate=nullptr;
    for(;;) {
        auto f=IrqSpinLockAcquire(&semLock);
        auto* s=SemKey(key);
        int result=0;
        if(s) {
            if((flags&01000)&&(flags&02000))result=-17;
            else if(unsigned(count)>s->ds.count)result=-22;
            else if(!credentials.permitted(s,((flags>>6)|(flags>>3)|flags)&7))result=-13;
            else result=s->id;
        } else if(key && !(flags&01000))result=-2;
        else if(!count)result=-22;
        else if(semSetCount>=SemMaxSets || semNextId>0x7fffffffU)result=-28;
        else if(candidate) {
            candidate->id=int(semNextId++);
            candidate->ds.permission={key,credentials.uid,credentials.gid,credentials.uid,credentials.gid,uint32_t(flags&0777),0,0,0,{0,0}};
            candidate->ds.count=unsigned(count);candidate->ds.changeTime=RtcNow();
            candidate->next=semSets;semSets=candidate;++semSetCount;
            result=candidate->id;candidate=nullptr;
        }
        IrqSpinLockRelease(&semLock,f);
        if(result){kfree(candidate);return result;}
        const uint64_t bytes=sizeof(SemSet)+uint64_t(count)*(sizeof(uint16_t)+sizeof(int32_t))+8;
        candidate=static_cast<SemSet*>(kmalloc(bytes));
        if(!candidate)return -12;
        *candidate={};candidate->values=reinterpret_cast<uint16_t*>(candidate+1);
        const auto aligned=(reinterpret_cast<uintptr_t>(candidate->values+count)+3)&~uintptr_t(3);
        candidate->pids=reinterpret_cast<int32_t*>(aligned);
        for(int i=0;i<count;++i){candidate->values[i]=0;candidate->pids[i]=0;}
    }
}
int SysvSemCount(Process*,int id) {
    auto f=IrqSpinLockAcquire(&semLock);auto* s=SemLookup(id);
    const int result=s?int(s->ds.count):-22;IrqSpinLockRelease(&semLock,f);return result;
}
int SysvSemControl(Process* p,int id,int number,int command,int value,SysvSemDs* ds,uint16_t* values,unsigned count) {
    if(!p)return -22;
    SemOperationRegion region(p);SemCredentials credentials(p);SemDrainGarbage();
    command &= ~0x100; // IPC_64 is redundant in the native x86-64 ABI.
    auto f=IrqSpinLockAcquire(&semLock);auto* s=SemLookup(id);
    int result=0;bool notify=false;SemSet* retired=nullptr;
    if(!s)result=-22;
    else if(command==0 || command==1) {
        if(!credentials.owner(s))result=-1;
        else if(command==0) {
            SemSet** link=&semSets;while(*link!=s)link=&(*link)->next;
            *link=s->next;--semSetCount;SemClearUndo(id,-1);retired=s;notify=true;
        } else if(!ds)result=-14;
        else if(ds->permission.uid==~uint32_t{0} || ds->permission.gid==~uint32_t{0})result=-22;
        else {
            s->ds.permission.uid=ds->permission.uid;s->ds.permission.gid=ds->permission.gid;
            s->ds.permission.mode=ds->permission.mode&0777;s->ds.changeTime=RtcNow();
        }
    } else if(!credentials.permitted(s,command==16||command==17?2:4))result=-13;
    else if(command==2) {if(ds)*ds=s->ds;else result=-14;}
    else if(command==13 || command==17) {
        if(!values || count!=s->ds.count)result=-22;
        else {
            if(command==17)for(unsigned i=0;i<count;++i)if(values[i]>SysvSemMaxValue){result=-34;break;}
            if(!result)for(unsigned i=0;i<count;++i) {
                if(command==13)values[i]=s->values[i];
                else {s->values[i]=values[i];s->pids[i]=p->tgid;}
            }
            if(!result && command==17){SemClearUndo(id,-1);s->ds.changeTime=RtcNow();notify=true;}
        }
    } else if(number<0 || unsigned(number)>=s->ds.count)result=-22;
    else if(command==11)result=s->pids[number];
    else if(command==12)result=s->values[number];
    else if(command==14 || command==15) {
        for(auto* w=semWaiters;w;w=w->next)if(w->id==id && w->number==number && w->zero==(command==15))++result;
    } else if(command==16) {
        if(value<0 || value>SysvSemMaxValue)result=-34;
        else {s->values[number]=uint16_t(value);s->pids[number]=p->tgid;SemClearUndo(id,number);s->ds.changeTime=RtcNow();notify=true;}
    } else result=-22;
    IrqSpinLockRelease(&semLock,f);kfree(retired);if(notify)SemNotify();return result;
}
int SysvSemClone(Process* parent,Process* child,bool share) {
    if(!share)return 0;
    auto f=IrqSpinLockAcquire(&semLock);
    auto* group=SemGroup(parent,true);
    int result=-12;
    if(group && child->pid<MAX_PROCESSES && !semAliases[child->pid].group) {
        ++group->references;semAliases[child->pid]={child->incarnation,group};result=0;
    }
    IrqSpinLockRelease(&semLock,f);return result;
}
void SysvSemExit(Process* p) {
    if(!p || p->pid>=MAX_PROCESSES)return;
    auto f=IrqSpinLockAcquire(&semLock);auto& a=semAliases[p->pid];
    bool notify=false;
    if(a.group && a.incarnation==p->incarnation) {
        auto* g=a.group;a={};
        if(!--g->references) {
            while(g->records) {
                auto* r=g->records;g->records=r->next;
                if(auto* s=SemLookup(r->id)) {
                    bool touched=false;
                    for(unsigned i=0;i<r->count;++i)if(r->adjustments[i]) {
                        int64_t n=int64_t(s->values[i])+r->adjustments[i];
                        s->values[i]=uint16_t(n<0?0:n>SysvSemMaxValue?SysvSemMaxValue:n);
                        s->pids[i]=p->tgid;touched=true;
                    }
                    if(touched){s->ds.operationTime=RtcNow();notify=true;}
                }
                r->next=semGarbage;semGarbage=r;
            }
        }
    }
    IrqSpinLockRelease(&semLock,f);if(notify)SemNotify();
}
int SysvSemOperate(Process* p,int id,const SysvSemOp* ops,unsigned operations,uint64_t deadlineNs) {
    if(!p || !operations)return -22;
    if(operations>SemMaxOperations)return -7;
    SemOperationRegion region(p);SemCredentials credentials(p);SemDrainGarbage();
    const int size=SysvSemCount(p,id);if(size<0)return size;
    bool undo=false,alter=false;
    for(unsigned i=0;i<operations;++i){undo|=(ops[i].flags&SysvSemUndo)!=0;alter|=ops[i].delta!=0;}
    // One allocation supplies speculative values, undo and an unpublished record.
    const uint64_t bytes=sizeof(SemUndoRecord)+uint64_t(size)*sizeof(int32_t)*2+uint64_t(size)*sizeof(uint16_t);
    auto* memory=static_cast<SemUndoRecord*>(kmalloc(bytes));if(!memory)return -12;
    *memory={};memory->id=id;memory->count=unsigned(size);
    memory->adjustments=reinterpret_cast<int32_t*>(memory+1);
    for(int i=0;i<size;++i)memory->adjustments[i]=0;
    auto* proposedUndo=memory->adjustments+size;
    auto* proposed=reinterpret_cast<uint16_t*>(proposedUndo+size);
    int result=0;bool waited=false;
    for(;;) {
        auto f=IrqSpinLockAcquire(&semLock);auto* s=SemLookup(id);
        SemUndoGroup* group=nullptr;SemUndoRecord* record=nullptr;
        if(!s){result=waited?-43:-22;IrqSpinLockRelease(&semLock,f);break;}
        if(!credentials.permitted(s,alter?2:4)){result=-13;IrqSpinLockRelease(&semLock,f);break;}
        if(undo){group=SemGroup(p,true);record=SemRecord(group,id);}
        if(undo && !group){result=-12;IrqSpinLockRelease(&semLock,f);break;}
        const auto preview=SysvSemPreview(s->values,unsigned(size),record?record->adjustments:nullptr,ops,operations,proposed,proposedUndo);
        if(preview.state==SysvSemOpState::Error){result=-preview.error;IrqSpinLockRelease(&semLock,f);break;}
        if(preview.state==SysvSemOpState::Ready) {
            if(undo && !record) {
                // Preserve scratch in memory until after copying its result.
                record=memory;record->next=group->records;group->records=record;
            }
            for(int i=0;i<size;++i){s->values[i]=proposed[i];if(undo)record->adjustments[i]=proposedUndo[i];}
            for(unsigned i=0;i<operations;++i)s->pids[ops[i].number]=p->tgid;
            s->ds.operationTime=RtcNow();
            if(record==memory && undo)memory=nullptr;
            IrqSpinLockRelease(&semLock,f);SemNotify();break;
        }
        if(SemInterrupted(p)){result=-4;IrqSpinLockRelease(&semLock,f);break;}
        if(deadlineNs!=~uint64_t{0} && SemNowNs()>=deadlineNs){result=-11;IrqSpinLockRelease(&semLock,f);break;}
        if(semWaitSerial==~uint64_t{0}){result=-28;IrqSpinLockRelease(&semLock,f);break;}
        SemWaiter waiter{semWaiters,p,id,preview.waitSemaphore,preview.waitZero,false,++semWaitSerial};
        semWaiters=&waiter;
        __atomic_store_n(&p->pendingWakeup,0,__ATOMIC_RELEASE);p->wakeupTick=SemWakeupTick(deadlineNs);
        IrqSpinLockRelease(&semLock,f);
        waited=true;
        SchedulerBlockInterruptible(p);
        const bool interrupted=SemInterrupted(p);
        f=IrqSpinLockAcquire(&semLock);
        SemWaiter** link=&semWaiters;while(*link!=&waiter)link=&(*link)->next;
        *link=waiter.next;
        const bool removed=!SemLookup(id);
        IrqSpinLockRelease(&semLock,f);
        if(removed){result=-43;break;}
        if(interrupted){result=-4;break;}
#ifdef BROOK_SYSV_SEM_RETRY_TEST
        SemRetryTestHook();
#endif
    }
    kfree(memory);return result;
}
