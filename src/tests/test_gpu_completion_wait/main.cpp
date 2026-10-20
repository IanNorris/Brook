#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "../../drivers/virtio_gpu/completion_wait.h"

int main() {
    using brook::GpuCompletionWaitLimits;
    using brook::GpuWaitForCompletion;
    const GpuCompletionWaitLimits limits{1000,40,4};
    unsigned polls=0,clocks=0,pauses=0;
    auto clock=[&](uint64_t* ns){++clocks;*ns=pauses*10;return true;};
    auto pause=[&]{++pauses;};
    assert(GpuWaitForCompletion(true,[&]{return ++polls==2;},clock,pause,limits));
    assert(clocks==1 && pauses==1);

    clocks=pauses=0;
    assert(!GpuWaitForCompletion(false,[]{return false;},clock,pause,limits));
    assert(clocks==0 && pauses==40); // Early boot has no scheduler clock dependency.

    clocks=pauses=0;
    bool ownsCommand=true;
    assert(GpuWaitForCompletion(true,[&]{assert(ownsCommand);return pauses==16;},
        clock,[&]{assert(ownsCommand);++pauses;},limits));
    assert(pauses==16 && clocks==5);

    clocks=pauses=0;
    uint64_t now=0;
    assert(!GpuWaitForCompletion(true,[]{return false;},
        [&](uint64_t* ns){++clocks;*ns=now;return true;},
        [&]{++pauses;now+=100;},limits));
    assert(now>=1000 && pauses==12); // Stable elapsed deadline, no IRQ count.

    clocks=pauses=0;
    assert(!GpuWaitForCompletion(true,[]{return false;},
        [&](uint64_t*){++clocks;return false;},pause,limits));
    assert(clocks==1 && pauses==40);
    for (bool regression : {false,true}) {
        clocks=pauses=0;
        assert(!GpuWaitForCompletion(true,[]{return false;},
            [&](uint64_t* ns){*ns=clocks++ ? 99 : 100;return regression || clocks==1;},
            pause,limits));
        assert(clocks==2 && pauses==40);
    }
    // Completion at the boundary wins when observed before the timeout check.
    clocks=pauses=0;
    assert(GpuWaitForCompletion(true,[&]{return pauses==4;},
        [&](uint64_t* ns){*ns=clocks++ ? 1000 : 0;return true;},pause,
        GpuCompletionWaitLimits{1000,40,8}));
    assert(clocks==1);
    std::puts("GPU_COMPLETION_WAIT_PASS");
}
