// Actual IRQ notification/wait implementation, also compiled by hosted tests.
static IrqSpinLock g_completionLock{};
static GpuCompletionStats g_completionStats{};
static volatile uint8_t* g_completionIsr;
static volatile uint16_t* g_completionUsed;
static volatile uint16_t* g_completionAvailFlags;
static Process* g_completionWaiter;
static uint16_t g_completionExpected;
static uint64_t g_completionGeneration;
static uint64_t g_completionIrqNs;

static void CompletionCount(uint64_t& value) {
    if(value==~uint64_t(0))g_completionStats.saturated=true;else ++value;
}
static void CompletionWake(Process* process) {
    if (!process) return;
    __atomic_store_n(&process->pendingWakeup,1,__ATOMIC_RELEASE);
    SchedulerUnblock(process);
    ProcessUnref(process);
}
static void GpuCompletionIrq() {
    Process* process=nullptr;
    const uint64_t flags=IrqSpinLockAcquire(&g_completionLock);
    if(g_completionStats.registered) {
        // This is the sole read-clear reader, including zero/config-only IRQs.
        const uint8_t status=*g_completionIsr;
        CompletionCount(g_completionStats.irqCalls);
        if(status&1) {
            CompletionCount(g_completionStats.queueIrqs);
            __asm__ volatile("mfence" ::: "memory");
            if(g_completionWaiter && *g_completionUsed!=g_completionExpected) {
                process=g_completionWaiter;ProcessRef(process);
                if(!g_completionIrqNs && !KvmClockReadStableNs(&g_completionIrqNs))
                    CompletionCount(g_completionStats.invalidLatency);
                CompletionCount(g_completionStats.notifications);
            }
        }
    }
    IrqSpinLockRelease(&g_completionLock,flags);
    CompletionWake(process);
}

extern "C" bool GpuCompletionRegister(const void* provider,volatile uint8_t* isr,
    volatile uint16_t* used,volatile uint16_t* availFlags,uint8_t irq,bool eventIdx) {
    // Boot registration is serialized by the module loader. No replacement,
    // reset, unmap or runtime unregister is supported by this experiment.
    if(!provider || !isr || !used || !availFlags || eventIdx || irq==0 || irq==255)return false;
    const uint64_t flags=IrqSpinLockAcquire(&g_completionLock);
    if(g_completionStats.registered) {IrqSpinLockRelease(&g_completionLock,flags);return false;}
    const uintptr_t address=reinterpret_cast<uintptr_t>(provider);
    if(!ModuleRetainAddresses(&address,1) ||
       !IoApicHandlerCapacity(irq,reinterpret_cast<void*>(GpuCompletionIrq))) {
        IrqSpinLockRelease(&g_completionLock,flags);return false;
    }
    g_completionIsr=isr;g_completionUsed=used;g_completionAvailFlags=availFlags;
    *availFlags|=1;__asm__ volatile("mfence" ::: "memory");
    // Publish pointers before unmasking; the callback runs only on CPU0 and
    // registration retains IRQ masking until the complete service is visible.
    g_completionStats.registered=true;g_completionStats.irq=irq;
    g_completionStats.vector=IoApicRegisterHandler(irq,51,reinterpret_cast<void*>(GpuCompletionIrq));
    IrqSpinLockRelease(&g_completionLock,flags);return true;
}

bool GpuCompletionControl(bool enabled) {
    Process* process=nullptr;
    const uint64_t flags=IrqSpinLockAcquire(&g_completionLock);
    if(!g_completionStats.registered) {IrqSpinLockRelease(&g_completionLock,flags);return false;}
    __atomic_store_n(&g_completionStats.enabled,enabled,__ATOMIC_RELEASE);
    if(enabled)*g_completionAvailFlags&=uint16_t(~1u);
    else {
        *g_completionAvailFlags|=1;
        process=g_completionWaiter;
        if(process)ProcessRef(process);
        g_completionWaiter=nullptr;g_completionStats.waiting=false;
    }
    __asm__ volatile("mfence" ::: "memory");
    IrqSpinLockRelease(&g_completionLock,flags);
    CompletionWake(process);return true;
}
GpuCompletionStats GpuCompletionRead() {
    const uint64_t flags=IrqSpinLockAcquire(&g_completionLock);
    const auto stats=g_completionStats;
    IrqSpinLockRelease(&g_completionLock,flags);return stats;
}

extern "C" bool GpuCompletionEnabled() {
    return __atomic_load_n(&g_completionStats.enabled,__ATOMIC_ACQUIRE);
}

extern "C" bool GpuCompletionPause(uint16_t expected,uint64_t start,uint64_t deadline) {
    if(!GpuCompletionEnabled())return false;
    auto* self=ProcessCurrent();uint64_t now=0;
    if(!CompletionCanSleep(self) || !start || start>=deadline ||
       !KvmClockReadStableNs(&now) || now<start || now>=deadline || now-start<50000)return false;
    // One caller holds the GPU command mutex, so there is at most one waiter.
    // Keep a local registration reference even if `off` detaches the pointer.
    const uint64_t flags=IrqSpinLockAcquire(&g_completionLock);
    if(!g_completionStats.enabled || !g_completionStats.registered || g_completionWaiter ||
       g_completionGeneration==~uint64_t(0)) {
        IrqSpinLockRelease(&g_completionLock,flags);return false;
    }
    ProcessRef(self);g_completionWaiter=self;g_completionExpected=expected;
    const uint64_t generation=++g_completionGeneration;g_completionIrqNs=0;
    g_completionStats.waiting=true;CompletionCount(g_completionStats.waits);
    // Register -> arm -> recheck. A completion during arming is either visible
    // here or leaves pendingWakeup before scheduler admission.
    *g_completionAvailFlags&=uint16_t(~1u);__asm__ volatile("mfence" ::: "memory");
#ifdef BROOK_GPU_COMPLETION_HOST_TEST
    CompletionHostAfterArm();
#endif
    const bool ready=*g_completionUsed!=expected;
    if(ready)CompletionCount(g_completionStats.readyBeforeBlock);
    IrqSpinLockRelease(&g_completionLock,flags);
    const uint64_t oldTick=self->wakeupTick;
    uint64_t safetyTick=0;
    if(!ready && CompletionCanSleep(self) && KvmClockReadStableNs(&now) && now>=start && now<deadline) {
        uint64_t ms=(deadline-now)/1000000; // round down; final sub-ms remains polling
        if(ms) {
            if(ms>10)ms=10;
            const uint64_t tick=ApicTickCount();
            if(tick<=~uint64_t(0)-ms) {
                safetyTick=tick+ms;self->wakeupTick=safetyTick;
                const uint64_t f=IrqSpinLockAcquire(&g_completionLock);
                CompletionCount(g_completionStats.blockCalls);
                IrqSpinLockRelease(&g_completionLock,f);
                SchedulerBlockInterruptible(self);
            }
        }
    }
    self->wakeupTick=oldTick;
    const bool clockOk=KvmClockReadStableNs(&now);
    const uint64_t cleanupFlags=IrqSpinLockAcquire(&g_completionLock);
    if(g_completionGeneration==generation) {
        if(g_completionWaiter==self)g_completionWaiter=nullptr;
        g_completionStats.waiting=false;
        if(!ready && *g_completionUsed==expected) {
            CompletionCount(g_completionStats.incompleteResumes);
            if(safetyTick && ApicTickCount()>=safetyTick)CompletionCount(g_completionStats.safetyExpiries);
        }
        if(g_completionIrqNs) {
            if(clockOk && now>=g_completionIrqNs) {
                const uint64_t latency=now-g_completionIrqNs;
                CompletionCount(g_completionStats.latencySamples);
                if(g_completionStats.latencySumNs>~uint64_t(0)-latency)g_completionStats.saturated=true;
                else g_completionStats.latencySumNs+=latency;
                if(latency>g_completionStats.latencyMaxNs)g_completionStats.latencyMaxNs=latency;
            } else CompletionCount(g_completionStats.invalidLatency);
        }
    }
    IrqSpinLockRelease(&g_completionLock,cleanupFlags);
    ProcessUnref(self);return true;
}
