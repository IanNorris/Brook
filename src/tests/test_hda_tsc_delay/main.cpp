#include "../../drivers/intel_hda/hda_tsc_delay.h"
#include <cassert>
using namespace brook;
struct Fake {uint64_t time=1,step=1000;uint32_t index=0;unsigned reads=0,migrateAt=0,failAt=0,backwardsAt=0;bool interrupts=true;
    uint64_t nowNs(){++reads;if(reads==failAt)return 0;if(reads==backwardsAt)return time-step-1;time+=step;return time;}
    uint32_t cpu(){return migrateAt && reads>=migrateAt?index+1:index;}
    uint64_t disableInterrupts(){const auto flags=interrupts?0x200u:0u;interrupts=false;return flags;}
    void restoreInterrupts(uint64_t flags){interrupts=(flags&0x200)!=0;}
    void pause(){} };
int main(){
    Fake ok;const auto start=ok.time;assert(HdaTscDelay(ok,3,100) && ok.time-start>=50000);
    Fake stalled;stalled.step=0;assert(!HdaTscDelay(stalled,3,20));
    assert(stalled.interrupts);Fake off;off.interrupts=false;assert(HdaTscDelay(off,3,100) && !off.interrupts);
    Fake below;assert(!HdaTscDelay(below,3,49));Fake exact;assert(HdaTscDelay(exact,3,50));
    Fake migrating;migrating.migrateAt=3;assert(!HdaTscDelay(migrating,3,100));
    Fake failed;failed.failAt=3;assert(!HdaTscDelay(failed,3,100));
    Fake backward;backward.time=100000;backward.backwardsAt=2;assert(!HdaTscDelay(backward,3,100) && backward.interrupts);
    Fake bounds;assert(!HdaTscDelay(bounds,0) && !HdaTscDelay(bounds,11) && !HdaTscDelay(bounds,3,0) && bounds.interrupts);
}
