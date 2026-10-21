// Actual single-outstanding control path, also compiled by hosted tests.
// Normal callers hold g_gpuCmdLock; panic may only inspect EmergencySafe.
static uint64_t g_nextRenderFence=0;
static bool g_controlQueuePoisoned;
static GpuCommandStorage* g_pendingCommand;
alignas(16) static uint8_t g_quarantinedCmdBuf[CMD_PAGES*4096];

static void PoisonControlQueue() {
    auto* pending=__atomic_load_n(&g_pendingCommand,__ATOMIC_ACQUIRE);
    if(pending)pending->state=GpuCommandStorageState::Quarantined;
    __atomic_store_n(&g_controlQueuePoisoned,true,__ATOMIC_RELEASE);
    // The legacy builder must never modify its original device-owned pages.
    // Explicit storage objects keep their original bytes/physical identity.
    g_cmdBuf=g_quarantinedCmdBuf;
}

static bool ControlQueueEmergencySafe() {
    return !__atomic_load_n(&g_controlQueuePoisoned,__ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&g_pendingCommand,__ATOMIC_ACQUIRE) && g_usedIdx &&
           *g_usedIdx==g_usedIdxShadow;
}

static GpuCommandToken BeginCommand(GpuCommandStorage& storage,uint32_t reqLen,
                                    uint32_t respCap,bool fenced=false) {
    if(__atomic_load_n(&g_controlQueuePoisoned,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&g_pendingCommand,__ATOMIC_ACQUIRE) || !storage.bytes ||
       !storage.physical || storage.physical>~uint64_t(0)-CMD_PAGES*4096 ||
       storage.generation==~uint64_t(0) || g_queueSize<2 || !g_usedIdx || !g_availIdx ||
       !g_descTable || !g_usedRing || !g_availRing ||
       (storage.state!=GpuCommandStorageState::Writable &&
        storage.state!=GpuCommandStorageState::Completed) ||
       reqLen<sizeof(VirtioGpuCtrlHdr) || reqLen>CMD_RESP_OFF-CMD_REQ_OFF ||
       respCap<sizeof(VirtioGpuCtrlHdr) || respCap>CMD_PAGES*4096-CMD_RESP_OFF ||
       (fenced && g_nextRenderFence==~uint64_t(0)))return {};
    // A late/unowned used entry is not permission to overwrite descriptors.
    if(*g_usedIdx!=g_usedIdxShadow){PoisonControlQueue();return {};}
    auto* hdr=reinterpret_cast<VirtioGpuCtrlHdr*>(storage.bytes+CMD_REQ_OFF);
    storage.expectedFence=0;storage.expectedContext=hdr->ctx_id;
    storage.expectedRing=hdr->ring_idx;storage.timeline=fenced && (hdr->flags&2u);
    if(fenced){hdr->flags|=1u;hdr->fence_id=storage.expectedFence=++g_nextRenderFence;}
    storage.head=static_cast<uint16_t>(g_availIdxShadow%g_queueSize);
    storage.usedPosition=g_usedIdxShadow;storage.responseCapacity=respCap;
    ++storage.generation;storage.state=GpuCommandStorageState::Preparing;
    // Publish reservation before touching descriptors, so emergency display
    // cannot reuse storage even if panic interrupts the publication sequence.
    __atomic_store_n(&g_pendingCommand,&storage,__ATOMIC_RELEASE);
    storage.origin=GpuCompletionOrigin(ProcessCurrent()!=nullptr,
        [](uint64_t* ns){return KvmClockReadStableNs(ns);});
    FrameTimingCommand(reqLen,fenced,storage.expectedFence);
    const uint16_t d1=static_cast<uint16_t>((storage.head+1)%g_queueSize);
    g_descTable[storage.head].addr=storage.physical+CMD_REQ_OFF;
    g_descTable[storage.head].len=reqLen;
    g_descTable[storage.head].flags=VIRTQ_DESC_F_NEXT;g_descTable[storage.head].next=d1;
    g_descTable[d1].addr=storage.physical+CMD_RESP_OFF;g_descTable[d1].len=respCap;
    g_descTable[d1].flags=VIRTQ_DESC_F_WRITE;g_descTable[d1].next=0;
    __asm__ volatile("mfence" ::: "memory");
    g_availRing[g_availIdxShadow%g_queueSize]=storage.head;
    __asm__ volatile("mfence" ::: "memory");
    storage.state=GpuCommandStorageState::Published;
    *g_availIdx=++g_availIdxShadow;
    __asm__ volatile("mfence" ::: "memory");
    NotifyControlQueue();
    return {&storage,storage.generation};
}

static uint32_t FinishCommand(GpuCommandToken token,GpuCompletionWaitLimits limits={}) {
    auto* pending=__atomic_load_n(&g_pendingCommand,__ATOMIC_ACQUIRE);
    // Compare identity before dereferencing a supplied pointer. A stale or
    // duplicate finish cannot consume a later completion or renew its budget.
    if(!token || token.storage!=pending || pending->generation!=token.generation ||
       pending->state!=GpuCommandStorageState::Published ||
       __atomic_load_n(&g_controlQueuePoisoned,__ATOMIC_ACQUIRE))return 0;
    if(g_usedIdxShadow!=pending->usedPosition){PoisonControlQueue();return 0;}
    bool completed;
    {
        FrameTimingScope phase(pending->expectedFence?FramePhase::FenceWait:FramePhase::AckWait);
        completed=GpuWaitForCompletionSince(pending->origin,
            []{return *g_usedIdx!=g_usedIdxShadow;},
            [](uint64_t* ns){return KvmClockReadStableNs(ns);},
            []{__asm__ volatile("pause" ::: "memory");},limits);
    }
    if(!completed){
        PoisonControlQueue();SerialPuts("virtio_gpu: command timeout; control queue quarantined\n");return 0;
    }
    __asm__ volatile("mfence" ::: "memory");
    if(*g_usedIdx!=static_cast<uint16_t>(pending->usedPosition+1)){
        PoisonControlQueue();SerialPuts("virtio_gpu: unexpected used-ring advance; control queue quarantined\n");return 0;
    }
    const uint16_t slot=pending->usedPosition%g_queueSize;
    const uint32_t id=g_usedRing[slot].id,len=g_usedRing[slot].len;
    const auto* reply=reinterpret_cast<const VirtioGpuCtrlHdr*>(pending->bytes+CMD_RESP_OFF);
    if(!brook::GpuControlCompletionValid(pending->head,id,len,pending->responseCapacity,
                                        reply->flags,reply->fence_id,pending->expectedFence) ||
       // QEMU10.2.2 echoes fence/context, not INFO_RING_IDX/ring_idx.
       (pending->expectedFence && reply->ctx_id!=pending->expectedContext)){
        PoisonControlQueue();
        SerialPrintf("virtio_gpu: invalid completion head=%u/%u bytes=%u/%u flags=0x%x fence=%lu/%lu ctx=%u/%u ring=%u/%u timeline=%u; queue quarantined\n",
            id,pending->head,len,pending->responseCapacity,reply->flags,reply->fence_id,
            pending->expectedFence,reply->ctx_id,pending->expectedContext,
            unsigned(reply->ring_idx),unsigned(pending->expectedRing),pending->timeline?1u:0u);
        return 0;
    }
    ++g_usedIdxShadow;pending->state=GpuCommandStorageState::Completed;
    __atomic_store_n(&g_pendingCommand,static_cast<GpuCommandStorage*>(nullptr),__ATOMIC_RELEASE);
    return len;
}

static uint32_t SubmitCommand(uint32_t reqLen,uint32_t respCap,bool fenced=false) {
    if(__atomic_load_n(&g_controlQueuePoisoned,__ATOMIC_ACQUIRE))return 0;
    FrameTimingScope phase(FramePhase::QueueSetup);
    return FinishCommand(BeginCommand(g_legacyCommand,reqLen,respCap,fenced));
}
