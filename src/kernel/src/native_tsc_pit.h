#pragma once
#include <stdint.h>

namespace brook {
// Early-boot PIT channel-2 acquisition only. The caller must own channel 2,
// guarantee no concurrent speaker/timer user and order TSC reads. This restores
// port 0x61 on every exit, but intentionally does not restore the previous PIT
// channel-2 programming. It must not run on a live system with other owners.
// Five samples are published only after every bounded one-shot succeeds.
template<class Io>
bool NativeTscMeasurePit(Io& io,uint32_t pitTicks,uint32_t pollLimit,uint64_t (&ticks)[5]) {
    if(!pitTicks || pitTicks>65535 || !pollLimit || pollLimit>100000000)return false;
    const uint8_t saved=io.readGate();uint64_t measured[5]{};bool ok=true;
    io.writeGate(uint8_t(saved&~3u));
    for(unsigned i=0;i<5 && ok;++i){
        io.writeControl(0xb0);io.writeCounter(uint8_t(pitTicks));io.writeCounter(uint8_t(pitTicks>>8));
        bool low=false;
        for(uint32_t j=0;j<pollLimit;++j){if(!(io.readGate()&0x20)){low=true;break;}io.pause();}
        if(!low){ok=false;break;}
        uint64_t start=0,end=0;if(!io.tsc(start)){ok=false;break;}
        io.writeGate(uint8_t((saved&~2u)|1u)); // speaker disabled; gate high
        bool high=false;
        for(uint32_t j=0;j<pollLimit;++j){if(io.readGate()&0x20){high=true;break;}io.pause();}
        if(!high || !io.tsc(end) || end<=start){ok=false;break;}
        measured[i]=end-start;io.writeGate(uint8_t(saved&~3u));
    }
    io.writeGate(saved);
    if(!ok)return false;for(unsigned i=0;i<5;++i)ticks[i]=measured[i];return true;
}
}
