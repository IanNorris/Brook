#pragma once
#include "intel_bdw_bcs_reset.h"

namespace brook {
// Narrow VCS0 (video/BSD engine) reset accessor, mirroring BdwRcsResetMmio.
// The caller must hold forcewake, own the engine and serialize all GT resets
// (the shared BdwForcewakeClaim does). The two uncached pages map BAR0+0x12000
// and BAR0+0x9000. Only the reset-control, MI_MODE stop and GDRST media writes
// are permitted.
template<class Owner> class BdwVcsResetMmio {
    Owner& owner_; volatile uint32_t* engine_; volatile uint32_t* gt_;
    bool mapped() const { return engine_ && gt_ &&
        !(reinterpret_cast<uintptr_t>(engine_)&3) && !(reinterpret_cast<uintptr_t>(gt_)&3); }
public:
    BdwVcsResetMmio(Owner& owner,volatile uint32_t* engine,volatile uint32_t* gt)
        :owner_(owner),engine_(engine),gt_(gt) {}
    BdwVcsResetMmio(const BdwVcsResetMmio&)=delete;
    BdwVcsResetMmio& operator=(const BdwVcsResetMmio&)=delete;
    bool owned(){return mapped() && owner_.owned();}
    bool quiescent(){return owned() && owner_.quiescent();}
    bool monotonicMs(uint64_t& value){return owned() && owner_.monotonicMs(value);}
    void relax(){owner_.relax();}
    bool readReg(uint32_t reg,uint32_t& value){
        if(!owned() || (reg!=0x120d0 && reg!=0x1209c && reg!=0x941c))return false;
        __asm__ volatile("lfence":::"memory");
        value=reg==0x120d0?engine_[0xd0/4]:reg==0x1209c?engine_[0x9c/4]:gt_[0x41c/4];
        __asm__ volatile("lfence":::"memory");return owner_.owned();
    }
    bool writeReg(uint32_t reg,uint32_t value){
        if(!owned() || !((reg==0x1209c && value==0x01000100u) ||
                         (reg==0x120d0 && (value==0x10001u || value==0x10000u)) ||
                         (reg==0x941c && value==4)))return false;
        if(reg==0x120d0)engine_[0xd0/4]=value;
        else if(reg==0x1209c)engine_[0x9c/4]=value;
        else gt_[0x41c/4]=value;
        __asm__ volatile("mfence":::"memory");return owner_.owned();
    }
    BdwBcsResetResult reset(BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover){
        return BdwResetVcsDomain(*this,intent);
    }
};
}
