#include <cassert>
#include "intel_bdw_gtt_probe.h"
using namespace brook;
struct Fake {
    DellI2cCriticalEvidence e{};unsigned evidenceCalls=0,reads=0;bool denySecond=false;
    bool evidence(uint64_t,DellI2cCriticalEvidence& out){++evidenceCalls;out=e;return !(denySecond && evidenceCalls==2);}
    bool read(uint64_t address,uint64_t& out){assert(address==(reads?0xf680afc0ull:0xf6800000ull));out=++reads;return true;}
};
int main(){
    BdwGopPtePlan p{0,5624,0xf6800000,0xf680afc0};BdwGopPtes result{7,8};Fake f;
    assert(!BdwGopPtesCritical(p,f,result) && !f.reads && result.first==7 && result.last==8);
    f={};f.e.aliasesValid=f.e.identityPresent=f.e.identityMatches=f.e.directPresent=f.e.directMatches=true;
    f.e.currentDirectPresent=f.e.currentDirectMatches=f.e.cpuValid=true;
    f.e.identityPat=f.e.directPat=f.e.currentDirectPat=0;f.e.patSlot0=6;f.e.mtrrType=0;
    f.denySecond=true;assert(!BdwGopPtesCritical(p,f,result) && f.evidenceCalls==2 && !f.reads);
    f.denySecond=false;f.evidenceCalls=0;assert(BdwGopPtesCritical(p,f,result) && f.evidenceCalls==2 && f.reads==2 && result.first==1 && result.last==2);
    f.reads=0;f.evidenceCalls=0;p.lastIndex++;assert(!BdwGopPtesCritical(p,f,result) && !f.reads && !f.evidenceCalls);
}
