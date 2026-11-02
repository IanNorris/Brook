#pragma once
#include "cpu_cache_snapshot.h"
namespace brook {
// Static mailbox storage and one-shot admission belong to the caller. AP status
// must publish each snapshot with release semantics and load with acquire.
template<class Io> bool CacheSnapshotCollect(Io& io,CpuCacheSnapshot* out,uint32_t count,uint32_t capacity,unsigned spinLimit){
    if(!out || !count || capacity<count || !spinLimit || !io.captureBsp())return false;
    io.arm();bool complete=true;
    for(uint32_t cpu=1;cpu<count;++cpu){
        unsigned spins=0;uint32_t state=io.status(cpu);
        while(!state && spins++<spinLimit){io.pause();state=io.status(cpu);}
        if(state!=1){complete=false;break;}
    }
    io.disarm();if(!complete)return false;
    for(uint32_t cpu=0;cpu<count;++cpu)out[cpu]=io.snapshot(cpu);
    return true;
}
}
