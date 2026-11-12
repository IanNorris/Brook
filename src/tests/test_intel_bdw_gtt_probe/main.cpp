#include <cassert>
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_display_probe.h"
using namespace brook;
struct Fake {
    DellI2cCriticalEvidence e{};unsigned evidenceCalls=0,reads=0;bool denySecond=false;
    bool evidence(uint64_t,DellI2cCriticalEvidence& out){++evidenceCalls;out=e;return !(denySecond && evidenceCalls==2);}
    bool read(uint64_t address,uint64_t& out){assert(address==(reads?0xf680afc0ull:0xf6800000ull));out=++reads;return true;}
};
struct ReservedFake {
    DellI2cCriticalEvidence e{};unsigned reads=0,evidenceCalls=0;bool unstable=false;
    bool evidence(uint64_t page,DellI2cCriticalEvidence& out){++evidenceCalls;assert(page==0xf6880000ull);out=e;return true;}
    bool read(uint64_t address,uint64_t& out){assert(address==0xf6880000ull+(reads%8)*8);out=(reads%8)+1;
        if(unstable && reads==11)++out;++reads;return true;}
};
struct DisplayFake {
    DellI2cCriticalEvidence e{};unsigned reads=0,evidenceCalls=0;bool unstable=false;
    bool evidence(uint64_t page,DellI2cCriticalEvidence& out){assert(page==0xf6070000ull+4096*evidenceCalls);++evidenceCalls;out=e;return true;}
    bool read32(uint64_t address,uint32_t& out){out=uint32_t(address);if(unstable && reads==35)++out;++reads;return true;}
};
struct SurveyFake {
    DellI2cCriticalEvidence e{};unsigned reads=0,evidenceCalls=0;bool unstable=false;
    bool evidence(uint64_t page,DellI2cCriticalEvidence& out){++evidenceCalls;assert(page==0xf6880000ull);out=e;return true;}
    bool read(uint64_t address,uint64_t& out){unsigned i=reads%64;assert(address==0xf6880000ull+i*8);
        out=i%3==0?0:i%3==1?0x100001:0x100000;if(unstable && reads==95)out^=1;++reads;return true;}
};
static void TestGopBacking(const DellI2cCriticalEvidence& evidence) {
    constexpr uint64_t stolenBase=0xdc000000;
    constexpr uint64_t stolenBytes=32ull<<20;
    const BdwStolenRange stolen{true,stolenBase,stolenBytes,stolenBase+stolenBytes};
    const BdwGopPtePlan plan{0,BdwDellGopPages-1,BdwDellGgttBase,BdwDellGgttBase+uint64_t(BdwDellGopPages-1)*8};
    struct Io {
        DellI2cCriticalEvidence e;
        unsigned evidenceCalls=0,reads=0,badIndex=~0u;
        bool evidence(uint64_t address,DellI2cCriticalEvidence& out){
            assert(!(address&4095));++evidenceCalls;out=e;return true;
        }
        bool read(uint64_t address,uint64_t& out){
            const uint64_t index=(address-BdwDellGgttBase)/8;
            ++reads;out=(0xdc000000ull+index*4096)|1;
            if(index==badIndex)out^=4096;
            return true;
        }
    } io{evidence};
    unsigned calls=0,changed=0;
    auto page=[&](uint32_t first,uint32_t count,uint64_t& physical){
        ++calls;
        if(!BdwGopBackingPageCritical(first,count,io,physical))return false;
        if(calls==changed)physical+=4096;
        return true;
    };
    BdwGopBacking result{};
    assert(BdwGopBackingSnapshot(plan,stolen,page,result));
    assert(result.physicalBase==stolenBase && result.bytes==uint64_t(BdwDellGopPages)*4096);
    assert(io.reads==2*BdwDellGopPages && io.evidenceCalls==22 && calls==22);
    BdwGopBacking unchanged{17,19};
    calls=0;changed=12;
    assert(!BdwGopBackingSnapshot(plan,stolen,page,unchanged) && calls==changed);
    assert(unchanged.physicalBase==17 && unchanged.bytes==19);
    calls=0;changed=0;io.badIndex=1029;
    assert(!BdwGopBackingSnapshot(plan,stolen,page,unchanged) && calls==3);
    calls=0;io.badIndex=~0u;
    const BdwStolenRange unrelated{true,stolenBase+4096,stolenBytes,stolenBase+stolenBytes+4096};
    assert(!BdwGopBackingSnapshot(plan,unrelated,page,unchanged) && calls==1);
    uint64_t physical=7;
    assert(!BdwGopBackingPageCritical(1,512,io,physical) && physical==7);
    assert(!BdwGopBackingPageCritical(5120,506,io,physical) && physical==7);
}
int main(){
    BdwGopPtePlan p{0,5624,0xf6800000,0xf680afc0};BdwGopPtes result{7,8};Fake f;
    assert(!BdwGopPtesCritical(p,f,result) && !f.reads && result.first==7 && result.last==8);
    f={};f.e.aliasesValid=f.e.identityPresent=f.e.identityMatches=f.e.directPresent=f.e.directMatches=true;
    f.e.currentDirectPresent=f.e.currentDirectMatches=f.e.cpuValid=true;
    f.e.identityPat=f.e.directPat=f.e.currentDirectPat=0;f.e.patSlot0=6;f.e.mtrrType=0;
    TestGopBacking(f.e);
    f.denySecond=true;assert(!BdwGopPtesCritical(p,f,result) && f.evidenceCalls==2 && !f.reads);
    f.denySecond=false;f.evidenceCalls=0;assert(BdwGopPtesCritical(p,f,result) && f.evidenceCalls==2 && f.reads==2 && result.first==1 && result.last==2);
    f.reads=0;f.evidenceCalls=0;p.lastIndex++;assert(!BdwGopPtesCritical(p,f,result) && !f.reads && !f.evidenceCalls);
    ReservedFake q{};BdwGgttReservedPtes entries{};entries.values[0]=99;
    assert(!BdwReservedGgttPtesCritical(65536,q,entries) && !q.reads && entries.values[0]==99);
    q.e=f.e;q.unstable=true;
    assert(!BdwReservedGgttPtesCritical(65536,q,entries) && entries.values[0]==99);
    q.reads=q.evidenceCalls=0;q.unstable=false;
    assert(BdwReservedGgttPtesCritical(65536,q,entries) && q.reads==16 && q.evidenceCalls==1 && entries.values[7]==8);
    q.reads=q.evidenceCalls=0;
    assert(!BdwReservedGgttPtesCritical(65535,q,entries) && !q.reads && !q.evidenceCalls);
    assert(!BdwReservedGgttPtesCritical(66041,q,entries) && !q.reads && !q.evidenceCalls);
    SurveyFake survey{};BdwGgttSurvey counts{42,1,2,3};
    assert(!BdwGgttSurveyCritical(65536,survey,counts) && !survey.reads && counts.first==42);
    survey.e=f.e;survey.unstable=true;
    assert(!BdwGgttSurveyCritical(65536,survey,counts) && counts.first==42);
    survey.reads=survey.evidenceCalls=0;survey.unstable=false;
    assert(BdwGgttSurveyCritical(65536,survey,counts) && survey.reads==128 && survey.evidenceCalls==1);
    assert(counts.first==65536 && counts.zero==22 && counts.valid==21 && counts.other==21);
    survey.reads=survey.evidenceCalls=0;
    assert(!BdwGgttSurveyCritical(65537,survey,counts) && !survey.reads && !survey.evidenceCalls);
    assert(!BdwGgttSurveyCritical(1048576,survey,counts) && !survey.reads && !survey.evidenceCalls);
    DisplayFake display{};BdwDisplaySnapshot observed{};observed.regs[0][0]=7;
    assert(!BdwDisplayCriticalRead(display,observed) && !display.reads && observed.regs[0][0]==7);
    display={};display.e=f.e;display.unstable=true;
    assert(!BdwDisplayCriticalRead(display,observed) && observed.regs[0][0]==7);
    display={};display.e=f.e;
    assert(BdwDisplayCriticalRead(display,observed) && display.evidenceCalls==3 && display.reads==66);
    assert(observed.regs[0][0]==0xf6070180u && observed.regs[2][9]==0xf60720acu);
    assert(observed.regs[0][BdwDisplaySnapshot::PrimaryOffset]==0xf60701a4u);
    observed={};observed.regs[0][0]=0x98000000u;observed.regs[0][1]=12800;
    assert(BdwDisplayUsesGop(observed));
    const unsigned guardedRegisters[]={2,3,BdwDisplaySnapshot::PrimaryOffset};
    for(unsigned i:guardedRegisters) {
        observed.regs[0][i]=1;
        assert(!BdwDisplayUsesGop(observed));
        observed.regs[0][i]=0;
    }
}
