#include <cassert>
#include "intel_vtd_probe_policy.h"
using namespace brook;
struct Fake {
    DellI2cCriticalEvidence e{};uint32_t ver=0x10, status=0;unsigned reads=0;
    bool evidence(DellI2cCriticalEvidence& out){out=e;return true;}
    bool readVersion(uint32_t& v){assert(reads++==0);v=ver;return true;}
    bool readStatus(uint32_t& v){assert(reads++==1);v=status;return true;}
};
int main(){
    Fake f;DellGpuVtdStatus out{77,88};
    assert(!DellGpuVtdCritical(f,out) && !f.reads && out.version==77 && out.globalStatus==88);
    f.e.aliasesValid=f.e.identityPresent=f.e.identityMatches=f.e.directPresent=f.e.directMatches=true;
    f.e.currentDirectPresent=f.e.currentDirectMatches=f.e.cpuValid=true;
    f.e.identityPat=f.e.directPat=f.e.currentDirectPat=0;f.e.patSlot0=6;f.e.mtrrType=0;
    assert(DellGpuVtdCritical(f,out) && f.reads==2 && out.version==0x10 && !out.globalStatus);
    f.reads=0;f.status=0x80000000;assert(DellGpuVtdCritical(f,out) && f.reads==2 && out.globalStatus==0x80000000);
    f.reads=0;f.ver=0xffffffff;assert(!DellGpuVtdCritical(f,out) && f.reads==1 && out.globalStatus==0x80000000);
    f.reads=0;f.ver=0x10;f.status=0xffffffff;assert(!DellGpuVtdCritical(f,out) && f.reads==2 && out.globalStatus==0x80000000);
    f.reads=0;f.e.mtrrType=6;assert(!DellGpuVtdCritical(f,out) && !f.reads);
}
