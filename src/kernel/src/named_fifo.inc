// Actual named FIFO descriptions, included after anonymous-pipe release helpers.
// Registry keys pin the backing inode, never a pathname. The mutex serializes
// last-description retirement against a new open of that same inode.
struct NamedFifoWait { Process* process;NamedFifoWait* next; };
struct NamedFifoState {
    NamedFifoState* next;
    VfsPath identity;
    PipeBuffer* pipe;
    uint64_t descriptions,readerEpoch,writerEpoch;
    NamedFifoWait* waiters;
};
struct NamedFifoOpen { NamedFifoState* state;uint32_t access;uint64_t writerEpoch;bool writerSeen; };
static KMutex g_namedFifoMutex{};
static NamedFifoState* g_namedFifos=nullptr;
static void NamedFifoWake(NamedFifoState* state) {
    for(auto* w=state->waiters;w;w=w->next)WakeProcess(w->process);
}
static void NamedFifoClose(Vnode* node) {
    auto* opened=static_cast<NamedFifoOpen*>(node->streamOwner);auto* state=opened->state;
    KMutexLock(&g_namedFifoMutex);
    if(!state->descriptions)__builtin_trap();
    const bool last=!--state->descriptions;
    if(last) {
        auto** link=&g_namedFifos;while(*link && *link!=state)link=&(*link)->next;
        if(!*link || state->waiters)__builtin_trap();
        *link=state->next;
    }
    // An RDWR description keeps both endpoints until its final vnode release.
    if(opened->access!=1)PipeReleaseEnd(state->pipe,false);
    if(opened->access!=0)PipeReleaseEnd(state->pipe,true);
    NamedFifoWake(state);
    KMutexUnlock(&g_namedFifoMutex);
    node->streamPipe=nullptr;node->streamOwner=nullptr;node->streamRelease=nullptr;kfree(opened);
    if(last) {VfsPathRelease(state->identity);kfree(state);}
}
static int NamedFifoAttach(Vnode* node,uint32_t flags) {
    KernelOperationGuard operation(ProcessCurrent());
    const uint32_t access=flags&3;if(access==3)return -EINVAL;
    VfsPath identity{};int error=VfsPathFromVnode(node,&identity);if(error)return error;
    auto* opened=static_cast<NamedFifoOpen*>(kmalloc(sizeof(NamedFifoOpen)));
    if(!opened) {VfsPathRelease(identity);return -ENOMEM;}
    KMutexLock(&g_namedFifoMutex);
    NamedFifoState* state=g_namedFifos;
    while(state && !VfsPathSameObject(state->identity,identity))state=state->next;
    if(!state) {
        state=static_cast<NamedFifoState*>(kmalloc(sizeof(NamedFifoState)));
        PipeBuffer* pipe=state?PipeBufferCreate(PIPE_BUF_DEFAULT_SIZE):nullptr;
        if(!state || !pipe) {KMutexUnlock(&g_namedFifoMutex);kfree(state);kfree(opened);VfsPathRelease(identity);return -ENOMEM;}
        *state={g_namedFifos,identity,pipe,0,0,0,nullptr};identity={};g_namedFifos=state;
    }
    // A nonblocking writer cannot manufacture its own reader. Empty new states
    // are retired on this failure just like a last-description close.
    if(access==1 && (flags&0x800) && !state->pipe->readers) {
        const bool unused=!state->descriptions;
        if(unused) {auto** link=&g_namedFifos;while(*link!=state)link=&(*link)->next;*link=state->next;}
        KMutexUnlock(&g_namedFifoMutex);VfsPathRelease(identity);kfree(opened);
        if(unused) {PipeBufferDestroy(state->pipe);VfsPathRelease(state->identity);kfree(state);}
        return -6; // ENXIO
    }
    const uint64_t opposite=access==0?state->writerEpoch:state->readerEpoch;
    *opened={state,access,state->writerEpoch,state->pipe->writers!=0};
    ++state->descriptions;
    SpinLockAcquire(&state->pipe->lock);
    if(access!=1) {__atomic_fetch_add(&state->pipe->readers,1,__ATOMIC_RELEASE);__atomic_fetch_add(&state->readerEpoch,uint64_t{1},__ATOMIC_RELEASE);}
    if(access!=0) {__atomic_fetch_add(&state->pipe->writers,1,__ATOMIC_RELEASE);__atomic_fetch_add(&state->writerEpoch,uint64_t{1},__ATOMIC_RELEASE);}
    SpinLockRelease(&state->pipe->lock);
    node->streamPipe=state->pipe;node->streamOwner=opened;node->streamRelease=NamedFifoClose;
    NamedFifoWake(state);
    // Readiness subscriptions may predate this new writer/reader.
    PipePollNotify(state->pipe,1|4);
    NamedFifoWait waiter{ProcessCurrent(),state->waiters};
    bool registered=false;
    while(access!=2 && !(flags&0x800)) {
        bool paired=access==0 ? state->pipe->writers!=0 || state->writerEpoch!=opposite
                             : state->pipe->readers!=0 || state->readerEpoch!=opposite;
        if(paired)break;
        if(HasReadInterruptSignal()) {error=-EINTR;break;}
        if(!registered) {waiter.next=state->waiters;state->waiters=&waiter;registered=true;}
        waiter.process->wakeupTick=ApicTickCount()+10;
        KMutexUnlock(&g_namedFifoMutex);SchedulerBlockInterruptible(waiter.process);KMutexLock(&g_namedFifoMutex);
    }
    if(registered) {auto** link=&state->waiters;while(*link && *link!=&waiter)link=&(*link)->next;if(!*link)__builtin_trap();*link=waiter.next;}
    KMutexUnlock(&g_namedFifoMutex);VfsPathRelease(identity);
    if(error)NamedFifoClose(node);
    return error;
}
static PipeBuffer* FdPipe(const FdEntry* entry) {
    if(!entry || !entry->handle)return nullptr;
    if(entry->type==FdType::Pipe)return static_cast<PipeBuffer*>(entry->handle);
    if(entry->type==FdType::Vnode)return static_cast<Vnode*>(entry->handle)->streamPipe;
    return nullptr;
}
static NamedFifoOpen* FdNamedFifo(const FdEntry* entry) {
    return entry && entry->type==FdType::Vnode && entry->handle && static_cast<Vnode*>(entry->handle)->streamPipe
        ?static_cast<NamedFifoOpen*>(static_cast<Vnode*>(entry->handle)->streamOwner):nullptr;
}
static bool FdPipeRead(const FdEntry* entry) {return FdNamedFifo(entry)?(entry->statusFlags&3)!=1:!(entry->flags&1);}
static bool FdPipeWrite(const FdEntry* entry) {return FdNamedFifo(entry)?(entry->statusFlags&3)!=0:(entry->flags&1)!=0;}
static uint32_t NamedFifoReady(const FdEntry* entry,uint32_t events) {
    auto* opened=FdNamedFifo(entry);if(!opened)return 0;
    auto* pipe=opened->state->pipe;uint32_t ready=0;
    SpinLockAcquire(&pipe->lock);
    if(FdPipeRead(entry)) {
        if(pipe->count() && (events&1))ready|=1;
        if(!pipe->writers && (opened->writerSeen || __atomic_load_n(&opened->state->writerEpoch,__ATOMIC_ACQUIRE)!=opened->writerEpoch))ready|=16;
    }
    if(FdPipeWrite(entry)) {
        if(!pipe->readers)ready|=8;
        else if(pipe->space() && (events&4))ready|=4;
    }
    SpinLockRelease(&pipe->lock);return ready;
}
