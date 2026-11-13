#pragma once
#include "intel_bdw_bcs_init.h"
#include "intel_bdw_bcs_reset.h"
namespace brook {
// The selected engine's BAR0 page is mapped uncached and held stable by the
// caller. The owner must hold forcewake and exclusive engine ownership, and
// pin the already-bound HWS.
// This class does not establish those rights or submit a batch by itself.
template<class Owner,uint32_t EngineBase> class BdwEngineMmio {
    static_assert(EngineBase==0x22000 || EngineBase==0x2000,"unsupported engine");
    Owner& owner_;volatile uint32_t* page_;uint64_t descriptor_=0;uint8_t portState_=0;
    bool mapped()const{return page_ && !(reinterpret_cast<uintptr_t>(page_)&3);}
public:
    BdwEngineMmio(Owner& owner,volatile uint32_t* page):owner_(owner),page_(page){}
    BdwEngineMmio(const BdwEngineMmio&)=delete;
    BdwEngineMmio& operator=(const BdwEngineMmio&)=delete;
    bool owned(){return mapped() && owner_.owned();}
    bool quiescent(){return owned() && owner_.quiescent();}
    bool csbPrepared(uint64_t hws){return owned() && owner_.csbPrepared(hws);}
    // This runs only after HWS has been bound and programmed. The backend
    // must prove a completed reset, initialize/flush the pinned status page,
    // prevent submissions, and provide a coherent producer read.
    bool synchronizeCsb(uint64_t hws){
        if(!owned() || !quiescent() || !owner_.resetVerified() || !csbPrepared(hws))return false;
        uint32_t ptr=0,producer=0;
        // Gen8 reset exposes an invalid hardware write cursor (7). Verify
        // that this reset state still holds before overwriting the cursor;
        // a stale/reset proof alone cannot rule out another submission.
        if(!readReg(EngineBase+0x3a0,ptr) || ptr==0xffffffffu || (ptr&7u)!=7u ||
           !writeReg(EngineBase+0x3a0,0xffff0505u) || !readReg(EngineBase+0x3a0,ptr) ||
           !owner_.prepareCsbMemory(hws) || !writeReg(EngineBase+0x3a0,0xffff0505u) ||
           !readReg(EngineBase+0x3a0,ptr) || (ptr&0x707u)!=0x505u ||
           !owner_.csbMemoryProducer(hws,producer) || producer!=5)return false;
        return owned() && quiescent() && owner_.resetVerified();
    }
    bool readReg(uint32_t reg,uint32_t& value){
        if(!owned() || (reg!=EngineBase+0x29c && reg!=EngineBase+0x9c && reg!=EngineBase+0x80 && reg!=EngineBase+0x3a0))return false;
        __asm__ volatile("lfence":::"memory");
        value=page_[(reg-EngineBase)/4];
        __asm__ volatile("lfence":::"memory");return owner_.owned();
    }
    bool writeReg(uint32_t reg,uint32_t value){
        if(!owned())return false;
        const bool allowed=(reg==EngineBase+0x98 && value==0xffffffffu) ||
            (reg==EngineBase+0x29c && value==0x80008000u) ||
            (reg==EngineBase+0x9c && value==0x01000000u) ||
            (reg==EngineBase+0x80 && value && !(value&4095u) && owner_.csbPrepared(value)) ||
            (reg==EngineBase+0x3a0 && value==0xffff0505u && owner_.resetVerified());
        if(!allowed)return false;
        page_[(reg-EngineBase)/4]=value;
        __asm__ volatile("mfence":::"memory");return owner_.owned();
    }
    bool initializeExeclists(uint64_t hws){return BdwInitializeEngineExeclists<EngineBase>(*this,hws);}
    // The four writes form one transaction: an empty second descriptor and
    // the validated first descriptor, high dword before low. Never reuse this
    // accessor after any failure or a completed submission.
    bool prepareSubmission(uint64_t descriptor){
        if(portState_ || !descriptor || !owned() || !quiescent() || !owner_.submissionReady(descriptor)) {
            portState_=255;return false;
        }
        descriptor_=descriptor;portState_=1;return true;
    }
    bool submitDword(uint32_t value){
        if(portState_<1 || portState_>4 || !owned() || !owner_.submissionReady(descriptor_)) {
            portState_=255;return false;
        }
        const uint32_t expected=portState_<=2?0:portState_==3?uint32_t(descriptor_>>32):uint32_t(descriptor_);
        if(value!=expected){portState_=255;return false;}
        __asm__ volatile("mfence":::"memory");
        page_[0x230/4]=value;
        __asm__ volatile("mfence":::"memory");
        if(!owner_.owned()){portState_=255;return false;}
        if(++portState_==5) {
            // ELSP is write-only: drain its posted writes through status.
            const uint32_t posted=page_[0x234/4];
            __asm__ volatile("lfence":::"memory");
            if(posted==0xffffffffu || !owner_.owned()){portState_=255;return false;}
        }
        return true;
    }
};
template<class Owner> using BdwBcsMmio=BdwEngineMmio<Owner,0x22000>;
template<class Owner> using BdwRcsMmio=BdwEngineMmio<Owner,0x2000>;
// Separate, narrowly scoped reset accessor. engine and gt map BAR0+0x22000
// and BAR0+0x9000 respectively. Both mappings and all ownership
// guarantees must be established by the caller; this type does not establish
// or re-establish forcewake after a reset.
template<class Owner> class BdwBcsResetMmio {
    Owner& owner_;volatile uint32_t* engine_;volatile uint32_t* gt_;
    bool mapped()const{return engine_ && gt_ && !(reinterpret_cast<uintptr_t>(engine_)&3) &&
        !(reinterpret_cast<uintptr_t>(gt_)&3);}
public:
    BdwBcsResetMmio(Owner& o,volatile uint32_t* engine,volatile uint32_t* gt):owner_(o),engine_(engine),gt_(gt){}
    BdwBcsResetMmio(const BdwBcsResetMmio&)=delete;
    BdwBcsResetMmio& operator=(const BdwBcsResetMmio&)=delete;
    bool owned(){return mapped() && owner_.owned();}
    bool quiescent(){return owned() && owner_.quiescent();}
    bool monotonicMs(uint64_t& value){return owned() && owner_.monotonicMs(value);}
    void relax(){owner_.relax();}
    bool readReg(uint32_t reg,uint32_t& value){
        if(!owned() || (reg!=0x220d0 && reg!=0x2209c && reg!=0x941c))return false;
        __asm__ volatile("lfence":::"memory");
        value=reg==0x220d0?engine_[0xd0/4]:reg==0x2209c?engine_[0x9c/4]:gt_[0x41c/4];
        __asm__ volatile("lfence":::"memory");return owner_.owned();
    }
    bool writeReg(uint32_t reg,uint32_t value){
        if(!owned() || !((reg==0x2209c && value==0x01000100u) || (reg==0x220d0 && (value==0x10001u || value==0x10000u)) ||
                         (reg==0x941c && value==8)))return false;
        if(reg==0x220d0)engine_[0xd0/4]=value;else if(reg==0x2209c)engine_[0x9c/4]=value;else gt_[0x41c/4]=value;
        __asm__ volatile("mfence":::"memory");return owner_.owned();
    }
    BdwBcsResetResult reset(BdwBcsResetIntent intent=BdwBcsResetIntent::IdleTakeover){return BdwResetBcsDomain(*this,intent);}
};
}
