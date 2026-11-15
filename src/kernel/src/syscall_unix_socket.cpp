// syscall_unix_socket.cpp -- AF_UNIX socket objects: registry, queues, stream I/O and readiness.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "tsc_clock.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_pipe.h"
#include "syscall_unix_socket.h"
#include "syscall_socket.h"

namespace brook {

// ---------------------------------------------------------------------------
// AF_UNIX path sockets — structs and helpers defined here so
// sys_read/sys_write/sys_close can use them
// ---------------------------------------------------------------------------

static constexpr int UNIX_MAX_SERVERS   = 32;

void UnixFdSnapRelease(const UnixFdSnap* snap);

UnixCredentials UnixCurrentCredentials(Process* process)
{
    return {static_cast<int32_t>((process->tgid ? process->tgid : process->pid)), process->euid, process->egid};
}
uint64_t UnixCurrentIncarnation(Process* process)
{
    return process->isThread?process->leaderIncarnation:process->incarnation;
}

// The direction's pipe lock protects BOTH bytes and descriptor ownership.
// Publishing descriptors separately lets a receiver steal them before their
// bytes arrive; unsynchronized receivers can also install the same ref twice.
static int64_t UnixStreamTryWrite(PipeBuffer* pipe, UnixFdQueue* queue,
                                  const char* bytes, uint64_t length,
                                  const UnixFdSnap* descriptors=nullptr, int count=0,
                                  const UnixCredentials* explicitCredentials=nullptr,bool record=false) {
    auto* process = ProcessCurrent();
    const UnixCredentials sender=explicitCredentials?*explicitCredentials:UnixCredentials{
        static_cast<int32_t>(process->tgid ? process->tgid : process->pid),
        ProcessReadIds(process,false).real,ProcessReadIds(process,true).real};
    SpinLockAcquire(&pipe->lock);
    bool merge = false;
    if (!record && queue && queue->credentialHead != queue->credentialTail) {
        auto& previous = queue->credentials[(queue->credentialHead + UNIX_FD_QUEUE_CAP - 1) % UNIX_FD_QUEUE_CAP];
        merge = previous.sender.pid == sender.pid && previous.sender.uid == sender.uid && previous.sender.gid == sender.gid;
    }
    const bool credentialRoom = !queue || merge ||
        (queue->credentialHead + 1) % UNIX_FD_QUEUE_CAP != queue->credentialTail;
    int64_t result=-EAGAIN;
    if (!pipe->readers || pipe->unixReadShutdown || pipe->unixWriteShutdown) result=-EPIPE;
    else if (!length && !record) result=0;
    else if (record && length>=pipe->capacity) result=-90; // EMSGSIZE; never publish a prefix.
    else if ((record?length<=pipe->space():pipe->space()!=0) && credentialRoom &&
             (!count || (queue && (queue->head-queue->tail+UNIX_FD_QUEUE_CAP)%UNIX_FD_QUEUE_CAP+count<UNIX_FD_QUEUE_CAP))) {
        uint32_t n=pipe->space();if (length<n)n=static_cast<uint32_t>(length);
        if (n || record) {
            for(int i=0;i<count;++i) {
                queue->msgs[queue->head]=descriptors[i];
                queue->offsets[queue->head]=queue->written;
                queue->records[queue->head]=queue->credentialHead;
                queue->head=(queue->head+1)%UNIX_FD_QUEUE_CAP;
            }
            uint32_t first=pipe->capacity-pipe->head;if(first>n)first=n;
            if(first)memcpy(pipe->data+pipe->head,bytes,first);
            if(n>first)memcpy(pipe->data,bytes+first,n-first);
            pipe->head=(pipe->head+n)%pipe->capacity;
            if(queue) {
                queue->written += n;
                if (merge) queue->credentials[(queue->credentialHead + UNIX_FD_QUEUE_CAP - 1) % UNIX_FD_QUEUE_CAP].end = queue->written;
                else {
                    queue->credentials[queue->credentialHead] = {queue->written, sender};
                    queue->credentialHead = (queue->credentialHead + 1) % UNIX_FD_QUEUE_CAP;
                }
            }
        }
        result=n;
    }
    SpinLockRelease(&pipe->lock);return result;
}
static uint32_t UnixStreamTryRead(PipeBuffer* pipe, UnixFdQueue* queue,
                                  char* bytes, uint64_t length,
                                  UnixFdSnap* descriptors, int& count, UnixCredentials* sender,
                                  bool record=false,bool* consumed=nullptr,uint64_t* recordLength=nullptr) {
    count=0;SpinLockAcquire(&pipe->lock);
    if(consumed)*consumed=false;
    if(recordLength)*recordLength=0;
    if(record) {
        if(!queue || queue->credentialTail==queue->credentialHead) {SpinLockRelease(&pipe->lock);return 0;}
        const uint32_t slot=queue->credentialTail;
        const auto& span=queue->credentials[slot];
        const uint64_t size=span.end-queue->read;
        uint32_t n=static_cast<uint32_t>(size<length?size:length);
        if(sender)*sender=span.sender;
        while(queue->tail!=queue->head && queue->records[queue->tail]==slot) {
            descriptors[count++]=queue->msgs[queue->tail];
            queue->tail=(queue->tail+1)%UNIX_FD_QUEUE_CAP;
        }
        uint32_t first=pipe->capacity-pipe->tail;if(first>n)first=n;
        if(first)memcpy(bytes,pipe->data+pipe->tail,first);
        if(n>first)memcpy(bytes+first,pipe->data,n-first);
        // A short receive discards the rest of this record, including bytes
        // beyond the supplied buffer. Following records remain untouched.
        pipe->tail=(pipe->tail+static_cast<uint32_t>(size))%pipe->capacity;
        queue->read=span.end;queue->credentialTail=(slot+1)%UNIX_FD_QUEUE_CAP;
        if(consumed)*consumed=true;
        if(recordLength)*recordLength=size;
        SpinLockRelease(&pipe->lock);return n;
    }
    uint32_t n=pipe->count();if(length<n)n=static_cast<uint32_t>(length);
    if(queue && n) {
        if (sender && queue->credentialTail != queue->credentialHead) {
            auto& span = queue->credentials[queue->credentialTail];
            *sender = span.sender;
            if (span.end - queue->read < n) n = static_cast<uint32_t>(span.end - queue->read);
        }
        // Stop before the next distinct descriptor group. A one-byte receive
        // must never collect rights attached to a later byte in the stream.
        bool first=true;uint64_t offset=0;
        for(int i=queue->tail;i!=queue->head;i=(i+1)%UNIX_FD_QUEUE_CAP) {
            uint64_t at=queue->offsets[i];
            if(first) {offset=at;first=false;}
            else if(at!=offset) {
                if(at<queue->read+n)n=static_cast<uint32_t>(at-queue->read);
                break;
            }
        }
        while(queue->tail!=queue->head && queue->offsets[queue->tail]<queue->read+n) {
            descriptors[count++]=queue->msgs[queue->tail];
            queue->tail=(queue->tail+1)%UNIX_FD_QUEUE_CAP;
        }
        queue->read+=n;
        while (queue->credentialTail != queue->credentialHead &&
               queue->credentials[queue->credentialTail].end <= queue->read)
            queue->credentialTail = (queue->credentialTail + 1) % UNIX_FD_QUEUE_CAP;
    }
    uint32_t first=pipe->capacity-pipe->tail;if(first>n)first=n;
    if(n)memcpy(bytes,pipe->data+pipe->tail,first);
    if(n>first)memcpy(bytes+first,pipe->data,n-first);
    pipe->tail=(pipe->tail+n)%pipe->capacity;
    SpinLockRelease(&pipe->lock);return n;
}

static inline int UnixFdQueueCount(const UnixFdQueue* q)
{
    return (q->head - q->tail + UNIX_FD_QUEUE_CAP) % UNIX_FD_QUEUE_CAP;
}
[[maybe_unused]] static inline bool UnixFdQueueFull(const UnixFdQueue* q)
{
    return UnixFdQueueCount(q) >= UNIX_FD_QUEUE_CAP - 1;
}

SpinLock g_unixRegistryLock;
// Explicitly armed with "unixlife on" after workload gates. No UART traffic.
extern "C" { UnixLifeCore g_unixLifeTrace{}; }
static IrqSpinLock g_unixLifeLock;
void UnixLifeControl(bool enabled)
{
    uint64_t flags = IrqSpinLockAcquire(&g_unixLifeLock);
    // Re-arm clears bounded state; readers should extract only while stopped.
    if (enabled) memset(&g_unixLifeTrace, 0, sizeof(g_unixLifeTrace));
    __atomic_store_n(&g_unixLifeTrace.enabled, enabled ? 1u : 0u, __ATOMIC_RELEASE);
    IrqSpinLockRelease(&g_unixLifeLock, flags);
}
void UnixLife(UnixLifeEvent event, void* handle, uint32_t previous,
              uint32_t next, int fd, uint64_t caller, uint64_t size)
{
    if (!__atomic_load_n(&g_unixLifeTrace.enabled, __ATOMIC_RELAXED)) return;
    Process* current = ProcessCurrent();
    UnixLifeRecord record{0, reinterpret_cast<uint64_t>(handle), caller, 0, size,
        previous, next, current ? current->pid : uint16_t{0},
        current ? (current->tgid ? current->tgid : current->pid) : uint16_t{0},
        fd, event, 0};
    uint64_t flags = IrqSpinLockAcquire(&g_unixLifeLock);
    if (g_unixLifeTrace.enabled) {
        if (event == UnixLifeEvent::Allocate) g_unixLifeTrace.Watch(record);
        else g_unixLifeTrace.Event(record);
    }
    IrqSpinLockRelease(&g_unixLifeLock, flags);
}
void UnixLifeHeap(void* address, uint64_t size, UnixLifeEvent event, uint64_t caller)
{
    if (!__atomic_load_n(&g_unixLifeTrace.enabled, __ATOMIC_RELAXED)) return;
    Process* current = ProcessCurrent();
    UnixLifeRecord record{0, 0, caller, reinterpret_cast<uint64_t>(address), size,
        0, 0, current ? current->pid : uint16_t{0},
        current ? (current->tgid ? current->tgid : current->pid) : uint16_t{0},
        -1, event, 0};
    uint64_t flags = IrqSpinLockAcquire(&g_unixLifeLock);
    if (g_unixLifeTrace.enabled) g_unixLifeTrace.Heap(record, event == UnixLifeEvent::HeapFree || event == UnixLifeEvent::HeapWriteFree);
    IrqSpinLockRelease(&g_unixLifeLock, flags);
}

void UnixLifeClaim(int fd, const FdClaimResult& claim)
{
    if (claim.type == FdType::UnixSocket)
        UNIX_LIFE(Claim, claim.handle, 0, 0, fd);
}

void UnixLifePublished(int fd, const FdEntry& entry)
{
    if (entry.type == FdType::UnixSocket)
        UNIX_LIFE(Publish, entry.handle, 0, 0, fd);
}

static UnixSocketData* g_unixServers[UNIX_MAX_SERVERS];
// Name operations can perform filesystem I/O. Serialize them with a sleeping
// lock; never retain the registry spinlock across lookup or inode creation.
KMutex g_unixNameLock;

UnixSocketData* UnixFindServer(const char* path, const VnodeStat* node)
{
    SpinLockAcquire(&g_unixRegistryLock);
    UnixSocketData* found = nullptr;
    for (int i = 0; i < UNIX_MAX_SERVERS; i++) {
        auto* candidate = g_unixServers[i];
        if (!candidate || candidate->state != UnixSocketData::State::Listening) continue;
        const bool match = node ? candidate->boundNode &&
            candidate->boundDev == node->dev && candidate->boundIno == node->ino :
            !candidate->boundNode && strcmp(candidate->path, path) == 0;
        if (match) {
            uint32_t previous = __atomic_fetch_add(&candidate->refCount, 1, __ATOMIC_RELAXED);
            UNIX_LIFE(RegistryPin, candidate, previous, previous + 1, -1);
            found = candidate;
            break;
        }
    }
    SpinLockRelease(&g_unixRegistryLock);

    return found;
}

bool UnixRegisterServer(UnixSocketData* usd)
{
    SpinLockAcquire(&g_unixRegistryLock);
    int available = -1;
    for (int i = 0; i < UNIX_MAX_SERVERS; i++) {
        if (g_unixServers[i] == usd) { SpinLockRelease(&g_unixRegistryLock); return true; }
        if (!g_unixServers[i]) available = i;
    }
    if (available >= 0) g_unixServers[available] = usd;
    SpinLockRelease(&g_unixRegistryLock);
    return available >= 0;
}

void UnixSocketHandleRef(void* handle)
{
    if (!handle) return;
    auto* usd = static_cast<UnixSocketData*>(handle);
    uint32_t previous = __atomic_fetch_add(&usd->refCount, 1, __ATOMIC_RELEASE);
    UNIX_LIFE(Retain, usd, previous, previous + 1, -1);
}

static void UnixFdQueueUnref(UnixFdQueue* q)
{
    if (!q) return;
    uint32_t prev = __atomic_fetch_sub(&q->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev > 1) return;
    while (q->head != q->tail) {
        UnixFdSnap& m = q->msgs[q->tail];
        q->tail = (q->tail + 1) % UNIX_FD_QUEUE_CAP;
        UnixFdSnapRelease(&m);
    }
    kfree(q);
}

// The receiver owns delivery, even while the sender still references the
// queue. Stop new writes and detach all queued capabilities under the stream
// lock; releasing them can recursively close sockets, so do that unlocked.
static void UnixDiscardIncoming(PipeBuffer* pipe, UnixFdQueue* queue)
{
    if (!pipe || !queue) return;
    UnixFdSnap discarded[UNIX_FD_QUEUE_CAP];
    int count = 0;
    SpinLockAcquire(&pipe->lock);
    pipe->unixReadShutdown = true;
    while (queue->tail != queue->head) {
        discarded[count++] = queue->msgs[queue->tail];
        queue->tail = (queue->tail + 1) % UNIX_FD_QUEUE_CAP;
    }
    SpinLockRelease(&pipe->lock);
    for (int i = 0; i < count; ++i) UnixFdSnapRelease(&discarded[i]);
}

static void UnixPendingRelease(const UnixPendingConn& pending)
{
    UnixDiscardIncoming(pending.serverRx, pending.serverRxFds);
    PipeReleaseEnd(pending.serverRx, false);
    PipeReleaseEnd(pending.serverTx, true);
    UnixFdQueueUnref(pending.serverRxFds);
    UnixFdQueueUnref(pending.serverTxFds);
}

void UnixSocketHandleUnref(void* handle)
{
    if (!handle) return;
    auto* usd = static_cast<UnixSocketData*>(handle);
    UnixPendingConn abandoned[UNIX_ACCEPT_QUEUE]{};
    SpinLockAcquire(&g_unixRegistryLock);
    uint32_t prev = __atomic_fetch_sub(&usd->refCount, 1, __ATOMIC_ACQ_REL);
    // Trace before unlocking: another release could free the object afterward.
    UNIX_LIFE(Release, usd, prev, prev - 1, -1);
    if (prev == 1) {
        for (int i = 0; i < UNIX_MAX_SERVERS; ++i)
            if (g_unixServers[i] == usd) g_unixServers[i] = nullptr;
        for (int i = 0; i < UNIX_ACCEPT_QUEUE; ++i) {
            abandoned[i] = usd->pending[i];
            usd->pending[i] = {};
        }
        usd->pendingCount = 0;
    }
    SpinLockRelease(&g_unixRegistryLock);
    if (prev > 1) return;
    for (const auto& pending : abandoned)
        if (pending.used) UnixPendingRelease(pending);
    UnixDiscardIncoming(usd->rxPipe, usd->incomingFds);
    PipeReleaseEnd(usd->rxPipe, false);
    PipeReleaseEnd(usd->txPipe, true);

    UnixFdQueueUnref(usd->incomingFds);
    UnixFdQueueUnref(usd->peerIncomingFds);
    if (usd->boundNode) VfsClose(usd->boundNode);
    UNIX_LIFE(DirectFree, usd, 0, 0, -1);
    kfree(usd);
}

int UnixInstallSocket(Process* process, UnixSocketData* socket, uint32_t flags,
                      const char* site)
{
    FdEntry owned{};
    owned.type = FdType::UnixSocket;
    owned.handle = socket;
    owned.statusFlags = flags & 0x800;
    owned.fdFlags = (flags & 0x80000) ? 1 : 0;
    (void)site;
    int fd = FdTablePublishOwned(process->fds, ProcessFdLock(process), owned, 0, -1, nullptr, UnixLifePublished);
    return fd;
}

void UnixShutdownDirection(PipeBuffer* pipe, bool writer)
{
    SpinLockAcquire(&pipe->lock);
    if (writer) __atomic_store_n(&pipe->unixWriteShutdown,true,__ATOMIC_RELEASE);
    else __atomic_store_n(&pipe->unixReadShutdown,true,__ATOMIC_RELEASE);
    // Wake both local and peer operations, including the thread which is
    // cancelling a blocked receive. References remain until actual close.
    auto* reader=pipe->readerWaiter;pipe->readerWaiter=nullptr;
    auto* waitingWriter=pipe->writerWaiter;pipe->writerWaiter=nullptr;
    if(reader) WakeProcess(reader);
    if(waitingWriter) WakeProcess(waitingWriter);
    PipeWakeEpollRaw(pipe);
    SpinLockRelease(&pipe->lock);
    PipePollNotify(pipe, 1 | 4);
}

// Linux poll/epoll bits share these values. Closed directions remain ready:
// reads return EOF after queued bytes; writes fail immediately with EPIPE.
uint32_t UnixSocketReady(UnixSocketData* socket,uint32_t events)
{
    if(socket->state==UnixSocketData::State::Listening)
        return (events&1) && socket->pendingCount ? 1 : 0;
    if(socket->state!=UnixSocketData::State::Connected || !socket->rxPipe || !socket->txPipe)return 0;
    auto* rx=socket->rxPipe;auto* tx=socket->txPipe;
    SpinLockAcquire(&rx->lock);
    bool readClosed=rx->unixReadShutdown || rx->unixWriteShutdown || !rx->writers;
    bool readable=rx->count() || readClosed || (socket->socketType==5 && socket->incomingFds &&
        socket->incomingFds->credentialHead!=socket->incomingFds->credentialTail);
    SpinLockRelease(&rx->lock);
    SpinLockAcquire(&tx->lock);
    bool writeClosed=tx->unixReadShutdown || tx->unixWriteShutdown || !tx->readers;
    bool room=socket->socketType!=5 || (socket->peerIncomingFds &&
        (socket->peerIncomingFds->credentialHead+1)%UNIX_FD_QUEUE_CAP!=socket->peerIncomingFds->credentialTail);
    bool writable=(tx->space() && room) || writeClosed;
    SpinLockRelease(&tx->lock);
    return ((events&1)&&readable?1u:0u) | ((events&4)&&writable?4u:0u) |
        ((events&0x2000)&&readClosed?0x2000u:0u) | (readClosed&&writeClosed?0x10u:0u);
}

struct UnixSocketStage {
    char* bytes=nullptr;
    ~UnixSocketStage(){if(bytes)kfree(bytes);}
};
int64_t UnixSocketWriteSome(UnixSocketData* socket,bool nonblock,
                            const char* bytes,uint64_t length,
                            const UnixFdSnap* descriptors,int count,bool noSignal,
                            const UnixCredentials* explicitCredentials) {
    if(socket->state!=UnixSocketData::State::Connected || !socket->txPipe)return -ENOTCONN;
    auto* pipe=socket->txPipe;
    UnixSocketStage staged;
    if(socket->socketType==5) {
        if(length>=pipe->capacity)return -90;
        if(length && reinterpret_cast<uint64_t>(bytes)<0x0000800000000000ULL) {
            staged.bytes=static_cast<char*>(kmalloc(length));if(!staged.bytes)return -ENOMEM;
            if(!CopyFromUser(staged.bytes,reinterpret_cast<uint64_t>(bytes),length))return -EFAULT;
            bytes=staged.bytes;
        }
    }
    for(;;) {
        auto result=UnixStreamTryWrite(pipe,socket->peerIncomingFds,bytes,length,descriptors,count,explicitCredentials,socket->socketType==5);
        if(result!=-EAGAIN) {
            if(result==-EPIPE && !noSignal)ProcessSendSignal(ProcessCurrent(),13);
            if(result>0 || (!result && socket->socketType==5)) {
                if(auto* reader=pipe->readerWaiter) {pipe->readerWaiter=nullptr;WakeProcess(reader);}
                PipeWakeEpoll(pipe);
            }
            return result;
        }
        if(nonblock || socket->nonblock)return -EAGAIN;
        auto* self=ProcessCurrent();pipe->writerWaiter=self;
        self->wakeupTick=ApicTickCount()+10;SchedulerBlock(self);
        if(HasPendingSignals()) {
            if(pipe->writerWaiter==self)pipe->writerWaiter=nullptr;
            return -EINTR;
        }
    }
}
int64_t UnixSocketReadSome(UnixSocketData* socket,bool nonblock,
                           char* bytes,uint64_t length,UnixFdSnap* descriptors,int& count,
                           UnixCredentials* sender,uint64_t* recordLength,bool* consumed) {
    count=0;
    if(consumed)*consumed=false;
    if(recordLength)*recordLength=0;
    if(socket->state!=UnixSocketData::State::Connected || !socket->rxPipe)return -ENOTCONN;
    if(!length && socket->socketType!=5)return 0;
    auto* pipe=socket->rxPipe;
    UnixSocketStage staged;
    char* userTarget=nullptr;
    if(socket->socketType==5 && length && reinterpret_cast<uint64_t>(bytes)<0x0000800000000000ULL) {
        if(length>=pipe->capacity)length=pipe->capacity-1;
        staged.bytes=static_cast<char*>(kmalloc(length));if(!staged.bytes)return -ENOMEM;
        userTarget=bytes;bytes=staged.bytes;
    }
    for(;;) {
        bool received=false;
        auto n=UnixStreamTryRead(pipe,socket->incomingFds,bytes,length,descriptors,count,sender,
            socket->socketType==5,&received,recordLength);
        if(consumed)*consumed=received;
        if(n || received) {
            if(auto* writer=pipe->writerWaiter) {pipe->writerWaiter=nullptr;WakeProcess(writer);}
            PipeWakeEpoll(pipe);
            if(userTarget && n && !CopyToUser(reinterpret_cast<uint64_t>(userTarget),bytes,n))return -EFAULT;
            return n;
        }
        if(!__atomic_load_n(&pipe->writers,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&pipe->unixReadShutdown,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&pipe->unixWriteShutdown,__ATOMIC_ACQUIRE))return 0;
        if(nonblock || socket->nonblock)return -EAGAIN;
        auto* self=ProcessCurrent();pipe->readerWaiter=self;
        self->wakeupTick=ApicTickCount()+10;SchedulerBlock(self);
        if(HasReadInterruptSignal()) {
            if(pipe->readerWaiter==self)pipe->readerWaiter=nullptr;
            return -EINTR;
        }
    }
}

} // namespace brook
