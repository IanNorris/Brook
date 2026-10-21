// Entry is serialized with every normal GPU operation. A successful reap only
// releases transport ownership; the following fence is required before touching
// vertex backing. Abandon all old frame assembly and start from BeginFrame.
static bool TryRecoverLateVertexTransfer() {
    if(!ReapLateVertexTransfer())return false;
    g_drawQuadCount=0;g_blurBarrierCount=0;g_composeN=0;
    g_compFrameAssembling=false;
    if(!AppFenceLocked(COMP_CTX_ID)) {
        // A valid error response still cannot authorize new frames. A fence
        // timeout already quarantines its own distinct command generation.
        if(!g_controlQueuePoisoned)PoisonControlQueue(GpuControlFault::RecoveryFailed);
        return false;
    }
    g_recoveryFreshFramePending=true;
    SerialPuts("virtio_gpu: late vertex transfer fenced; fresh frame required\n");
    return true;
}
