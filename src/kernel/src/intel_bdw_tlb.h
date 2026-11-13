#pragma once
#include <stdint.h>
namespace brook {
// Gen8 full engine TLB invalidation. This does not flush GPU data caches,
// synchronize display translations, establish forcewake, or drain prior
// posted PTE/PAT writes; its caller must do those separately. The owner must
// hold all four engines, their power domains, GT reset serialization and UC
// mappings for the whole operation. Any attempted write can have reached the
// device even when it reports failure; uncertain completion is permanent.
class BdwGen8TlbInvalidate {
    bool working_=false,uncertain_=false;
public:
    BdwGen8TlbInvalidate()=default;
    BdwGen8TlbInvalidate(const BdwGen8TlbInvalidate&)=delete;
    BdwGen8TlbInvalidate& operator=(const BdwGen8TlbInvalidate&)=delete;
    bool Uncertain()const{return uncertain_;}
    template<class Io> bool Run(Io& io) {
        constexpr uint32_t regs[]={0x4260,0x4264,0x426c,0x4270};
        if(working_ || uncertain_ || !io.owned() || !io.allEnginesQuiescent())return false;
        working_=true;
        uint64_t preflight=0;uint32_t v=0;
        if(!io.monotonicMs(preflight)){working_=false;return false;}
        for(auto reg:regs)if(!io.readReg(reg,v) || v==0xffffffffu || (v&1)){
            working_=false;return false;
        }
        // A write attempt can have reached the device even on failure.
        for(auto reg:regs)if(!io.owned() || !io.writeReg(reg,1)){
            uncertain_=true;working_=false;return false;
        }
        for(auto reg:regs) {
            uint64_t start=0,prev=0;
            if(!io.monotonicMs(start)){uncertain_=true;working_=false;return false;}
            prev=start;bool complete=false;
            for(unsigned n=0;n<100000;++n){uint64_t now=0;
                if(!io.owned() || !io.readReg(reg,v) || v==0xffffffffu ||
                   !io.monotonicMs(now) || now<prev || now-start>=4)break;
                prev=now;if(!(v&1)){complete=true;break;}io.relax();
            }
            if(!complete){uncertain_=true;working_=false;return false;}
        }
        working_=false;return true;
    }
};

// Read/write only the four Gen8 TLB registers within the audited UC mapping
// at BAR0+0x4000. A matching device owner and forcewake are mandatory.
template<class Owner> class BdwGen8TlbMmio {
    Owner& owner_;volatile uint32_t* page_;
    static bool allowed(uint32_t r){return r==0x4260 || r==0x4264 || r==0x426c || r==0x4270;}
public:
    BdwGen8TlbMmio(Owner& owner,volatile uint32_t* page):owner_(owner),page_(page){}
    BdwGen8TlbMmio(const BdwGen8TlbMmio&)=delete;
    BdwGen8TlbMmio& operator=(const BdwGen8TlbMmio&)=delete;
    bool owned(){return page_ && !(reinterpret_cast<uintptr_t>(page_)&3) && owner_.owned();}
    bool allEnginesQuiescent(){return owned() && owner_.allEnginesQuiescent();}
    bool monotonicMs(uint64_t& v){return owned() && owner_.monotonicMs(v);}
    void relax(){owner_.relax();}
    bool readReg(uint32_t reg,uint32_t& v){
        if(!owned() || !allowed(reg))return false;
        __asm__ volatile("lfence":::"memory");v=page_[(reg-0x4000)/4];
        __asm__ volatile("lfence":::"memory");return owner_.owned();
    }
    bool writeReg(uint32_t reg,uint32_t v){
        if(!owned() || !allowed(reg) || v!=1)return false;
        page_[(reg-0x4000)/4]=v;__asm__ volatile("mfence":::"memory");return owner_.owned();
    }
};
}
