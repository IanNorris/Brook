// Actual legacy upload body, shared with ownership fault-injection tests.
static int32_t AppBufferUpload(int32_t ctxId, const void* src, uint32_t bytes)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c || !src || bytes == 0 || bytes > 16u * 1024 * 1024) return -1;
    uint32_t slot = MAX_APP_UPLOADS;
    for (uint32_t i = 0; i < MAX_APP_UPLOADS; ++i)
        if (!g_appUploads[i].used) { slot = i; break; }
    if (slot == MAX_APP_UPLOADS) return -1;

    uint32_t ci = (uint32_t)ctxId - APP_FIRST_CTX;
    uint32_t gres = g_appResourceIds.Allocate(ci);
    if (!gres) return -1;

    uint64_t pages = (bytes + 4095) / 4096;
    VirtualAddress va = VmmAllocPages(pages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!va) return -1;
    memcpy(reinterpret_cast<void*>(va.raw()), src, bytes);
    auto& upload = g_appUploads[slot];
    upload = {true, false, false, ctxId, c->incarnation, gres, va.raw(), pages};
    bool cr = ResourceCreateBuffer(gres, VIRGL_BIND_VERTEX_BUFFER, bytes);
    bool ab = false;
    if (cr) {
        upload.published = true; // before submit: a failure may be ambiguous
        ab = ResourceAttachBackingVirt(gres, va.raw(), bytes);
    }
    bool ca = ab && CtxAttachResource((uint32_t)ctxId, gres);
    bool tr = ca && TransferToHostBuffer((uint32_t)ctxId, gres, bytes);
    if (!tr)
    {
        SerialPrintf("virtio_gpu: AppBufferUpload FAILED ctx=%d gres=%u bytes=%u create=%d attach=%d ctxattach=%d xfer=%d\n",
                     ctxId, gres, bytes, cr, ab, ca, tr);
        if (upload.published) upload.quarantined = true;
        else { VmmFreePages(va, pages); upload = {}; }
        return -1;
    }
    return static_cast<int32_t>(gres);
}

