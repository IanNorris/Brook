#pragma once
#include <stdint.h>

namespace brook {
inline constexpr uint32_t GpuControlHeaderBytes=24;
inline bool GpuControlCompletionValid(uint32_t expectedHead,uint32_t usedId,
                                     uint32_t bytes,uint32_t capacity,
                                     uint32_t flags,uint64_t fence,uint64_t expectedFence) {
    if (usedId!=expectedHead || bytes<GpuControlHeaderBytes || bytes>capacity) return false;
    return expectedFence ? (flags&1u) && fence==expectedFence : !(flags&1u);
}
enum class GpuRetirementStep { ProducerFence,CompositorFence,DetachContext,DetachBacking,Unref };
enum class GpuCaptureStep { ProducerFence,AttachSource,Copy,CompositorFence,DetachSource };
template<class Step>
bool GpuCaptureSequence(Step step) {
    return step(GpuCaptureStep::ProducerFence) && step(GpuCaptureStep::AttachSource) &&
           step(GpuCaptureStep::Copy) && step(GpuCaptureStep::CompositorFence) &&
           step(GpuCaptureStep::DetachSource);
}
enum class GpuOwnedTextureRetirementStep { UnbindViewAndFence,DetachTexture,UnrefTexture };
enum class GpuOwnedUploadStep { AttachBacking,Transfer,CompletionFence,DetachBackingFenced };
template<class Step> bool GpuOwnedUploadSequence(Step step) {
    return step(GpuOwnedUploadStep::AttachBacking) && step(GpuOwnedUploadStep::Transfer) &&
           step(GpuOwnedUploadStep::CompletionFence) && step(GpuOwnedUploadStep::DetachBackingFenced);
}
template<class Step>
bool GpuOwnedTextureRetirementSequence(Step step) {
    return step(GpuOwnedTextureRetirementStep::UnbindViewAndFence) &&
           step(GpuOwnedTextureRetirementStep::DetachTexture) &&
           step(GpuOwnedTextureRetirementStep::UnrefTexture);
}
template<class Step>
bool GpuRetirementSequence(bool compositorReady,Step step) {
    if (!step(GpuRetirementStep::ProducerFence)) return false;
    if (compositorReady && !step(GpuRetirementStep::CompositorFence)) return false;
    return step(GpuRetirementStep::DetachContext) &&
           step(GpuRetirementStep::DetachBacking) && step(GpuRetirementStep::Unref);
}
}
