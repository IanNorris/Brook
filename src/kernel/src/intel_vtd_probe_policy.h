#pragma once
#include <stdint.h>
#include "dell_i2c_id_policy.h"
namespace brook {
constexpr uint64_t DellGpuVtdPage=0xfed90000ull;
struct DellGpuVtdStatus { uint32_t version=0, globalStatus=0; };
// The caller must have proved the audited DMAR and PCI identity, and must
// hold the kernel page-table lock with migration/interrupts excluded. This
// checks aliases and cacheability before exactly two fixed-width reads.
template<class Io> bool DellGpuVtdCritical(Io& io,DellGpuVtdStatus& out) {
    DellI2cCriticalEvidence e{};
    if(!io.evidence(e) || !e.aliasesValid || !e.identityPresent || !e.identityMatches ||
       !e.directPresent || !e.directMatches || !e.currentDirectPresent || !e.currentDirectMatches ||
       !e.cpuValid || e.cacheDisabled || e.notWriteThrough || e.identityPat!=0 ||
       e.directPat!=0 || e.currentDirectPat!=0 || e.patSlot0!=6 || e.mtrrType!=0)return false;
    uint32_t ver=0,status=0;
    if(!io.readVersion(ver) || (ver&~0xffu) || (ver>>4)<1 || (ver>>4)>3)return false;
    if(!io.readStatus(status) || status==0xffffffffu)return false;
    out={ver,status};return true;
}
}
