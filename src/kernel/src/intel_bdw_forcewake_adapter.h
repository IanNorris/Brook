#pragma once
#include "intel_bdw_forcewake.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_tlb.h"
#include "intel_bdw_bcs_reset.h"
namespace brook {
// Dell XPS 9343 only. The caller establishes device identity, D0 and boot-map
// ownership and holds the mapping lock throughout. The backend supplies only
// fixed-address DWORD device accesses and an ordered, invariant BSP TSC.
constexpr uint64_t BdwDellBar0=0xf6000000ull;
#if defined(BROOK_BDW_RPS_TRIAL)
constexpr uint32_t BdwRenderFrequencyCaps=0x145998;
constexpr uint64_t BdwRenderFrequencyCapsPage=BdwDellBar0+(BdwRenderFrequencyCaps&~4095u);
#endif
constexpr uint64_t BdwForcewakePages[]={0xf600a000ull,0xf6130000ull,0xf6138000ull,0xf6022000ull};
constexpr uint64_t BdwOtherEnginePages[]={0xf6002000ull,0xf6012000ull,0xf601a000ull};
constexpr uint64_t BdwPrivatePatPage=0xf6004000ull;
constexpr uint64_t BdwResetPage=0xf6009000ull;
struct BdwForcewakeAdapterResult {
    bool aliases=false,clock=false,clearDelivered=false;
    BdwForcewakeResult sequence{};
    // Why the adapter stopped before writing the request bit
    // (BdwForcewakeRefusal in intel_bdw_render_refusal.h), with the page
    // index for evidence refusals.
    uint8_t refusal=0,refusalDetail=0;
    // The callback ran but refused before any device write (its caller
    // proves this); the claim is then not left uncertain.
    bool workRefusedUntouched=false;
};
class BdwForcewakeClaim {
    uint32_t state_=0; // available, held, uncertain
public:
    BdwForcewakeClaim()=default;BdwForcewakeClaim(const BdwForcewakeClaim&)=delete;
    BdwForcewakeClaim& operator=(const BdwForcewakeClaim&)=delete;
    bool acquire(){uint32_t expected=0;return __atomic_compare_exchange_n(&state_,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
    uint32_t state()const{return __atomic_load_n(&state_,__ATOMIC_ACQUIRE);}
    void finish(const BdwForcewakeAdapterResult& r){
        const bool clean=(!r.sequence.requested || (r.clearDelivered && r.sequence.released)) &&
            (!r.sequence.tlbAttempted || r.sequence.tlbComplete) &&
            (!r.sequence.resetAttempted || r.sequence.resetComplete) &&
            (!r.sequence.workAttempted || r.sequence.workComplete || r.workRefusedUntouched);
        uint32_t held=1;__atomic_compare_exchange_n(&state_,&held,clean?0u:2u,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
    }
};
template<class Backend,class Work> BdwForcewakeAdapterResult BdwDellForcewakeCriticalWithWork(Backend& b,uint32_t khz,Work& work,bool workRequested,
        bool otherEngines=false,bool patRequested=false,bool tlbRequested=false,bool resetRequested=false) {
    BdwForcewakeAdapterResult result{};
    auto refuse=[&result](uint8_t code,uint8_t detail=0){result.refusal=code;result.refusalDetail=detail;return result;};
    if(!b.owned() || !khz || khz>20000000 || ((tlbRequested || resetRequested) && (!otherEngines || !patRequested)) ||
       (tlbRequested && resetRequested))return refuse(3);
    {uint8_t i=0;for(auto page:BdwForcewakePages){DellI2cCriticalEvidence e{};
        if(!b.evidence(page,e) || !BdwGttEvidenceValid(e))return refuse(4,i);++i;}}
    if(otherEngines){uint8_t i=0;for(auto page:BdwOtherEnginePages){DellI2cCriticalEvidence e{};
        if(!b.evidence(page,e) || !BdwGttEvidenceValid(e))return refuse(5,i);++i;}}
    if(patRequested){DellI2cCriticalEvidence e{};
        if(!b.evidence(BdwPrivatePatPage,e) || !BdwGttEvidenceValid(e))return refuse(6);}
    if(resetRequested){DellI2cCriticalEvidence e{};
        if(!b.evidence(BdwResetPage,e) || !BdwGttEvidenceValid(e))return refuse(7);}
    result.aliases=true;
    uint64_t origin=0,now=0;if(!b.tsc(origin))return refuse(8);
    bool advancing=false;
    for(unsigned i=0;i<100000;++i){if(!b.tsc(now) || now<origin)return refuse(8);
        if(now>origin){advancing=true;break;}b.relax();}
    if(!advancing)return refuse(9);
    result.clock=true;
    struct Io {
        Backend& b;uint32_t khz;uint64_t origin;bool& clearDelivered;bool otherEngines,patRequested;
        bool owned(){return b.owned();}
        bool read(uint32_t offset,uint32_t& value){
            if(offset!=0x130044 && offset!=0x13805c && !(patRequested && (offset==0x40e0 || offset==0x40e4))) {
                bool allowed=false;
                constexpr uint32_t bases[]={0x22000,0x2000,0x12000,0x1a000};
                constexpr uint32_t regs[]={0x30,0x34,0x38,0x3c,0x9c,0x234,0x238,0x29c,0x3a0};
                for(unsigned i=0;i<(otherEngines?4u:1u);++i)
                    for(auto reg:regs)if(offset==bases[i]+reg)allowed=true;
                if(!allowed)return false;
            }
            return b.read32(BdwDellBar0+offset,value);
        }
        bool write(uint32_t offset,uint32_t value){
            if(offset!=0xa188 || (value!=0x00010001 && value!=0x00010000))return false;
            // The posting read is required even when a store reports failure:
            // the device may have received it. A later clear always drains it.
            bool stored=b.write32(BdwDellBar0+offset,value);uint32_t posted=0;
            bool drained=b.read32(BdwDellBar0+0x130044,posted) && posted!=0xffffffffu;
            if(value==0x00010000)clearDelivered=stored && drained;
            return stored && drained;
        }
        bool monotonicMs(uint64_t& ms){uint64_t ticks=0;if(!b.tsc(ticks) || ticks<origin)return false;
            ms=(ticks-origin)/khz;return true;}
        void relax(){b.relax();}
        bool invalidateTlb(){
            struct TlbIo {
                Io& parent;
                bool owned(){return parent.owned();}
                bool allEnginesQuiescent(){return owned();} // checked immediately before this callback
                bool monotonicMs(uint64_t& v){return parent.monotonicMs(v);}
                void relax(){parent.relax();}
                bool readReg(uint32_t r,uint32_t& v){return (r==0x4260 || r==0x4264 || r==0x426c || r==0x4270) && parent.b.read32(BdwDellBar0+r,v);}
                bool writeReg(uint32_t r,uint32_t v){return v==1 && (r==0x4260 || r==0x4264 || r==0x426c || r==0x4270) && parent.b.write32(BdwDellBar0+r,v);}
            } tlb{*this};
            BdwGen8TlbInvalidate invalidator;return invalidator.Run(tlb);
        }
        bool resetBcs(){
            struct ResetIo {
                Io& parent;
                bool owned(){return parent.owned();}
                bool quiescent(){return owned();} // checked immediately before this callback
                bool monotonicMs(uint64_t& v){return parent.monotonicMs(v);}
                void relax(){parent.relax();}
                bool readReg(uint32_t r,uint32_t& v){return (r==0x220d0 || r==0x941c) && parent.b.read32(BdwDellBar0+r,v);}
                bool writeReg(uint32_t r,uint32_t v){return ((r==0x220d0 && (v==0x10001u || v==0x10000u)) || (r==0x941c && v==8)) && parent.b.write32(BdwDellBar0+r,v);}
            } reset{*this};
            return BdwResetBcsDomain(reset)==BdwBcsResetResult::DomainAcknowledged;
        }
    } io{b,khz,origin,result.clearDelivered,otherEngines,patRequested};
    auto heldWork=[&](Io&,const BdwForcewakeResult& state){return work(b,state);};
    result.sequence=BdwObserveBcsWithWork(io,heldWork,workRequested,otherEngines,patRequested,tlbRequested,resetRequested);return result;
}
template<class Backend> BdwForcewakeAdapterResult BdwDellForcewakeCritical(Backend& b,uint32_t khz,bool otherEngines=false,bool patRequested=false,bool tlbRequested=false,bool resetRequested=false) {
    auto none=[](Backend&,const BdwForcewakeResult&){return false;};
    return BdwDellForcewakeCriticalWithWork(b,khz,none,false,otherEngines,patRequested,tlbRequested,resetRequested);
}
}
