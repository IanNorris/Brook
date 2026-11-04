#pragma once
#include "intel_bdw_scanout.h"
#include "dell_i2c_id_policy.h"
namespace brook {
struct BdwGopPtes { uint64_t first=0,last=0; };
inline bool BdwGttEvidenceValid(const DellI2cCriticalEvidence& e) {
    return e.aliasesValid && e.identityPresent && e.identityMatches && e.directPresent && e.directMatches &&
        e.currentDirectPresent && e.currentDirectMatches && e.cpuValid && !e.cacheDisabled && !e.notWriteThrough &&
        e.identityPat==0 && e.directPat==0 && e.currentDirectPat==0 && e.patSlot0==6 && e.mtrrType==0;
}
// Caller proves exact Dell/GOP identity and GPU D0; Io holds the page-table
// lock with interrupts disabled until the two fixed-width reads are complete.
template<class Io> bool BdwGopPtesCritical(const BdwGopPtePlan& p,Io& io,BdwGopPtes& out) {
    constexpr uint64_t base=0xf6800000ull,entries=(8ull<<20)/8;
    if(p.firstIndex!=0 || p.lastIndex!=5624 || p.lastIndex>=entries ||
       p.firstAddress!=base+p.firstIndex*8 || p.lastAddress!=base+p.lastIndex*8)return false;
    DellI2cCriticalEvidence a{},b{};
    if(!io.evidence(p.firstAddress&~4095ull,a) || !BdwGttEvidenceValid(a))return false;
    if((p.firstAddress&~4095ull)!=(p.lastAddress&~4095ull) &&
       (!io.evidence(p.lastAddress&~4095ull,b) || !BdwGttEvidenceValid(b)))return false;
    uint64_t first=0,last=0;
    if(!io.read(p.firstAddress,first))return false;
    if(p.lastAddress==p.firstAddress)last=first;
    else if(!io.read(p.lastAddress,last))return false;
    out={first,last};return true;
}
}
