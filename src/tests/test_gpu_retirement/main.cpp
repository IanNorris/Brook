#include "../../drivers/virtio_gpu/retirement_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"GPU_RETIREMENT line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
int main() {
    using namespace brook;
    CHECK(GpuControlCompletionValid(7,7,24,4096,0,0,0));
    CHECK(GpuControlCompletionValid(7,7,24,4096,1,42,42));
    CHECK(!GpuControlCompletionValid(7,8,24,4096,1,42,42));
    CHECK(!GpuControlCompletionValid(7,7,23,4096,1,42,42));
    CHECK(!GpuControlCompletionValid(7,7,4097,4096,1,42,42));
    CHECK(!GpuControlCompletionValid(7,7,24,4096,0,42,42));
    CHECK(!GpuControlCompletionValid(7,7,24,4096,1,41,42));
    CHECK(!GpuControlCompletionValid(7,7,24,4096,1,42,0));
    const std::vector<GpuRetirementStep> required={GpuRetirementStep::ProducerFence,
        GpuRetirementStep::CompositorFence,GpuRetirementStep::DetachContext,
        GpuRetirementStep::DetachBacking,GpuRetirementStep::Unref};
    for (unsigned fail=0;fail<=required.size();++fail) {
        std::vector<GpuRetirementStep> seen;
        const bool retired=GpuRetirementSequence(true,[&](auto step) {
            CHECK(step==required[seen.size()]);seen.push_back(step);
            return seen.size()-1!=fail;
        });
        CHECK(retired==(fail==required.size()));
        CHECK(seen.size()==(fail<required.size()?fail+1:required.size()));
    }
    std::vector<GpuRetirementStep> noCompositor;
    CHECK(GpuRetirementSequence(false,[&](auto step){noCompositor.push_back(step);return true;}));
    CHECK(noCompositor.size()==4 && noCompositor[1]==GpuRetirementStep::DetachContext);
    std::puts("GPU_RETIREMENT_PASS completion identity/bounds/fences and every retirement failure phase");
}
