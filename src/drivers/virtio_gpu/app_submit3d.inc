// Production body shared with failure-injection tests.
static int32_t AppSubmit3D(int32_t ctxId, const uint32_t* dwords, uint32_t n)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c || c->config.capsetId == VIRTIO_GPU_CAPSET_VENUS || !dwords || n == 0) return -1;
    // Bound the stream to the request region (last page is the response buffer).
    const uint32_t maxDw = ((CMD_PAGES - 1) * 4096 - sizeof(VirtioGpuCmdSubmit)) / 4;
    if (!VirglStreamValid(dwords, n, maxDw)) return -1;
    bool shared=false;
    if(!AppPrepareSharedUseLocked(ctxId,shared))return -1;
    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
    uint32_t* dst = reinterpret_cast<uint32_t*>(g_cmdBuf + CMD_REQ_OFF + sizeof(VirtioGpuCmdSubmit));
    for (uint32_t start = 0; start < n;) {
        uint32_t end = VirglChunkEnd(dwords, n, start, maxDw);
        if (end == start) return -1;
        uint32_t count = end - start;
        memset(sub, 0, sizeof(*sub));
        sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
        sub->hdr.ctx_id = (uint32_t)ctxId;
        for (uint32_t i = 0; i < count; ++i) dst[i] = dwords[start + i];
        sub->size = count * 4;
        if (!CmdRespOk(SubmitCommand(sizeof(*sub) + count * 4, CMD_RESP_CAP))) {
            SerialPrintf("virtio_gpu: AppSubmit3D FAILED ctx=%d ndw=%u offset=%u firstcmd=0x%x\n",
                         ctxId, count, start, dwords[start]);
            // Earlier chunks may already be executing. Do not release shared
            // serialization without attempting their completion; failed work
            // remains unusable even if the completion fence itself succeeds.
            if(shared) { (void)AppFenceLocked(static_cast<uint32_t>(ctxId));AppSharedFailureLocked(ctxId); }
            return -1;
        }
        start = end;
    }
    if(shared && !AppFenceLocked(static_cast<uint32_t>(ctxId))) {
        AppSharedFailureLocked(ctxId);return -1;
    }
    // Mesa treats NOWAIT errors other than EBUSY as idle. For this initial
    // host-backed GL path, complete work synchronously instead of claiming an
    // untracked asynchronous blob is idle. Guest-backed GL stays unchanged.
    for (const auto& blob:g_appBlobs)
        if (blob.used && blob.context==ctxId)
            return AppFenceLocked(static_cast<uint32_t>(ctxId)) ? 0 : -1;
    return 0;
}
