// Actual resource retirement body; caller holds GpuCmdGuard.
static int32_t AppRetireResourceLocked(int32_t ctxId,int32_t gres)
{
    if (auto* blob=AppBlobFind(ctxId,static_cast<uint32_t>(gres))) return AppRetireBlobLocked(blob);
    if (g_controlQueuePoisoned || !AppCtxFind(ctxId) || !AppResValid(ctxId,static_cast<uint32_t>(gres))) return -1;
    AppUpload* upload = nullptr;
    for (auto& entry : g_appUploads) {
        if (!entry.used || entry.resource != static_cast<uint32_t>(gres)) continue;
        if (entry.ctxId != ctxId || entry.incarnation != AppCtxFind(ctxId)->incarnation ||
            entry.quarantined) return -1;
        upload = &entry;
        break;
    }
    auto command=[&](uint32_t type,uint32_t context) {
        auto* req=reinterpret_cast<VirtioGpuCtxResource*>(g_cmdBuf+CMD_REQ_OFF);
        memset(req,0,sizeof(*req));req->hdr.type=type;req->hdr.ctx_id=context;
        req->resource_id=static_cast<uint32_t>(gres);
        return CmdRespOk(SubmitCommand(sizeof(*req),CMD_RESP_CAP));
    };
    // Retiring callers exclude future users. Any failure leaves the guest
    // allocation quarantined rather than treating ambiguous DMA as complete.
    const bool retired=brook::GpuRetirementSequence(g_compReady,[&](brook::GpuRetirementStep step) {
        switch (step) {
        case brook::GpuRetirementStep::ProducerFence:return AppFenceLocked(static_cast<uint32_t>(ctxId));
        case brook::GpuRetirementStep::CompositorFence:return AppFenceLocked(COMP_CTX_ID);
        case brook::GpuRetirementStep::DetachContext:return command(VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,static_cast<uint32_t>(ctxId));
        case brook::GpuRetirementStep::DetachBacking:return command(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,0);
        case brook::GpuRetirementStep::Unref:return command(VIRTIO_GPU_CMD_RESOURCE_UNREF,0);
        }
        return false;
    });
    if (upload) {
        if (retired) {
            VmmFreePages(VirtualAddress(upload->vaddr), upload->pages);
            *upload = {};
        } else upload->quarantined = true;
    }
    return retired ? 0 : -1;
}

static int32_t AppRetireResource(int32_t ctxId,int32_t gres)
{
    GpuCmdGuard guard;
    return AppRetireResourceLocked(ctxId, gres);
}

