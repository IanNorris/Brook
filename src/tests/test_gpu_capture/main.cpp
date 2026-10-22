#include "../../drivers/virtio_gpu/retirement_protocol.h"
#include "../../kernel/src/gpu_compositor.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"GPU_CAPTURE line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)

int main() {
    using namespace brook;
    CHECK(GpuCaptureViewSupported(1,1) && GpuCaptureViewSupported(1,2));
    CHECK(GpuCaptureViewSupported(2,2) && !GpuCaptureViewSupported(2,1));
    CHECK(!GpuCaptureViewSupported(67,1) && !GpuCaptureViewSupported(1,3));
    const std::vector<GpuCaptureStep> required={GpuCaptureStep::ProducerFence,
        GpuCaptureStep::AttachSource,GpuCaptureStep::Copy,
        GpuCaptureStep::CompositorFence,GpuCaptureStep::DetachSource};
    for (unsigned fail=0;fail<=required.size();++fail) {
        std::vector<GpuCaptureStep> seen;
        bool attached=false,pendingRead=false,producerReady=false;
        unsigned source=0xff00ff00,destination=0;
        const bool complete=GpuCaptureSequence([&](GpuCaptureStep step) {
            CHECK(step==required[seen.size()]);seen.push_back(step);
            if (seen.size()-1==fail) return false;
            switch (step) {
            case GpuCaptureStep::ProducerFence:producerReady=true;break;
            case GpuCaptureStep::AttachSource:CHECK(producerReady);attached=true;break;
            case GpuCaptureStep::Copy:CHECK(attached);pendingRead=true;break;
            case GpuCaptureStep::CompositorFence:CHECK(pendingRead);destination=source;pendingRead=false;break;
            case GpuCaptureStep::DetachSource:CHECK(attached && !pendingRead);attached=false;break;
            }
            return true;
        });
        CHECK(complete==(fail==required.size()));
        CHECK(seen.size()==(fail<required.size()?fail+1:required.size()));
        if (complete) {
            CHECK(!attached && !pendingRead && destination==0xff00ff00);
            source=0xff0000ff;CHECK(destination==0xff00ff00);
        }
    }
    const std::vector<GpuOwnedTextureRetirementStep> destroy={
        GpuOwnedTextureRetirementStep::UnbindViewAndFence,
        GpuOwnedTextureRetirementStep::DetachTexture,
        GpuOwnedTextureRetirementStep::UnrefTexture};
    for (unsigned fail=0;fail<=destroy.size();++fail) {
        std::vector<GpuOwnedTextureRetirementStep> seen;
        const bool retired=GpuOwnedTextureRetirementSequence([&](auto step) {
            CHECK(step==destroy[seen.size()]);seen.push_back(step);
            return seen.size()-1!=fail;
        });
        CHECK(retired==(fail==destroy.size()));
        CHECK(seen.size()==(fail<destroy.size()?fail+1:destroy.size()));
    }
    const std::vector<GpuOwnedUploadStep> upload={GpuOwnedUploadStep::AttachBacking,
        GpuOwnedUploadStep::Transfer,GpuOwnedUploadStep::CompletionFence,
        GpuOwnedUploadStep::DetachBackingFenced};
    for (unsigned fail=0;fail<=upload.size();++fail) {
        unsigned seen=0;bool attached=false,pendingRead=false;
        const bool complete=GpuOwnedUploadSequence([&](auto step) {
            CHECK(step==upload[seen]);if (seen++==fail) return false;
            switch (step) {
            case GpuOwnedUploadStep::AttachBacking:attached=true;break;
            case GpuOwnedUploadStep::Transfer:CHECK(attached);pendingRead=true;break;
            case GpuOwnedUploadStep::CompletionFence:CHECK(attached && pendingRead);pendingRead=false;break;
            case GpuOwnedUploadStep::DetachBackingFenced:CHECK(attached && !pendingRead);attached=false;break;
            }
            return true;
        });
        CHECK(complete==(fail==upload.size()));
        CHECK(seen==(fail<upload.size()?fail+1:upload.size()));
        if (complete) CHECK(!attached && !pendingRead);
    }
    std::puts("GPU_CAPTURE_PASS copy/fence/detach ordering, retained pixels and all capture/teardown failure phases");
}
