// Actual owned snapshot upload body, shared with failure/lifetime tests.
static GpuCaptureResult CompUploadOwnedTexture(GpuTexId destination,uint64_t snapshot,uint32_t bytes)
{
    GpuCmdGuard guard;
    auto* target=CompTex(destination);
    if (!g_ownedCaptureReady || !g_compReady || !g_drawReady || g_compFrameAssembling ||
        g_controlQueuePoisoned || !target || !target->owned || target->quarantined ||
        target->uploadBackingAttached || !GpuCaptureFormatSupported(target->format) ||
        !target->w || !target->h || target->w>GPU_SCENE_MAX_DIMENSION || target->h>GPU_SCENE_MAX_DIMENSION ||
        !snapshot || (snapshot&4095u) || !bytes || uint64_t(target->w)*target->h*4!=bytes ||
        snapshot>~uint64_t{0}-bytes) return GpuCaptureResult::Rejected;
    bool submitted=false;
    const bool complete=GpuOwnedUploadSequence([&](GpuOwnedUploadStep step) {
        switch (step) {
        case GpuOwnedUploadStep::AttachBacking: {
            const bool attached=ResourceAttachBackingVirt(target->resId,snapshot,bytes,&submitted);
            // A submitted attach may have succeeded even when its reply failed.
            target->uploadBackingAttached=submitted;
            return attached;
        }
        case GpuOwnedUploadStep::Transfer:
            return TransferToHost3D(COMP_CTX_ID,target->resId,0,0,target->w,target->h,target->w,target->h);
        case GpuOwnedUploadStep::CompletionFence:return AppFenceLocked(COMP_CTX_ID);
        case GpuOwnedUploadStep::DetachBackingFenced:
            if (!ResourceControlLocked(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,0,target->resId,true)) return false;
            // A fenced response proves the detach itself completed. An ordinary
            // acknowledgement alone is not the backing-release boundary.
            target->uploadBackingAttached=false;return true;
        }
        return false;
    });
    if (!complete) {
        if (!submitted) return GpuCaptureResult::Rejected;
        target->quarantined=true;
        // The caller retains the snapshot lease until device reset. Never free
        // it merely because an attach/transfer/detach acknowledgement failed.
        return GpuCaptureResult::Uncertain;
    }
    return GpuCaptureResult::Complete;
}

