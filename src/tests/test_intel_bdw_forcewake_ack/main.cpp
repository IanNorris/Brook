#include <cassert>
#include "intel_bdw_forcewake_ack.h"
using namespace brook;
struct Fake { DellI2cCriticalEvidence e{};unsigned reads=0;uint32_t value=0;
    bool evidence(DellI2cCriticalEvidence& out){out=e;return true;}
    bool read(uint32_t& out){++reads;out=value;return true;}
};
int main(){Fake f;uint32_t out=42;assert(!BdwForcewakeAckCritical(f,out) && !f.reads && out==42);
    f.e.aliasesValid=f.e.identityPresent=f.e.identityMatches=f.e.directPresent=f.e.directMatches=true;
    f.e.currentDirectPresent=f.e.currentDirectMatches=f.e.cpuValid=true;
    f.e.identityPat=f.e.directPat=f.e.currentDirectPat=0;f.e.patSlot0=6;f.e.mtrrType=0;
    f.value=0xffffffff;assert(!BdwForcewakeAckCritical(f,out) && f.reads==1 && out==42);
    f.value=0;assert(BdwForcewakeAckCritical(f,out) && f.reads==2 && out==0);
    f.value=1;assert(BdwForcewakeAckCritical(f,out) && f.reads==3 && out==1);
}
