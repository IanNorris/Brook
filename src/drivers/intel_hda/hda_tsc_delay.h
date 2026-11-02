#pragma once
#include <stdint.h>
namespace brook {
// Runtime-only bounded busy delay. The admitted clock must be serialized and
// sampled on the current CPU with interrupts disabled. A tenfold wait plus a 20 us
// cross-core allowance is deliberately conservative, but does not prove an
// absolute frequency bound. It does not require interrupts or timer ticks.
template<class Io> bool HdaTscDelay(Io& io,unsigned us,unsigned iterations=1000000){
    if(!us || us>10 || !iterations)return false;
    const uint64_t flags=io.disableInterrupts();
    struct Restore {Io& io;uint64_t flags;~Restore(){io.restoreInterrupts(flags);}} restore{io,flags};
    const uint32_t cpu=io.cpu();const uint64_t start=io.nowNs();if(!start)return false;
    const uint64_t required=uint64_t(us)*10000+20000;
    for(unsigned i=0;i<iterations;++i){
        const uint64_t now=io.nowNs();if(!now || now<start || io.cpu()!=cpu)return false;
        if(now-start>=required)return true;io.pause();
    }
    return false;
}
}
