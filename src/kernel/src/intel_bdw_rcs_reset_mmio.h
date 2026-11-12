#pragma once
#include "intel_bdw_bcs_reset.h"

namespace brook {
// Narrow RCS reset accessor, not a complete render-engine owner. The caller
// must hold render forcewake, own the engine and serialize all GT resets.
// The two uncached pages map BAR0+0x2000 and BAR0+0x9000 respectively.
template<class Owner> class BdwRcsResetMmio {
    Owner& owner_; volatile uint32_t* engine_; volatile uint32_t* gt_;
    bool mapped() const { return engine_ && gt_ &&
        !(reinterpret_cast<uintptr_t>(engine_)&3) && !(reinterpret_cast<uintptr_t>(gt_)&3); }
public:
    BdwRcsResetMmio(Owner& owner,volatile uint32_t* engine,volatile uint32_t* gt)
        :owner_(owner),engine_(engine),gt_(gt) {}
    BdwRcsResetMmio(const BdwRcsResetMmio&)=delete;
    BdwRcsResetMmio& operator=(const BdwRcsResetMmio&)=delete;
    bool owned(){return mapped() && owner_.owned();}
    bool quiescent(){return owned() && owner_.quiescent();}
    bool monotonicMs(uint64_t& value){return owned() && owner_.monotonicMs(value);}
    void relax(){owner_.relax();}
    bool readReg(uint32_t reg,uint32_t& value){
        if(!owned() || (reg!=0x20d0 && reg!=0x209c && reg!=0x941c))return false;
        __asm__ volatile("lfence":::"memory");
        value=reg==0x20d0?engine_[0xd0/4]:reg==0x209c?engine_[0x9c/4]:gt_[0x41c/4];
        __asm__ volatile("lfence":::"memory");return owner_.owned();
    }
    bool writeReg(uint32_t reg,uint32_t value){
        if(!owned() || !((reg==0x209c && value==0x01000100u) ||
                         (reg==0x20d0 && (value==0x10001u || value==0x10000u)) ||
                         (reg==0x941c && value==2)))return false;
        if(reg==0x20d0)engine_[0xd0/4]=value;
        else if(reg==0x209c)engine_[0x9c/4]=value;
        else gt_[0x41c/4]=value;
        __asm__ volatile("mfence":::"memory");return owner_.owned();
    }
    BdwBcsResetResult reset(BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover){
        return BdwResetRcsDomain(*this,intent);
    }
};
}
