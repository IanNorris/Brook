// Production body shared with failure-injection tests.
static int32_t AppTransfer3D(int32_t ctxId, int32_t gres, int dir,
                             const brook::GpuTransfer3D& transfer)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c || !AppResValid(ctxId, (uint32_t)gres) || dir < 0 || dir > 1 ||
        !transfer.w || !transfer.h || !transfer.d) return -1;
    bool shared=false;if(!AppPrepareSharedUseLocked(ctxId,shared))return -1;
    auto* req = reinterpret_cast<VirtioGpuTransferHost3D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type = dir == 0 ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D
                             : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    req->hdr.ctx_id = (uint32_t)ctxId;
    req->resource_id = (uint32_t)gres;
    req->box = {transfer.x,transfer.y,transfer.z,transfer.w,transfer.h,transfer.d};
    req->offset = transfer.offset;
    req->level = transfer.level;
    req->stride = transfer.stride;
    req->layer_stride = transfer.layerStride;
    bool ok = CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
    if(shared) {
        const bool completed=AppFenceLocked(static_cast<uint32_t>(ctxId));
        if(!ok || !completed) {AppSharedFailureLocked(ctxId);ok=false;}
    }
    if (!ok)
        SerialPrintf("virtio_gpu: AppTransfer3D FAILED ctx=%d gres=%u dir=%d box=%u,%u,%u+%u,%u,%u level=%u offset=%lu stride=%u layer=%u\n",
                     ctxId, (uint32_t)gres, dir, transfer.x,transfer.y,transfer.z,
                     transfer.w,transfer.h,transfer.d,transfer.level,transfer.offset,
                     transfer.stride,transfer.layerStride);
    return ok ? 0 : -1;
}
