#include "../../kernel/src/cache_snapshot_collect.h"
#include <cassert>
using namespace brook;
struct Fake {
    CpuCacheSnapshot slots[3]{};unsigned statusValues[3]{},polls=0,readyAt=2,arms=0,disarms=0;
    bool bspOk=true,armed=false,fail=false;
    bool captureBsp(){slots[0].valid=bspOk;slots[0].pat=7;return bspOk;}
    void arm(){armed=true;++arms;}void disarm(){armed=false;++disarms;}
    uint32_t status(uint32_t cpu){return statusValues[cpu];}
    void pause(){if(++polls==readyAt){slots[1].valid=true;slots[1].pat=11;statusValues[1]=fail?2:1;
        slots[2].valid=true;slots[2].pat=13;statusValues[2]=1;}}
    const CpuCacheSnapshot& snapshot(uint32_t cpu){return slots[cpu];}
};
int main(){
    CpuCacheSnapshot out[3]{};Fake ok;assert(CacheSnapshotCollect(ok,out,3,3,10));
    assert(ok.arms==1 && ok.disarms==1 && !ok.armed && out[0].pat==7 && out[1].pat==11 && out[2].pat==13);
    Fake timeout;timeout.readyAt=4;out[0].pat=41;out[1].pat=42;out[2].pat=43;
    assert(!CacheSnapshotCollect(timeout,out,3,3,2) && !timeout.armed && timeout.disarms==1);
    timeout.pause();timeout.pause();assert(timeout.statusValues[1]==1); // late publication does not touch output
    assert(out[0].pat==41 && out[1].pat==42 && out[2].pat==43);
    Fake failed;failed.fail=true;assert(!CacheSnapshotCollect(failed,out,3,3,10) && !failed.armed && failed.disarms==1);
    Fake bsp;bsp.bspOk=false;assert(!CacheSnapshotCollect(bsp,out,3,3,10) && !bsp.arms && !bsp.disarms);
    Fake bounds;assert(!CacheSnapshotCollect(bounds,out,3,2,10) && !bounds.arms);
}
