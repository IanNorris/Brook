#pragma once
#include <stdint.h>
#include "dell_i2c_id_policy.h"
namespace brook {
constexpr uint64_t DellGpuVtdPage=0xfed90000ull;
struct DellGpuVtdStatus {
    uint32_t version=0, globalStatus=0;
    uint64_t capability=0;
    uint32_t protectedMemory=0;
    bool protectedMemorySampled=false;
};
// The caller must have proved the audited DMAR and PCI identity, and must
// hold the kernel page-table lock with migration/interrupts excluded. This
// checks aliases and cacheability before fixed-width read-only register access.
// This snapshot does not grant DMA access or establish ownership of the unit.
template<class Io> bool DellGpuVtdCritical(Io& io,DellGpuVtdStatus& out) {
    DellI2cCriticalEvidence e{};
    if(!io.evidence(e) || !e.aliasesValid || !e.identityPresent || !e.identityMatches ||
       !e.directPresent || !e.directMatches || !e.currentDirectPresent || !e.currentDirectMatches ||
       !e.cpuValid || e.cacheDisabled || e.notWriteThrough || e.identityPat!=0 ||
       e.directPat!=0 || e.currentDirectPat!=0 || e.patSlot0!=6 || e.mtrrType!=0)return false;
    uint32_t ver=0,status=0;
    if(!io.readVersion(ver) || (ver&~0xffu) || (ver>>4)<1 || (ver>>4)>3)return false;
    if(!io.readStatus(status) || status==0xffffffffu)return false;
    uint64_t cap=0;
    if(!io.readCapability(cap) || cap==~uint64_t(0))return false;
    uint32_t pmen=0;const bool supported=(cap&((1ull<<5)|(1ull<<6)))!=0;
    if(supported && (!io.readProtectedMemory(pmen) || pmen==0xffffffffu))return false;
    out={ver,status,cap,pmen,supported};return true;
}
}
