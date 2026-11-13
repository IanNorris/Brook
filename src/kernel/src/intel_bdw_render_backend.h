#pragma once
#include "intel_bdw_render_oneshot.h"
#include "intel_bdw_ggtt_mmio.h"
#include "intel_bdw_bcs_mmio.h"
#include "intel_bdw_csb.h"
#if defined(BROOK_GPU_SUBMISSION_TIMING)
#include "intel_bdw_submission_timing.h"
#endif

namespace brook {
// Adapts the RCS transaction to an already-held device owner and three
// already-audited UC mappings. It does not acquire forcewake, establish
// DMA/IOMMU safety, install render default state, serialize GT reset, or
// perform failure recovery. Those are mandatory owner responsibilities.
template<class Owner> class BdwRenderDeviceBackend final : public BdwRenderExecutionBackend {
    Owner& owner_;uint32_t first_,contextId_;uint64_t token_,scratch_;BdwOwnedPage* pages_=nullptr;
    BdwOwnedPage* extra_=nullptr;unsigned extraCount_=0;BdwOwnedPage extraEvidence_[128]{};
    uint32_t ggttWrites_=0,portWrites_=0;bool resetDone_=false;const BdwOwnedPage* hws_=nullptr;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    uint64_t issuedTicks_=0,breadcrumbTicks_=0;
#endif
    BdwGgttMmio<BdwRenderDeviceBackend> ggtt_;
    BdwRcsMmio<BdwRenderDeviceBackend> rcs_;
public:
    BdwRenderDeviceBackend(Owner& owner,volatile uint64_t* ptes,volatile uint32_t* flush,
                           volatile uint32_t* engine,uint32_t first,uint64_t token,uint64_t scratch,uint32_t contextId=1)
        :owner_(owner),first_(first),contextId_(contextId),token_(token),scratch_(scratch),
         ggtt_(*this,ptes,flush,first,29),rcs_(*this,engine){}
    BdwRenderDeviceBackend(const BdwRenderDeviceBackend&)=delete;
    BdwRenderDeviceBackend& operator=(const BdwRenderDeviceBackend&)=delete;
    bool owned(){return owner_.owned();}
    bool readable(){return owned();}
    bool exclusiveHandoff(){return owned();}
    bool renderStateReady(){return owned() && owner_.renderStateReady();}
    bool gpuPatLease(){return owned() && owner_.gpuPatLease();}
    bool preparePrivatePat(uint64_t descriptor){return gpuPatLease() && owner_.preparePrivatePat(descriptor);}
    bool readPrivatePat(uint32_t& lo,uint32_t& hi){return gpuPatLease() && owner_.readPrivatePat(lo,hi);}
    bool admitPages(BdwOwnedPage* pages,unsigned count){
        if(pages_ || !pages || count!=37 || !owner_.admitPages(pages,count))return false;
        pages_=pages;return true;
    }
    bool publish(BdwOwnedPage* pages,unsigned count){return pages==pages_ && count==37 && owned() && owner_.publish(pages,count);}
    bool admitExtraTables(BdwOwnedPage* pages,unsigned count){
        if(!owned() || !pages_ || extra_ || !pages || !count || count>128)return false;
        for(unsigned i=0;i<count;++i){
            if(!pages[i].cpu || !pages[i].dma || pages[i].dma==scratch_)return false;
            for(unsigned j=0;j<37;++j)if(pages[i].dma==pages_[j].dma || pages[i].cpu==pages_[j].cpu)return false;
            for(unsigned j=0;j<i;++j)if(pages[i].dma==pages[j].dma || pages[i].cpu==pages[j].cpu)return false;
        }
        if(!owner_.admitExtraTables(pages,count))return false;
        for(unsigned i=0;i<count;++i)extraEvidence_[i]=pages[i];
        extra_=pages;extraCount_=count;return true;
    }
    bool publishExtraTables(BdwOwnedPage* pages,unsigned count){
        if(!owned() || pages!=extra_ || count!=extraCount_)return false;
        for(unsigned i=0;i<count;++i)if(pages[i].dma!=extraEvidence_[i].dma || pages[i].cpu!=extraEvidence_[i].cpu)return false;
        return owner_.publishExtraTables(pages,count);
    }
    bool validateLease(const BdwGgttLease& lease){
        if(!owned() || !pages_ || !token_ || lease.token!=token_ || lease.count!=29)return false;
        for(unsigned i=0;i<28;++i)if(lease.index[i]!=first_+i || lease.dma[i]!=pages_[i].dma)return false;
        if(lease.index[28]!=first_+28 || lease.dma[28]!=pages_[36].dma)return false;
        return owner_.validateLease(lease);
    }
    bool validateTakeover(const BdwGgttLease& lease,uint64_t scratch){
        return validateLease(lease) && scratch && scratch==scratch_ && owner_.validateTakeover(lease,scratch);
    }
    bool quiescent(){return owned() && owner_.quiescent();}
    bool read(uint32_t index,uint64_t& value){return ggtt_.read(index,value);}
    bool write(uint32_t index,uint64_t value){++ggttWrites_;return ggtt_.write(index,value);}
    bool invalidate(){return ggtt_.invalidate();}
    bool invalidateGpuTranslations(){return owned() && owner_.invalidateGpuTranslations();}
    bool initializeExeclists(uint64_t address,const BdwOwnedPage& hws){
        if(!pages_ || &hws!=&pages_[24] || address!=(uint64_t(first_)+24)*4096 || !quiescent())return false;
        hws_=&hws;resetDone_=owner_.resetRcs();
        // Reset may invalidate engine workarounds. The owner must restore
        // them before asserting renderStateReady on return to the transaction.
        return resetDone_ && rcs_.initializeExeclists(address) && owner_.restoreRenderState();
    }
    bool resetVerified(){return resetDone_ && owned();}
    bool csbPrepared(uint64_t hws){return resetVerified() && hws_ && hws==(uint64_t(first_)+24)*4096;}
    bool prepareCsbMemory(uint64_t hws){return csbPrepared(hws) && BdwCsbInitializeForReset(hws_->cpu,[&](uint8_t*){return owner_.syncForDevice(*hws_);});}
    bool csbMemoryProducer(uint64_t hws,uint32_t& producer){return csbPrepared(hws) && owner_.syncForCpu(*hws_) && BdwCsbReadProducer(hws_->cpu,producer);}
    bool submissionReady(uint64_t descriptor){return resetVerified() && gpuPatLease() && renderStateReady() &&
        contextId_ && contextId_<=0x1fffff && descriptor==((uint64_t(contextId_)<<32)|(uint64_t(first_)<<12)|0x13d);}
    bool prepareSubmission(uint64_t descriptor){return rcs_.prepareSubmission(descriptor);}
    bool submitDword(uint32_t value){++portWrites_;const bool ok=rcs_.submitDword(value);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
        if(ok && portWrites_==4)issuedTicks_=BdwTimingNow();
#endif
        return ok;}
    uint32_t ggttWriteAttempts()const{return ggttWrites_;}
    uint32_t portWriteAttempts()const{return portWrites_;}
    bool waitCompletion(uint32_t nonce){
        if(!pages_ || !nonce)return false;
        while(owner_.completionContinue()){
            if(!exclusiveHandoff() || !gpuPatLease() || !owner_.syncForCpu(pages_[25]))return false;
            if(__atomic_load_n(reinterpret_cast<const uint32_t*>(pages_[25].cpu),__ATOMIC_RELAXED)==nonce){
#if defined(BROOK_GPU_SUBMISSION_TIMING)
                breadcrumbTicks_=BdwTimingNow();
#endif
                return true;}
            owner_.relax();
        }
        return false;
    }
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    void timingTicks(uint64_t& issued,uint64_t& breadcrumb)const{issued=issuedTicks_;breadcrumb=breadcrumbTicks_;}
#endif
    bool cpuVisible(BdwOwnedPage* pages,unsigned count){
        if(pages!=pages_ || count!=37 || !owned())return false;
        for(unsigned i=0;i<count;++i)if(!owner_.syncForCpu(pages[i]))return false;
        return true;
    }
    bool csbReadTail(BdwOwnedPage& hws,uint32_t& tail){
        return &hws==hws_ && owned() && owner_.syncForCpu(hws) && BdwCsbReadProducer(hws.cpu,tail);
    }
    bool csbReadEntry(BdwOwnedPage& hws,unsigned slot,uint64_t& entry){
        return &hws==hws_ && owned() && owner_.syncForCpu(hws) && BdwCsbReadRecord(hws.cpu,slot,entry);
    }
    bool csbAcknowledge(unsigned slot){return slot<6 && owned() && owner_.acknowledgeCsb(slot);}
    bool retirementContinue(){return owned() && owner_.retirementContinue();}
};
}
