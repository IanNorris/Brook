#include "../../drivers/virtio_gpu/retirement_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"GPU_CAPTURE line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)

int main() {
    using namespace brook;
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
    std::puts("GPU_CAPTURE_PASS copy/fence/detach ordering, retained pixels and all capture/teardown failure phases");
}
