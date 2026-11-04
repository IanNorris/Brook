#pragma once
#include "intel_bdw_forcewake.h"
#include "intel_bdw_gtt_probe.h"
namespace brook {
// Dell XPS 9343 only. The caller establishes device identity, D0 and boot-map
// ownership and holds the mapping lock throughout. The backend supplies only
// fixed-address DWORD device accesses and an ordered, invariant BSP TSC.
constexpr uint64_t BdwDellBar0=0xf6000000ull;
constexpr uint64_t BdwForcewakePages[]={0xf600a000ull,0xf6130000ull,0xf6138000ull,0xf6022000ull};
constexpr uint64_t BdwOtherEnginePages[]={0xf6002000ull,0xf6012000ull,0xf601a000ull};
struct BdwForcewakeAdapterResult {
    bool aliases=false,clock=false,clearDelivered=false;
    BdwForcewakeResult sequence{};
};
template<class Backend> BdwForcewakeAdapterResult BdwDellForcewakeCritical(Backend& b,uint32_t khz,bool otherEngines=false) {
    BdwForcewakeAdapterResult result{};
    if(!b.owned() || !khz || khz>20000000)return result;
    for(auto page:BdwForcewakePages){DellI2cCriticalEvidence e{};
        if(!b.evidence(page,e) || !BdwGttEvidenceValid(e))return result;}
    if(otherEngines)for(auto page:BdwOtherEnginePages){DellI2cCriticalEvidence e{};
        if(!b.evidence(page,e) || !BdwGttEvidenceValid(e))return result;}
    result.aliases=true;
    uint64_t origin=0,now=0;if(!b.tsc(origin))return result;
    bool advancing=false;
    for(unsigned i=0;i<100000;++i){if(!b.tsc(now) || now<origin)return result;
        if(now>origin){advancing=true;break;}b.relax();}
    if(!advancing)return result;
    result.clock=true;
    struct Io {
        Backend& b;uint32_t khz;uint64_t origin;bool& clearDelivered;bool otherEngines;
        bool owned(){return b.owned();}
        bool read(uint32_t offset,uint32_t& value){
            if(offset!=0x130044 && offset!=0x13805c) {
                bool allowed=false;
                constexpr uint32_t bases[]={0x22000,0x2000,0x12000,0x1a000};
                constexpr uint32_t regs[]={0x30,0x34,0x38,0x3c,0x9c,0x234,0x238,0x29c};
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
    } io{b,khz,origin,result.clearDelivered,otherEngines};
    result.sequence=BdwObserveBcs(io,otherEngines);return result;
}
}
