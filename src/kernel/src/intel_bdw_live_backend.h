#pragma once
#include "intel_bdw_oneshot.h"
#include "intel_bdw_ggtt_mmio.h"
#include "intel_bdw_bcs_mmio.h"
#include "intel_bdw_csb.h"

namespace brook {
// Bridges one retained copy to an already-held device owner. Owner supplies
// the actual forcewake/engine/PAT lease, page admission, reset, cache
// synchronization, bounded clock and CSB register acknowledgement.
// This class alone neither grants ownership nor touches hardware until called.
template<class Owner> class BdwBcsDeviceBackend final : public BdwBcsExecutionBackend {
    Owner& owner_;uint32_t first_;uint64_t token_,scratch_;BdwOwnedPage* pages_=nullptr;unsigned pageCount_=0;
    uint32_t ggttWrites_=0,portWrites_=0;
    bool resetDone_=false;const BdwOwnedPage* hws_=nullptr;
    BdwGgttMmio<BdwBcsDeviceBackend> ggtt_;
    BdwBcsMmio<BdwBcsDeviceBackend> bcs_;
public:
    BdwBcsDeviceBackend(Owner& owner,volatile uint64_t* ptes,volatile uint32_t* flush,
                        volatile uint32_t* engine,uint32_t first,uint64_t token,uint64_t scratch)
        :owner_(owner),first_(first),token_(token),scratch_(scratch),ggtt_(*this,ptes,flush,first,8),bcs_(*this,engine){}
    BdwBcsDeviceBackend(const BdwBcsDeviceBackend&)=delete;
    BdwBcsDeviceBackend& operator=(const BdwBcsDeviceBackend&)=delete;
    bool owned(){return owner_.owned();}
    bool readable(){return owned();}
    bool exclusiveHandoff() override{return owned();}
    bool execlistReady() override{return false;} // takeover initializes it after binding
    bool gpuPatLease() override{return owned() && owner_.gpuPatLease();}
    bool preparePrivatePat(uint64_t descriptor) override{return gpuPatLease() && owner_.preparePrivatePat(descriptor);}
    bool readPrivatePat(uint32_t& lo,uint32_t& hi) override{return gpuPatLease() && owner_.readPrivatePat(lo,hi);}
    bool admitPages(BdwOwnedPage* pages,unsigned count) override {
        if(pages_ || !pages || count<BdwBcsLegacyPrivatePages || count>BdwBcsMaxPrivatePages ||
           !owner_.admitPages(pages,count))return false;
        pages_=pages;pageCount_=count;return true;
    }
    bool publish(BdwOwnedPage* pages,unsigned count) override {
        return pages==pages_ && count==pageCount_ && owned() && owner_.publish(pages,count);
    }
    bool validateLease(const BdwGgttLease& lease) override {
        if(!owned() || !pages_ || !token_ || lease.token!=token_ || lease.count!=8)return false;
        for(unsigned i=0;i<8;++i)if(lease.index[i]!=first_+i || lease.dma[i]!=pages_[i].dma)return false;
        return owner_.validateLease(lease);
    }
    bool validateTakeover(const BdwGgttLease& lease,uint64_t scratch) override {
        return validateLease(lease) && scratch && scratch==scratch_ && owner_.validateTakeover(lease,scratch);
    }
    bool quiescent() override{return owned() && owner_.quiescent();}
    bool read(uint32_t index,uint64_t& value) override{return ggtt_.read(index,value);}
    bool write(uint32_t index,uint64_t value) override{++ggttWrites_;return ggtt_.write(index,value);}
    bool invalidate() override{return ggtt_.invalidate();}
    bool invalidateGpuTranslations(){return owned() && owner_.invalidateGpuTranslations();}
    bool programHws(uint64_t) override{return false;}
    bool initializeExeclists(uint64_t address,const BdwOwnedPage& hws) override {
        if(!pages_ || &hws!=&pages_[6] || address!=(uint64_t(first_)+6)*4096 || !quiescent())return false;
        hws_=&hws;resetDone_=owner_.resetBcs();
        return resetDone_ && bcs_.initializeExeclists(address);
    }
    bool resetVerified(){return resetDone_ && owned();}
    bool csbPrepared(uint64_t hws){return resetVerified() && hws_ && hws==(uint64_t(first_)+6)*4096;}
    bool prepareCsbMemory(uint64_t hws){return csbPrepared(hws) && BdwCsbInitializeForReset(hws_->cpu,[&](uint8_t*){return owner_.syncForDevice(*hws_);});}
    bool csbMemoryProducer(uint64_t hws,uint32_t& producer){return csbPrepared(hws) && owner_.syncForCpu(*hws_) && BdwCsbReadProducer(hws_->cpu,producer);}
    bool submissionReady(uint64_t descriptor){return resetVerified() && gpuPatLease() && descriptor==((uint64_t{1}<<32)|(uint64_t(first_)<<12)|0x13d);}
    bool prepareSubmission(uint64_t descriptor) override{return bcs_.prepareSubmission(descriptor);}
    bool submitDword(uint32_t value) override{++portWrites_;return bcs_.submitDword(value);}
    uint32_t ggttWriteAttempts()const{return ggttWrites_;}
    uint32_t portWriteAttempts()const{return portWrites_;}
    bool waitCompletion(uint32_t nonce) override {
        if(!pages_ || !nonce)return false;
        while(owner_.completionContinue()) {
            if(!exclusiveHandoff() || !gpuPatLease() || !owner_.syncForCpu(pages_[7]))return false;
            const uint32_t value=__atomic_load_n(reinterpret_cast<const uint32_t*>(pages_[7].cpu),__ATOMIC_RELAXED);
            if(value==nonce)return true;
            owner_.relax();
        }
        return false;
    }
    bool cpuVisible(BdwOwnedPage* pages,unsigned count) override {
        if(pages!=pages_ || count!=pageCount_ || !owned())return false;
        for(unsigned i=0;i<count;++i)if(!owner_.syncForCpu(pages[i]))return false;
        return true;
    }
    bool csbReadTail(BdwOwnedPage& hws,uint32_t& tail) override {
        return &hws==hws_ && owned() && owner_.syncForCpu(hws) && BdwCsbReadProducer(hws.cpu,tail);
    }
    bool csbReadEntry(BdwOwnedPage& hws,unsigned slot,uint64_t& entry) override {
        return &hws==hws_ && owned() && owner_.syncForCpu(hws) && BdwCsbReadRecord(hws.cpu,slot,entry);
    }
    bool csbAcknowledge(unsigned slot) override{return slot<6 && owned() && owner_.acknowledgeCsb(slot);}
    bool retirementContinue() override{return owned() && owner_.retirementContinue();}
};
}
