#pragma once
#include "intel_bdw_ggtt_bind.h"
#include "intel_bdw_submission.h"
#include "intel_bdw_ppgtt.h"
#include "intel_bdw_pat.h"
namespace brook {
// Boot-lifetime owner for a single planned BCS copy. A backend must implement
// real DMA identity, page pinning, exclusive handoff, PAT verification, cache
// publication, ordered MMIO and deadline/CPU visibility. No backend is
// provided here and preparation alone never publishes a GPU address.
struct BdwOwnedPage {uint64_t dma=0;uint8_t* cpu=nullptr;};
// A concrete device owner implements this interface while holding its GPU
// lease. The retained allocator remains private to the preparation module;
// the interface allows its pinned pages to be used without exposing or
// copying the one-shot object across the VMM boundary.
struct BdwBcsExecutionBackend {
    virtual bool exclusiveHandoff()=0;
    virtual bool execlistReady()=0;
    virtual bool gpuPatLease()=0;
    virtual bool preparePrivatePat(uint64_t)=0;
    virtual bool readPrivatePat(uint32_t&,uint32_t&)=0;
    virtual bool admitPages(BdwOwnedPage*,unsigned)=0;
    virtual bool publish(BdwOwnedPage*,unsigned)=0;
    virtual bool validateLease(const BdwGgttLease&)=0;
    virtual bool validateTakeover(const BdwGgttLease&,uint64_t)=0;
    virtual bool quiescent()=0;
    virtual bool read(uint32_t,uint64_t&)=0;
    virtual bool write(uint32_t,uint64_t)=0;
    virtual bool invalidate()=0;
    virtual bool programHws(uint64_t)=0;
    virtual bool initializeExeclists(uint64_t,const BdwOwnedPage&)=0;
    virtual bool prepareSubmission(uint64_t)=0;
    virtual bool submitDword(uint32_t)=0;
    virtual bool waitCompletion(uint32_t)=0;
    virtual bool cpuVisible(BdwOwnedPage*,unsigned)=0;
    virtual bool csbReadTail(BdwOwnedPage&,uint32_t&)=0;
    virtual bool csbReadEntry(BdwOwnedPage&,unsigned,uint64_t&)=0;
    virtual bool csbAcknowledge(unsigned)=0;
    // Monotonic deadline and a bounded iteration budget belong to the owner.
    virtual bool retirementContinue()=0;
};
struct BdwBcsRunResult {
    bool attempted=false,copyComplete=false,contextComplete=false;
    bool resetBeforeAttempted=false,resetBefore=false,patEstablished=false,patUncertain=false,resetAfter=false,patRestored=false;
    bool scanoutRetired=false;
    uint32_t ggttWriteAttempts=0,portWriteAttempts=0;
    bool engineSampled=false;
    uint32_t head=0,tail=0,mode=0,control=0,acthd=0,ipeir=0,ipehr=0,execlist=0;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    uint64_t runTicks=0; // diagnostic TSC interval of the prepared blit run
#endif
};
// Caller must serialize all operations; intermediate states reject reentrant callbacks.
enum class BdwOneShotState : uint8_t {Fresh,Preparing,Prepared,Executing,CompleteRetained,Polling,ContextCompleteRetained,Quarantined,StoppedRetained,Released};
enum class BdwCsbPoll : uint8_t {Pending,Complete,Fault};
constexpr uint64_t BdwBcsSourceAddress=0x02000000, BdwBcsScanoutAddress=0x04000000;
constexpr uint32_t BdwBcsOperandPages=8192; // one 32 MiB virtual interval per operand
constexpr unsigned BdwBcsLegacyPrivatePages=19;
constexpr unsigned BdwBcsMaxPrivatePages=BdwBcsLegacyPrivatePages+2*(BdwBcsOperandPages/512);
class BdwBcsOneShot {
    enum : unsigned {Ctx0,Ctx1,Ring0,Ring1,Ring2,Ring3,Hws,Completion,Batch,Source,Destination,
        Pml4,Pdpt,Pd,Pt,ScratchData,ScratchPt,ScratchPd,ScratchPdpt,Count};
    static constexpr unsigned ExtraTables=2*(BdwBcsOperandPages/512);
    static_assert(Count==BdwBcsLegacyPrivatePages);
    BdwOwnedPage pages_[BdwBcsMaxPrivatePages]{};unsigned allocated_=0;
    BdwOneShotState state_=BdwOneShotState::Fresh;const void* owner_=nullptr;const void* executionOwner_=nullptr;
    BdwGgttLease lease_{};BdwGgttBind bind_{};uint64_t descriptor_=0;uint32_t nonce_=0;bool attempted_=false,scanout_=false;
    uint8_t csbHead_=5;bool csbActivated_=false;
    template<class Io> void ReleasePages(Io& io){while(allocated_)io.free(pages_[--allocated_]);}
    static void Zero(BdwOwnedPage& p){for(unsigned i=0;i<4096;++i)p.cpu[i]=0;}
    static void Store32(BdwOwnedPage& p,unsigned offset,uint32_t v){for(unsigned i=0;i<4;++i)p.cpu[offset+i]=uint8_t(v>>(8*i));}
    static uint32_t Load32(const BdwOwnedPage& p,unsigned offset){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p.cpu[offset+i])<<(8*i);return v;}
    static uint8_t Pattern(unsigned i){return uint8_t((i*37+13)^(i>>8));}
    template<class Io> bool Allocate(Io& io,uint32_t firstIndex,uint64_t token,uint32_t nonce,unsigned count) {
        if(state_!=BdwOneShotState::Fresh || !token || !nonce || firstIndex<65536 || firstIndex>1048576-8)return false;
        owner_=&io;state_=BdwOneShotState::Preparing;
        for(unsigned i=0;i<count;++i) {
            BdwOwnedPage p{};if(!io.alloc(p)){ReleasePages(io);state_=BdwOneShotState::Released;return false;}
            pages_[allocated_++]=p;
            if(!p.cpu || (reinterpret_cast<uintptr_t>(p.cpu)&4095) || !p.dma || (p.dma&4095) || p.dma>=(uint64_t{1}<<39) ||
               (p.dma<0xdf800000ull && p.dma+4096>0xdd000000ull)) {state_=BdwOneShotState::Quarantined;return false;}
            for(unsigned j=0;j<i;++j)if(p.dma==pages_[j].dma || p.cpu==pages_[j].cpu){state_=BdwOneShotState::Quarantined;return false;}
        }
        for(unsigned i=0;i<allocated_;++i)Zero(pages_[i]);
        // Gen8 HWS uses six 64-bit CSB records at dword 0x10 and the
        // producer index at dword 0x1f. Mark records unused and point to
        // the last slot so the first produced status is consumed at zero.
        // This prepares private RAM only: hardware pointers, cache
        // publication and engine/CSB ownership still require a backend.
        for(unsigned i=0x40;i<0x70;++i)pages_[Hws].cpu[i]=0xff;
        Store32(pages_[Hws],0x7c,5);
        lease_={};lease_.token=token;lease_.count=8;
        for(unsigned i=0;i<8;++i){lease_.index[i]=firstIndex+i;lease_.dma[i]=pages_[i].dma;}
        nonce_=nonce;
        return true;
    }
    template<class Io> bool PrepareFailed(Io& io){ReleasePages(io);state_=BdwOneShotState::Released;return false;}
    bool BuildTables(const BdwPpgttMapping* maps,uint32_t count,bool sorted) {
        // Execute verifies the private PAT selected by these mappings.
        const BdwPpgttPage scratch[]={{pages_[ScratchData].dma,pages_[ScratchData].cpu},
            {pages_[ScratchPt].dma,pages_[ScratchPt].cpu},{pages_[ScratchPd].dma,pages_[ScratchPd].cpu},
            {pages_[ScratchPdpt].dma,pages_[ScratchPdpt].cpu}};
        BdwPpgttPage tables[4+ExtraTables]={{pages_[Pml4].dma,pages_[Pml4].cpu},{pages_[Pdpt].dma,pages_[Pdpt].cpu},
            {pages_[Pd].dma,pages_[Pd].cpu},{pages_[Pt].dma,pages_[Pt].cpu}};
        for(unsigned i=Count;i<allocated_;++i)tables[4+i-Count]={pages_[i].dma,pages_[i].cpu};
        return BdwBuildPpgttImage(scratch,tables,4+allocated_-Count,maps,count,sorted);
    }
    BdwBcsLaunch Launch()const{
        const uint64_t ggtt=uint64_t(lease_.index[0])*4096;
        return {{ggtt,ggtt+8192,pages_[Pml4].dma,16384,56,1},0x10000,ggtt+7*4096,nonce_};
    }
    void StoreImages(const uint32_t (&staging)[2048],const uint32_t* batch,unsigned words,const uint32_t (&ring)[14]){
        for(unsigned i=0;i<1024;++i){Store32(pages_[Ctx0],i*4,staging[i]);Store32(pages_[Ctx1],i*4,staging[1024+i]);}
        for(unsigned i=0;i<words;++i)Store32(pages_[Batch],i*4,batch[i]);
        for(unsigned i=0;i<14;++i)Store32(pages_[Ring0],i*4,ring[i]);
        state_=BdwOneShotState::Prepared;
    }
public:
    BdwBcsOneShot()=default;BdwBcsOneShot(const BdwBcsOneShot&)=delete;BdwBcsOneShot& operator=(const BdwBcsOneShot&)=delete;
    BdwBcsOneShot(BdwBcsOneShot&&)=delete;BdwBcsOneShot& operator=(BdwBcsOneShot&&)=delete;
    BdwOneShotState State()const{return state_;}
    bool ScratchNeverPublished()const{return bind_.State()==BdwGgttBindState::Fresh;}
    bool DistinctPage(const BdwOwnedPage& page) const {
        if(state_!=BdwOneShotState::Prepared || !page.cpu || !page.dma)return false;
        for(unsigned i=0;i<allocated_;++i)
            if(pages_[i].cpu==page.cpu || pages_[i].dma==page.dma)return false;
        return true;
    }
    template<class Io> bool Prepare(Io& io,uint32_t firstIndex,uint64_t token,uint32_t nonce,uint32_t (&staging)[2048]) {
        if(!Allocate(io,firstIndex,token,nonce,Count))return false;
        const BdwPpgttMapping maps[]={{0x10000,pages_[Batch].dma,false},
            {0x11000,pages_[Source].dma,false},{0x12000,pages_[Destination].dma,true}};
        if(!BuildTables(maps,3,false))return PrepareFailed(io);
        for(unsigned i=0;i<4096;++i){pages_[Source].cpu[i]=Pattern(i);pages_[Destination].cpu[i]=0xa5;}
        const BdwBlitRegion region{{0x11000,4096,64},{0x12000,4096,64},0,0,0,0,16,4};
        uint32_t batch[12]{},ring[14]{};
        if(!BdwBuildBcsRegions(Launch(),&region,1,staging,batch,ring,descriptor_))return PrepareFailed(io);
        StoreImages(staging,batch,12,ring);
        return true;
    }
    // Operands remain pinned by the caller until the engine and mappings
    // retire. The framebuffer range must already have live GOP admission.
    template<class Io> bool PrepareScanout(Io& io,uint32_t first,uint64_t token,uint32_t nonce,
            const uint64_t* source,uint32_t sourceCount,uint64_t scanout,uint32_t scanoutCount,
            const BdwBlitRegion* regions,uint32_t count,BdwPpgttMapping* maps,uint32_t mapCapacity,uint32_t (&staging)[2048]){
        if(!source || !sourceCount || sourceCount>BdwBcsOperandPages || !scanoutCount ||
           scanoutCount>BdwBcsOperandPages || !maps || !regions || !count || count>BdwBcsMaxRegions ||
           mapCapacity<1+sourceCount+scanoutCount ||
           scanout<0xdd000000ull || scanout>0xdf800000ull-uint64_t(scanoutCount)*4096 || (scanout&4095))return false;
        const uint64_t sourceBytes=uint64_t(sourceCount)*4096,scanoutBytes=uint64_t(scanoutCount)*4096;
        for(uint32_t i=0;i<count;++i){
            const auto& r=regions[i];
            if(r.source.gpuAddress<BdwBcsSourceAddress || r.source.gpuAddress>=BdwBcsSourceAddress+sourceBytes ||
               r.source.allocationBytes>BdwBcsSourceAddress+sourceBytes-r.source.gpuAddress ||
               r.destination.gpuAddress!=BdwBcsScanoutAddress || r.destination.allocationBytes!=scanoutBytes)return false;
        }
        const unsigned extra=(sourceCount+511)/512+(scanoutCount+511)/512;
        if(!Allocate(io,first,token,nonce,Count+extra))return false;
        maps[0]={0x10000,pages_[Batch].dma,false};
        for(uint32_t i=0;i<sourceCount;++i){
            const uint64_t dma=source[i];
            if(dma>=0xdd000000ull && dma<0xdf800000ull)return PrepareFailed(io);
            maps[i+1]={BdwBcsSourceAddress+uint64_t(i)*4096,dma,false};
        }
        for(uint32_t i=0;i<scanoutCount;++i)
            maps[1+sourceCount+i]={BdwBcsScanoutAddress+uint64_t(i)*4096,scanout+uint64_t(i)*4096,true,BdwPpgttCache::Uncached};
        uint32_t batch[BdwBcsBatchWords]{},ring[14]{};
        if(!BuildTables(maps,1+sourceCount+scanoutCount,true) ||
           !BdwBuildBcsRegions(Launch(),regions,count,staging,batch,ring,descriptor_))return PrepareFailed(io);
        scanout_=true;
        StoreImages(staging,batch,count*10+2,ring);
        return true;
    }
    template<class Io> bool ReleasePrepared(Io& io) {
        if(owner_!=static_cast<const void*>(&io) ||
           (state_!=BdwOneShotState::Prepared && state_!=BdwOneShotState::StoppedRetained) || !bind_.SafeToRelease())return false;
        state_=BdwOneShotState::Released;ReleasePages(io);return true;
    }
    // Private CPU writeback only; no GGTT binding or GPU submission.
    template<class Io> bool PublishPrepared(Io& io) {
        if(owner_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared)return false;
        state_=BdwOneShotState::Executing;
        const bool ok=io.publish(pages_,allocated_);
        state_=BdwOneShotState::Prepared;return ok;
    }
    // A diagnostic of the retained pages and a fresh platform snapshot. This
    // is not a DMA lease: the backend must still secure translation, firmware
    // and device ownership through the entire GPU operation.
    template<class Io> uint32_t InspectPreparedDma(Io& io) {
        if(owner_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared)return 1;
        return io.inspectDma(pages_,allocated_);
    }
    // A bounded one-shot: no engine write before all backend handoff proofs,
    // publication and binding. HWS may be consumed even if its write reports
    // failure, so retain all pages from the first attempted engine write on.
private:
    template<class Allocator,class Io,class Binder,class EngineStart> bool ExecuteImpl(Allocator& allocator,Io& io,bool takeover,Binder bind,EngineStart start) {
        if(owner_!=static_cast<const void*>(&allocator) || state_!=BdwOneShotState::Prepared || attempted_)return false;
        attempted_=true;state_=BdwOneShotState::Executing;
        // Admission must establish pinned, distinct usable RAM, DMA translation
        // and exclusion of all firmware and device-owned regions for all pages.
        // Idle alone does not imply execlist mode: the Dell has been observed
        // with BCS idle in legacy submission mode. The backend must establish
        // the mode and a usable, owned context-status buffer. An explicit
        // takeover binds the status page first, then initializes the engine;
        // the existing path still requires execlist mode already active.
        uint32_t patLo=0,patHi=0,patLo2=0,patHi2=0;
        if(!io.exclusiveHandoff() || (!takeover && !io.execlistReady()) || !io.admitPages(pages_,allocated_) ||
           !io.gpuPatLease() || !io.preparePrivatePat(descriptor_) || !io.readPrivatePat(patLo,patHi) || !io.readPrivatePat(patLo2,patHi2) ||
           patLo==0xffffffffu || patHi==0xffffffffu || patLo!=patLo2 || patHi!=patHi2 ||
           !BdwLegacyBcsPatCompatible(descriptor_,patLo,patHi,scanout_) || !io.publish(pages_,allocated_)) {
            state_=BdwOneShotState::Prepared;return false;
        }
        // Firmware may leave nonzero entries even in an otherwise unclaimed
        // range. Snapshot them only after the backend has established its
        // exclusive lease, and require a second identical read. Binding checks
        // them again before its first write. The ordinary path refuses values
        // outside the 39-bit encoding because it would replay them; takeover
        // may compare them but restores only the backend's pinned scratch PTE.
        if(!io.validateLease(lease_) || !io.quiescent()) {state_=BdwOneShotState::Prepared;return false;}
        uint64_t previous[8]{};
        for(unsigned i=0;i<8;++i)
            if(!io.read(lease_.index[i],previous[i]) || (!takeover && (previous[i]>>39))) {
                state_=BdwOneShotState::Prepared;return false;
            }
        for(unsigned i=0;i<8;++i){uint64_t second=0;
            if(!io.read(lease_.index[i],second) || second!=previous[i]) {
                state_=BdwOneShotState::Prepared;return false;
            }
        }
        for(unsigned i=0;i<8;++i)lease_.expected[i]=previous[i];
        if(!bind(lease_)) {state_=bind_.SafeToRelease()?BdwOneShotState::Prepared:BdwOneShotState::Quarantined;return false;}
        executionOwner_=static_cast<const void*>(&io);state_=BdwOneShotState::Quarantined;
        // The policy and exclusive owner must survive binding, setup and all
        // four ELSP writes; a point-in-time preflight is not a lifetime lease.
        if(!io.exclusiveHandoff() || !io.gpuPatLease())return false;
        if(!start(uint64_t(lease_.index[Hws])*4096))return false;
        if(!io.prepareSubmission(descriptor_))return false;
        const uint32_t words[]={0,0,uint32_t(descriptor_>>32),uint32_t(descriptor_)};
        for(auto word:words)if(!io.exclusiveHandoff() || !io.gpuPatLease() || !io.submitDword(word))return false;
        if(!io.exclusiveHandoff() || !io.gpuPatLease() || !io.waitCompletion(nonce_) ||
           !io.exclusiveHandoff() || !io.gpuPatLease() || (!scanout_ && !io.cpuVisible(pages_,allocated_)) ||
           Load32(pages_[Completion],0)!=nonce_)return false;
        for(unsigned i=0;!scanout_ && i<4096;++i) {
            if(pages_[Source].cpu[i]!=Pattern(i))return false;
            if(pages_[Destination].cpu[i]!=(i<256?pages_[Source].cpu[i]:0xa5))return false;
        }
        // A breadcrumb precedes arbitration re-enable and context save. No
        // retirement proof exists yet: retain all mappings even on success.
        state_=BdwOneShotState::CompleteRetained;return true;
    }
public:
    // The hardware owner calls this only after an acknowledged BCS domain
    // reset. A failed or uncertain unbind retains every referenced page.
    template<class Io> bool RetireScanout(Io& io){
        if(!scanout_ || (executionOwner_ && executionOwner_!=static_cast<const void*>(&io)))return false;
        if(bind_.State()==BdwGgttBindState::Bound && !bind_.RetireToInvalid(io))return false;
        if(!bind_.SafeToRelease())return false;
        state_=BdwOneShotState::StoppedRetained;
        return true;
    }
    bool Recycle(){
        if(state_!=BdwOneShotState::Released || !bind_.Recycle())return false;
        state_=BdwOneShotState::Fresh;owner_=executionOwner_=nullptr;
        descriptor_=0;nonce_=0;attempted_=scanout_=csbActivated_=false;csbHead_=5;
        return true;
    }
    template<class Io> bool Execute(Io& io) {
        return ExecuteWithBackend(io,io);
    }
    // The allocator owns the retained pages; a separate persistent backend
    // may own the device transaction. Both identities remain fixed until
    // retirement, and the backend must independently admit the exact pages.
    template<class Allocator,class Io> bool ExecuteWithBackend(Allocator& allocator,Io& io) {
        return ExecuteImpl(allocator,io,false,[&](const BdwGgttLease& lease){return bind_.Bind(lease,io);},
            [&](uint64_t hws){return io.programHws(hws);});
    }
    // scratchDma belongs to the backend, not this object's releasable pages.
    // validateTakeover must reserve it for as long as its PTEs may survive,
    // even if this call fails or the one-shot's other pages are released.
    template<class Io> bool ExecuteTakenOver(Io& io,uint64_t scratchDma) {
        return ExecuteTakenOverWithBackend(io,io,scratchDma);
    }
    template<class Allocator,class Io> bool ExecuteTakenOverWithBackend(Allocator& allocator,Io& io,uint64_t scratchDma) {
        // The lease covers only the first eight pages. The scratch page must
        // also be distinct from the PPGTT hierarchy, batch and copy buffers:
        // a speculative or stray GGTT access must not alter those objects.
        if(owner_!=static_cast<const void*>(&allocator) || state_!=BdwOneShotState::Prepared ||
           !scratchDma || (scratchDma&4095) || scratchDma>=(uint64_t{1}<<39))return false;
        for(unsigned i=0;i<allocated_;++i)if(pages_[i].dma==scratchDma)return false;
        return ExecuteImpl(allocator,io,true,[&](const BdwGgttLease& lease){return bind_.BindTakenOver(lease,io,scratchDma);},
            [&](uint64_t hws){return io.initializeExeclists(hws,static_cast<const BdwOwnedPage&>(pages_[Hws]));});
    }
    // One exclusive descriptor from an initially idle six-slot CSB. The
    // backend must have synchronized the hardware/software cursors at five,
    // retain ownership, and provide ordered device-to-CPU reads and pointer
    // acknowledgements. A poison record is left pending for a later retry;
    // callers must impose their own deadline. Even successful context switch
    // completion does not release the HWS, GGTT bindings or pages.
    template<class Io> BdwCsbPoll PollContextSwitch(Io& io) {
        if(executionOwner_!=static_cast<const void*>(&io))return BdwCsbPoll::Fault;
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
            if(!csbActivated_) {
                // Idle-to-active makes every other bit, including ID, invalid.
                if(!(event&1)){state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;}
                csbActivated_=true;
            } else {
                // Bits 11..15 only have meaning for other switch reasons.
                if((uint32_t(event)&0xffff07ffu)!=0x18 || uint32_t(event>>32)!=uint32_t(descriptor_>>32) || next!=tail) {
                    state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;
                }
                terminal=true;
            }
            if(!io.exclusiveHandoff() || !io.gpuPatLease() || !io.csbAcknowledge(next)){state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;}
            csbHead_=next;
            if(terminal) {
                state_=BdwOneShotState::ContextCompleteRetained;return BdwCsbPoll::Complete;
            }
        }
        state_=BdwOneShotState::CompleteRetained;return BdwCsbPoll::Pending;
    }
};
}
