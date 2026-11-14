#pragma once
#include "intel_bdw_exec_snapshot.h"
#include "intel_bdw_oneshot.h"

namespace brook {
// PPGTT table reuse counters (worker-thread writes; relaxed reads elsewhere).
struct BdwPpgttCacheStats {
    uint64_t builds,hits,missDisabled,missEmpty,missSignature,missChecksum,evictions,dropped;
    uint64_t buildUs,adoptUs,builtPages,lastBuildUs;
};
// Owns the variable number of private PPGTT pages for one submission. This
// supplies memory and lifetime only: its owner must exclude CPU writers,
// inspect every page for DMA/IOMMU safety, publish caches, own the device and
// stop all GPU access before calling ReleaseStopped. The allocator and this
// object must outlive the submitted work, including failures.
//
// Optional reuse (SetCacheEnabled): a table set built with its own scratch
// pages is kept after a verified stop, together with the exact expanded
// mapping list it was built from and a checksum of every table and scratch
// word. A later snapshot adopts it only if BdwExecSnapshotMatchesMappings
// proves the same addresses, DMA pages, permissions and cache policy, and the
// checksum proves no word changed, i.e. rebuilding would produce the same
// bytes. Between jobs the tables are not reachable by the GPU (each job loads
// its root explicitly and the RCS is reset after it), and adoption requires
// the new job to pin exactly the mapped pages, which the caller's per-job
// buffer ownership and DMA inspection still verify. The pages are always
// re-inspected and republished for each job.
class BdwExecTables {
public:
    static constexpr uint32_t Max=128, ScratchPages=4, CacheSlots=4;
    using ListFree=void(*)(BdwPpgttMapping*);
private:
    struct Entry {BdwOwnedPage pages[Max+ScratchPages];uint32_t count=0,tables=0;BdwPpgttMapping* list=nullptr;
        uint32_t listCount=0;uint64_t checksum=0,lastUse=0;bool valid=false;};
    BdwOwnedPage pages_[Max+ScratchPages]{};BdwPpgttPage tables_[Max]{};
    uint32_t count_=0,tableCount_=0;const void* allocator_=nullptr;const BdwExecSnapshot* snapshot_=nullptr;bool built_=false,dmaChecked_=false;
    bool ownScratch_=false,reused_=false;BdwPpgttMapping* list_=nullptr;uint32_t listCount_=0;uint64_t checksum_=0;
    Entry cache_[CacheSlots];uint64_t clock_=0;bool cacheEnabled_=false;ListFree listFree_=nullptr;
    BdwPpgttCacheStats stats_{};
    enum class State:uint8_t { Fresh,Prepared,Published,InUse,Released,Quarantined } state_=State::Fresh;
    template<class Allocator> void Free(Allocator& a){while(count_)a.free(pages_[--count_]);tableCount_=0;}
    void FreeList(){if(list_ && listFree_)listFree_(list_);list_=nullptr;listCount_=0;}
    template<class Allocator> void DropEntry(Allocator& a,Entry& e){
        for(uint32_t i=0;i<e.count;++i)a.free(e.pages[i]);
        if(e.list && listFree_)listFree_(e.list);e=Entry{};
    }
    static uint64_t Checksum(const BdwOwnedPage* pages,uint32_t count){
        uint64_t h=0x9e3779b97f4a7c15ull;
        for(uint32_t p=0;p<count;++p){const auto* w=reinterpret_cast<const uint64_t*>(pages[p].cpu);
            for(unsigned i=0;i<512;++i)h^=w[i]+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);
            h^=pages[p].dma;}
        return h;
    }
    template<class Allocator> bool Allocate(Allocator& a,uint32_t count){
        for(uint32_t i=0;i<count;++i){
            BdwOwnedPage p{};if(!a.alloc(p)){Free(a);state_=State::Released;return false;}
            pages_[count_++]=p;
            if(!p.cpu || (reinterpret_cast<uintptr_t>(p.cpu)&4095) || !p.dma || (p.dma&4095) ||
               p.dma>=(uint64_t{1}<<39) || (p.dma<0xdf800000ull && p.dma+4096>0xdd000000ull)){
                state_=State::Quarantined;return false;
            }
            for(uint32_t j=0;j<i;++j)if(p.cpu==pages_[j].cpu || p.dma==pages_[j].dma){state_=State::Quarantined;return false;}
        }
        return true;
    }
    void MapTables(){const uint32_t base=ownScratch_?ScratchPages:0;
        for(uint32_t i=0;i<tableCount_;++i)tables_[i]={pages_[base+i].dma,pages_[base+i].cpu};}
public:
    BdwExecTables()=default;
    BdwExecTables(const BdwExecTables&)=delete;
    BdwExecTables& operator=(const BdwExecTables&)=delete;
    // Takes effect at the next Prepare; disabling drops cached sets there.
    void SetCacheEnabled(bool on,ListFree freeList){if(freeList)listFree_=freeList;__atomic_store_n(&cacheEnabled_,on,__ATOMIC_RELEASE);}
    bool CacheEnabled()const{return __atomic_load_n(&cacheEnabled_,__ATOMIC_ACQUIRE) && listFree_;}
    BdwPpgttCacheStats Stats()const{return stats_;}
    uint32_t CachedSets()const{uint32_t n=0;for(const auto& e:cache_)n+=e.valid;return n;}
    bool Reused()const{return reused_;}
    bool Released()const{return state_==State::Released;}
    bool Recycle(){
        if(state_!=State::Released || count_)return false;
        allocator_=nullptr;snapshot_=nullptr;built_=dmaChecked_=reused_=ownScratch_=false;state_=State::Fresh;return true;
    }
    template<class Allocator> bool Prepare(Allocator& a,const BdwExecSnapshot* snapshot){
        uint32_t count=0;if(state_!=State::Fresh || !BdwExecSnapshotTableCount(snapshot,count) || !count || count>Max)return false;
        allocator_=&a;snapshot_=snapshot;reused_=false;checksum_=0;
        // Inspection, admission and publication accept at most Max pages, so
        // a reusable set (tables plus its own scratch) must fit within Max.
        const bool enabled=CacheEnabled(),cache=enabled && count+ScratchPages<=Max;
        if(!enabled){
            bool any=false;for(auto& e:cache_)if(e.valid){DropEntry(a,e);any=true;}
            if(any)++stats_.dropped;
            ++stats_.missDisabled;
        } else {
            bool any=false,changed=false;
            for(auto& e:cache_){
                if(!e.valid)continue;any=true;
                if(e.tables!=count || !BdwExecSnapshotMatchesMappings(snapshot,e.list,e.listCount))continue;
                // A changed word means the set is no longer the image this
                // list builds; discard it and build afresh.
                if(Checksum(e.pages,e.count)!=e.checksum){DropEntry(a,e);++stats_.missChecksum;changed=true;break;}
                for(uint32_t i=0;i<e.count;++i)pages_[i]=e.pages[i];
                count_=e.count;tableCount_=e.tables;list_=e.list;listCount_=e.listCount;checksum_=e.checksum;
                e=Entry{};ownScratch_=true;reused_=true;MapTables();state_=State::Prepared;return true;
            }
            if(!changed){if(any)++stats_.missSignature;else ++stats_.missEmpty;}
        }
        ownScratch_=cache;tableCount_=count;
        if(!Allocate(a,count+(cache?ScratchPages:0)))return false;
        MapTables();state_=State::Prepared;return true;
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
    // Workspace may be null only when Reused(). After a fresh build with the
    // cache enabled the workspace becomes the retained mapping list; the
    // caller must then not free it (KeepsWorkspace() reports this).
    template<class Allocator> bool Build(Allocator& a,BdwExecSnapshot* snapshot,const BdwPpgttPage (&scratch)[4],
            BdwPpgttMapping* workspace,uint32_t workspaceCount,uint64_t completion,uint32_t nonce,bool gt3,uint32_t (&ring)[56]){
        if(allocator_!=static_cast<const void*>(&a) || state_!=State::Prepared || snapshot_!=snapshot)return false;
        built_=false;dmaChecked_=false;
        if(reused_){
            built_=BdwExecSnapshotAdoptRenderImage(snapshot,list_,listCount_,completion,nonce,gt3,ring);
            if(built_)++stats_.hits;
            return built_;
        }
        BdwPpgttPage own[4];
        if(ownScratch_)for(unsigned i=0;i<4;++i)own[i]={pages_[i].dma,pages_[i].cpu};
        built_=BdwExecSnapshotBuildRenderImage(snapshot,ownScratch_?own:scratch,tables_,tableCount_,workspace,workspaceCount,
                                               completion,nonce,gt3,ring);
        if(built_){++stats_.builds;stats_.builtPages+=workspaceCount;
            if(ownScratch_){list_=workspace;listCount_=workspaceCount;checksum_=Checksum(pages_,count_);}}
        return built_;
    }
    bool KeepsWorkspace()const{return ownScratch_ && !reused_ && list_;}
    void RecordTiming(uint64_t us){if(reused_)stats_.adoptUs+=us;else{stats_.buildUs+=us;stats_.lastBuildUs=us;}}
    template<class Allocator> bool Publish(Allocator& a){
        if(allocator_!=static_cast<const void*>(&a) || state_!=State::Prepared || !built_ || !dmaChecked_ || !a.publish(pages_,count_))return false;
        state_=State::Published;return true;
    }
    uint64_t RootDma()const{return (state_==State::Prepared || state_==State::Published || state_==State::InUse)?
        pages_[ownScratch_?ScratchPages:0].dma:0;}
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
        state_=State::Released;Free(a);FreeList();return true;
    }
    template<class Allocator> bool ReleaseStopped(Allocator& a,bool verifiedStopped){
        if(allocator_!=static_cast<const void*>(&a) || state_!=State::InUse || !verifiedStopped)return false;
        state_=State::Released;
        if(ownScratch_ && built_ && list_ && CacheEnabled() && Checksum(pages_,count_)==checksum_){
            Entry* slot=nullptr;
            for(auto& e:cache_)if(!e.valid){slot=&e;break;}
            if(!slot){slot=&cache_[0];for(auto& e:cache_)if(e.lastUse<slot->lastUse)slot=&e;DropEntry(a,*slot);++stats_.evictions;}
            for(uint32_t i=0;i<count_;++i)slot->pages[i]=pages_[i];
            slot->count=count_;slot->tables=tableCount_;slot->list=list_;slot->listCount=listCount_;
            slot->checksum=checksum_;slot->lastUse=++clock_;slot->valid=true;
            count_=0;tableCount_=0;list_=nullptr;listCount_=0;return true;
        }
        Free(a);FreeList();return true;
    }
};
}
