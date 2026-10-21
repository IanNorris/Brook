// Actual context activation body, shared with incarnation/reuse tests.
static int32_t AppCtxCreate(uint32_t pid, const brook::GpuContextConfig& config = {})
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    const auto caps = AppCapabilities();
    if (!g_gpu3dWorks || g_controlQueuePoisoned || config.capsetId >= 64 || config.numRings > 64 ||
        config.pollRingsMask || !(caps.capsetMask & (uint64_t{1} << config.capsetId))) return -1;
    for (uint32_t i = 0; i < MAX_APP_CTX; ++i)
    {
        if (g_appCtx[i].used) continue;
        if (g_appContextIncarnation == ~0ULL) return -1;
        uint32_t ctxId = APP_FIRST_CTX + i;
        if (!CtxCreate(ctxId, config.capsetId))
        { SerialPuts("virtio_gpu: app ctx create failed\n"); return -1; }
        g_appCtx[i] = { true, ctxId, pid, config, ++g_appContextIncarnation };
        g_appResourceIds.BeginContext(i);
        SerialPrintf("virtio_gpu: app ctx %u capset %u rings %u for pid %u\n",
                     ctxId, config.capsetId, config.numRings, pid);
        return static_cast<int32_t>(ctxId);
    }
    SerialPuts("virtio_gpu: app ctx table full\n");
    return -1;
}

