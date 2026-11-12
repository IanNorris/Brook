#pragma once
#include "intel_bdw_oneshot.h"
#include "intel_bdw_render_plan.h"
#include "intel_bdw_renderstate.h"
#include "intel_bdw_render_ring.h"
#include "intel_bdw_exec_snapshot.h"
#include "intel_bdw_exec_tables.h"

namespace brook {
// Device ownership is held outside this interface. Keeping the interface
// separate from the allocator permits the retained object to cross the VMM
// critical-section boundary without transferring or copying page ownership.
struct BdwRenderExecutionBackend {
    virtual bool exclusiveHandoff()=0;
    virtual bool renderStateReady()=0;
    virtual bool admitPages(BdwOwnedPage*,unsigned)=0;
    // Extra private page tables must have been pinned and DMA-inspected by
    // the outer owner before entering the VMM critical section.
    virtual bool admitExtraTables(BdwOwnedPage*,unsigned){return false;}
    virtual bool publishExtraTables(BdwOwnedPage*,unsigned){return false;}
    virtual bool gpuPatLease()=0;
    virtual bool preparePrivatePat(uint64_t)=0;
    virtual bool readPrivatePat(uint32_t&,uint32_t&)=0;
    virtual bool publish(BdwOwnedPage*,unsigned)=0;
    virtual bool validateLease(const BdwGgttLease&)=0;
    virtual bool validateTakeover(const BdwGgttLease&,uint64_t)=0;
    virtual bool quiescent()=0;
    virtual bool read(uint32_t,uint64_t&)=0;
    virtual bool write(uint32_t,uint64_t)=0;
    virtual bool invalidate()=0;
    virtual bool initializeExeclists(uint64_t,const BdwOwnedPage&)=0;
    virtual bool prepareSubmission(uint64_t)=0;
    virtual bool submitDword(uint32_t)=0;
    virtual bool waitCompletion(uint32_t)=0;
    virtual bool cpuVisible(BdwOwnedPage*,unsigned)=0;
    virtual bool csbReadTail(BdwOwnedPage&,uint32_t&)=0;
    virtual bool csbReadEntry(BdwOwnedPage&,unsigned,uint64_t&)=0;
    virtual bool csbAcknowledge(unsigned)=0;
    virtual bool retirementContinue()=0;
};
// A private, single-use render command. This is an engine bring-up workload,
// not an application 3D draw. The backend must own the render engine and
// forcewake, establish the engine/GT workarounds and MCR steering required to
// bootstrap the context, pin the PAT and DMA mappings, and recover from
// uncertain execution before reuse. The null-state batch and context WA are
// submitted here; a later owner must still verify and save the resulting
// context before treating it as an application default. The object cannot
// establish those ownership or recovery guarantees by itself.
class BdwRenderOneShot {
    enum : unsigned { Hws=24, Completion=25, Wa=26, WaScratch=27,
        ScratchData=28, ScratchPt=29, ScratchPd=30, ScratchPdpt=31,
        Pml4=32, Pdpt=33, Pd=34, Pt=35, Batch=36, Count=37 };
    BdwOwnedPage pages_[Count]{};unsigned allocated_=0;
    BdwOneShotState state_=BdwOneShotState::Fresh;
    const void* allocator_=nullptr;const void* backend_=nullptr;
    BdwRenderPlan plan_{};BdwGgttBind bind_{};uint32_t nonce_=0;
    uint8_t csbHead_=5;bool activated_=false,attempted_=false,storeProbe_=false,gt3_=false;
    BdwExecSnapshot* execSnapshot_=nullptr;BdwExecTables* execExternal_=nullptr;
    bool execPublished_=false,execArmed_=false,execDetached_=false;
    template<class Io> void freePages(Io& io){while(allocated_)io.free(pages_[--allocated_]);}
    static void store(BdwOwnedPage& p,unsigned offset,uint32_t value){
        for(unsigned i=0;i<4;++i)p.cpu[offset+i]=uint8_t(value>>(8*i));
    }
    static uint32_t load(const BdwOwnedPage& p,unsigned offset){
        uint32_t value=0;for(unsigned i=0;i<4;++i)value|=uint32_t(p.cpu[offset+i])<<(8*i);return value;
    }
public:
    BdwRenderOneShot()=default;
    BdwRenderOneShot(const BdwRenderOneShot&)=delete;
    BdwRenderOneShot& operator=(const BdwRenderOneShot&)=delete;
    BdwOneShotState State()const{return state_;}
    // The caller serializes reuse and separately releases external tables.
    // No in-flight, quarantined, or partially owned image can be recycled.
    template<class Allocator> bool Recycle(Allocator& io){
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Released || allocated_ || execSnapshot_ ||
           (execExternal_ && !execExternal_->Released()) || !bind_.Recycle())return false;
        allocator_=backend_=nullptr;execExternal_=nullptr;plan_={};nonce_=0;csbHead_=5;
        activated_=attempted_=storeProbe_=gt3_=false;execPublished_=execArmed_=execDetached_=false;
        state_=BdwOneShotState::Fresh;return true;
    }
    bool ScratchNeverPublished()const{return bind_.State()==BdwGgttBindState::Fresh;}
    bool DistinctPage(const BdwOwnedPage& p)const {
        if(state_!=BdwOneShotState::Prepared || !p.cpu || !p.dma)return false;
        for(unsigned i=0;i<allocated_;++i)if(p.cpu==pages_[i].cpu || p.dma==pages_[i].dma)return false;
        return true;
    }
    template<class Allocator> bool Prepare(Allocator& io,uint32_t first,uint64_t token,uint32_t nonce,
                                           uint32_t (&staging)[20*1024],bool gt3=false,
                                           const uint32_t (*saved)[20*1024]=nullptr,uint32_t contextId=1,
                                           bool ppgttProbe=false,bool storeProbe=false) {
        if(state_!=BdwOneShotState::Fresh || !token || !nonce || first<65536 || first>1048576-29)return false;
        if(storeProbe && !ppgttProbe)return false;
        allocator_=&io;state_=BdwOneShotState::Preparing;
        for(unsigned i=0;i<Count;++i) {
            BdwOwnedPage p{};
            if(!io.alloc(p)){freePages(io);state_=BdwOneShotState::Released;return false;}
            pages_[allocated_++]=p;
            if(!p.cpu || (reinterpret_cast<uintptr_t>(p.cpu)&4095) || !p.dma || (p.dma&4095) ||
               p.dma>=(uint64_t{1}<<39) || (p.dma<0xdf800000ull && p.dma+4096>0xdd000000ull)) {
                state_=BdwOneShotState::Quarantined;return false;
            }
            for(unsigned j=0;j<i;++j)if(p.dma==pages_[j].dma || p.cpu==pages_[j].cpu){
                state_=BdwOneShotState::Quarantined;return false;
            }
            for(unsigned j=0;j<4096;++j)p.cpu[j]=0;
        }
        const BdwPpgttPage scratch[]={{pages_[ScratchData].dma,pages_[ScratchData].cpu},
            {pages_[ScratchPt].dma,pages_[ScratchPt].cpu},{pages_[ScratchPd].dma,pages_[ScratchPd].cpu},
            {pages_[ScratchPdpt].dma,pages_[ScratchPdpt].cpu}};
        const BdwPpgttPage tables[]={{pages_[Pml4].dma,pages_[Pml4].cpu},{pages_[Pdpt].dma,pages_[Pdpt].cpu},
            {pages_[Pd].dma,pages_[Pd].cpu},{pages_[Pt].dma,pages_[Pt].cpu}};
        const BdwPpgttMapping map[]={{0x10000,pages_[Batch].dma,false},{0x11000,pages_[WaScratch].dma,true}};
        uint64_t dma[28]{},expected[28]{};for(unsigned i=0;i<28;++i)dma[i]=pages_[i].dma;
        auto& wa=*reinterpret_cast<uint32_t (*)[1024]>(pages_[Wa].cpu);
        if(!BdwBuildPpgttImage(scratch,tables,4,map,storeProbe?2:1) ||
           !BdwBuildRenderPlan(first,token,contextId,224,pages_[Pml4].dma,dma,expected,staging,wa,plan_,saved)) {
            freePages(io);state_=BdwOneShotState::Released;return false;
        }
        const uint64_t batchGgtt=(uint64_t(first)+28)*4096;
        auto& nullState=*reinterpret_cast<uint32_t (*)[1024]>(pages_[Batch].cpu);
        if(ppgttProbe){
            // Optional private GPU store targets offset zero of WaScratch;
            // the workaround uses its separate region at offset 256.
            if(storeProbe){nullState[0]=0x10000002;nullState[1]=0x11000;
                nullState[2]=0;nullState[3]=nonce;nullState[4]=0x05000000;}
            else nullState[0]=0x05000000;
        } else if(!BdwBuildRenderNullState(batchGgtt,nullState)) {
            freePages(io);state_=BdwOneShotState::Released;return false;
        }
        // The null-state batch references its own GGTT allocation, and Linux
        // dispatches it as a secure GGTT batch. Keep that page in the same
        // lease as the context, ring and status pages.
        plan_.lease.index[28]=first+28;plan_.lease.dma[28]=pages_[Batch].dma;
        plan_.lease.expected[28]=0;plan_.lease.count=29;
        for(unsigned i=0;i<20;++i)for(unsigned j=0;j<4096;++j)
            pages_[i].cpu[j]=reinterpret_cast<const uint8_t*>(staging)[i*4096+j];
        for(unsigned i=0x40;i<0x70;++i)pages_[Hws].cpu[i]=0xff;
        store(pages_[Hws],0x7c,5);
        // Gen8 render invalidation uses PIPE_CONTROL and context-HWSP
        // scratch, unlike the blitter's MI_FLUSH_DW. Follow it with a
        // privileged GGTT null-state batch, then a separate flush and GGTT
        // qword breadcrumb. The breadcrumb is not a context retirement proof.
        uint32_t ring[56]{};
        if(!BdwBuildRenderRing(ppgttProbe?0x10000:batchGgtt,plan_.completionGgtt,nonce,ppgttProbe,gt3,ring)) {
            freePages(io);state_=BdwOneShotState::Released;return false;
        }
        for(unsigned i=0;i<56;++i)store(pages_[20],4*i,ring[i]);
        nonce_=nonce;storeProbe_=storeProbe;gt3_=gt3;state_=BdwOneShotState::Prepared;return true;
    }
    template<class Allocator> bool ReleasePrepared(Allocator& io) {
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared || !bind_.SafeToRelease() || execSnapshot_)return false;
        state_=BdwOneShotState::Released;freePages(io);return true;
    }
    template<class Allocator> bool PublishPrepared(Allocator& io) {
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared || execArmed_)return false;
        state_=BdwOneShotState::Executing;const bool ok=io.publish(pages_,Count);state_=BdwOneShotState::Prepared;
        if(execSnapshot_)execPublished_=ok;
        return ok;
    }
    // Attach a caller-retained snapshot to this private one-shot. The caller
    // owns it until explicitly detached before arming; after arming it must
    // retain it through a proven reset/retirement. This does not validate
    // user commands or grant permission to submit them.
    template<class Allocator> bool AttachSnapshot(Allocator& io,BdwExecSnapshot* snapshot,
                                                   BdwPpgttMapping* workspace,uint32_t workspaceCount) {
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared || attempted_ ||
           execSnapshot_ || execDetached_ || !snapshot)return false;
        const BdwPpgttPage scratch[]={{pages_[ScratchData].dma,pages_[ScratchData].cpu},
            {pages_[ScratchPt].dma,pages_[ScratchPt].cpu},{pages_[ScratchPd].dma,pages_[ScratchPd].cpu},
            {pages_[ScratchPdpt].dma,pages_[ScratchPdpt].cpu}};
        const BdwPpgttPage tables[]={{pages_[Pml4].dma,pages_[Pml4].cpu},{pages_[Pdpt].dma,pages_[Pdpt].cpu},
            {pages_[Pd].dma,pages_[Pd].cpu},{pages_[Pt].dma,pages_[Pt].cpu}};
        auto& ring=*reinterpret_cast<uint32_t (*)[56]>(pages_[20].cpu);
        if(!BdwExecSnapshotBuildRenderImage(snapshot,scratch,tables,4,workspace,workspaceCount,
                                           plan_.completionGgtt,nonce_,gt3_,ring))return false;
        // The caller checks the attached snapshot's result after retirement;
        // the private WaScratch store no longer belongs to this image.
        storeProbe_=false;execSnapshot_=snapshot;execPublished_=false;return true;
    }
    template<class Allocator> bool ArmSnapshot(Allocator& io) {
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared || attempted_ ||
           !execSnapshot_ || !execPublished_ || execArmed_ || execExternal_)return false;
        execArmed_=BdwExecSnapshotArm(execSnapshot_,true);return execArmed_;
    }
    // Build a variable-size address space into separately retained tables and
    // point this private context at its root. The caller retains the tables,
    // snapshot and their allocators through GPU retirement or recovery.
    template<class Allocator,class TableAllocator> bool AttachExternalSnapshot(Allocator& io,BdwExecTables& external,
            TableAllocator& tableAllocator,BdwExecSnapshot* snapshot,BdwPpgttMapping* workspace,uint32_t workspaceCount){
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared || attempted_ ||
           execSnapshot_ || execDetached_ || !snapshot)return false;
        if(!external.Inspect(tableAllocator,[&](const BdwOwnedPage* tables,uint32_t count){
            for(uint32_t i=0;i<count;++i)for(unsigned j=0;j<Count;++j)
                if(tables[i].dma==pages_[j].dma || tables[i].cpu==pages_[j].cpu)return false;
            return true;
        }))return false;
        const BdwPpgttPage scratch[]={{pages_[ScratchData].dma,pages_[ScratchData].cpu},
            {pages_[ScratchPt].dma,pages_[ScratchPt].cpu},{pages_[ScratchPd].dma,pages_[ScratchPd].cpu},
            {pages_[ScratchPdpt].dma,pages_[ScratchPdpt].cpu}};
        auto& ring=*reinterpret_cast<uint32_t (*)[56]>(pages_[20].cpu);
        const uint64_t root=external.RootDma();
        if(!root || !external.Build(tableAllocator,snapshot,scratch,workspace,workspaceCount,
                                    plan_.completionGgtt,nonce_,gt3_,ring))return false;
        store(pages_[1],49*4,uint32_t(root>>32));store(pages_[1],51*4,uint32_t(root));
        storeProbe_=false;execSnapshot_=snapshot;execPublished_=false;execExternal_=&external;return true;
    }
    // A failed arm leaves the external pages retained in use; the owner must
    // establish that nothing was submitted before releasing them.
    template<class Allocator,class TableAllocator> bool ArmExternalSnapshot(Allocator& io,TableAllocator& tableAllocator){
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared || attempted_ ||
           !execExternal_ || !execSnapshot_ || !execPublished_ || execArmed_ || !execExternal_->BeginUse(tableAllocator))return false;
        execArmed_=BdwExecSnapshotArm(execSnapshot_,true);return execArmed_;
    }
    template<class Allocator> BdwExecSnapshot* DetachSnapshot(Allocator& io) {
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared || attempted_ || execArmed_)return nullptr;
        auto* snapshot=execSnapshot_;execSnapshot_=nullptr;execPublished_=false;
        if(snapshot)execDetached_=true;
        return snapshot;
    }
    // Access for the owner to validate its exact allocation and reservation;
    // the returned pointers are valid only while this object remains retained.
    template<class Allocator,class Visitor> bool InspectPrepared(Allocator& io,Visitor visitor) {
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared)return false;
        return visitor(static_cast<const BdwOwnedPage*>(pages_),Count,static_cast<const BdwRenderPlan&>(plan_));
    }
    // The backend owns the exact reserved range and separately pinned scratch
    // page, and maintains those guarantees across every callback. Once a PTE
    // or engine operation is attempted, failures retain affected pages until
    // the owner performs a proven reset/recovery; timeout is not retirement.
    template<class Allocator,class Backend> bool ExecuteTakenOver(Allocator& allocator,Backend& io,uint64_t scratchDma) {
        if(allocator_!=static_cast<const void*>(&allocator) || state_!=BdwOneShotState::Prepared || attempted_ ||
           execDetached_ || (execSnapshot_ && !execArmed_) ||
           !scratchDma || (scratchDma&4095) || scratchDma>=(uint64_t{1}<<39))return false;
        for(unsigned i=0;i<Count;++i)if(scratchDma==pages_[i].dma)return false;
        attempted_=true;state_=BdwOneShotState::Executing;
        uint32_t lo=0,hi=0,lo2=0,hi2=0;
        if(!io.exclusiveHandoff() || !io.renderStateReady() || !io.admitPages(pages_,Count) ||
           (execExternal_ && !execExternal_->AdmitAndPublish(io)) ||
           !io.gpuPatLease() || !io.preparePrivatePat(plan_.descriptor) ||
           !io.readPrivatePat(lo,hi) || !io.readPrivatePat(lo2,hi2) || lo!=lo2 || hi!=hi2 ||
           !BdwLegacyBcsPatCompatible(plan_.descriptor,lo,hi) || !io.publish(pages_,Count) ||
           !io.validateLease(plan_.lease) || !io.quiescent()) {
            state_=BdwOneShotState::Prepared;return false;
        }
        for(unsigned i=0;i<plan_.lease.count;++i)if(!io.read(plan_.lease.index[i],plan_.lease.expected[i])){
            state_=BdwOneShotState::Prepared;return false;
        }
        for(unsigned i=0;i<plan_.lease.count;++i){uint64_t second=0;
            if(!io.read(plan_.lease.index[i],second) || second!=plan_.lease.expected[i]){
                state_=BdwOneShotState::Prepared;return false;
            }
        }
        if(!bind_.BindTakenOver(plan_.lease,io,scratchDma)){
            state_=bind_.SafeToRelease()?BdwOneShotState::Prepared:BdwOneShotState::Quarantined;return false;
        }
        backend_=static_cast<const void*>(&io);state_=BdwOneShotState::Quarantined;
        // initializeExeclists may reset RCS, so its owner must re-establish
        // any reset-sensitive render state before the second readiness check.
        if(!io.exclusiveHandoff() || !io.gpuPatLease() || !io.renderStateReady() ||
           !io.initializeExeclists(plan_.hwsGgtt,static_cast<const BdwOwnedPage&>(pages_[Hws])) ||
           !io.renderStateReady() || !io.prepareSubmission(plan_.descriptor))return false;
        const uint32_t words[]={0,0,uint32_t(plan_.descriptor>>32),uint32_t(plan_.descriptor)};
        for(auto word:words)if(!io.exclusiveHandoff() || !io.gpuPatLease() || !io.submitDword(word))return false;
        if(!io.exclusiveHandoff() || !io.gpuPatLease() || !io.waitCompletion(nonce_) ||
           !io.exclusiveHandoff() || !io.gpuPatLease() || !io.cpuVisible(pages_,Count) ||
           load(pages_[Completion],0)!=nonce_ || load(pages_[Completion],4)!=0 ||
           (storeProbe_ && load(pages_[WaScratch],0)!=nonce_))return false;
        state_=BdwOneShotState::CompleteRetained;return true;
    }
    template<class Backend> BdwCsbPoll PollContextSwitch(Backend& io) {
        if(backend_!=static_cast<const void*>(&io))return BdwCsbPoll::Fault;
        if(state_==BdwOneShotState::ContextCompleteRetained)return BdwCsbPoll::Complete;
        if(state_!=BdwOneShotState::CompleteRetained)return BdwCsbPoll::Fault;
        state_=BdwOneShotState::Polling;
        if(!io.exclusiveHandoff() || !io.gpuPatLease()){state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;}
        uint32_t tail=0;
        if(!io.csbReadTail(pages_[Hws],tail) || tail>=6){state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;}
        while(csbHead_!=tail) {
            if(!io.exclusiveHandoff() || !io.gpuPatLease()){state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;}
            const uint8_t next=(csbHead_+1)%6;uint64_t event=0;
            if(!io.csbReadEntry(pages_[Hws],next,event)){state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;}
            if(event==~uint64_t{0}){state_=BdwOneShotState::CompleteRetained;return BdwCsbPoll::Pending;}
            bool terminal=false;
            if(!activated_){if(!(event&1)){state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;}activated_=true;}
            else {
                if((uint32_t(event)&0xffff07ffu)!=0x18 || uint32_t(event>>32)!=uint32_t(plan_.descriptor>>32) || next!=tail){
                    state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;
                }
                terminal=true;
            }
            if(!io.exclusiveHandoff() || !io.gpuPatLease() || !io.csbAcknowledge(next)){
                state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;
            }
            csbHead_=next;
            if(terminal){state_=BdwOneShotState::ContextCompleteRetained;return BdwCsbPoll::Complete;}
        }
        state_=BdwOneShotState::CompleteRetained;return BdwCsbPoll::Pending;
    }
    // Copy a retired hardware-saved context into caller-owned private memory.
    // This is a snapshot, not an immediately reusable context: ring, HWS,
    // PPGTT and other per-context addresses must be rebuilt for each user.
    // The owner must continue holding the engine/PAT/page lease throughout.
    template<class Backend> bool CopyRetiredContext(Backend& io,uint32_t (&out)[20*1024]) {
        if(backend_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::ContextCompleteRetained)return false;
        const uintptr_t start=reinterpret_cast<uintptr_t>(out),size=sizeof(out);
        if(start>~uintptr_t{0}-size)return false;
        for(unsigned i=0;i<Count;++i){
            const uintptr_t p=reinterpret_cast<uintptr_t>(pages_[i].cpu);
            if(p>~uintptr_t{0}-4096 || (start<p+4096 && p<start+size))return false;
        }
        state_=BdwOneShotState::Polling;
        if(!io.exclusiveHandoff() || !io.gpuPatLease() || !io.cpuVisible(pages_,Count) ||
           !io.exclusiveHandoff() || !io.gpuPatLease()){
            state_=BdwOneShotState::Quarantined;return false;
        }
        auto* bytes=reinterpret_cast<uint8_t*>(out);
        for(unsigned i=0;i<20;++i)for(unsigned j=0;j<4096;++j)bytes[i*4096+j]=pages_[i].cpu[j];
        state_=BdwOneShotState::ContextCompleteRetained;return true;
    }
    // The caller must prove a successful engine reset, retain the exact
    // backend/lease and hold the forcewake/PAT ownership throughout. An
    // attempted or ambiguous clear retains all pages. This clears only this
    // takeover's PTEs; it does not establish cross-boot firmware exclusion.
    template<class Backend> bool RetireAfterReset(Backend& io,bool resetVerified) {
        if(!resetVerified || backend_!=static_cast<const void*>(&io) ||
           (state_!=BdwOneShotState::CompleteRetained && state_!=BdwOneShotState::ContextCompleteRetained &&
            state_!=BdwOneShotState::Quarantined))return false;
        state_=BdwOneShotState::Quarantined;
        if(!io.exclusiveHandoff() || !io.gpuPatLease() || !bind_.RetireToInvalid(io))return false;
        state_=BdwOneShotState::StoppedRetained;return true;
    }
    // Retain the snapshot until CPU visibility and release both succeed. The
    // owner must exclude concurrent CPU access and keep the device stopped;
    // the reset and invalidated PTEs alone do not establish those guarantees.
    template<class Allocator> bool ReleaseStoppedSnapshot(Allocator& io) {
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::StoppedRetained ||
           !execSnapshot_ || !bind_.SafeToRelease())return false;
        if(!BdwExecSnapshotSyncForCpu(execSnapshot_,true) || !BdwExecSnapshotRelease(execSnapshot_,true))return false;
        execSnapshot_=nullptr;return true;
    }
    // A serialized submission queue may retain this exact snapshot itself.
    // Transfer it back only after reset and mapping cleanup; its GPU-use
    // reservation remains active until the queue synchronizes and retires it.
    template<class Allocator> bool DetachStoppedSnapshot(Allocator& io,BdwExecSnapshot* expected){
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::StoppedRetained ||
           !expected || execSnapshot_!=expected || !bind_.SafeToRelease())return false;
        execSnapshot_=nullptr;return true;
    }
    template<class Allocator> bool ReleaseStopped(Allocator& io) {
        if(allocator_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::StoppedRetained ||
           execSnapshot_ || !bind_.SafeToRelease())return false;
        state_=BdwOneShotState::Released;freePages(io);return true;
    }
};
}
