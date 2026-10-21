// Do not negotiate features or reuse queue/backing memory until the device
// acknowledges reset. A write of zero alone does not establish DMA quiescence.
// Caller serializes device access; runtime recovery additionally needs client,
// mapping and IRQ quiescence before invoking this primitive.
static bool ResetGpuDevice(GpuCompletionWaitLimits limits={}) {
    mmio_write8(g_commonCfg,VIRTIO_COMMON_STATUS,0);
    return GpuWaitForCompletion(ProcessCurrent()!=nullptr,
        []{return mmio_read8(g_commonCfg,VIRTIO_COMMON_STATUS)==0;},
        [](uint64_t* ns){return KvmClockReadStableNs(ns);},
        []{__asm__ volatile("pause" ::: "memory");},limits);
}
