#pragma once
#include "intel_bdw_render_oneshot.h"
#include "intel_bdw_ggtt_mmio.h"
#include "intel_bdw_bcs_mmio.h"
#include "intel_bdw_csb.h"
#include "intel_bdw_hang_diag.h"
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
    BdwOwnedPage* extra_=nullptr;unsigned extraCount_=0;BdwOwnedPage extraEvidence_[BdwPpgttMaxTables+4]{};
    uint32_t ggttWrites_=0,portWrites_=0;bool resetDone_=false;const BdwOwnedPage* hws_=nullptr;
    // Hang diagnostics (intel_bdw_hang_diag.h); both optional.
    BdwRenderJobDiag* diag_=nullptr;BdwSubmitSampler* sampler_=nullptr;
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
    void attachDiag(BdwRenderJobDiag* diag,BdwSubmitSampler* sampler){diag_=diag;sampler_=sampler;}
    // Read-only views for the hang capture: one-shot page i after a CPU
    // cache flush, and the GGTT PTE / expected DMA of lease entry i (0..27
    // are pages 0..27, 28 is the batch page 36).
    const uint8_t* diagPage(unsigned i){
        if(!pages_ || i>=37 || !owned() || !owner_.syncForCpu(pages_[i]))return nullptr;
        return pages_[i].cpu;
    }
    bool diagPte(unsigned i,uint64_t& value){return i<29 && pages_ && ggtt_.read(first_+i,value);}
    uint64_t diagDma(unsigned i)const{return pages_ && i<29?pages_[i<28?i:36].dma:0;}
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
        if(!owned() || !pages_ || extra_ || !pages || !count || count>BdwPpgttMaxTables+4)return false;
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
        auto stage=[&](uint8_t s){if(diag_){diag_->initStage=s;diag_->initEndTsc=BdwHangTsc();
            const auto d=rcs_.InitDiag();diag_->execInitStage=d.stage;diag_->execInitValue=d.value;}return s==0;};
        if(!pages_ || &hws!=&pages_[24] || address!=(uint64_t(first_)+24)*4096)return stage(1);
        if(!quiescent())return stage(2);
        hws_=&hws;resetDone_=owner_.resetRcs();
        if(!resetDone_)return stage(3);
        // Reset may invalidate engine workarounds. The owner must restore
        // them before asserting renderStateReady on return to the transaction.
        if(!rcs_.initializeExeclists(address))return stage(4);
        if(!owner_.restoreRenderState())return stage(5);
        return stage(0);
    }
    bool resetVerified(){return resetDone_ && owned();}
    bool csbPrepared(uint64_t hws){return resetVerified() && hws_ && hws==(uint64_t(first_)+24)*4096;}
    bool prepareCsbMemory(uint64_t hws){return csbPrepared(hws) && BdwCsbInitializeForReset(hws_->cpu,[&](uint8_t*){return owner_.syncForDevice(*hws_);});}
    bool csbMemoryProducer(uint64_t hws,uint32_t& producer){return csbPrepared(hws) && owner_.syncForCpu(*hws_) && BdwCsbReadProducer(hws_->cpu,producer);}
    bool submissionReady(uint64_t descriptor){return resetVerified() && gpuPatLease() && renderStateReady() &&
        contextId_ && contextId_<=0x1fffff && descriptor==((uint64_t(contextId_)<<32)|(uint64_t(first_)<<12)|0x13d);}
    bool prepareSubmission(uint64_t descriptor){return rcs_.prepareSubmission(descriptor);}
    bool submitDword(uint32_t value){
        if(diag_ && !portWrites_){
            diag_->descriptor=(uint64_t(contextId_)<<32)|(uint64_t(first_)<<12)|0x13d;
            if(pages_){const auto* regs=reinterpret_cast<const uint32_t*>(pages_[1].cpu); // CTX_RING_HEAD/TAIL
                diag_->ringHead=__atomic_load_n(regs+5,__ATOMIC_RELAXED);diag_->ringTail=__atomic_load_n(regs+7,__ATOMIC_RELAXED);
                // Ring dword 33: flags of the PIPE_CONTROL before the batch; a
                // post-sync write there is the "started" marker.
                diag_->marker=(__atomic_load_n(reinterpret_cast<const uint32_t*>(pages_[20].cpu)+33,__ATOMIC_RELAXED)&(1u<<14))!=0;}
            if(sampler_)sampler_->sampleBefore(diag_->before);
        }
        ++portWrites_;const bool ok=rcs_.submitDword(value);
        if(diag_){diag_->portWrites=uint8_t(portWrites_);
            if(ok && portWrites_==4){diag_->elspTsc=BdwHangTsc();if(sampler_)sampler_->sampleAfter(diag_->after);}}
#if defined(BROOK_GPU_SUBMISSION_TIMING)
        if(ok && portWrites_==4)issuedTicks_=BdwTimingNow();
#endif
        return ok;}
    uint32_t ggttWriteAttempts()const{return ggttWrites_;}
    uint32_t portWriteAttempts()const{return portWrites_;}
    uint32_t completionSeen_=0,completionPolls_=0;
    void completionDiag(uint32_t& seen,uint32_t& polls)const{seen=completionSeen_;polls=completionPolls_;}
    bool waitCompletion(uint32_t nonce){
        if(!pages_ || !nonce)return false;
        while(owner_.completionContinue()){
            if(!exclusiveHandoff() || !gpuPatLease() || !owner_.syncForCpu(pages_[25]))return false;
            ++completionPolls_;
            completionSeen_=__atomic_load_n(reinterpret_cast<const uint32_t*>(pages_[25].cpu),__ATOMIC_RELAXED);
            if(diag_){const uint64_t now=BdwHangTsc();
                if(!diag_->firstPollTsc)diag_->firstPollTsc=now;
                else if(now>diag_->lastPollTsc && now-diag_->lastPollTsc>diag_->maxPollGapTicks)diag_->maxPollGapTicks=now-diag_->lastPollTsc;
                diag_->lastPollTsc=now;diag_->seen=completionSeen_;diag_->polls=completionPolls_;
                if(completionSeen_==nonce)diag_->breadcrumbTsc=now;}
            if(completionSeen_==nonce){
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
        if(diag_ && !diag_->firstCsbTsc)diag_->firstCsbTsc=BdwHangTsc();
        return &hws==hws_ && owned() && owner_.syncForCpu(hws) && BdwCsbReadRecord(hws.cpu,slot,entry);
    }
    bool csbAcknowledge(unsigned slot){return slot<6 && owned() && owner_.acknowledgeCsb(slot);}
    bool retirementContinue(){return owned() && owner_.retirementContinue();}
};
}
