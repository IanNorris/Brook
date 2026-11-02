#pragma once
#include "hda_dell_tone_buffer.h"
#include "hda_dell_tone_codec.h"
#include "hda_dell_tone_ici.h"
#include "hda_dell_tone_stream.h"
#include "hda_dell_tone_transaction.h"

namespace brook {
// One-shot adapter. Platform provides exclusive PCI/controller ownership,
// checked page allocation, MMIO, a monotonic clock, and a DMA write barrier.
// allocatePage(false) transfers no ownership; true returns a fresh owned RAM
// page. freePage only accepts pages whose ownership was actually transferred.
// A quarantined adapter, its platform and snapshot MUST remain alive with its
// controller and pages pinned for the remainder of the boot.
// Never use a previous boot's report or run alongside another HDA owner.
template<class Platform> class HdaDellToneAdapter {
    Platform& p_;const HdaSessionReport& saved_;HdaDellToneIci<Platform> ici_;
    HdaDellToneCodec<HdaDellToneIci<Platform>> codec_;HdaDellToneStream<Platform> stream_;
    uint64_t phys_[4]{};void* virt_[4]{};unsigned count_=0;bool quarantined_=false,begun_=false,active_=false,executed_=false;
    uint8_t prepareStage_=0,playReason_=0;uint32_t prepareValue_=0,playLoops_=0,playProgress_=0;uint64_t playElapsed_=0;
    void freePages() {while(count_){--count_;bool duplicate=false;for(unsigned i=0;i<count_;++i)if(phys_[i]==phys_[count_])duplicate=true;
        if(!duplicate)p_.freePage(phys_[count_]);phys_[count_]=0;virt_[count_]=nullptr;}}
    bool decoded(){if(!active_ || quarantined_)return false;const auto command=p_.command();return command==2 || command==6;}
    bool matches(const HdaDellTonePlan& x){HdaDellTonePlan p{};return HdaDellMakeTonePlan(saved_,p) &&
        x.pin==p.pin && x.mixer==p.mixer && x.dac==p.dac && x.pinSelection==p.pinSelection && x.dacGain==p.dacGain && x.peakSample==p.peakSample;}
public:
    HdaDellToneAdapter(Platform& p,const HdaSessionReport& s):p_(p),saved_(s),ici_(p,uint16_t(s.converterFormat)),codec_(ici_),stream_(p){}
    HdaDellToneAdapter(const HdaDellToneAdapter&)=delete;
    HdaDellToneAdapter& operator=(const HdaDellToneAdapter&)=delete;
    HdaDellToneAdapter(HdaDellToneAdapter&&)=delete;
    HdaDellToneAdapter& operator=(HdaDellToneAdapter&&)=delete;
    bool quarantined()const{return quarantined_;}
    uint8_t prepareStage()const{return prepareStage_;}uint32_t prepareValue()const{return prepareValue_;}
    uint8_t streamStage()const{return stream_.stage();}uint32_t streamDetail()const{return stream_.detail();}
    bool iciFailed()const{return ici_.failed();}
    uint8_t playReason()const{return playReason_;}uint32_t playLoops()const{return playLoops_;}uint64_t playElapsed()const{return playElapsed_;}
    uint32_t playProgress()const{return playProgress_;}
    uint8_t progressReason()const{return stream_.progressReason();}uint8_t progressStatus()const{return stream_.progressStatus();}
    uint16_t progressControl()const{return stream_.progressControl();}uint32_t progressFirst()const{return stream_.progressFirst();}uint32_t progressLast()const{return stream_.progressLast();}
    HdaToneResult run(bool audible) {
        if(!active_ || quarantined_ || executed_)return HdaToneResult::InvalidSnapshot;
        executed_=true;return HdaDellToneTransaction(*this,saved_,audible);
    }
    // Call before enabling memory decode or touching the codec. A failed
    // allocation is safe to release because no descriptor has been published.
    bool allocate() {
        HdaDellTonePlan plan{};if(count_ || begun_ || quarantined_ || !HdaDellMakeTonePlan(saved_,plan) || !p_.identityStable() || p_.command()!=0)return false;
        for(unsigned i=0;i<4;++i) {
            if(!p_.allocatePage(phys_[i],virt_[i])){freePages();return false;}
            ++count_;
            if(!p_.pageAllowed(phys_[i],virt_[i])){freePages();return false;}
            p_.clearPage(virt_[i]);
        }
        int16_t* pages[]={static_cast<int16_t*>(virt_[1]),static_cast<int16_t*>(virt_[2]),static_cast<int16_t*>(virt_[3])};
        const uint64_t pagesPhys[]={phys_[1],phys_[2],phys_[3]};
        if(!HdaDellBuildToneBuffer(static_cast<HdaToneBdlEntry*>(virt_[0]),phys_[0],pages,pagesPhys,
                                   (saved_.controller.gcap&1)!=0,plan.peakSample)){freePages();return false;}
        return true;
    }
    // Decode is enabled only after allocation, then live state is rechecked.
    // If restoration is uncertain, retain ownership rather than pretending the
    // device is idle. Caller must never retry a quarantined transaction.
    bool begin() {
        if(count_!=4 || quarantined_ || begun_)return false;
        begun_=true;
        if(!p_.identityStable() || p_.command()!=0){freePages();return false;}
        p_.setCommand(2);
        if(p_.command()!=2){p_.setCommand(0);if(p_.command()!=0)quarantine();else freePages();return false;}
        if(!p_.controllerIdle()) {p_.setCommand(0);if(p_.command()!=0)quarantine();else freePages();return false;}
        active_=true;return true;
    }
    bool muteAndVerify(const HdaDellTonePlan& plan){return decoded() && matches(plan) && codec_.muteAndVerify(plan);}
    bool configureMuted(const HdaDellTonePlan& plan){return active_ && !quarantined_ && matches(plan) && p_.command()==2 && codec_.configureMuted(plan);}
    bool prepareDma(const HdaDellTonePlan& plan) {
        uint32_t v=0;prepareStage_=1;
        if(!active_ || quarantined_ || !matches(plan) || p_.command()!=2)return false;
        prepareStage_=2;if(!ici_.setFormat(0x11))return false;
        prepareStage_=3;if(!ici_.getFormat(v))return false;prepareValue_=v;if(v!=0x11)return false;
        prepareStage_=4;if(!ici_.set8(3,0x706,0x10))return false;
        prepareStage_=5;if(!ici_.get8(3,0xf06,v))return false;prepareValue_=v;if(v!=0x10)return false;
        prepareStage_=6;if(!stream_.prepare(saved_.controller.gcap,phys_[0]))return false;
        prepareStage_=7;return true;
    }
    bool startDma() {
        if(!active_ || quarantined_ || p_.command()!=2)return false;
        if(!p_.publishDma())return false; // flush and fence before bus mastering
        p_.setCommand(6);if(p_.command()!=6)return false;
        return stream_.start();
    }
    bool checkProgress(){return active_ && !quarantined_ && p_.command()==6 && stream_.progress();}
    bool unmuteAndVerify(const HdaDellTonePlan& plan){return active_ && !quarantined_ && matches(plan) && p_.command()==6 && codec_.unmuteAndVerify(plan);}
    bool playBounded() {
        const uint64_t start=p_.ticks();playReason_=1;
        for(unsigned i=0;i<300000;++i) {
            playLoops_=i+1;if(!active_ || quarantined_ || p_.command()!=6){playReason_=2;return false;}
            if(!stream_.progress()){const auto now=p_.ticks();if(now>=start)playElapsed_=now-start;playReason_=3;return false;}++playProgress_;
            const uint64_t now=p_.ticks();if(now>=start){playElapsed_=now-start;if(playElapsed_>=2000){playReason_=4;return true;}}
        }
        playReason_=5;return false; // stalled clock or progress cannot authorize more sound
    }
    bool stopAndVerify() {
        if(!decoded() || !stream_.stop())return false;
        p_.setCommand(2);return p_.command()==2;
    }
    bool disconnectAndVerify(const HdaDellTonePlan&) {
        uint32_t v=0;return active_ && !quarantined_ && p_.command()==2 && ici_.set8(3,0x706,0) && ici_.get8(3,0xf06,v) && v==0;
    }
    bool restoreAndVerify(const HdaDellTonePlan& plan,const HdaSessionReport& saved) {
        uint32_t v=0;return active_ && !quarantined_ && &saved==&saved_ && matches(plan) && p_.command()==2 && ici_.setFormat(uint16_t(saved.converterFormat)) && ici_.getFormat(v) &&
            v==saved.converterFormat && codec_.restoreAndVerify(plan,saved);
    }
    bool releaseStoppedDma() {
        if(!active_ || quarantined_ || p_.command()!=2 || !stream_.retire())return false;
        freePages();return true;
    }
    void quarantine(){quarantined_=true;p_.quarantine();}
    // Called only after a fully successful transaction. An uncertain command
    // readback retains controller ownership; no command is blindly retried.
    bool finish() {
        if(quarantined_ || !active_)return false;
        if(count_ || p_.command()!=2){quarantine();return false;}
        p_.setCommand(0);if(p_.command()!=0){quarantine();return false;}active_=false;return true;
    }
};
}
