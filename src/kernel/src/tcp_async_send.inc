// Experimental per-connection asynchronous sender. Included by net.cpp after
// SockSendImpl. Each admitted job holds one socket owner reference and one
// bounded worker permit until all accepted bytes drain or the stream fails.
static constexpr uint32_t TCP_ASYNC_WORKER_LIMIT=8;
static constexpr uint32_t TCP_ASYNC_QUEUE_BYTES=32768;
static constexpr uint32_t TCP_ASYNC_BATCH_BYTES=16384;
struct TcpAsyncJob {
    int index;
    uint64_t epoch;
    TcpSendQueue queue;
    uint8_t storage[TCP_ASYNC_QUEUE_BYTES];
    uint8_t batch[TCP_ASYNC_BATCH_BYTES];
};

static bool TcpAsyncReserveWorker() {
    uint32_t n=__atomic_load_n(&g_tcpAsyncWorkers,__ATOMIC_RELAXED);
    while(n<TCP_ASYNC_WORKER_LIMIT)
        if(__atomic_compare_exchange_n(&g_tcpAsyncWorkers,&n,n+1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))return true;
    return false;
}
static void TcpAsyncWake(Process* waiter) {
    if(waiter){__atomic_store_n(&waiter->pendingWakeup,1,__ATOMIC_RELEASE);SchedulerUnblock(waiter);ProcessUnref(waiter);}
}
static void TcpAsyncWorker(void* argument) {
    auto* job=static_cast<TcpAsyncJob*>(argument);const int index=job->index;Socket& s=g_sockets[index];
    for(;;) {
        SpinLockAcquire(&s.lock);
        if(g_tcpAsyncEpoch[index]!=job->epoch || !TcpCanSendData(s.tcpState) || s.tcpRstRecv || !job->queue.used) {
            // Producers only access the job under s.lock. Detach before
            // releasing the worker reference or allowing another job to start.
            const bool attached=g_tcpAsyncJobs[index]==job;
            if(attached)g_tcpAsyncJobs[index]=nullptr;
            Process* waiter=attached ? TakeSocketWaiterLocked(s) : nullptr;
            SpinLockRelease(&s.lock);TcpAsyncWake(waiter);
#ifdef BROOK_SOCKET_LIFETIME_TEST
            extern void TcpAsyncTestDetached(int);
            TcpAsyncTestDetached(index);
#endif
            kfree(job);SockUnref(index);
            __atomic_fetch_sub(&g_tcpAsyncWorkers,1u,__ATOMIC_RELEASE);
            return; // the final unref may have destroyed and reused the slot
        }
        const uint32_t batch=job->queue.ReserveCopy(job->batch,TCP_ASYNC_BATCH_BYTES);
        SpinLockRelease(&s.lock);
        // The trusted source is owned by the job, never by a producer. The
        // existing sender retains its own flight copy and returns ACKed bytes.
        const int result=batch ? SockSendImpl(index,{job->batch,false,nullptr,0},batch) : -5;
        SpinLockAcquire(&s.lock);
        const uint32_t acked=result>0 ? static_cast<uint32_t>(result) : 0;
        const bool valid=acked<=batch && job->queue.Retire(acked);
        if(!valid || result!=static_cast<int>(batch)) {
            if(TcpCanSendData(s.tcpState)) {
                s.tcpState=TcpState::Closed;s.connected=false;s.tcpRstRecv=true;
                if(!s.connectError)s.connectError=result<0 ? -result : 5;
            }
        }
        Process* waiter=TakeSocketWaiterLocked(s);
        SpinLockRelease(&s.lock);TcpAsyncWake(waiter);
    }
}

// The outer reference survives waits with no producer gate held. The kernel
// region also covers releasing the reference, which may finalize the socket.
class TcpAsyncLifetime {
    int index_;Process* self_;bool held_=false;
public:
    TcpAsyncLifetime(int index,Process* self):index_(index),self_(self) {
        SchedulerKernelRegionEnter(self_);held_=SockTryRef(index_);
    }
    ~TcpAsyncLifetime(){if(held_)SockUnref(index_);SchedulerKernelRegionLeave(self_);}
    bool Held() const{return held_;}
};

static int TcpSendAsync(int index,const TcpSendSource& source,uint32_t len) {
    if(index<0 || index>=static_cast<int>(MAX_SOCKETS))return -1;
    Process* self=SchedulerCurrentProcess();if(!self)return -11;
    TcpAsyncLifetime lifetime(index,self);if(!lifetime.Held())return -1;
    const uint64_t deadline=ApicTickCount()+30000;uint32_t accepted=0;uint64_t epoch=0;bool haveEpoch=false;
    uint8_t staging[ETH_MTU];
    for(;;) {
        bool wait=false;
        {
            TcpSendOwnership producer(index,self,&g_tcpAsyncProducerGates[index]);
            int acquired=producer.Acquire(deadline);
            if(acquired)return accepted ? static_cast<int>(accepted) : acquired;
            // Serialize each accepted syscall prefix against other producers.
            // Release the gate before a full-queue or worker-pool wait.
            for(;;) {
                if(TcpSendInterrupted(self))return accepted ? static_cast<int>(accepted) : -4;
                if(ApicTickCount()>=deadline)return accepted ? static_cast<int>(accepted) : -110;
                Socket& s=g_sockets[index];
                SpinLockAcquire(&s.lock);
                if(s.type!=SOCK_STREAM || !TcpCanSendData(s.tcpState) || s.tcpRstRecv) {
                    const int error=s.type!=SOCK_STREAM ? -95 : -104;
                    SpinLockRelease(&s.lock);return accepted ? static_cast<int>(accepted) : error;
                }
                if(!haveEpoch){epoch=g_tcpAsyncEpoch[index];haveEpoch=true;}
                if(epoch!=g_tcpAsyncEpoch[index]){SpinLockRelease(&s.lock);return accepted ? static_cast<int>(accepted) : -104;}
                if(accepted==len){SpinLockRelease(&s.lock);return static_cast<int>(accepted);}
                TcpAsyncJob* job=g_tcpAsyncJobs[index];
                const bool nonblock=__atomic_load_n(&s.nonblock,__ATOMIC_ACQUIRE);
                const uint32_t mss=TcpEgressLimit(s.tcpPeerMss,g_tcpEgressCeiling);
                if(job && !job->queue.Free()) {
                    SpinLockRelease(&s.lock);
                    if(accepted)return static_cast<int>(accepted);
                    if(nonblock)return -11;
                    wait=true;break;
                }
                SpinLockRelease(&s.lock);
                if(!mss || mss>sizeof(staging))return accepted ? static_cast<int>(accepted) : -22;
                uint32_t chunk=len-accepted;if(chunk>mss)chunk=mss;
                if(!source.Copy(staging,accepted,chunk))return accepted ? static_cast<int>(accepted) : -14;
                if(TcpSendInterrupted(self))return accepted ? static_cast<int>(accepted) : -4;
                if(ApicTickCount()>=deadline)return accepted ? static_cast<int>(accepted) : -110;
                SpinLockAcquire(&s.lock);
                if(epoch!=g_tcpAsyncEpoch[index] || !TcpCanSendData(s.tcpState) || s.tcpRstRecv) {
                    SpinLockRelease(&s.lock);return accepted ? static_cast<int>(accepted) : -104;
                }
                job=g_tcpAsyncJobs[index];
                if(job) {
                    accepted+=job->queue.Append(staging,chunk);
                    SpinLockRelease(&s.lock);
                    continue;
                }
                SpinLockRelease(&s.lock);
                if(!TcpAsyncReserveWorker()) {
                    if(accepted)return static_cast<int>(accepted);
                    if(nonblock)return -11;
                    wait=true;break;
                }
                // The producer gate excludes another publisher. A separate
                // owner pins the socket until the worker detaches its job.
                if(!SockTryRef(index)) {__atomic_fetch_sub(&g_tcpAsyncWorkers,1u,__ATOMIC_RELEASE);return accepted ? static_cast<int>(accepted) : -1;}
                auto* memory=kmalloc(sizeof(TcpAsyncJob));
                auto* created=memory ? new (memory) TcpAsyncJob{} : nullptr;
                if(created){created->index=index;created->epoch=epoch;created->queue.Init(created->storage,sizeof(created->storage));}
                Process* worker=created ? KernelThreadCreate("tcp_async",TcpAsyncWorker,created,2) : nullptr;
                if(!worker) {
                    if(created)kfree(created);SockUnref(index);
                    __atomic_fetch_sub(&g_tcpAsyncWorkers,1u,__ATOMIC_RELEASE);
                    return accepted ? static_cast<int>(accepted) : -12;
                }
                SpinLockAcquire(&s.lock);
                // A peer can close or reconnect during allocation. Only
                // attach to the same live incarnation; an unattached worker
                // still runs once to release its resources and reference.
                const bool attach=epoch==g_tcpAsyncEpoch[index] && TcpCanSendData(s.tcpState) && !s.tcpRstRecv;
                if(attach){g_tcpAsyncJobs[index]=created;
                    accepted+=created->queue.Append(staging,chunk);
                }
                SpinLockRelease(&s.lock);SchedulerAddProcess(worker);
                if(!attach)return accepted ? static_cast<int>(accepted) : -104;
            }
        }
        if(wait){self->wakeupTick=ApicTickCount()+10;SchedulerBlockInterruptible(self);}
    }
}
