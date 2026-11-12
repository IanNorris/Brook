#pragma once
#include "intel_bdw_render_gt_init.h"

namespace brook {
// Adapts the GT initialization transaction to already-audited UC BAR pages
// at offsets 0x2000, 0x4000 and 0x9000. Owner supplies all lifetime,
// forcewake, display, firmware, MCR steering/broadcast and timing guarantees.
// readMcrTopology must sample GEN8_MCR_SELECTOR (0xfdc) and GEN8_FUSE2
// (0x9120) under that same ownership; fuse presence alone does not prove a
// selected unit is powered. See i915 intel_gt_mcr.c and intel_gt_regs.h.
// Constructing this accessor does not establish any of them.
template<class Owner> class BdwRenderGtMmio {
    Owner& owner_;volatile uint32_t *engine_,*gam_,*gt_;
    bool mapped()const{return engine_ && gam_ && gt_ &&
        !(reinterpret_cast<uintptr_t>(engine_)&3) && !(reinterpret_cast<uintptr_t>(gam_)&3) &&
        !(reinterpret_cast<uintptr_t>(gt_)&3);}
    volatile uint32_t* address(uint32_t reg)const {
        switch(reg){
        case 0x2050:case 0x20a0:return engine_+(reg-0x2000)/4;
        case 0x4090:return gam_+(reg-0x4000)/4;
        case 0x9400:case 0x9424:case 0x9430:return gt_+(reg-0x9000)/4;
        default:return nullptr;
        }
    }
public:
    BdwRenderGtMmio(Owner& owner,volatile uint32_t* engine,volatile uint32_t* gam,volatile uint32_t* gt)
        :owner_(owner),engine_(engine),gam_(gam),gt_(gt){}
    BdwRenderGtMmio(const BdwRenderGtMmio&)=delete;
    BdwRenderGtMmio& operator=(const BdwRenderGtMmio&)=delete;
    bool owned(){return mapped() && owner_.owned();}
    bool allEnginesQuiescent(){return owned() && owner_.allEnginesQuiescent();}
    bool displayAndFirmwareSafe(){return owned() && owner_.displayAndFirmwareSafe();}
    bool mcrSteeringReady(){
        uint32_t selector=0,fuse=0,selectorAgain=0,fuseAgain=0;
        return owned() && owner_.mcrSteeringReady() &&
            owner_.readMcrTopology(selector,fuse) && owner_.readMcrTopology(selectorAgain,fuseAgain) &&
            selector==selectorAgain && fuse==fuseAgain && BdwMcrSelectionPresent(selector,fuse) && owned();
    }
    bool readReg(uint32_t reg,uint32_t& value){
        auto* p=address(reg);if(!owned() || !p)return false;
        __asm__ volatile("lfence":::"memory");value=*p;
        __asm__ volatile("lfence":::"memory");return owner_.owned();
    }
    bool writeReg(uint32_t reg,uint32_t value){
        auto* p=address(reg);if(!owned() || !p)return false;
        uint32_t mask=0;
        switch(reg){
        case 0x4090:mask=0x40;break;case 0x20a0:mask=0x88000;break;
        case 0x9430:mask=0x4000;break;case 0x9400:mask=0x10000;break;
        case 0x9424:mask=1;break;
        case 0x2050:if(value!=0x10001000u)return false;break;
        default:return false;
        }
        if(mask){uint32_t old=0;if(!readReg(reg,old) || old==0xffffffffu || ((old^value)&~mask))return false;}
        *p=value;__asm__ volatile("mfence":::"memory");return owner_.owned();
    }
    bool readMcr(uint32_t reg,uint32_t& value){return reg==0xb100 && mcrSteeringReady() && owner_.readMcr(reg,value);}
    bool writeMcrBroadcast(uint32_t reg,uint32_t value){
        constexpr uint32_t mask=(0x1fu<<19)|(0x1fu<<14),target=(15u<<19)|(1u<<14);
        uint32_t old=0;
        return reg==0xb100 && mcrSteeringReady() && (value&mask)==target &&
            owner_.readMcr(reg,old) && old!=0xffffffffu && !((old^value)&~mask) &&
            owner_.writeMcrBroadcast(reg,value);
    }
    bool delayAtLeastOneMicrosecond(){return owned() && owner_.delayAtLeastOneMicrosecond();}
};
}
