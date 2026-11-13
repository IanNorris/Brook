#pragma once
#include "intel_bdw_scanout.h"
#include "intel_bdw_inventory.h"
#include "dell_i2c_id_policy.h"
namespace brook {
struct BdwGopPtes { uint64_t first=0,last=0; };
struct BdwGgttReservedPtes { uint64_t values[8]{}; };
struct BdwRcsGgttPtes { uint64_t values[29]{}; };
struct BdwGgttSurvey {uint32_t first=0,zero=0,valid=0,other=0;};
constexpr uint64_t BdwDellGgttBase=0xf6800000ull;
constexpr uint32_t BdwDellGopPages=5625;
struct BdwGopBacking {uint64_t physicalBase=0,bytes=0;};
inline bool BdwGttEvidenceValid(const DellI2cCriticalEvidence& e) {
    return e.aliasesValid && e.identityPresent && e.identityMatches && e.directPresent && e.directMatches &&
        e.currentDirectPresent && e.currentDirectMatches && e.cpuValid && !e.cacheDisabled && !e.notWriteThrough &&
        e.identityPat==0 && e.directPat==0 && e.currentDirectPat==0 && e.patSlot0==6 && e.mtrrType==0;
}
// Bounded, read-only observation of one aligned 64-entry slice in a single
// PTE page. Counts are only point-in-time evidence; they grant no reservation
// or permission to change the entries. Output stays unchanged on failure.
template<class Io> bool BdwGgttSurveyCritical(uint32_t first,Io& io,BdwGgttSurvey& out){
    if(first<65536 || first>1048576-64 || (first&63))return false;
    const uint64_t address=0xf6800000ull+uint64_t(first)*8;
    DellI2cCriticalEvidence evidence{};
    if(!io.evidence(address&~4095ull,evidence) || !BdwGttEvidenceValid(evidence))return false;
    uint64_t values[64]{};BdwGgttSurvey sample{};sample.first=first;
    for(unsigned i=0;i<64;++i)if(!io.read(address+uint64_t(i)*8,values[i]) || values[i]==~uint64_t{0})return false;
    for(unsigned i=0;i<64;++i){uint64_t again=0;
        if(!io.read(address+uint64_t(i)*8,again) || again!=values[i])return false;
        if(!again)++sample.zero;else if(again&1)++sample.valid;else ++sample.other;
    }
    out=sample;return true;
}
// Caller proves exact Dell/GOP identity and GPU D0; Io holds the page-table
// lock with interrupts disabled until the two fixed-width reads are complete.
template<class Io> bool BdwGopPtesCritical(const BdwGopPtePlan& p,Io& io,BdwGopPtes& out) {
    if(p.firstIndex!=0 || p.lastIndex!=BdwDellGopPages-1 ||
       p.firstAddress!=BdwDellGgttBase || p.lastAddress!=BdwDellGgttBase+p.lastIndex*8)return false;
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
// Validate one page of the existing GOP translation under the page-table
// lock. The narrow Dell path accepts only valid, contiguous stolen backing.
template<class Io> bool BdwGopBackingPageCritical(uint32_t first,uint32_t count,Io& io,uint64_t& out) {
    constexpr uint32_t entriesPerPage=4096/8;
    if(first>=BdwDellGopPages || first%entriesPerPage || !count || count>entriesPerPage ||
       count>BdwDellGopPages-first)return false;
    const uint64_t address=BdwDellGgttBase+uint64_t(first)*8;
    DellI2cCriticalEvidence evidence{};
    if(!io.evidence(address,evidence) || !BdwGttEvidenceValid(evidence))return false;
    uint64_t basePte=0;
    if(!io.read(address,basePte) || (basePte&4095)!=1 ||
       basePte>=(uint64_t{1}<<32)-uint64_t(count-1)*4096)return false;
    for(uint32_t i=1;i<count;++i) {
        uint64_t pte=0;
        if(!io.read(address+uint64_t(i)*8,pte) || pte!=basePte+uint64_t(i)*4096)return false;
    }
    out=basePte&~uint64_t{4095};
    return true;
}

// Acquire a twice-read span without holding an IRQ spinlock over the entire
// GGTT. The caller must still bind it to live display and memory ownership.
template<class ReadPage> bool BdwGopBackingSnapshot(const BdwGopPtePlan& plan,const BdwStolenRange& stolen,
                                                    ReadPage readPage,BdwGopBacking& out) {
    constexpr uint32_t entriesPerPage=4096/8;
    constexpr uint64_t bytes=uint64_t(BdwDellGopPages)*4096;
    if(!stolen.valid || stolen.base>=stolen.end || stolen.end-stolen.base<bytes ||
       plan.firstIndex || plan.lastIndex!=BdwDellGopPages-1 || plan.firstAddress!=BdwDellGgttBase ||
       plan.lastAddress!=BdwDellGgttBase+plan.lastIndex*8)return false;
    uint64_t base=0;
    for(unsigned pass=0;pass<2;++pass)for(uint32_t i=0;i<BdwDellGopPages;i+=entriesPerPage) {
        const auto remaining=BdwDellGopPages-i;
        const auto count=remaining<entriesPerPage?remaining:entriesPerPage;
        uint64_t physical=0;
        if(!readPage(i,count,physical))return false;
        if(!pass && !i) {
            base=physical;
            if(base<stolen.base || base>stolen.end-bytes)return false;
        }
        if(physical!=base+uint64_t(i)*4096)return false;
    }
    out={base,bytes};
    return true;
}
// A stable snapshot of one eight-entry candidate. This does not reserve the
// entries from firmware or permit writes; the caller owns a software lease.
template<class Io> bool BdwReservedGgttPtesCritical(uint32_t first,Io& io,BdwGgttReservedPtes& out) {
    constexpr uint32_t entries=1u<<20;
    if(first<65536 || first>entries-8 || (first&511)>504)return false;
    const uint64_t address=0xf6800000ull+uint64_t(first)*8;
    DellI2cCriticalEvidence evidence{};
    if(!io.evidence(address&~4095ull,evidence) || !BdwGttEvidenceValid(evidence))return false;
    BdwGgttReservedPtes result{};
    for(unsigned i=0;i<8;++i)if(!io.read(address+i*8,result.values[i]) || result.values[i]==~uint64_t{0})return false;
    for(unsigned i=0;i<8;++i){uint64_t again=0;if(!io.read(address+i*8,again) || again!=result.values[i])return false;}
    out=result;return true;
}
}
