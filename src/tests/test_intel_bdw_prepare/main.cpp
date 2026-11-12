#include "intel_bdw_scanout.h"
#include "memory/physical_memory.h"
#include "intel_vtd_observe.h"
#include "intel_bdw_ggtt_space.h"
#include "intel_bdw_oneshot.h"
#include "intel_bdw_render_oneshot.h"
#include <cassert>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned allocations=0,frees=0,failAt=0;
static bool disableFlush=false;
static bool resetArmed=true,sharedPage=false,translated=false,protectedEnabled=false,changedVtd=false;
static unsigned vtdReads=0;
static uint32_t renderStaging[20*1024];
namespace brook {
bool BdwTestDisableClflush(){return disableFlush;}
PhysicalAddress PmmAllocPage(MemTag tag,uint16_t pid) {
    assert(tag==MemTag::KernelData && pid==KernelPid);
    if(++allocations==failAt)return PhysicalAddress(0);
    void* p=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    assert(p!=MAP_FAILED);return PhysicalAddress(reinterpret_cast<uintptr_t>(p));
}
void PmmFreePage(PhysicalAddress p){assert(!munmap(reinterpret_cast<void*>(p.raw()),4096));++frees;}
MemTag PmmGetTag(PhysicalAddress){return MemTag::KernelData;}
uint16_t PmmGetPid(PhysicalAddress){return KernelPid;}
bool PmmHasSingleReference(PhysicalAddress){return !sharedPage;}
uint32_t PmmGetMapCount(PhysicalAddress){return 0;}
bool PanicRecoveryResetArmed(){return resetArmed;}
uint32_t SmpCurrentCpuIndex(){return 0;}
const DellGpuVtdObservation& DellGpuVtdObserveRead(){
    static DellGpuVtdObservation o{};o.sampled=o.topology=o.inventory=o.bootMap=true;return o;
}
bool VmmDellGpuVtdCriticalRead(DellGpuVtdStatus& s){++vtdReads;s={0x10,translated?0x80000000u:0u,32,
    protectedEnabled?0x80000001u:(changedVtd && !(vtdReads&1)?1u:0u),true};return true;}
}
struct RetainedRenderBackend final:brook::BdwRenderExecutionBackend {
    brook::BdwOwnedPage* pages=nullptr;uint64_t entries[29]{};uint32_t first=0,contextId=1;unsigned words=0,acks=0;
    bool exclusiveHandoff(){return true;}bool renderStateReady(){return true;}
    bool admitPages(brook::BdwOwnedPage* p,unsigned n){pages=p;return n==37;}
    bool gpuPatLease(){return true;}bool preparePrivatePat(uint64_t){return true;}
    bool readPrivatePat(uint32_t& lo,uint32_t& hi){lo=7;hi=11;return true;}
    bool publish(brook::BdwOwnedPage*,unsigned n){return n==37;}
    bool validateLease(const brook::BdwGgttLease& l){first=l.index[0];return l.count==29;}
    bool validateTakeover(const brook::BdwGgttLease&,uint64_t scratch){return scratch!=0;}
    bool quiescent(){return true;}
    bool read(uint32_t i,uint64_t& v){if(i<first || i-first>=29)return false;v=entries[i-first];return true;}
    bool write(uint32_t i,uint64_t v){if(i<first || i-first>=29)return false;entries[i-first]=v;return true;}
    bool invalidate(){return true;}
    bool initializeExeclists(uint64_t,const brook::BdwOwnedPage& p){return p.cpu==pages[24].cpu;}
    bool prepareSubmission(uint64_t){return true;}bool submitDword(uint32_t){++words;return true;}
    bool waitCompletion(uint32_t nonce){__atomic_store_n(reinterpret_cast<uint32_t*>(pages[25].cpu),nonce,__ATOMIC_RELAXED);return true;}
    bool cpuVisible(brook::BdwOwnedPage*,unsigned){return true;}
    bool csbReadTail(brook::BdwOwnedPage&,uint32_t& tail){tail=1;return true;}
    bool csbReadEntry(brook::BdwOwnedPage&,unsigned slot,uint64_t& event){event=slot==0?1:((uint64_t(contextId)<<32)|0x18);return true;}
    bool csbAcknowledge(unsigned){++acks;return true;}bool retirementContinue(){return true;}
};
int main(){
    {
        brook::BdwOwnedPage pages[6]{};
        for(auto& page:pages){auto p=brook::PmmAllocPage(brook::MemTag::KernelData,brook::KernelPid);
            page={p.raw(),reinterpret_cast<uint8_t*>(p.raw())};}
        vtdReads=0;assert(brook::BdwInspectAdditionalRenderDma(pages,6)==brook::BdwDmaSnapshotOk && vtdReads==2);
        sharedPage=true;assert(brook::BdwInspectAdditionalRenderDma(pages,6)==brook::BdwDmaPageShared);sharedPage=false;
        translated=true;assert(brook::BdwInspectAdditionalRenderDma(pages,6)==brook::BdwDmaFirstVtdRestricted);translated=false;
        vtdReads=0;changedVtd=true;assert(brook::BdwInspectAdditionalRenderDma(pages,6)==brook::BdwDmaSecondVtdRestricted);changedVtd=false;
        auto original=pages[5];pages[5]=pages[0];assert(brook::BdwInspectAdditionalRenderDma(pages,6)==brook::BdwDmaPageAlias);pages[5]=original;
        assert(brook::BdwInspectAdditionalRenderDma(pages,0)==brook::BdwDmaMissingVtdEvidence);
        for(auto page:pages)brook::PmmFreePage(brook::PhysicalAddress(page.dma));
        vtdReads=0;
    }
    for(unsigned i=1;i<=19;++i){allocations=frees=0;failAt=i;assert(!brook::BdwPreparePrivateImages());assert(frees==i-1);}
    allocations=frees=0;failAt=0;disableFlush=true;assert(!brook::BdwPreparePrivateImages());assert(allocations==19 && frees==19);
    allocations=frees=0;disableFlush=false;
    uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    const uint32_t line=((b>>8)&255)*8;
    const bool supported=(d&(1u<<19)) && line>=16 && line<=4096 && !(line&(line-1)) && !(4096%line);
    assert(brook::BdwPreparePrivateImages()==supported);assert(allocations==19 && frees==19);
    for(unsigned i=1;i<=37;++i){allocations=frees=0;failAt=i;assert(!brook::BdwPreparePrivateRenderImages(renderStaging));assert(frees==i-1);}
    allocations=frees=0;failAt=0;disableFlush=true;
    assert(!brook::BdwPreparePrivateRenderImages(renderStaging));assert(allocations==37 && frees==37);
    allocations=frees=0;disableFlush=false;
    assert(brook::BdwPreparePrivateRenderImages(renderStaging)==supported);assert(allocations==37 && frees==37);
    for(unsigned mode=0;mode<5;++mode){
        pid_t child=fork();assert(child>=0);
        if(!child){
            allocations=frees=0;failAt=mode==1?8:mode==3?20:0;disableFlush=mode==2;
            const bool ok=brook::BdwRetainPrivateBcsImages(262144);
            assert(ok==((mode==0 || mode==4) && supported));
            if(ok){
                assert(allocations==20 && frees==0);
                brook::BdwOwnedPage scratch{};assert(brook::BdwRetainedBcsScratch(scratch));
                assert(scratch.cpu && scratch.dma && !(scratch.dma&4095));
                for(unsigned i=0;i<4096;++i)assert(scratch.cpu[i]==0);
                uint32_t producer=0;assert(!brook::BdwPrepareRetainedScratchStatus() && !brook::BdwReadRetainedScratchProducer(producer));
                assert(!brook::BdwRetainPrivateBcsImages(262144) && frees==0);
                brook::BdwGgttReservation next{};
                assert(brook::BdwGlobalGgttSpace().reserve(8,next) && next.first==65544);
                assert(brook::BdwGlobalGgttSpace().release(next));
                assert(brook::BdwInspectPrivateBcsDma()==0 && vtdReads==2);
                sharedPage=true;assert(brook::BdwInspectPrivateBcsDma()==9);sharedPage=false;
                translated=true;assert(brook::BdwInspectPrivateBcsDma()==6);translated=false;
                protectedEnabled=true;assert(brook::BdwInspectPrivateBcsDma()==6);protectedEnabled=false;
                vtdReads=0;changedVtd=true;assert(brook::BdwInspectPrivateBcsDma()==12);changedVtd=false;
                resetArmed=false;assert(brook::BdwInspectPrivateBcsDma()==2);resetArmed=true;
                if(mode==4) {
                    brook::BdwOwnedPage pinned{},again{};brook::BdwGgttReservation range{},unused{};
                    assert(brook::BdwPinRetainedScratchForTakeover(pinned,range));
                    assert(pinned.dma==scratch.dma && pinned.cpu==scratch.cpu && range.count==8);
                    assert(!brook::BdwPinRetainedScratchForTakeover(again,unused));
                    assert(brook::BdwPrepareRetainedScratchStatus() && brook::BdwReadRetainedScratchProducer(producer) && producer==5);
                    for(unsigned i=0x40;i<0x70;++i)assert(pinned.cpu[i]==0xff);
                    assert(!brook::BdwReleasePrivateBcsImages() && frees==0);
                    _exit(0);
                }
                assert(brook::BdwReleasePrivateBcsImages() && frees==20);
                assert(!brook::BdwRetainedBcsScratch(scratch));
                assert(brook::BdwInspectPrivateBcsDma()==1);
                assert(!brook::BdwReleasePrivateBcsImages());
            } else assert(frees==allocations-((mode==1 || mode==3)?1u:0u));
            assert(!brook::BdwRetainPrivateBcsImages(262144));
            _exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    for(unsigned mode=0;mode<5;++mode){
        pid_t child=fork();assert(child>=0);
        if(!child){
            allocations=frees=0;failAt=mode==2?8:mode==3?38:0;disableFlush=false;
            if(mode==0)assert(brook::BdwRetainPrivateBcsImages(262144)==supported);
            if(!supported)_exit(0);
            const bool ok=brook::BdwRetainPrivateRenderImages(262144);
            assert(ok==(mode<2 || mode==4));
            if(ok){
                brook::BdwGgttReservation r{};assert(brook::BdwRetainedRenderReservation(r));
                assert(r.count==29 && r.first==(mode==0?65544u:65536u));
                assert(brook::BdwInspectPrivateRenderDma()==brook::BdwDmaSnapshotOk);
                sharedPage=true;assert(brook::BdwInspectPrivateRenderDma()==brook::BdwDmaPageShared);sharedPage=false;
                if(mode==0){assert(brook::BdwReleasePrivateRenderImages());assert(brook::BdwReleasePrivateBcsImages());}
                else {brook::BdwOwnedPage scratch{};brook::BdwGgttReservation pinned{};
                    assert(brook::BdwPinRetainedRenderScratch(scratch,pinned));assert(pinned.token==r.token);
                    assert(!brook::BdwPinRetainedRenderScratch(scratch,pinned) && !brook::BdwReleasePrivateRenderImages());
                    if(mode==4){RetainedRenderBackend backend;brook::BdwRenderRunResult result{};
                        assert(!brook::BdwCopyRetainedRenderContext(backend,renderStaging));
                        assert(brook::BdwRunRetainedRender(backend,scratch.dma,result));
                        assert(result.attempted && result.breadcrumb && result.contextComplete && backend.words==4 && backend.acks==2);
                        assert(brook::BdwCopyRetainedRenderContext(backend,renderStaging) && renderStaging[1024+7]==224);
                        renderStaging[4096]=0x472591ab;
                        assert(brook::BdwRetainRebasedRenderImages(renderStaging));
                        brook::BdwGgttReservation second{};assert(brook::BdwRebasedRenderReservation(second));
                        assert(second.first==r.first+29 && second.count==29 && second.token!=r.token);
                        assert(brook::BdwInspectRebasedRenderDma()==brook::BdwDmaSnapshotOk);
                        brook::BdwOwnedPage secondScratch{};brook::BdwGgttReservation secondPinned{};
                        assert(brook::BdwPinRebasedRenderScratch(secondScratch,secondPinned) && secondScratch.dma!=scratch.dma);
                        RetainedRenderBackend secondBackend;secondBackend.contextId=2;brook::BdwRenderRunResult secondResult{};
                        assert(brook::BdwRunRebasedRender(secondBackend,secondScratch.dma,secondResult));
                        assert(secondResult.breadcrumb && secondResult.contextComplete && secondBackend.words==4 && secondBackend.acks==2);
                        assert(brook::BdwCopyRebasedRenderContext(secondBackend,renderStaging) && renderStaging[4096]==0x472591ab);
                        assert(!brook::BdwRetainRebasedRenderImages(renderStaging));
                        assert(!brook::BdwReleasePrivateRenderImages());}}
            } else assert(frees==(mode==2?7u:37u));
            _exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
}
