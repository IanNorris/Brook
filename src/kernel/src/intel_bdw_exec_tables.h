#pragma once
#include "intel_bdw_exec_snapshot.h"
#include "intel_bdw_oneshot.h"

namespace brook {
// Owns the variable number of private PPGTT pages for one submission. This
// supplies memory and lifetime only: its owner must exclude CPU writers,
// inspect every page for DMA/IOMMU safety, publish caches, own the device and
// stop all GPU access before calling ReleaseStopped. The allocator and this
// object must outlive the submitted work, including failures.
class BdwExecTables {
    static constexpr uint32_t Max=128;
    BdwOwnedPage pages_[Max]{};BdwPpgttPage tables_[Max]{};
    uint32_t count_=0;const void* allocator_=nullptr;const BdwExecSnapshot* snapshot_=nullptr;bool built_=false,dmaChecked_=false;
    enum class State:uint8_t { Fresh,Prepared,Published,InUse,Released,Quarantined } state_=State::Fresh;
    template<class Allocator> void Free(Allocator& a){while(count_)a.free(pages_[--count_]);}
public:
    BdwExecTables()=default;
    BdwExecTables(const BdwExecTables&)=delete;
    BdwExecTables& operator=(const BdwExecTables&)=delete;
    bool Released()const{return state_==State::Released;}
    bool Recycle(){
        if(state_!=State::Released || count_)return false;
        allocator_=nullptr;snapshot_=nullptr;built_=dmaChecked_=false;state_=State::Fresh;return true;
    }
    template<class Allocator> bool Prepare(Allocator& a,const BdwExecSnapshot* snapshot){
        uint32_t count=0;if(state_!=State::Fresh || !BdwExecSnapshotTableCount(snapshot,count) || !count || count>Max)return false;
        allocator_=&a;snapshot_=snapshot;
        for(uint32_t i=0;i<count;++i){
            BdwOwnedPage p{};if(!a.alloc(p)){Free(a);state_=State::Released;return false;}
            pages_[count_++]=p;
            if(!p.cpu || (reinterpret_cast<uintptr_t>(p.cpu)&4095) || !p.dma || (p.dma&4095) ||
               p.dma>=(uint64_t{1}<<39) || (p.dma<0xdf800000ull && p.dma+4096>0xdd000000ull)){
                state_=State::Quarantined;return false;
            }
            for(uint32_t j=0;j<i;++j)if(p.cpu==pages_[j].cpu || p.dma==pages_[j].dma){state_=State::Quarantined;return false;}
            tables_[i]={p.dma,p.cpu};
        }
        state_=State::Prepared;return true;
    }
    template<class Allocator,class Visitor> bool Inspect(Allocator& a,Visitor visit){
        return allocator_==static_cast<const void*>(&a) && state_==State::Prepared && visit(static_cast<const BdwOwnedPage*>(pages_),count_);
    }
    // Point-in-time evidence only. The owner must keep page and IOMMU state
    // stable through submission; this does not authorize device DMA.
    template<class Allocator> bool InspectDma(Allocator& a,uint32_t& status){
        if(allocator_!=static_cast<const void*>(&a) || state_!=State::Prepared || !built_)return false;
        dmaChecked_=false;status=a.inspectDma(pages_,count_);
        dmaChecked_=(status==0);return dmaChecked_;
    }
    template<class Allocator> bool Build(Allocator& a,BdwExecSnapshot* snapshot,const BdwPpgttPage (&scratch)[4],
            BdwPpgttMapping* workspace,uint32_t workspaceCount,uint64_t completion,uint32_t nonce,bool gt3,uint32_t (&ring)[56]){
        if(allocator_!=static_cast<const void*>(&a) || state_!=State::Prepared || snapshot_!=snapshot)return false;
        built_=false;dmaChecked_=false;
        built_=BdwExecSnapshotBuildRenderImage(snapshot,scratch,tables_,count_,workspace,workspaceCount,completion,nonce,gt3,ring);
        return built_;
    }
    template<class Allocator> bool Publish(Allocator& a){
        if(allocator_!=static_cast<const void*>(&a) || state_!=State::Prepared || !built_ || !dmaChecked_ || !a.publish(pages_,count_))return false;
        state_=State::Published;return true;
    }
    uint64_t RootDma()const{return (state_==State::Prepared || state_==State::Published || state_==State::InUse)?pages_[0].dma:0;}
    template<class Allocator> bool BeginUse(Allocator& a){
        if(allocator_!=static_cast<const void*>(&a) || state_!=State::Published)return false;
        state_=State::InUse;return true;
    }
    // Called only under the same device lease as the render transaction.
    template<class Backend> bool AdmitAndPublish(Backend& backend){
        return state_==State::InUse && backend.admitExtraTables(pages_,count_) &&
            backend.publishExtraTables(pages_,count_);
    }
    template<class Allocator> bool ReleaseUnsubmitted(Allocator& a){
        if(allocator_!=static_cast<const void*>(&a) || (state_!=State::Prepared && state_!=State::Published))return false;
        state_=State::Released;Free(a);return true;
    }
    template<class Allocator> bool ReleaseStopped(Allocator& a,bool verifiedStopped){
        if(allocator_!=static_cast<const void*>(&a) || state_!=State::InUse || !verifiedStopped)return false;
        state_=State::Released;Free(a);return true;
    }
};
}
