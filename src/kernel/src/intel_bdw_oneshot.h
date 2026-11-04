#pragma once
#include "intel_bdw_ggtt_bind.h"
#include "intel_bdw_submission.h"
namespace brook {
// Boot-lifetime owner for a single planned BCS copy. A backend must implement
// real DMA identity, page pinning, exclusive handoff, PAT verification, cache
// publication, ordered MMIO and deadline/CPU visibility. No backend is
// provided here and preparation alone never publishes a GPU address.
struct BdwOwnedPage {uint64_t dma=0;uint8_t* cpu=nullptr;};
// Caller must serialize all operations; intermediate states reject reentrant callbacks.
enum class BdwOneShotState : uint8_t {Fresh,Preparing,Prepared,Executing,CompleteRetained,Polling,ContextCompleteRetained,Quarantined,Released};
enum class BdwCsbPoll : uint8_t {Pending,Complete,Fault};
class BdwBcsOneShot {
    enum : unsigned {Ctx0,Ctx1,Ring0,Ring1,Ring2,Ring3,Hws,Completion,Batch,Source,Destination,
        Pml4,Pdpt,Pd,Pt,ScratchData,ScratchPt,ScratchPd,ScratchPdpt,Count};
    BdwOwnedPage pages_[Count]{};unsigned allocated_=0;
    BdwOneShotState state_=BdwOneShotState::Fresh;const void* owner_=nullptr;
    BdwGgttLease lease_{};BdwGgttBind bind_{};uint64_t descriptor_=0;uint32_t nonce_=0;bool attempted_=false;
    uint8_t csbHead_=5;bool csbActivated_=false;
    template<class Io> void ReleasePages(Io& io){while(allocated_)io.free(pages_[--allocated_]);}
    static void Zero(BdwOwnedPage& p){for(unsigned i=0;i<4096;++i)p.cpu[i]=0;}
    static void Store32(BdwOwnedPage& p,unsigned offset,uint32_t v){for(unsigned i=0;i<4;++i)p.cpu[offset+i]=uint8_t(v>>(8*i));}
    static uint32_t Load32(const BdwOwnedPage& p,unsigned offset){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p.cpu[offset+i])<<(8*i);return v;}
    static void Entry(BdwOwnedPage& p,unsigned index,uint64_t value){for(unsigned i=0;i<8;++i)p.cpu[index*8+i]=uint8_t(value>>(8*i));}
    static uint8_t Pattern(unsigned i){return uint8_t((i*37+13)^(i>>8));}
public:
    BdwBcsOneShot()=default;BdwBcsOneShot(const BdwBcsOneShot&)=delete;BdwBcsOneShot& operator=(const BdwBcsOneShot&)=delete;
    BdwBcsOneShot(BdwBcsOneShot&&)=delete;BdwBcsOneShot& operator=(BdwBcsOneShot&&)=delete;
    BdwOneShotState State()const{return state_;}
    // The candidate slots are not granted by this method. The actual backend
    // must validate the lease under exclusive ownership before Bind can write.
    template<class Io> bool Prepare(Io& io,uint32_t firstIndex,uint64_t token,uint32_t nonce,uint32_t (&staging)[2048]) {
        if(state_!=BdwOneShotState::Fresh || !token || !nonce || firstIndex<65536 || firstIndex>1048576-8)return false;
        owner_=&io;state_=BdwOneShotState::Preparing;
        for(unsigned i=0;i<Count;++i) {
            BdwOwnedPage p{};if(!io.alloc(p)){ReleasePages(io);state_=BdwOneShotState::Released;return false;}
            pages_[allocated_++]=p;
            if(!p.cpu || (reinterpret_cast<uintptr_t>(p.cpu)&4095) || !p.dma || (p.dma&4095) || p.dma>=(uint64_t{1}<<39) ||
               (p.dma<0xdf800000ull && p.dma+4096>0xdd000000ull)) {state_=BdwOneShotState::Quarantined;return false;}
            for(unsigned j=0;j<i;++j)if(p.dma==pages_[j].dma || p.cpu==pages_[j].cpu){state_=BdwOneShotState::Quarantined;return false;}
        }
        for(auto& p:pages_)Zero(p);
        // Gen8 HWS uses six 64-bit CSB records at dword 0x10 and the
        // producer index at dword 0x1f. Mark records unused and point to
        // the last slot so the first produced status is consumed at zero.
        // This prepares private RAM only: hardware pointers, cache
        // publication and engine/CSB ownership still require a backend.
        for(unsigned i=0x40;i<0x70;++i)pages_[Hws].cpu[i]=0xff;
        Store32(pages_[Hws],0x7c,5);
        // Linux-equivalent Gen8 cached PTE encoding. Execute must independently
        // verify the GPU's private PAT and memory/cache publication policy.
        for(unsigned i=0;i<512;++i){Entry(pages_[ScratchPt],i,pages_[ScratchData].dma|0x81);
            Entry(pages_[ScratchPd],i,pages_[ScratchPt].dma|3);Entry(pages_[ScratchPdpt],i,pages_[ScratchPd].dma|3);
            Entry(pages_[Pt],i,pages_[ScratchData].dma|0x81);Entry(pages_[Pd],i,pages_[ScratchPt].dma|3);
            Entry(pages_[Pdpt],i,pages_[ScratchPd].dma|3);Entry(pages_[Pml4],i,pages_[ScratchPdpt].dma|3);}
        Entry(pages_[Pml4],0,pages_[Pdpt].dma|3);Entry(pages_[Pdpt],0,pages_[Pd].dma|3);Entry(pages_[Pd],0,pages_[Pt].dma|3);
        Entry(pages_[Pt],16,pages_[Batch].dma|0x81);Entry(pages_[Pt],17,pages_[Source].dma|0x81);Entry(pages_[Pt],18,pages_[Destination].dma|0x83);
        for(unsigned i=0;i<4096;++i){pages_[Source].cpu[i]=Pattern(i);pages_[Destination].cpu[i]=0xa5;}
        lease_={};lease_.token=token;lease_.count=8;
        for(unsigned i=0;i<8;++i){lease_.index[i]=firstIndex+i;lease_.dma[i]=pages_[i].dma;lease_.expected[i]=0;}
        const uint64_t ggtt=uint64_t(firstIndex)*4096;
        BdwBcsSubmission s{{ggtt,ggtt+8192,pages_[Pml4].dma,16384,56,1},
            {{0x11000,4096,64},{0x12000,4096,64},16,4},0x10000,ggtt+7*4096,nonce};
        uint32_t batch[12]{},ring[14]{};
        if(!BdwBuildBcsSubmission(s,staging,batch,ring,descriptor_)){ReleasePages(io);state_=BdwOneShotState::Released;return false;}
        for(unsigned i=0;i<1024;++i){Store32(pages_[Ctx0],i*4,staging[i]);Store32(pages_[Ctx1],i*4,staging[1024+i]);}
        for(unsigned i=0;i<12;++i)Store32(pages_[Batch],i*4,batch[i]);
        for(unsigned i=0;i<14;++i)Store32(pages_[Ring0],i*4,ring[i]);
        nonce_=nonce;state_=BdwOneShotState::Prepared;return true;
    }
    template<class Io> bool ReleasePrepared(Io& io) {
        if(owner_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared || !bind_.SafeToRelease())return false;
        state_=BdwOneShotState::Released;ReleasePages(io);return true;
    }
    // Private CPU writeback only; no GGTT binding or GPU submission.
    template<class Io> bool PublishPrepared(Io& io) {
        if(owner_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared)return false;
        state_=BdwOneShotState::Executing;
        const bool ok=io.publish(pages_,Count);
        state_=BdwOneShotState::Prepared;return ok;
    }
    // A bounded one-shot: no engine write before all backend handoff proofs,
    // publication and binding. HWS may be consumed even if its write reports
    // failure, so retain all pages from the first attempted engine write on.
    template<class Io> bool Execute(Io& io) {
        if(owner_!=static_cast<const void*>(&io) || state_!=BdwOneShotState::Prepared || attempted_)return false;
        attempted_=true;state_=BdwOneShotState::Executing;
        // Admission must establish pinned, distinct usable RAM, DMA translation
        // and exclusion of all firmware and device-owned regions for all pages.
        // Idle alone does not imply execlist mode: the Dell has been observed
        // with BCS idle in legacy submission mode. The backend must establish
        // both the mode and a usable, owned context-status buffer before any
        // GGTT or engine writes; this class does not switch the engine mode.
        if(!io.exclusiveHandoff() || !io.execlistReady() || !io.admitPages(pages_,Count) ||
           !io.gpuPatVerified() || !io.publish(pages_,Count)) {
            state_=BdwOneShotState::Prepared;return false;
        }
        if(!bind_.Bind(lease_,io)) {state_=bind_.SafeToRelease()?BdwOneShotState::Prepared:BdwOneShotState::Quarantined;return false;}
        state_=BdwOneShotState::Quarantined;
        if(!io.programHws(uint64_t(lease_.index[Hws])*4096))return false;
        const uint32_t words[]={0,0,uint32_t(descriptor_>>32),uint32_t(descriptor_)};
        for(auto word:words)if(!io.submitDword(word))return false;
        if(!io.waitCompletion(nonce_) || !io.cpuVisible(pages_,Count) || Load32(pages_[Completion],0)!=nonce_)return false;
        for(unsigned i=0;i<4096;++i) {
            if(pages_[Source].cpu[i]!=Pattern(i))return false;
            if(pages_[Destination].cpu[i]!=(i<256?pages_[Source].cpu[i]:0xa5))return false;
        }
        // A breadcrumb precedes arbitration re-enable and context save. No
        // retirement proof exists yet: retain all mappings even on success.
        state_=BdwOneShotState::CompleteRetained;return true;
    }
    // One exclusive descriptor from an initially idle six-slot CSB. The
    // backend must have synchronized the hardware/software cursors at five,
    // retain ownership, and provide ordered device-to-CPU reads and pointer
    // acknowledgements. A poison record is left pending for a later retry;
    // callers must impose their own deadline. Even successful context switch
    // completion does not release the HWS, GGTT bindings or pages.
    template<class Io> BdwCsbPoll PollContextSwitch(Io& io) {
        if(owner_!=static_cast<const void*>(&io))return BdwCsbPoll::Fault;
        if(state_==BdwOneShotState::ContextCompleteRetained)return BdwCsbPoll::Complete;
        if(state_!=BdwOneShotState::CompleteRetained)return BdwCsbPoll::Fault;
        state_=BdwOneShotState::Polling;
        uint32_t tail=0;
        if(!io.csbReadTail(pages_[Hws],tail) || tail>=6){state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;}
        while(csbHead_!=tail) {
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
            if(!io.csbAcknowledge(next)){state_=BdwOneShotState::Quarantined;return BdwCsbPoll::Fault;}
            csbHead_=next;
            if(terminal) {
                state_=BdwOneShotState::ContextCompleteRetained;return BdwCsbPoll::Complete;
            }
        }
        state_=BdwOneShotState::CompleteRetained;return BdwCsbPoll::Pending;
    }
};
}
