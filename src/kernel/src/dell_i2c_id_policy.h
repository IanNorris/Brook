#pragma once
#include <stdint.h>
namespace brook {
// Reviewed DSDT/NVS and this machine's observed I2C0 controller window.
// The PCI extent audit remains incomplete; this is an explicitly opted-in
// one-read hardware experiment, not a general resource-ownership primitive.
static constexpr uint64_t DellI2c0Page=0xfe103000ull;
static constexpr uint64_t DellI2c0PrivatePage=0xfe104000ull;
static constexpr uint32_t DellI2cComponentOffset=0xfcu;
static constexpr uint32_t DellI2cComponentValue=0x44570140u;
enum class DellI2cIdResult : uint8_t {Disabled,AlreadyUsed,Refused,Unknown,Identified};
struct DellI2cIdStatus {bool started=false,completed=false;DellI2cIdResult result=DellI2cIdResult::Disabled;uint32_t value=0;};
struct DellI2cIdPrerequisites {
    bool optedIn=false,reviewedFirmware=false,stableNvs=false,bootMapHole=false,pciInventoryComplete=false,noPciBaseInPage=false;
    uint64_t controller=0,privatePage=0;uint8_t mode=0;
};
struct DellI2cCriticalEvidence {
    bool aliasesValid=false,identityPresent=false,identityMatches=false,directPresent=false,directMatches=false;
    bool currentDirectPresent=false,currentDirectMatches=false,cpuValid=false,cacheDisabled=false,notWriteThrough=false;
    uint8_t identityPat=0xff,directPat=0xff,currentDirectPat=0xff,patSlot0=0xff,mtrrType=0xff;
};
inline bool DellI2cIdPreflight(const DellI2cIdPrerequisites& p) {
    return p.optedIn && p.reviewedFirmware && p.stableNvs && p.bootMapHole && p.pciInventoryComplete && p.noPciBaseInPage &&
        p.mode==1 && p.controller==DellI2c0Page && p.privatePage==DellI2c0PrivatePage;
}
// The caller holds the kernel page-table lock and prevents CPU migration for
// both the evidence checks and the single callback. Io may not retry the load.
template<class Io> bool DellI2cIdCritical(Io& io,uint32_t& value) {
    DellI2cCriticalEvidence e{};
    if(!io.evidence(e) || !e.aliasesValid || !e.identityPresent || !e.identityMatches || !e.directPresent || !e.directMatches ||
       !e.currentDirectPresent || !e.currentDirectMatches || !e.cpuValid || e.cacheDisabled || e.notWriteThrough ||
       e.identityPat!=0 || e.directPat!=0 || e.currentDirectPat!=0 || e.patSlot0!=6 || e.mtrrType!=0)return false;
    return io.readComponent(value);
}
class DellI2cIdOneShot {
    uint32_t state_=0,result_=0,value_=0;
public:
    template<class Io> DellI2cIdResult Run(const DellI2cIdPrerequisites& p,Io& io,uint32_t& value) {
        value=0;if(!p.optedIn)return DellI2cIdResult::Disabled;
        uint32_t unused=0;if(!__atomic_compare_exchange_n(&state_,&unused,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return DellI2cIdResult::AlreadyUsed;
        DellI2cIdResult result=DellI2cIdResult::Refused;
        if(DellI2cIdPreflight(p) && io.criticalRead(value))result=value==DellI2cComponentValue?DellI2cIdResult::Identified:DellI2cIdResult::Unknown;
        __atomic_store_n(&value_,value,__ATOMIC_RELAXED);__atomic_store_n(&result_,uint32_t(result),__ATOMIC_RELAXED);
        __atomic_store_n(&state_,2u,__ATOMIC_RELEASE);return result;
    }
    DellI2cIdStatus Status() const {
        const uint32_t state=__atomic_load_n(&state_,__ATOMIC_ACQUIRE);DellI2cIdStatus status{};
        status.started=state!=0;status.completed=state==2;
        if(status.completed){status.result=static_cast<DellI2cIdResult>(__atomic_load_n(&result_,__ATOMIC_RELAXED));status.value=__atomic_load_n(&value_,__ATOMIC_RELAXED);}
        return status;
    }
};
}
