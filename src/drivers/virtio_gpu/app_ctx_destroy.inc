// Actual context teardown body, shared with ownership fault-injection tests.
static void AppCtxDestroy(int32_t ctxId)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c) return;
    for (auto& upload : g_appUploads) {
        if (!upload.used || upload.ctxId != ctxId) continue;
        if (upload.incarnation != c->incarnation || upload.quarantined ||
            AppRetireResourceLocked(ctxId, static_cast<int32_t>(upload.resource)) != 0) {
            SerialPrintf("virtio_gpu: app ctx %d upload retirement unverified; retained\n", ctxId);
            return;
        }
    }
    auto* req = reinterpret_cast<VirtioGpuCtrlHdr*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->type   = VIRTIO_GPU_CMD_CTX_DESTROY;
    req->ctx_id = (uint32_t)ctxId;
    if (!CmdRespOk(SubmitCommand(sizeof(*req),CMD_RESP_CAP))) {
        SerialPrintf("virtio_gpu: app ctx %d teardown unverified; retained\n",ctxId);
        return;
    }
    c->used = false;
    SerialPrintf("virtio_gpu: app virgl ctx %d destroyed\n", ctxId);
}

