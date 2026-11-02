#pragma once
#include "hda_dell_stream_memory.h"
#include "hda_dell_tone_codec.h"
#include "hda_dell_tone_ici.h"
#include "hda_dell_tone_stream.h"

namespace brook {
// Exclusively owns one Dell controller and its 64 KiB streaming ring. This is
// an injected integration layer. The platform establishes the verified identity, full PCI resource
// ownership, correct uncached MMIO mapping, a monotonic clock and safe WB DMA
// pages. No concurrent controller owner or codec transport is permitted.
// Every call must be serialized, including service, submit and shutdown.
template<class Platform> class HdaDellStreamOwner {
    Platform& p_;const HdaSessionReport saved_;HdaDellStreamMemory<Platform> memory_;
    HdaDellToneIci<Platform> ici_;HdaDellToneCodec<HdaDellToneIci<Platform>> codec_;
    struct StreamIo {
        HdaDellStreamOwner& o;
        uint8_t read8(uint32_t a){return o.p_.read8(a);}uint16_t read16(uint32_t a){return o.p_.read16(a);}
        uint32_t read32(uint32_t a){return o.p_.read32(a);}
        void write8(uint32_t a,uint8_t v){o.p_.write8(a,v);}void write16(uint32_t a,uint16_t v){o.p_.write16(a,v);}
        void write32(uint32_t a,uint32_t v){o.p_.write32(a,v);}
        uint64_t ticks(){return o.p_.ticks();}void pause(){o.p_.pause();}
        bool delayUs(unsigned n){return o.p_.delayUs(n);}bool publishDma(){return o.memory_.publishAll();}
    } streamIo_;
    HdaDellToneStream<StreamIo> stream_;HdaDellTonePlan plan_{};
    uint32_t base_=0,stage_=0,failureStage_=0,failedStreamStage_=0,failedStreamDetail_=0;bool active_=false,prepared_=false,stopped_=false,retained_=false,held_=false,codecTouched_=false,muted_=false;
    void stage(uint32_t n){__atomic_store_n(&stage_,n,__ATOMIC_RELAXED);}
    void noteFailure(uint32_t n){uint32_t empty=0;__atomic_compare_exchange_n(&failureStage_,&empty,n,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);}
    bool hold(){if(!held_){
        noteFailure(__atomic_load_n(&stage_,__ATOMIC_RELAXED));
        if(__atomic_load_n(&stage_,__ATOMIC_RELAXED)==14){__atomic_store_n(&failedStreamDetail_,stream_.detail(),__ATOMIC_RELAXED);
            __atomic_store_n(&failedStreamStage_,stream_.stage(),__ATOMIC_RELEASE);}
        const uint16_t command=p_.command();
        if(codecTouched_ && !muted_ && !ici_.failed() && (command==2 || command==6))muted_=codec_.muteAndVerify(plan_);
        held_=true;memory_.quarantine();p_.quarantine();}return false;}
    bool configure(){
        stage(30);uint16_t format=0;if(!HdaDellMixerFormat(saved_,format) || !p_.controllerIdle())return hold();
        stage(31);uint32_t value=0;codecTouched_=true;if(!codec_.muteAndVerify(plan_))return hold();muted_=true;
        stage(32);
        if(!codec_.configureMuted(plan_) || !ici_.setFormat(format) || !ici_.getFormat(value) || value!=format ||
           !ici_.set8(3,0x706,0x10) || !ici_.get8(3,0xf06,value) || value!=0x10 ||
           !stream_.prepare(saved_.controller.gcap,memory_.bdlPhysical(),format,65536))return hold();
        base_=0x80+((saved_.controller.gcap>>8)&15)*0x20;prepared_=true;stopped_=retained_=false;stage(33);return true;
    }
public:
    HdaDellStreamOwner(Platform& p,const HdaSessionReport& saved,uint32_t line):p_(p),saved_(saved),memory_(p,line),
        ici_(p,uint16_t(saved.converterFormat),true),codec_(ici_),streamIo_{*this},stream_(streamIo_){}
    HdaDellStreamOwner(const HdaDellStreamOwner&)=delete;
    HdaDellStreamOwner& operator=(const HdaDellStreamOwner&)=delete;
    HdaDellStreamOwner(HdaDellStreamOwner&&)=delete;
    HdaDellStreamOwner& operator=(HdaDellStreamOwner&&)=delete;
    uint32_t failureStage()const{return __atomic_load_n(&failureStage_,__ATOMIC_ACQUIRE);}
    uint32_t failedStreamStage()const{return __atomic_load_n(&failedStreamStage_,__ATOMIC_ACQUIRE);}
    uint32_t failedStreamDetail()const{return __atomic_load_n(&failedStreamDetail_,__ATOMIC_RELAXED);}
    bool prepare(){
        uint16_t format=0;
        if(active_ || held_ || !HdaDellMakeTonePlan(saved_,plan_) || !HdaDellMixerFormat(saved_,format) ||
           !p_.identityStable() || p_.command()!=0 || !memory_.allocate((saved_.controller.gcap&1)!=0))return false;
        p_.setCommand(2);
        if(p_.command()!=2){if(p_.command()==0)memory_.release(false);else hold();return false;}
        active_=true;return configure();
    }
    uint64_t nowNs(){return p_.nowNs();}
    bool position(uint32_t& raw,bool& error,bool& running){
        if(!prepared_ || held_ || stopped_ || p_.command()!=6)return false;
        raw=p_.read32(base_+4);const uint8_t status=p_.read8(base_+3);
        running=(p_.read16(base_)&2)!=0;error=(status&0x18)!=0;return true;
    }
    bool write(uint32_t o,const uint8_t* d,uint32_t n){return prepared_ && !held_ && memory_.write(o,d,n);}
    bool publish(uint32_t o,uint32_t n){return prepared_ && !held_ && memory_.publish(o,n);}
    bool fence(){return prepared_ && !held_ && memory_.fence();}
    bool start(){
        if(!prepared_ || held_ || stopped_ || p_.command()!=2 || !memory_.publishAll() || !memory_.expose())return false;
        p_.setCommand(6);if(p_.command()!=6)return false;
        return stream_.start();
    }
    bool mute(){stage(9);if(!active_ || held_ || (p_.command()!=2 && p_.command()!=6)){noteFailure(9);return false;}
        muted_=codec_.muteAndVerify(plan_);if(!muted_)noteFailure(9);return muted_;}
    bool stop(){
        stage(10);
        if(!prepared_ || held_ || (p_.command()!=2 && p_.command()!=6) || !stream_.stop())return false;
        p_.setCommand(2);if(p_.command()!=2)return false;stopped_=true;return true;
    }
    void quarantine(){hold();}
    // Call only after the queue has successfully muted and stopped. A failure
    // may have changed hardware state: never retry or release pages.
    bool retireRetained(){
        stage(11);
        if(!prepared_ || held_ || !stopped_)return false;if(p_.command()!=2)return hold();
        if(!mute())return hold();stage(12);uint32_t value=0;
        if(!ici_.set8(3,0x706,0) || !ici_.get8(3,0xf06,value) || value!=0 ||
           !ici_.setFormat(uint16_t(saved_.converterFormat)) || !ici_.getFormat(value) || value!=saved_.converterFormat)return hold();
        muted_=false; // restore can partially unmute before reporting failure
        stage(13);if(!codec_.restoreAndVerify(plan_,saved_))return hold();
        stage(14);if(!stream_.retire())return hold();
        prepared_=false;retained_=true;stage(15);return true;
    }
    // The caller serializes all access and supplies a platform that validates
    // the executing CPU as well as the retained page mappings.
    bool prepareRetained(){
        stage(20);
        if(!active_ || !retained_ || held_)return false;
        if(!p_.identityStable() || p_.command()!=2)return hold();
        stage(21);if(!memory_.revalidate())return hold();
        return configure();
    }
    bool retire(){
        if(!retained_ && !retireRetained())return false;
        if(held_ || !active_)return false;if(p_.command()!=2)return hold();
        p_.setCommand(0);if(p_.command()!=0)return hold();
        if(!memory_.release(true))return hold();active_=retained_=false;return true;
    }
};
}
