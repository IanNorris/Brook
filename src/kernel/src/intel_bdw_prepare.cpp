#include "intel_bdw_oneshot.h"
#include "intel_bdw_scanout.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_publish.h"
#include "intel_bdw_render_plan.h"
#include "intel_bdw_render_oneshot.h"
#include "intel_bdw_file.h"
#include "intel_bdw_buffer.h"
#include "intel_bdw_submission_queue.h"
#include "intel_bdw_submission_worker.h"
#include "intel_bdw_handoff_session.h"
#include "intel_bdw_present_queue.h"
#include "../../shared/gpu_output_abi.h"
#include "compositor.h"
#include "window.h"
#include "desktop_chrome_binding.h"
#include "desktop_frame_scenes.h"
#include "intel_bdw_ioctl.h"
#include "intel_bdw_ggtt_space.h"
#include "intel_bdw_csb.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/heap.h"
#include "intel_vtd_observe.h"
#include "smp.h"
#ifndef BROOK_HOST_TEST
#include "process.h"
#include "scheduler.h"
#include "panic.h"
#include "apic.h"
#include "sync/ksemaphore.h"
#endif
#include "panic_test_reset.h"

namespace brook {
#ifdef BROOK_HOST_TEST
bool BdwTestDisableClflush();
#endif
namespace {
// Preparation only: the addresses below are placeholders for a future DMA
// mapping. No GGTT reservation, hardware access or GPU submission occurs.
struct PrivatePages {
    bool alloc(BdwOwnedPage& out) {
        auto p=PmmAllocPage(MemTag::KernelData,KernelPid);
        if(!p.raw())return false;
        // The boot direct map used here covers only the first 16 GiB.
        if(p.raw()>=(uint64_t{16}<<30)){PmmFreePage(p);return false;}
        out={p.raw(),reinterpret_cast<uint8_t*>(PhysToVirt(p).raw())};return true;
    }
    void free(BdwOwnedPage p){PmmFreePage(PhysicalAddress(p.dma));}
    uint32_t inspectDma(const BdwOwnedPage* pages,unsigned count) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
        const auto& prior=DellGpuVtdObserveRead();
        if(!PanicRecoveryResetArmed())return BdwDmaResetUnarmed;
        // Each VT-d read validates aliases and memory types on the CPU that
        // executes it under the page-table lock with interrupts disabled.
        if(!prior.sampled || !prior.topology || !prior.inventory || !prior.bootMap || !pages || !count || count>128)return BdwDmaMissingVtdEvidence;
        auto acceptable=[](const DellGpuVtdStatus& s) {
            constexpr uint64_t protectedRegions=(1ull<<5)|(1ull<<6);
            return !(s.globalStatus&(1u<<31)) &&
                (!(s.capability&protectedRegions) ||
                 (s.protectedMemorySampled && !(s.protectedMemory&0x80000001u)));
        };
        DellGpuVtdStatus before{},after{};
        if(!VmmDellGpuVtdCriticalRead(before))return BdwDmaFirstVtdRead;
        if(!acceptable(before))return BdwDmaFirstVtdRestricted;
        for(unsigned i=0;i<count;++i) {
            const PhysicalAddress p(pages[i].dma);
            if(!p.raw() || p.raw()>=(uint64_t{16}<<30) || (p.raw()&4095) ||
               pages[i].cpu!=reinterpret_cast<uint8_t*>(PhysToVirt(p).raw()))return BdwDmaPageAddress;
            if(PmmGetTag(p)!=MemTag::KernelData || PmmGetPid(p)!=KernelPid)return BdwDmaPageOwner;
            if(!PmmHasSingleReference(p) || PmmGetMapCount(p)!=0)return BdwDmaPageShared;
            for(unsigned j=0;j<i;++j)if(pages[j].dma==p.raw())return BdwDmaPageAlias;
        }
        if(!VmmDellGpuVtdCriticalRead(after))return BdwDmaSecondVtdRead;
        if(!acceptable(after))return BdwDmaSecondVtdRestricted;
        if(before.version!=after.version || before.capability!=after.capability ||
           before.globalStatus!=after.globalStatus || before.protectedMemorySampled!=after.protectedMemorySampled ||
           before.protectedMemory!=after.protectedMemory)return BdwDmaVtdChanged;
        return BdwDmaSnapshotOk;
#else
        (void)pages;(void)count;return BdwDmaUnavailable;
#endif
    }
    bool publish(BdwOwnedPage* pages,unsigned count) {
        // The diagnostic runs on private PMM pages and does not grant device
        // access. Pin execution while reading this CPU's CPUID and flushing.
#ifndef BROOK_HOST_TEST
        uint64_t flags;__asm__ volatile("pushfq; popq %0; cli":"=r"(flags)::"memory");
#endif
        uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
#ifdef BROOK_HOST_TEST
        if(BdwTestDisableClflush())d&=~(1u<<19);
#endif
        struct Flush {
            void fence(){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
            void line(void* p){__asm__ volatile("clflush (%0)"::"r"(p):"memory");}
        } flush;
        const bool ok=(d&(1u<<19)) && BdwPublishCpuPages(flush,pages,count,((b>>8)&255)*8);
#ifndef BROOK_HOST_TEST
        if(flags&(1ull<<9))__asm__ volatile("sti":::"memory");
#endif
        return ok;
    }
    // These routines supply CPU cache maintenance for a retained status page.
    // The eventual engine owner must separately bind this exact page, exclude
    // concurrent writers and establish device-side DMA/cache visibility.
    bool statusPagePrivate(const BdwOwnedPage& page) {
        const PhysicalAddress p(page.dma);
        return p.raw() && !(p.raw()&4095) && p.raw()<(uint64_t{16}<<30) &&
            page.cpu==reinterpret_cast<uint8_t*>(PhysToVirt(p).raw()) &&
            PmmGetTag(p)==MemTag::KernelData && PmmGetPid(p)==KernelPid &&
            PmmHasSingleReference(p) && PmmGetMapCount(p)==0;
    }
    bool prepareStatusPage(const BdwOwnedPage& page) {
        if(!statusPagePrivate(page))return false;
        return BdwCsbInitializeForReset(page.cpu,[&](uint8_t*) {
            BdwOwnedPage p=page;return publish(&p,1);
        });
    }
    bool statusProducer(const BdwOwnedPage& page,uint32_t& producer) {
        if(!statusPagePrivate(page))return false;
        BdwOwnedPage p=page;
        return publish(&p,1) && BdwCsbReadProducer(page.cpu,producer);
    }
};
// These objects have boot lifetime: BdwBcsOneShot retains its allocator by
// address and must never outlive it. They are intentionally separate from the
// disposable preparation path. Submission and retirement must be serialized
// by the eventual hardware owner; neither is exposed by this allocator.
PrivatePages retainedAllocator;
BdwBcsOneShot retainedBcs;
BdwGgttSpace retainedGgttSpace;
BdwGgttReservation retainedReservation;
BdwOwnedPage retainedScratch{};
bool retainedScratchValid=false,retainedScratchAmbiguous=false;
uint32_t retainedScratchPinned=0;
uint32_t retainedStaging[2048];
PrivatePages retainedRenderAllocator;
BdwRenderOneShot retainedRender;
BdwGgttReservation retainedRenderReservation;
BdwOwnedPage retainedRenderScratch{};
bool retainedRenderScratchValid=false,retainedRenderScratchAmbiguous=false;
uint32_t retainedRenderScratchPinned=0;
uint32_t retainedRenderStaging[20*1024];
#ifndef BROOK_HOST_TEST
BdwGpuFile* gemProbeFile=nullptr;
BdwGpuBuffer* gemProbeData=nullptr;
BdwGpuBuffer* gemProbeBatch=nullptr;
BdwExecSnapshot* gemProbeSnapshot=nullptr;
BdwExecTables gemProbeTables;
BdwSubmissionQueue gemProbeQueue;
bool gemProbeQueueInitialized=false;
uint64_t gemProbeTicket=0;
bool gemProbeExternal=false;
#endif
PrivatePages rebasedRenderAllocator;
BdwRenderOneShot rebasedRender;
BdwGgttReservation rebasedRenderReservation;
BdwOwnedPage rebasedRenderScratch{};
bool rebasedRenderScratchValid=false,rebasedRenderScratchAmbiguous=false;
uint32_t rebasedRenderScratchPinned=0;
uint32_t rebasedRenderStaging[20*1024];
}
BdwGgttSpace& BdwGlobalGgttSpace(){return retainedGgttSpace;}
bool BdwRetainPrivateBcsImages(uint32_t ggttEntries) {
    if(retainedBcs.State()!=BdwOneShotState::Fresh)return false;
    if(!BdwGlobalGgttSpace().configuredFor(ggttEntries) && !BdwGlobalGgttSpace().configure(ggttEntries))return false;
    if(!BdwGlobalGgttSpace().reserve(8,retainedReservation))return false;
    if(!retainedBcs.Prepare(retainedAllocator,retainedReservation.first,retainedReservation.token,0x42524f4f,retainedStaging)) {
        if(retainedBcs.State()==BdwOneShotState::Released)retainedGgttSpace.release(retainedReservation);
        return false;
    }
    if(retainedBcs.PublishPrepared(retainedAllocator)) {
        BdwOwnedPage scratch{};
        if(!retainedAllocator.alloc(scratch)) {
            if(retainedBcs.ReleasePrepared(retainedAllocator))retainedGgttSpace.release(retainedReservation);
            return false;
        }
        // An alias or malformed allocator result makes both page lifetimes
        // uncertain. Do not free either page or release its reservation.
        if(!scratch.cpu || (reinterpret_cast<uintptr_t>(scratch.cpu)&4095) || !scratch.dma ||
           (scratch.dma&4095) || scratch.dma>=(uint64_t{1}<<39) ||
           (scratch.dma<0xdf800000ull && scratch.dma+4096>0xdd000000ull) ||
           !retainedBcs.DistinctPage(scratch)) {
            retainedScratchAmbiguous=true;return false;
        }
        retainedScratch=scratch;
        for(unsigned i=0;i<4096;++i)scratch.cpu[i]=0;
        if(!retainedAllocator.publish(&retainedScratch,1)) {
            retainedAllocator.free(retainedScratch);retainedScratch={};
            if(retainedBcs.ReleasePrepared(retainedAllocator))retainedGgttSpace.release(retainedReservation);
            return false;
        }
        retainedScratchValid=true;return true;
    }
    // Publication has no device access. On failure release the unpublished
    // images only when the state machine confirms it is safe to do so.
    if(retainedBcs.ReleasePrepared(retainedAllocator))retainedGgttSpace.release(retainedReservation);
    return false;
}
bool BdwReleasePrivateBcsImages() {
    if(retainedScratchAmbiguous || !retainedScratchValid || __atomic_load_n(&retainedScratchPinned,__ATOMIC_ACQUIRE) || !retainedBcs.ScratchNeverPublished() ||
       !retainedBcs.ReleasePrepared(retainedAllocator))return false;
    retainedAllocator.free(retainedScratch);retainedScratch={};retainedScratchValid=false;
    return retainedGgttSpace.release(retainedReservation);
}
bool BdwRetainedBcsScratch(BdwOwnedPage& out) {
    if(!retainedScratchValid || retainedScratchAmbiguous ||
       !retainedGgttSpace.owns(retainedReservation) ||
       !retainedBcs.DistinctPage(retainedScratch) || !retainedAllocator.statusPagePrivate(retainedScratch))return false;
    out=retainedScratch;return true;
}
bool BdwPinRetainedScratchForTakeover(BdwOwnedPage& out,BdwGgttReservation& reservation) {
    BdwOwnedPage scratch{};
    if(!BdwRetainedBcsScratch(scratch) || !retainedGgttSpace.owns(retainedReservation))return false;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&retainedScratchPinned,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    out=scratch;reservation=retainedReservation;return true;
}
bool BdwPrepareRetainedScratchStatus() {
    BdwOwnedPage scratch{};
    return __atomic_load_n(&retainedScratchPinned,__ATOMIC_ACQUIRE) && BdwRetainedBcsScratch(scratch) &&
        retainedAllocator.prepareStatusPage(scratch);
}
bool BdwReadRetainedScratchProducer(uint32_t& producer) {
    BdwOwnedPage scratch{};
    return __atomic_load_n(&retainedScratchPinned,__ATOMIC_ACQUIRE) && BdwRetainedBcsScratch(scratch) &&
        retainedAllocator.statusProducer(scratch,producer);
}
bool BdwRunRetainedBcs(BdwBcsExecutionBackend& backend,uint64_t scratchDma,BdwBcsRunResult& result) {
    // This entry runs inside the VMM critical section. PMM and VT-d checks
    // must have completed beforehand; do not reacquire their locks here.
    if(!__atomic_load_n(&retainedScratchPinned,__ATOMIC_ACQUIRE) || !retainedScratchValid ||
       retainedScratchAmbiguous || scratchDma!=retainedScratch.dma ||
       !retainedGgttSpace.owns(retainedReservation))return false;
    result.attempted=true;
    if(!retainedBcs.ExecuteTakenOverWithBackend(retainedAllocator,backend,scratchDma))return false;
    result.copyComplete=true;
    while(backend.retirementContinue()) {
        const auto state=retainedBcs.PollContextSwitch(backend);
        if(state==BdwCsbPoll::Fault)return false;
        if(state==BdwCsbPoll::Complete){result.contextComplete=true;return true;}
    }
    return false;
}
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
namespace {
BdwBcsOneShot scanoutImage;
BdwGgttReservation scanoutReservation;
BdwOwnedPage scanoutScratch{};
uint32_t scanoutStaging[2048];
}

bool BdwPrepareBcsScanout(BdwGpuBuffer* source,uint64_t token,const BdwGopBacking& backing,
        const CompositorPresentRegion* regions,uint32_t count,BdwGgttReservation& reservation,BdwOwnedPage& scratch) {
    static_assert(COMPOSITOR_MAX_REGIONS==BdwBcsMaxRegions);
    static uint32_t nonce=0x42534300;
    if(!source || !BdwGpuBufferHandoffCompleted(source,token) || !regions || !count || count>COMPOSITOR_MAX_REGIONS ||
       backing.bytes!=uint64_t(BdwDellGopPages)*4096 || nonce==~uint32_t{0})return false;
    uint32_t bufferPages=0;
    const uint64_t* sourcePages=BdwGpuBufferPages(source,bufferPages);
    const uint64_t capacity=BdwGpuBufferBytes(source);
    if(!sourcePages || !bufferPages)return false;
    BdwBlitRegion copies[COMPOSITOR_MAX_REGIONS]{};
    uint64_t end=0;
    for(uint32_t i=0;i<count;++i){
        const auto& region=regions[i];
        if(!region.width || !region.height || region.stride<region.width || region.stride>=8192 ||
           region.offset>=capacity || region.offset>=uint64_t(BdwBcsOperandPages)*4096 ||
           (region.offset&63) || region.x>=3200 || region.y>=1800 ||
           region.width>3200-region.x || region.height>1800-region.y)return false;
        const uint64_t bytes=(uint64_t(region.height-1)*region.stride+region.width)*4;
        if(bytes>capacity-region.offset)return false;
        if(region.offset+bytes>end)end=region.offset+bytes;
        copies[i]={{BdwBcsSourceAddress+region.offset,bytes,region.stride*4},
                   {BdwBcsScanoutAddress,backing.bytes,12800},0,0,
                   region.x,region.y,region.width,region.height};
    }
    const uint32_t pages=uint32_t((end+4095)/4096);
    if(!pages || pages>bufferPages || pages>BdwBcsOperandPages)return false;

    if(!scanoutReservation.token){
        auto& space=BdwGlobalGgttSpace();
        if(!space.reserve(8,scanoutReservation))return false;
        if(scanoutReservation.first<65536 || scanoutReservation.first>65536+512-8 ||
           !retainedAllocator.alloc(scanoutScratch)){
            space.release(scanoutReservation);scanoutReservation={};return false;
        }
        for(unsigned i=0;i<4096;++i)scanoutScratch.cpu[i]=0;
    }
    if(!retainedAllocator.statusPagePrivate(scanoutScratch) || !retainedAllocator.publish(&scanoutScratch,1))return false;
    // The completed RCS handoff already checked these exact device pages
    // and flushed the GPU writes. The source remains unmappable to users.
    for(uint32_t i=0;i<pages;++i)
        if(sourcePages[i]==scanoutScratch.dma || sourcePages[i]>=(uint64_t{16}<<30))return false;
    const uint32_t mappingCount=1+pages+BdwDellGopPages;
    auto* mappings=static_cast<BdwPpgttMapping*>(kmalloc(uint64_t(mappingCount)*sizeof(BdwPpgttMapping)));
    if(!mappings)return false;
    const bool prepared=scanoutImage.PrepareScanout(retainedAllocator,scanoutReservation.first,scanoutReservation.token,
        ++nonce,sourcePages,pages,backing.physicalBase,BdwDellGopPages,copies,count,mappings,mappingCount,scanoutStaging);
    kfree(mappings);
    if(!prepared){
        if(scanoutImage.State()==BdwOneShotState::Released)(void)scanoutImage.Recycle();
        return false;
    }
    if(scanoutImage.InspectPreparedDma(retainedAllocator)!=BdwDmaSnapshotOk ||
       !scanoutImage.DistinctPage(scanoutScratch)){
        if(scanoutImage.ReleasePrepared(retainedAllocator))(void)scanoutImage.Recycle();
        return false;
    }
    reservation=scanoutReservation;scratch=scanoutScratch;
    return true;
}

bool BdwRunPreparedBcsScanout(BdwBcsExecutionBackend& backend,uint64_t scratchDma,BdwBcsRunResult& result){
    if(scratchDma!=scanoutScratch.dma || !BdwGlobalGgttSpace().owns(scanoutReservation))return false;
    result.attempted=true;
    if(!scanoutImage.ExecuteTakenOverWithBackend(retainedAllocator,backend,scratchDma))return false;
    result.copyComplete=true;
    while(backend.retirementContinue()){
        const auto status=scanoutImage.PollContextSwitch(backend);
        if(status==BdwCsbPoll::Fault)return false;
        if(status==BdwCsbPoll::Complete){result.contextComplete=true;return true;}
    }
    return false;
}

bool BdwRetirePreparedBcsScanout(BdwBcsExecutionBackend& backend){return scanoutImage.RetireScanout(backend);}
bool BdwReleasePreparedBcsScanout(){
    return scanoutImage.ReleasePrepared(retainedAllocator) && scanoutImage.Recycle();
}
#endif
uint32_t BdwInspectPrivateBcsDma() {
    if(retainedBcs.State()==BdwOneShotState::Prepared && !BdwGlobalGgttSpace().owns(retainedReservation))
        return BdwDmaReservationLost;
    const uint32_t status=retainedBcs.InspectPreparedDma(retainedAllocator);
    if(status)return status;
    BdwOwnedPage scratch{};
    return BdwRetainedBcsScratch(scratch)?BdwDmaSnapshotOk:BdwDmaPageOwner;
}
bool BdwRetainedBcsReservation(BdwGgttReservation& out) {
    if(!retainedScratchValid || retainedScratchAmbiguous || retainedBcs.State()!=BdwOneShotState::Prepared ||
       !BdwGlobalGgttSpace().owns(retainedReservation))return false;
    out=retainedReservation;return true;
}
bool BdwRetainPrivateRenderImages(uint32_t ggttEntries,bool ppgttProbe,bool storeProbe) {
    if(retainedRender.State()!=BdwOneShotState::Fresh)return false;
    auto& space=BdwGlobalGgttSpace();
    if(!space.configuredFor(ggttEntries) && !space.configure(ggttEntries))return false;
    if(!space.reserve(29,retainedRenderReservation))return false;
    if(!retainedRender.Prepare(retainedRenderAllocator,retainedRenderReservation.first,
                               retainedRenderReservation.token,0x42524f52,retainedRenderStaging,false,nullptr,1,ppgttProbe,storeProbe)) {
        if(retainedRender.State()==BdwOneShotState::Released)space.release(retainedRenderReservation);
        return false;
    }
    if(!retainedRender.PublishPrepared(retainedRenderAllocator)) {
        if(retainedRender.ReleasePrepared(retainedRenderAllocator))space.release(retainedRenderReservation);
        return false;
    }
    BdwOwnedPage scratch{};
    if(!retainedRenderAllocator.alloc(scratch)) {
        if(retainedRender.ReleasePrepared(retainedRenderAllocator))space.release(retainedRenderReservation);
        return false;
    }
    if(!scratch.cpu || (reinterpret_cast<uintptr_t>(scratch.cpu)&4095) || !scratch.dma ||
       (scratch.dma&4095) || scratch.dma>=(uint64_t{1}<<39) ||
       (scratch.dma<0xdf800000ull && scratch.dma+4096>0xdd000000ull) ||
       !retainedRender.DistinctPage(scratch)) {
        retainedRenderScratchAmbiguous=true;return false;
    }
    retainedRenderScratch=scratch;for(unsigned i=0;i<4096;++i)scratch.cpu[i]=0;
    if(!retainedRenderAllocator.publish(&retainedRenderScratch,1)) {
        retainedRenderAllocator.free(retainedRenderScratch);retainedRenderScratch={};
        if(retainedRender.ReleasePrepared(retainedRenderAllocator))space.release(retainedRenderReservation);
        return false;
    }
    retainedRenderScratchValid=true;return true;
}
#ifndef BROOK_HOST_TEST
bool BdwStartKernelSubmissionWorker(BdwSubmissionWorker& worker,void* context,
                                    BdwSubmissionOutcome (*execute)(void*,BdwExecSnapshot*),bool knownOwnersOnly,
                                    bool (*service)(void*,BdwSubmissionQueue&),bool (*servicePending)(void*)) {
    BdwSubmissionWorker::Hooks hooks{};hooks.context=context;hooks.execute=execute;hooks.service=service;hooks.servicePending=servicePending;
    hooks.launch=[](void*,void (*entry)(void*),void* arg,uint32_t cpu){
        auto* thread=KernelThreadCreate("gpu-submit",entry,arg);if(!thread)return false;
        thread->cpuAffinity=cpu;SchedulerAddProcess(thread);return true;
    };
    hooks.sleep=[](void*){SchedulerSleepMs(1);};
    hooks.recover=[](void*){KernelPanic("Broadwell submission worker requires recovery");};
    return worker.Start(hooks,0,knownOwnersOnly);
}
#if defined(BROOK_SSD_AUTO_RECOVERY)
// Fixed kernel-authored jobs only. Retain everything for the boot on error.
static bool TestSubmissionWorker(BdwWorkerProbeResult& result,bool contexts) {
    result={};
    static BdwSubmissionWorker workers[2];static BdwGpuFile* files[2]{};
    static BdwGpuBuffer* dataByMode[2][3]{};static BdwGpuBuffer* batchByMode[2][3]{};
    const unsigned mode=contexts?1:0,count=contexts?3:2;
    auto& worker=workers[mode];auto*& file=files[mode];auto& data=dataByMode[mode];auto& batch=batchByMode[mode];
    if(file || !BdwStartKernelSubmissionWorker(worker,nullptr,contexts?BdwExecuteRetainedContextWorkerSnapshot:BdwExecuteRetainedWorkerSnapshot))return false;
    file=BdwGpuFileCreate();if(!file)return false;
    uint32_t contextIds[2]{};
    if(contexts){contextIds[0]=BdwGpuFileCreateContext(file);contextIds[1]=BdwGpuFileCreateContext(file);
        if(!contextIds[0] || !contextIds[1])return false;}
    struct ProbeObject {uint32_t handle,relocCount;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
    struct ProbeExec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clipCount;uint64_t clips,flags,context,fences;};
    static_assert(sizeof(ProbeObject)==56 && sizeof(ProbeExec)==64,"i915 probe ABI");
    BdwI915UserAccess input{};input.context=&worker;
    input.read=[](void*,void* destination,uint64_t source,uint64_t bytes){
        if(!destination || !source)return false;
        auto* out=static_cast<uint8_t*>(destination);const auto* in=reinterpret_cast<const uint8_t*>(source);
        for(uint64_t i=0;i<bytes;++i)out[i]=in[i];return true;
    };
    input.writable=[](void*,uint64_t,uint64_t){return false;};
    input.write=[](void*,uint64_t,const void*,uint64_t){return false;};
    input.submit=BdwSubmissionWorker::SubmitCallback;
    constexpr uint64_t command=0x40000000ull|(sizeof(ProbeExec)<<16)|('d'<<8)|0x69;
    for(unsigned i=0;i<count;++i){
        uint64_t rounded=0;
        const uint32_t d=BdwGpuFileCreateBuffer(file,4096,rounded),b=BdwGpuFileCreateBuffer(file,4096,rounded);
        if(!d || !b || !(data[i]=BdwGpuFileGetBuffer(file,d)) || !(batch[i]=BdwGpuFileGetBuffer(file,b)))return false;
        auto* words=reinterpret_cast<uint32_t*>(BdwGpuBufferPageKernelAddress(batch[i],0));
        if(!words || !BdwGpuBufferPageKernelAddress(data[i],0))return false;
        words[0]=0x10000002;words[1]=0x40011000;words[2]=0;words[3]=0x42526000+i;words[4]=0x05000000;
        const ProbeObject objects[]={{d,0,0,4096,0x40011000,16|4,0,0},{b,0,0,4096,0x10000,16,0,0}};
        const ProbeExec exec{reinterpret_cast<uint64_t>(objects),2,0,24,0,0,0,0,uint64_t{1}<<11,contexts?contextIds[i==1?1:0]:0,0};
        if(BdwIntelRenderIoctl(file,command,reinterpret_cast<uint64_t>(&exec),input))return false;
        ++result.submitted;
    }
    const uint64_t deadline=ApicTickCount()+30000;
    for(;;){
        bool busy=false;result.completed=0;result.failed=false;
        for(unsigned i=0;i<count;++i){
            uint32_t uses=0;bool failed=false;BdwGpuBufferGpuStatus(data[i],uses,failed);
            busy|=uses!=0;result.failed|=failed;
            if(!uses){
                auto* value=reinterpret_cast<volatile uint32_t*>(BdwGpuBufferPageKernelAddress(data[i],0));
                if(!value)return false;result.values[i]=*value;
                if(!failed && result.values[i]==0x42526000+i)++result.completed;
            }
        }
        if(!busy)break;
        if(ApicTickCount()>=deadline)return false;
        SchedulerSleepMs(1);
    }
    if(result.failed || result.completed!=count)return false;
    if(contexts){
        auto* memory=static_cast<uint32_t*>(kmalloc(20*4096));if(!memory)return false;
        auto& image=*reinterpret_cast<uint32_t (*)[20*1024]>(memory);
        for(unsigned i=0;i<2;++i){auto* context=BdwGpuFileGetContext(file,contextIds[i]);bool present=false;
            if(!context){kfree(memory);return false;}
            const bool begun=BdwGpuContextBegin(context,image,present);
            if(begun && present){uint32_t hash=2166136261u;for(auto word:image){hash^=word;hash*=16777619u;}result.contextHashes[i]=hash;}
            const bool finished=begun && BdwGpuContextFinish(context,nullptr,true);BdwGpuContextUnref(context);
            if(!finished || !present){kfree(memory);return false;}}
        kfree(memory);
    }
    return true;
}
bool BdwTestSubmissionWorker(BdwWorkerProbeResult& result){return TestSubmissionWorker(result,false);}
bool BdwTestContextSubmissionWorker(BdwWorkerProbeResult& result){return TestSubmissionWorker(result,true);}
static BdwSubmissionWorker applicationWorker;
bool BdwSetScanoutFairness(bool enabled){return applicationWorker.SetServiceFairness(enabled);}
bool BdwScanoutFairness(){return applicationWorker.ServiceFairness();}
void BdwScanoutFairnessCounts(uint64_t& pending,uint64_t& bypass,uint64_t& noEligible,uint64_t& forced){
    applicationWorker.ServiceFairnessCounts(pending,bypass,noEligible,forced);
}
static BdwHandoffSession offscreenSession;
static KMutex offscreenMutex{};
static uint32_t offscreenInit=0;
static KMutex presentMutex{};
static KSemaphore presentWake{};
static BdwPresentQueue presentQueue;
static struct AsyncPresentRequest {
    uint64_t owner;
    BrookGpuPresentProbe frame;
    CompositorPresentRegion regions[COMPOSITOR_MAX_REGIONS];
    uint32_t count;
} presentRequest{};
static bool presentWorkerStarted=false;
static uint64_t asyncPresentAccepted=0,asyncPresentPresented=0;
static WmDesktopLayers* offscreenScene=nullptr;
// Independent references keep the last full-frame identities/backings alive
// even when a later capture replaces offscreenScene.
static WmDesktopLayers* offscreenBaseScene=nullptr;
using OffscreenFrameScenes = DesktopFrameScenes<WmDesktopLayers, BROOK_GPU_HANDOFF_MAX_DEPTH>;
static OffscreenFrameScenes offscreenFrameScenes;
static uint64_t offscreenSceneOwner=0,offscreenSceneId=0,offscreenDisplay=0,nextSceneId=1;
static uint64_t offscreenBackdrop=0;
static uint32_t offscreenChromeMask=0;
static bool offscreenChromeTracking=false;
enum : uint32_t {ScanoutIdle,ScanoutQueued,ScanoutRunning,ScanoutDone};
static struct {
    uint32_t state=ScanoutIdle;
    BdwGpuBuffer* source=nullptr;
    uint64_t token=0;
    CompositorPresentRegion regions[COMPOSITOR_MAX_REGIONS]{};
    uint32_t count=0;
    bool completed=false;
} scanoutWork;
static bool ScanoutPending(void*){return __atomic_load_n(&scanoutWork.state,__ATOMIC_ACQUIRE)==ScanoutQueued;}
// This runs between RCS submissions on the same BSP kernel worker. The
// presenter holds the scene and framebuffer locks. The worker takes neither.
static bool ServiceScanout(void*,BdwSubmissionQueue& queue){
    if(__atomic_load_n(&scanoutWork.state,__ATOMIC_ACQUIRE)!=ScanoutQueued || queue.Poisoned())return false;
    uint32_t queued=ScanoutQueued;
    if(!__atomic_compare_exchange_n(&scanoutWork.state,&queued,ScanoutRunning,false,
                                    __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    scanoutWork.completed=BdwExecuteBcsScanout(scanoutWork.source,scanoutWork.token,scanoutWork.regions,scanoutWork.count);
    __atomic_store_n(&scanoutWork.state,ScanoutDone,__ATOMIC_RELEASE);
    return true;
}
static void FreeOffscreenScene(WmDesktopLayers* scene){if(scene){WmReleaseDesktopLayers(scene);kfree(scene);}}
// Called with offscreenMutex held. Return retired scene references to the
// caller so their final release occurs outside the session lock.
static void CollectRetiredFrameScenes(WmDesktopLayers* (&retired)[BROOK_GPU_HANDOFF_MAX_DEPTH],unsigned& count){
    for(auto& e:offscreenFrameScenes.entries)if(e.ticket && !offscreenSession.ContainsTicket(e.owner,e.ticket)){
        if(count<BROOK_GPU_HANDOFF_MAX_DEPTH){retired[count++]=e.scene;e={};}
    }
}
static bool InitOffscreenMutex(){
    uint32_t state=__atomic_load_n(&offscreenInit,__ATOMIC_ACQUIRE);
    if(!state){uint32_t expected=0;
        if(__atomic_compare_exchange_n(&offscreenInit,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
            KMutexInit(&offscreenMutex);KMutexInit(&presentMutex);KSemaphoreInit(&presentWake,0);
            __atomic_store_n(&offscreenInit,2,__ATOMIC_RELEASE);return true;
        }
        state=expected;
    }
    return state==2;
}
static void AsyncPresentWorker(void*){
    for(;;){
        KSemaphoreWait(&presentWake);
        KMutexLock(&presentMutex);
        if(!presentQueue.Take()){KMutexUnlock(&presentMutex);continue;}
        auto request=presentRequest;
        KMutexUnlock(&presentMutex);

        const auto result=request.count?
            BdwOffscreenHandoffPresentRegions(request.owner,request.frame.ticket,request.regions,request.count,true):
            BdwOffscreenHandoffPresentProbe(request.owner,request.frame,true,false,false,true);

        KMutexLock(&presentMutex);
        if(!presentQueue.Complete(result))KernelPanic("Broadwell presentation lost its queued ticket");
        if(!result)__atomic_add_fetch(&asyncPresentPresented,1,__ATOMIC_RELAXED);
        KMutexUnlock(&presentMutex);
    }
}
// Queue lock is never held across scene or GPU work. Admission, detach and
// release take the session lock first. The presenter retains it through retire.
static bool QueuedPresentHolds(uint64_t owner,uint64_t ticket){
    KMutexLock(&presentMutex);
    const bool held=presentQueue.Holds(owner,ticket);
    KMutexUnlock(&presentMutex);
    return held;
}
static void DropQueuedPresent(uint64_t owner){
    KMutexLock(&presentMutex);
    presentQueue.DropOwner(owner);
    KMutexUnlock(&presentMutex);
}
int64_t BdwQueueOffscreenPresent(uint64_t owner,uint64_t ticket,uint32_t width,uint32_t height,uint32_t stride,
                                 const CompositorPresentRegion* regions,uint32_t count){
    if(!owner || !ticket || count>COMPOSITOR_MAX_REGIONS || (count && !regions) ||
       (!count && (!width || !height || width>16384 || height>16384 || stride<width || stride>16384)))return -22;
    if(!BdwRenderExperimentEnabled())return -19;
    if(!InitOffscreenMutex() || !KMutexTryLock(&offscreenMutex))return -16;
    int64_t result=-116;
    auto* scene=offscreenFrameScenes.Find(owner,ticket);
    if(scene && (!count || !scene->fullOnly) && offscreenSession.Observe(owner,ticket)==1){
        if(!presentWorkerStarted){
            if(auto* worker=KernelThreadCreate("gpu-present",AsyncPresentWorker,nullptr)){
                presentWorkerStarted=true;
                SchedulerAddProcess(worker);
            }
        }
        result=presentWorkerStarted?-16:-12;
        if(presentWorkerStarted){
            KMutexLock(&presentMutex);
            if(presentQueue.Admit(owner,ticket)){
                presentRequest.owner=owner;
                presentRequest.frame={ticket,0,0,width,height,stride,0,{}};
                presentRequest.count=count;
                for(uint32_t i=0;i<count;++i)presentRequest.regions[i]=regions[i];
                __atomic_add_fetch(&asyncPresentAccepted,1,__ATOMIC_RELAXED);
                result=0;
            }
            KMutexUnlock(&presentMutex);
        }
    }
    KMutexUnlock(&offscreenMutex);
    if(!result)KSemaphoreSignal(&presentWake);
    return result;
}
int64_t BdwOffscreenPresentResult(uint64_t owner,uint64_t ticket){
    if(!InitOffscreenMutex())return -16;
    KMutexLock(&presentMutex);
    const auto result=presentQueue.Result(owner,ticket);
    KMutexUnlock(&presentMutex);
    return result;
}
void BdwOffscreenPresentCounts(uint64_t& accepted,uint64_t& presented){
    accepted=__atomic_load_n(&asyncPresentAccepted,__ATOMIC_RELAXED);
    presented=__atomic_load_n(&asyncPresentPresented,__ATOMIC_RELAXED);
}
bool BdwOffscreenRendererOwnedBy(uint64_t owner){
    if(!owner || !InitOffscreenMutex())return false;
    KMutexLock(&offscreenMutex);const bool result=offscreenSession.OwnedBy(owner);KMutexUnlock(&offscreenMutex);return result;
}
static int64_t InitApplicationWorker(){
    static uint32_t state=0;
    uint32_t current=__atomic_load_n(&state,__ATOMIC_ACQUIRE);
    if(!current){uint32_t expected=0;
        if(__atomic_compare_exchange_n(&state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
            const bool ok=BdwStartKernelSubmissionWorker(applicationWorker,nullptr,BdwExecuteRetainedContextWorkerSnapshot,true,ServiceScanout,ScanoutPending);
            __atomic_store_n(&state,ok?2u:3u,__ATOMIC_RELEASE);current=ok?2u:3u;
        }else current=expected;
    }
    if(current!=2)return current==1?-16:-19;
    return 0;
}
// offscreenMutex serializes the producer and holds the exact completed
// source until the worker finishes. Cancellation can win only before the
// worker claims the request.
enum class BdwPresentCopy { Completed, Unavailable, Expired };
static BdwPresentCopy PresentOnBcs(BdwGpuBuffer* source,uint64_t token,const CompositorPresentRegion* regions,uint32_t count){
    if(!source || !token || !regions || !count || count>COMPOSITOR_MAX_REGIONS ||
       !BdwBcsScanoutEnabled() || InitApplicationWorker() ||
       __atomic_load_n(&scanoutWork.state,__ATOMIC_ACQUIRE)!=ScanoutIdle)return BdwPresentCopy::Unavailable;
    scanoutWork.source=source;scanoutWork.token=token;scanoutWork.count=count;
    for(uint32_t i=0;i<count;++i)scanoutWork.regions[i]=regions[i];
    __atomic_store_n(&scanoutWork.state,ScanoutQueued,__ATOMIC_RELEASE);
    const auto deadline=ApicTickCount()+32;
    for(;;){
        if(__atomic_load_n(&scanoutWork.state,__ATOMIC_ACQUIRE)==ScanoutDone){
            const bool result=scanoutWork.completed;
            scanoutWork.source=nullptr;
            __atomic_store_n(&scanoutWork.state,ScanoutIdle,__ATOMIC_RELEASE);
            return result?BdwPresentCopy::Completed:BdwPresentCopy::Unavailable;
        }
        if(ApicTickCount()>=deadline){
            uint32_t queued=ScanoutQueued;
            if(__atomic_compare_exchange_n(&scanoutWork.state,&queued,ScanoutIdle,false,
                                           __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
                scanoutWork.source=nullptr;
                return BdwPresentCopy::Expired;
            }
        }
        SchedulerSleepMs(1);
    }
}
static int64_t SubmitApplicationSnapshot(BdwExecSnapshot* snapshot){
    if(!snapshot || !BdwExecSnapshotContext(snapshot))return -22;
    const auto ready=InitApplicationWorker();return ready?ready:applicationWorker.Submit(snapshot);
}
int64_t BdwAcquireApplicationBufferFence(BdwGpuBuffer* buffer,BdwFence** out){
    if(!buffer || !out)return -22;*out=nullptr;
    const auto ready=InitApplicationWorker();if(ready)return ready;
    if(BdwGpuBufferGpuFailed(buffer))return -5;
    auto* fence=applicationWorker.AcquireBufferFence(buffer);
    if(!fence)return BdwGpuBufferGpuFailed(buffer)?-5:-11;
    *out=fence;return 0;
}
int64_t BdwSubmitStoreProbe(void*,BdwExecSnapshot* snapshot){
    if(!BdwStoreProbeEnabled())return -19;
    if(!snapshot || !BdwExecSnapshotContext(snapshot) || !BdwExecSnapshotShadowStoreProbe(snapshot))return -22;
    return SubmitApplicationSnapshot(snapshot);
}
int64_t BdwSubmitRenderExperiment(void*,BdwExecSnapshot* snapshot){
    if(!BdwRenderExperimentEnabled())return -19;
    return SubmitApplicationSnapshot(snapshot);
}
int64_t BdwBeginRenderPreparation(void*,uint64_t owner,BdwSubmissionPreparation& entry){
    const auto ready=InitApplicationWorker();return ready?ready:applicationWorker.BeginPreparation(owner,entry);
}
void BdwEndRenderPreparation(void*,BdwSubmissionPreparation& entry){applicationWorker.EndPreparation(entry);}
int64_t BdwOffscreenRegisterScene(uint64_t owner,WmDesktopLayers* layers,uint64_t* token){
    if(!BdwRenderExperimentEnabled())return -19;
    if(!owner || !layers || !token || !InitOffscreenMutex())return -22;
    KMutexLock(&offscreenMutex);
    int64_t result=0;WmDesktopLayers* old=nullptr;
    if(!offscreenSession.OwnedBy(owner))result=-1;
    else if(nextSceneId==~uint64_t{0})result=-75;
    else if(!WmDesktopLayersCurrent(*layers))result=-116;
    else {
        const auto display=CompositorDisplayGeneration();
        if(!display)result=-19;
        else {
            CompositorBackdropInfo current{};
            const bool sameOwner=offscreenScene && offscreenSceneOwner==owner;
            const bool retainBackdrop=sameOwner && offscreenBackdrop && offscreenDisplay==display &&
                CompositorGetBackdropInfo(&current) && current.display==display && current.generation==offscreenBackdrop;
            const uint32_t carried=sameOwner && offscreenChromeTracking?
                DesktopChromeCarry(*offscreenScene,offscreenChromeMask,*layers):0;
            old=offscreenScene;offscreenScene=layers;offscreenSceneOwner=owner;
            offscreenSceneId=++nextSceneId;offscreenDisplay=display;
            if(!retainBackdrop)offscreenBackdrop=0;
            offscreenChromeMask=carried;if(!sameOwner)offscreenChromeTracking=false;
            *token=offscreenSceneId;
        }
    }
    KMutexUnlock(&offscreenMutex);FreeOffscreenScene(old);return result;
}
int64_t BdwOffscreenRegisterBackdrop(uint64_t owner,const CompositorBackdropInfo& info){
    if(!BdwRenderExperimentEnabled())return -19;
    if(!owner || !InitOffscreenMutex())return -22;
    KMutexLock(&offscreenMutex);int64_t result=0;CompositorBackdropInfo current{};
    if(!offscreenSession.OwnedBy(owner) || offscreenSceneOwner!=owner || !offscreenScene)result=-1;
    else if(!CompositorGetBackdropInfo(&current) || current.display!=info.display ||
            current.display!=offscreenDisplay || current.generation!=info.generation ||
            current.width!=info.width || current.height!=info.height)result=-116;
    else offscreenBackdrop=info.generation;
    KMutexUnlock(&offscreenMutex);return result;
}
int64_t BdwOffscreenRegisterChrome(uint64_t owner,const WmDesktopLayer& layer){
    if(!BdwRenderExperimentEnabled())return -19;
    if(!owner || !InitOffscreenMutex())return -22;
    KMutexLock(&offscreenMutex);int64_t result=0;
    if(!offscreenSession.OwnedBy(owner) || offscreenSceneOwner!=owner || !offscreenScene)result=-1;
    else if(WmDesktopLayersUseIfPresentable(*offscreenScene,nullptr,[](void*)->int64_t{return 0;}))result=-116;
    else if(!DesktopChromeBind(*offscreenScene,layer,offscreenChromeMask))result=-116;
    else offscreenChromeTracking=true;
    KMutexUnlock(&offscreenMutex);return result;
}
int64_t BdwOffscreenHandoffControl(uint64_t owner,uint64_t op,uint64_t arg,BdwGpuBuffer* buffer,uint64_t baseline){
    if(!BdwRenderExperimentEnabled())return -19;
    if(!owner || !InitOffscreenMutex())return -16;
    KMutexLock(&offscreenMutex);int64_t result=-22;WmDesktopLayers* old=nullptr;WmDesktopLayers* oldBase=nullptr;bool detached=false;
    WmDesktopLayers* retired[BROOK_GPU_HANDOFF_MAX_DEPTH]={};unsigned retiredCount=0;
    (void)offscreenSession.Idle();CollectRetiredFrameScenes(retired,retiredCount);
    switch(op){
    case BROOK_GPU_HANDOFF_ATTACH:
        if(!arg || (arg>=BROOK_GPU_HANDOFF_DEFAULT_DEPTH && arg<=BROOK_GPU_HANDOFF_MAX_DEPTH)) {
            const auto depth=arg?static_cast<uint32_t>(arg):BROOK_GPU_HANDOFF_DEFAULT_DEPTH;
            result=offscreenSession.Attach(owner,depth)?0:-16;
        }
        break;
    case BROOK_GPU_HANDOFF_RESERVE:
    case BROOK_GPU_HANDOFF_RESERVE_DEPENDENT:
        if(offscreenSceneOwner==owner && offscreenScene){
            if(op==BROOK_GPU_HANDOFF_RESERVE_DEPENDENT &&
               (!offscreenBaseScene || !DesktopDependencyBaselineCurrent(baseline,offscreenSession.LastPresented(owner)))){result=-116;break;}
            bool freeBinding=false;for(const auto& e:offscreenFrameScenes.entries)if(!e.ticket)freeBinding=true;
            if(!freeBinding){result=-16;break;}
            auto* snapshot=static_cast<WmDesktopLayers*>(kmalloc(sizeof(WmDesktopLayers)));
            if(!snapshot){result=-12;break;}
            if(!WmRetainDesktopLayers(*offscreenScene,snapshot)){kfree(snapshot);result=-22;break;}
            // Damage for a second capture may be relative to an output that
            // has not presented yet. Require a complete output until that
            // dependency is represented explicitly.
            const bool fullOnly=op!=BROOK_GPU_HANDOFF_RESERVE_DEPENDENT && !offscreenSession.Idle();
            result=offscreenSession.Reserve(owner,buffer,offscreenSceneId,offscreenDisplay);
            if(result>0){
                OffscreenFrameScenes::Binding binding{owner,static_cast<uint64_t>(result),offscreenSceneId,
                    offscreenDisplay,offscreenBackdrop,offscreenChromeMask,offscreenChromeTracking,snapshot,fullOnly};
                if(!offscreenFrameScenes.Insert(binding)){(void)offscreenSession.Release(owner,result);result=-5;old=snapshot;}
            }else old=snapshot;
        }else if(op==BROOK_GPU_HANDOFF_RESERVE_DEPENDENT)result=-116;
        else result=offscreenSession.Reserve(owner,buffer);
        break;
    case BROOK_GPU_HANDOFF_OBSERVE: result=offscreenSession.Observe(owner,arg);break;
    case BROOK_GPU_HANDOFF_RELEASE: result=QueuedPresentHolds(owner,arg)?-16:offscreenSession.Release(owner,arg);break;
    case BROOK_GPU_HANDOFF_CANCEL_BLOCKED: result=offscreenSession.CancelBlocked(owner,arg,applicationWorker);break;
    case BROOK_GPU_HANDOFF_RECOVERY_BEGIN: result=offscreenSession.BeginBlockedRecovery(owner,arg,applicationWorker);break;
    case BROOK_GPU_HANDOFF_RECOVERY_STATUS: if(!arg){BdwExecOwnerWork work{};
        if(!offscreenSession.RecoveryWork(owner,applicationWorker,work))result=-2;
        else result=(work.queued?BROOK_GPU_RECOVERY_QUEUED:0) | (work.active?BROOK_GPU_RECOVERY_ACTIVE:0) |
                    (work.quarantined?BROOK_GPU_RECOVERY_QUARANTINED:0) | (work.unknown?BROOK_GPU_RECOVERY_UNKNOWN:0) |
                    (work.retiring?BROOK_GPU_RECOVERY_RETIRING:0) | (work.preparing?BROOK_GPU_RECOVERY_PREPARING:0);
        }break;
    case BROOK_GPU_HANDOFF_RECOVERY_ABORT: if(!arg)result=offscreenSession.AbandonRecovery(owner,applicationWorker)?0:-2;break;
#ifdef BROOK_GPU_BLOCKED_RECOVERY_TEST
    case BROOK_GPU_HANDOFF_TEST_BLOCK_NEXT: result=offscreenSession.TestBlockNext(owner,arg)?0:-22;break;
#endif
    case BROOK_GPU_HANDOFF_DETACH: if(!arg){detached=offscreenSession.OwnedBy(owner);
        if(!offscreenSession.Detach(owner)){detached=false;result=-16;break;}result=0;
        if(offscreenSceneOwner==owner){old=offscreenScene;offscreenScene=nullptr;oldBase=offscreenBaseScene;offscreenBaseScene=nullptr;offscreenSceneOwner=offscreenSceneId=offscreenDisplay=offscreenBackdrop=0;offscreenChromeMask=0;offscreenChromeTracking=false;}}break;
    }
    if(detached){DropQueuedPresent(owner);CompositorReleasePresentLease(owner);}
    CollectRetiredFrameScenes(retired,retiredCount);
    KMutexUnlock(&offscreenMutex);FreeOffscreenScene(old);FreeOffscreenScene(oldBase);
    for(unsigned i=0;i<retiredCount;++i)FreeOffscreenScene(retired[i]);return result;
}
static bool CopyOffscreenBuffer(void* source,uint64_t offset,void* destination,uint64_t bytes){
    auto* b=static_cast<BdwGpuBuffer*>(source);auto* out=static_cast<uint8_t*>(destination);
    if(offset>BdwGpuBufferBytes(b) || bytes>BdwGpuBufferBytes(b)-offset)return false;
    while(bytes){
        const uint64_t within=offset&4095,chunk=bytes<4096-within?bytes:4096-within;
        const uint64_t page=BdwGpuBufferPageKernelAddress(b,uint32_t(offset/4096));if(!page)return false;
        __builtin_memcpy(out,reinterpret_cast<const void*>(page+within),chunk);
        out+=chunk;offset+=chunk;bytes-=chunk;
    }
    return true;
}
// The handoff session pins the completed source through retirement. Reserve
// the fixed framebuffer while holding the WM lock, then release it before
// queueing the copy or waiting for the device.
static int64_t PresentGopWrite(const WmDesktopLayers& scene,const WmDesktopLayers* base,
                              BdwGpuBuffer* source,uint64_t token,const CompositorPresentRegion* regions,
                              uint32_t count,uint64_t first,uint64_t bytes,uint64_t display,
                              uint64_t backdrop,uint64_t owner,uint32_t* samples){
    struct Admission {
        const CompositorPresentRegion* regions;
        uint32_t count;
        uint64_t capacity,display,backdrop,owner;
        bool partial;
        CompositorGopWrite write{};
    } admission{regions,count,BdwGpuBufferBytes(source),display,backdrop,owner,base!=nullptr};
    auto reserve=[](void* opaque)->int64_t {
        auto& a=*static_cast<Admission*>(opaque);
        return CompositorReserveGopWrite(a.write,a.regions,a.count,a.capacity,
                                         a.display,a.backdrop,a.owner,a.partial);
    };
    const auto admitted=base?WmDesktopLayersUseIfBothPresentable(*base,scene,&admission,reserve):
                             WmDesktopLayersUseIfPresentable(scene,&admission,reserve);
    if(admitted)return admitted;
    const auto copied=PresentOnBcs(source,token,regions,count);
    bool touched=copied==BdwPresentCopy::Completed;
    int result=copied==BdwPresentCopy::Expired?-62:0;
    if(copied==BdwPresentCopy::Unavailable){
        // The worker may have reached the framebuffer before reporting that
        // the accelerated path could not complete. A failing fallback
        // must force the software compositor to repair the whole scanout.
        touched=true;
        if(!BdwGpuBufferPrepareHandoffRead(source,token,first,bytes))result=-5;
        else result=CompositorCopyGopWrite(admission.write,regions,count,source,CopyOffscreenBuffer);
    }
    if(!result && samples)CompositorSampleGopWrite(admission.write,samples);
    return CompositorFinishGopWrite(admission.write,result,touched);
}
int64_t BdwOffscreenHandoffPresentProbe(uint64_t owner,::BrookGpuPresentProbe& probe,bool lease,bool region,bool collectSamples,
                                       bool fromQueue){
    if(!BdwRenderExperimentEnabled())return -19;
    if(!owner || (region && !lease) || (!collectSamples && (!lease || region)) || !probe.ticket || probe.reserved || !probe.width || !probe.height ||
       probe.width>16384 || probe.height>16384 || probe.stride<probe.width || probe.stride>16384)return -22;
    if(!InitOffscreenMutex())return -16;
    KMutexLock(&offscreenMutex);
    if(!fromQueue && QueuedPresentHolds(owner,probe.ticket)){KMutexUnlock(&offscreenMutex);return -16;}
    WmDesktopLayers* newBase=nullptr;WmDesktopLayers* oldBase=nullptr;
    auto* frame=offscreenFrameScenes.Find(owner,probe.ticket);
    if(lease && !region && frame){
        newBase=static_cast<WmDesktopLayers*>(kmalloc(sizeof(WmDesktopLayers)));
        if(!newBase){KMutexUnlock(&offscreenMutex);return -12;}
        if(!WmRetainDesktopLayers(*frame->scene,newBase)){kfree(newBase);KMutexUnlock(&offscreenMutex);return -22;}
    }
    const auto result=offscreenSession.PresentProbe(owner,probe.ticket,[&](BdwGpuBuffer* buffer,uint64_t token,uint64_t scene,uint64_t display)->int64_t{
        const uint64_t required=(uint64_t(probe.height-1)*probe.stride+probe.width)*4;
        if(!buffer || required>BdwGpuBufferBytes(buffer))return -22;
        if(scene==1){
            if(lease)return -22;
            if(!BdwGpuBufferPrepareHandoffRead(buffer,token,0,required))return -5;
            return CompositorPresentProbe(probe.x,probe.y,probe.width,probe.height,probe.stride,
                buffer,CopyOffscreenBuffer,probe.samples);
        }
        if(!frame || frame->sceneId!=scene || frame->display!=display)return -116;
        const DesktopDependencyRect single{probe.x,probe.y,probe.width,probe.height};
        if(region && (!offscreenBaseScene || frame->fullOnly ||
                      !DesktopDependenciesCovered(frame->dependencies,&single,1)))return -116;
        if(lease && !frame->backdrop)return -22;
        if(lease && frame->chromeTracking && !DesktopChromeComplete(frame->scene->count,frame->chromeMask))return -116;
        if(lease && !region && BdwBcsScanoutEnabled()){
            const CompositorPresentRegion rect{probe.x,probe.y,probe.width,probe.height,probe.stride,0};
            const auto presented=PresentGopWrite(*frame->scene,nullptr,buffer,token,&rect,1,0,required,
                                                  display,frame->backdrop,owner,collectSamples?probe.samples:nullptr);
            if(presented!=-95){
                if(!presented)offscreenFrameScenes.RecordAccepted(owner,probe.ticket,nullptr,0,true);
                return presented;
            }
        }
        if(!BdwGpuBufferPrepareHandoffRead(buffer,token,0,required))return -5;
        struct PresentContext {::BrookGpuPresentProbe* probe;BdwGpuBuffer* buffer;uint64_t display,backdrop,owner;bool region,collectSamples;};
        PresentContext context{&probe,buffer,display,frame->backdrop,lease?owner:0,region,collectSamples};
        auto present=[](void* raw)->int64_t{
            auto& c=*static_cast<PresentContext*>(raw);auto& p=*c.probe;
            return CompositorPresentProbe(p.x,p.y,p.width,p.height,p.stride,c.buffer,CopyOffscreenBuffer,c.collectSamples?p.samples:nullptr,
                c.display,c.backdrop,c.owner,c.region);
        };
        const int64_t outcome=region?WmDesktopLayersUseIfBothPresentable(*offscreenBaseScene,*frame->scene,&context,present):
            (lease?WmDesktopLayersUseIfPresentable(*frame->scene,&context,present):WmDesktopLayersUseIfCurrent(*frame->scene,&context,present));
        if(!outcome && lease)offscreenFrameScenes.RecordAccepted(owner,probe.ticket,region?&single:nullptr,region?1:0,!region);
        return outcome;
    });
    if(!result && newBase){oldBase=offscreenBaseScene;offscreenBaseScene=newBase;newBase=nullptr;}
    WmDesktopLayers* retired[BROOK_GPU_HANDOFF_MAX_DEPTH]={};unsigned retiredCount=0;CollectRetiredFrameScenes(retired,retiredCount);
    KMutexUnlock(&offscreenMutex);FreeOffscreenScene(oldBase);FreeOffscreenScene(newBase);
    for(unsigned i=0;i<retiredCount;++i)FreeOffscreenScene(retired[i]);return result;
}
// Internal only: regions must be immutable kernel-owned metadata. A future
// syscall must copy/validate them and prove scene damage coverage first.
int64_t BdwOffscreenHandoffPresentRegions(uint64_t owner,uint64_t ticket,const CompositorPresentRegion* regions,uint32_t count,
                                         bool fromQueue){
    if(!BdwRenderExperimentEnabled())return -19;
    if(!owner || !ticket || !regions || !count || count>COMPOSITOR_MAX_REGIONS)return -22;
    if(!InitOffscreenMutex())return -16;
    KMutexLock(&offscreenMutex);
    if(!fromQueue && QueuedPresentHolds(owner,ticket)){KMutexUnlock(&offscreenMutex);return -16;}
    auto* frame=offscreenFrameScenes.Find(owner,ticket);
    DesktopDependencyRect writes[COMPOSITOR_MAX_REGIONS]{};
    for(uint32_t i=0;i<count;++i)writes[i]={regions[i].x,regions[i].y,regions[i].width,regions[i].height};
    const auto result=offscreenSession.PresentProbe(owner,ticket,[&](BdwGpuBuffer* buffer,uint64_t token,uint64_t scene,uint64_t display)->int64_t{
        if(!buffer || !frame || frame->fullOnly || !offscreenBaseScene || frame->sceneId!=scene || frame->display!=display || !frame->backdrop)return -116;
        if(!DesktopDependenciesCovered(frame->dependencies,writes,count))return -116;
        if(frame->chromeTracking && !DesktopChromeComplete(frame->scene->count,frame->chromeMask))return -116;
        const uint64_t capacity=BdwGpuBufferBytes(buffer);
        uint64_t start=capacity,end=0;
        for(uint32_t i=0;i<count;++i) {
            const auto& r=regions[i];
            if(!r.width || !r.height || r.stride<r.width || r.stride>16384)return -22;
            const uint64_t bytes=(uint64_t(r.height-1)*r.stride+r.width)*4;
            if(r.offset>capacity || bytes>capacity-r.offset)return -22;
            if(r.offset<start)start=r.offset;
            if(r.offset+bytes>end)end=r.offset+bytes;
        }
        if(BdwBcsScanoutEnabled()){
            const auto presented=PresentGopWrite(*frame->scene,offscreenBaseScene,buffer,token,regions,count,
                                                  start,end-start,display,frame->backdrop,owner,nullptr);
            if(presented!=-95){
                if(!presented)offscreenFrameScenes.RecordAccepted(owner,ticket,writes,count,false);
                return presented;
            }
        }
        if(!BdwGpuBufferPrepareHandoffRead(buffer,token,start,end-start))return -5;
        struct Context {const CompositorPresentRegion* regions;uint32_t count;BdwGpuBuffer* buffer;uint64_t display,backdrop,owner;};
        Context context{regions,count,buffer,display,frame->backdrop,owner};
        auto present=[](void* raw)->int64_t {auto& c=*static_cast<Context*>(raw);
            return CompositorPresentRegions(c.regions,c.count,BdwGpuBufferBytes(c.buffer),c.buffer,
                CopyOffscreenBuffer,c.display,c.backdrop,c.owner);};
        const int64_t outcome=WmDesktopLayersUseIfBothPresentable(*offscreenBaseScene,*frame->scene,&context,present);
        if(!outcome)offscreenFrameScenes.RecordAccepted(owner,ticket,writes,count,false);
        return outcome;
    });
    WmDesktopLayers* retired[BROOK_GPU_HANDOFF_MAX_DEPTH]={};unsigned retiredCount=0;CollectRetiredFrameScenes(retired,retiredCount);
    KMutexUnlock(&offscreenMutex);for(unsigned i=0;i<retiredCount;++i)FreeOffscreenScene(retired[i]);return result;
}
void BdwOffscreenHandoffOwnerExit(uint64_t owner){
    if(!owner || __atomic_load_n(&offscreenInit,__ATOMIC_ACQUIRE)!=2)return;
    KMutexLock(&offscreenMutex);DropQueuedPresent(owner);offscreenSession.OwnerExit(owner,applicationWorker);
    WmDesktopLayers* old=nullptr;WmDesktopLayers* oldBase=nullptr;
    if(offscreenSceneOwner==owner){old=offscreenScene;offscreenScene=nullptr;oldBase=offscreenBaseScene;offscreenBaseScene=nullptr;offscreenSceneOwner=offscreenSceneId=offscreenDisplay=offscreenBackdrop=0;offscreenChromeMask=0;offscreenChromeTracking=false;}
    WmDesktopLayers* retired[BROOK_GPU_HANDOFF_MAX_DEPTH]={};unsigned retiredCount=0;CollectRetiredFrameScenes(retired,retiredCount);
    CompositorReleasePresentLease(owner);KMutexUnlock(&offscreenMutex);FreeOffscreenScene(old);FreeOffscreenScene(oldBase);
    for(unsigned i=0;i<retiredCount;++i)FreeOffscreenScene(retired[i]);
}
bool BdwOffscreenHandoffSubmit(void*,BdwExecSnapshot* snapshot,int64_t* result){
    // Every reserved output has immutable non-mappable backing. Keep the
    // session check for all aliases of protected buffers, including foreign
    // owners. Ordinary GEM snapshots can proceed without the present lock.
    if(!result || !BdwExecSnapshotHasProtectedBuffer(snapshot) || !InitOffscreenMutex())return false;
    auto* current=ProcessCurrent();const uint64_t owner=current?(current->leaderIncarnation?current->leaderIncarnation:current->incarnation):0;
    KMutexLock(&offscreenMutex);
    // The initialized worker is shared with ordinary DRM submissions.
    const auto ready=InitApplicationWorker();
    bool claimed=offscreenSession.Submit(owner,applicationWorker,snapshot,*result);
    if(claimed && ready && *result==-19)*result=ready;
    KMutexUnlock(&offscreenMutex);return claimed;
}
#endif
// The caller owns the render reservation, serialized device lease and the
// snapshot throughout. This builds a private image only; it does not validate
// arbitrary commands, authorize DMA, or submit work.
bool BdwPrepareRetainedSubmission(BdwExecSnapshot* snapshot) {
    if(!snapshot || retainedRender.State()!=BdwOneShotState::Prepared)return false;
    const uint32_t pages=BdwExecSnapshotPageCount(snapshot);
    if(!pages || pages>65536)return false;
    auto* workspace=static_cast<BdwPpgttMapping*>(kmalloc(uint64_t(pages)*sizeof(BdwPpgttMapping)));
    if(!workspace)return false;
    uint32_t status=BdwDmaNotPrepared;
    const bool prepared=gemProbeTables.Prepare(retainedRenderAllocator,snapshot) &&
        retainedRender.AttachExternalSnapshot(retainedRenderAllocator,gemProbeTables,retainedRenderAllocator,snapshot,workspace,pages) &&
        gemProbeTables.InspectDma(retainedRenderAllocator,status) && status==BdwDmaSnapshotOk &&
        gemProbeTables.Publish(retainedRenderAllocator) && retainedRender.PublishPrepared(retainedRenderAllocator) &&
        BdwExecSnapshotSyncForDevice(snapshot) && BdwExecSnapshotInspectDma(snapshot)==BdwDmaSnapshotOk &&
        retainedRender.ArmExternalSnapshot(retainedRenderAllocator,retainedRenderAllocator);
    kfree(workspace);return prepared;
}
// Detach only this stopped snapshot; the caller retains it until queue
// completion has synchronized the buffers and dropped their use counts.
bool BdwRetireRetainedSubmission(BdwExecSnapshot* snapshot) {
    return snapshot && BdwGlobalGgttSpace().owns(retainedRenderReservation) &&
        retainedRender.DetachStoppedSnapshot(retainedRenderAllocator,snapshot) &&
        retainedRender.ReleaseStopped(retainedRenderAllocator) && gemProbeTables.ReleaseStopped(retainedRenderAllocator,true);
}
// Fixed kernel-authored test only. Failed or uncertain execution retains all
// objects for this boot; a verified stopped run may retire them explicitly.
bool BdwSetupRetainedGemProbe(bool externalTables) {
    if(gemProbeFile || retainedRender.State()!=BdwOneShotState::Prepared)return false;
    if(!gemProbeQueueInitialized){if(!gemProbeQueue.Init())return false;gemProbeQueueInitialized=true;}
    gemProbeFile=BdwGpuFileCreate();if(!gemProbeFile)return false;
    uint64_t rounded=0;
    const uint32_t data=BdwGpuFileCreateBuffer(gemProbeFile,4096,rounded);
    const uint32_t batch=BdwGpuFileCreateBuffer(gemProbeFile,4096,rounded);
    if(!data || !batch)return false;
    gemProbeData=BdwGpuFileGetBuffer(gemProbeFile,data);
    gemProbeBatch=BdwGpuFileGetBuffer(gemProbeFile,batch);
    if(!gemProbeData || !gemProbeBatch)return false;
    auto* words=reinterpret_cast<uint32_t*>(BdwGpuBufferPageKernelAddress(gemProbeBatch,0));
    if(!words || !BdwGpuBufferPageKernelAddress(gemProbeData,0))return false;
    const uint32_t dataAddress=externalTables?0x40011000u:0x11000u;
    words[0]=0x10000002;words[1]=dataAddress;words[2]=0;words[3]=0x42524f52;words[4]=0x05000000;
    // Exercise the real EXECBUFFER2 decoder with fixed kernel-owned input.
    // These pointers never originate from a user and this does not expose an
    // executable ioctl or validate arbitrary command streams.
    struct ProbeObject {uint32_t handle,relocCount;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
    struct ProbeExec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clipCount;uint64_t clips,flags,context,fences;};
    static_assert(sizeof(ProbeObject)==56 && sizeof(ProbeExec)==64,"i915 probe ABI");
    const ProbeObject objects[]={{data,0,0,4096,dataAddress,16|4,0,0},{batch,0,0,4096,0x10000,16,0,0}};
    const ProbeExec exec{reinterpret_cast<uint64_t>(objects),2,0,24,0,0,0,0,uint64_t{1}<<11,0,0};
    BdwI915UserAccess kernelInput{};
    kernelInput.read=[](void*,void* destination,uint64_t source,uint64_t bytes){
        if(!destination || !source)return false;
        auto* out=static_cast<uint8_t*>(destination);const auto* in=reinterpret_cast<const uint8_t*>(source);
        for(uint64_t i=0;i<bytes;++i)out[i]=in[i];return true;
    };
    kernelInput.writable=[](void*,uint64_t,uint64_t){return false;};
    kernelInput.write=[](void*,uint64_t,const void*,uint64_t){return false;};
    kernelInput.submit=[](void*,BdwExecSnapshot* snapshot)->int64_t{
        return gemProbeQueue.Submit(snapshot);
    };
    constexpr uint64_t execCommand=0x40000000ull|(sizeof(ProbeExec)<<16)|('d'<<8)|0x69;
    if(BdwIntelRenderIoctl(gemProbeFile,execCommand,reinterpret_cast<uint64_t>(&exec),kernelInput))return false;
    uint64_t active=0;BdwExecSnapshot* queued=nullptr;
    if(!gemProbeQueue.Take(active,queued) || !active || !queued ||
       BdwGpuBufferGpuUses(gemProbeData)!=1 || BdwGpuBufferGpuUses(gemProbeBatch)!=1)return false;
    gemProbeTicket=active;gemProbeSnapshot=queued;
    gemProbeExternal=externalTables;
    if(externalTables)return BdwPrepareRetainedSubmission(gemProbeSnapshot);
    BdwPpgttMapping workspace[2]{};
    return retainedRender.AttachSnapshot(retainedRenderAllocator,gemProbeSnapshot,workspace,2) &&
        retainedRender.PublishPrepared(retainedRenderAllocator) && BdwExecSnapshotSyncForDevice(gemProbeSnapshot) &&
        BdwExecSnapshotInspectDma(gemProbeSnapshot)==BdwDmaSnapshotOk && retainedRender.ArmSnapshot(retainedRenderAllocator);
}
bool BdwFinishRetainedGemProbe(bool safelyStopped) {
    if(!gemProbeSnapshot || !gemProbeData || !safelyStopped ||
       !BdwExecSnapshotSyncForCpu(gemProbeSnapshot,true))return false;
    auto* result=reinterpret_cast<volatile uint32_t*>(BdwGpuBufferPageKernelAddress(gemProbeData,0));
    return result && *result==0x42524f52;
}
// The caller serializes the render owner and proves that the engine stopped,
// the exact GGTT range was invalidated, and CPU access is excluded. Keep the
// separately pinned scratch page and GGTT reservation for the entire boot.
bool BdwReleaseRetainedGemProbe() {
    if(!gemProbeSnapshot || !gemProbeData || !gemProbeBatch || !gemProbeFile ||
       !gemProbeTicket || !BdwGlobalGgttSpace().owns(retainedRenderReservation))return false;
    if(gemProbeExternal){if(!BdwRetireRetainedSubmission(gemProbeSnapshot))return false;}
    else if(!retainedRender.DetachStoppedSnapshot(retainedRenderAllocator,gemProbeSnapshot) ||
            !retainedRender.ReleaseStopped(retainedRenderAllocator))return false;
    if(!gemProbeQueue.Finish(gemProbeTicket,true,true))return false;
    gemProbeSnapshot=nullptr;gemProbeTicket=0;
    if(BdwGpuBufferGpuUses(gemProbeData) || BdwGpuBufferGpuUses(gemProbeBatch))return false;
    BdwGpuBufferUnref(gemProbeData);gemProbeData=nullptr;
    BdwGpuBufferUnref(gemProbeBatch);gemProbeBatch=nullptr;
    BdwGpuFileUnref(gemProbeFile);gemProbeFile=nullptr;
    return true;
}
void BdwFailRetainedGemProbe(){if(gemProbeSnapshot)BdwExecSnapshotMarkFailed(gemProbeSnapshot);}
bool BdwReprepareRetainedSubmission(const uint32_t (*saved)[20*1024],uint32_t nonce) {
    if(!nonce || !retainedRenderScratchValid || retainedRenderScratchAmbiguous ||
       !__atomic_load_n(&retainedRenderScratchPinned,__ATOMIC_ACQUIRE) ||
       !BdwGlobalGgttSpace().owns(retainedRenderReservation) || !gemProbeTables.Released() ||
       !retainedRender.Recycle(retainedRenderAllocator) || !gemProbeTables.Recycle())return false;
    if(!retainedRender.Prepare(retainedRenderAllocator,retainedRenderReservation.first,retainedRenderReservation.token,
                               nonce,retainedRenderStaging,false,saved) ||
       !retainedRender.DistinctPage(retainedRenderScratch) ||
       !retainedRenderAllocator.statusPagePrivate(retainedRenderScratch))return false;
    return retainedRender.PublishPrepared(retainedRenderAllocator);
}
bool BdwReprepareRetainedGemProbe() {
    if(!gemProbeExternal || gemProbeFile || gemProbeData || gemProbeBatch || gemProbeSnapshot || gemProbeTicket || gemProbeQueue.Poisoned())return false;
    return BdwReprepareRetainedSubmission(nullptr,0x42524f53);
}
bool BdwReuseRetainedRenderScratch(BdwOwnedPage& out,BdwGgttReservation& reservation) {
    if(!retainedRenderScratchValid || retainedRenderScratchAmbiguous ||
       !__atomic_load_n(&retainedRenderScratchPinned,__ATOMIC_ACQUIRE) ||
       !BdwGlobalGgttSpace().owns(retainedRenderReservation) || !retainedRender.DistinctPage(retainedRenderScratch) ||
       !retainedRenderAllocator.statusPagePrivate(retainedRenderScratch))return false;
    out=retainedRenderScratch;reservation=retainedRenderReservation;return true;
}
#endif
bool BdwReleasePrivateRenderImages() {
    if(retainedRenderScratchAmbiguous || !retainedRenderScratchValid ||
       __atomic_load_n(&retainedRenderScratchPinned,__ATOMIC_ACQUIRE) || !retainedRender.ScratchNeverPublished() ||
       !retainedRender.ReleasePrepared(retainedRenderAllocator))return false;
    retainedRenderAllocator.free(retainedRenderScratch);retainedRenderScratch={};retainedRenderScratchValid=false;
    return BdwGlobalGgttSpace().release(retainedRenderReservation);
}
bool BdwRetainedRenderReservation(BdwGgttReservation& out) {
    if(!retainedRenderScratchValid || retainedRenderScratchAmbiguous || retainedRender.State()!=BdwOneShotState::Prepared ||
       !BdwGlobalGgttSpace().owns(retainedRenderReservation))return false;
    out=retainedRenderReservation;return true;
}
uint32_t BdwInspectPrivateRenderDma() {
    if(retainedRender.State()!=BdwOneShotState::Prepared)return BdwDmaNotPrepared;
    if(!BdwGlobalGgttSpace().owns(retainedRenderReservation))return BdwDmaReservationLost;
    uint32_t result=BdwDmaNotPrepared;
    if(!retainedRender.InspectPrepared(retainedRenderAllocator,[&](const BdwOwnedPage* pages,unsigned count,const BdwRenderPlan&){
        result=retainedRenderAllocator.inspectDma(pages,count);return true;
    }))return BdwDmaNotPrepared;
    if(result)return result;
    return retainedRenderScratchValid && !retainedRenderScratchAmbiguous &&
        retainedRender.DistinctPage(retainedRenderScratch) && retainedRenderAllocator.statusPagePrivate(retainedRenderScratch)
        ?BdwDmaSnapshotOk:BdwDmaPageOwner;
}
uint32_t BdwInspectAdditionalRenderDma(const BdwOwnedPage* pages,unsigned count) {
    return retainedRenderAllocator.inspectDma(pages,count);
}
bool BdwPinRetainedRenderScratch(BdwOwnedPage& out,BdwGgttReservation& reservation) {
    if(!retainedRenderScratchValid || retainedRenderScratchAmbiguous ||
       !BdwGlobalGgttSpace().owns(retainedRenderReservation) || !retainedRender.DistinctPage(retainedRenderScratch) ||
       !retainedRenderAllocator.statusPagePrivate(retainedRenderScratch))return false;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&retainedRenderScratchPinned,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    out=retainedRenderScratch;reservation=retainedRenderReservation;return true;
}
bool BdwRunRetainedRender(BdwRenderExecutionBackend& backend,uint64_t scratchDma,BdwRenderRunResult& result) {
    // PMM, DMA and ownership checks happen before entering the VMM critical
    // section. An attempted transaction retains every page until recovery.
    if(!__atomic_load_n(&retainedRenderScratchPinned,__ATOMIC_ACQUIRE) || !retainedRenderScratchValid ||
       retainedRenderScratchAmbiguous || scratchDma!=retainedRenderScratch.dma ||
       !BdwGlobalGgttSpace().owns(retainedRenderReservation))return false;
    result.attempted=true;
    if(!retainedRender.ExecuteTakenOver(retainedRenderAllocator,backend,scratchDma))return false;
    result.breadcrumb=true;
    while(backend.retirementContinue()){
        const auto state=retainedRender.PollContextSwitch(backend);
        if(state==BdwCsbPoll::Fault)return false;
        if(state==BdwCsbPoll::Complete){result.contextComplete=true;return true;}
    }
    return false;
}
bool BdwCopyRetainedRenderContext(BdwRenderExecutionBackend& backend,uint32_t (&out)[20*1024]) {
    return __atomic_load_n(&retainedRenderScratchPinned,__ATOMIC_ACQUIRE) &&
        BdwGlobalGgttSpace().owns(retainedRenderReservation) && retainedRender.CopyRetiredContext(backend,out);
}
bool BdwRetireRetainedRenderAfterReset(BdwRenderExecutionBackend& backend) {
    return __atomic_load_n(&retainedRenderScratchPinned,__ATOMIC_ACQUIRE) &&
        BdwGlobalGgttSpace().owns(retainedRenderReservation) && retainedRender.RetireAfterReset(backend,true);
}
// Caller supplies a context captured from a completed, retired render run.
// Both the first allocation and this distinct allocation remain pinned.
bool BdwRetainRebasedRenderImages(const uint32_t (&saved)[20*1024]) {
    if(rebasedRender.State()!=BdwOneShotState::Fresh || retainedRender.State()!=BdwOneShotState::ContextCompleteRetained)return false;
    auto& space=BdwGlobalGgttSpace();
    if(!space.owns(retainedRenderReservation) || !space.reserve(29,rebasedRenderReservation))return false;
    if(!rebasedRender.Prepare(rebasedRenderAllocator,rebasedRenderReservation.first,rebasedRenderReservation.token,
                               0x42524f53,rebasedRenderStaging,false,&saved,2)) {
        if(rebasedRender.State()==BdwOneShotState::Released)space.release(rebasedRenderReservation);
        return false;
    }
    if(!rebasedRender.PublishPrepared(rebasedRenderAllocator)) {
        if(rebasedRender.ReleasePrepared(rebasedRenderAllocator))space.release(rebasedRenderReservation);
        return false;
    }
    BdwOwnedPage scratch{};
    if(!rebasedRenderAllocator.alloc(scratch)) {
        if(rebasedRender.ReleasePrepared(rebasedRenderAllocator))space.release(rebasedRenderReservation);
        return false;
    }
    if(!scratch.cpu || (reinterpret_cast<uintptr_t>(scratch.cpu)&4095) || !scratch.dma ||
       (scratch.dma&4095) || scratch.dma>=(uint64_t{1}<<39) ||
       (scratch.dma<0xdf800000ull && scratch.dma+4096>0xdd000000ull) || !rebasedRender.DistinctPage(scratch)) {
        rebasedRenderScratchAmbiguous=true;return false;
    }
    rebasedRenderScratch=scratch;for(unsigned i=0;i<4096;++i)scratch.cpu[i]=0;
    if(!rebasedRenderAllocator.publish(&rebasedRenderScratch,1)) {
        rebasedRenderAllocator.free(rebasedRenderScratch);rebasedRenderScratch={};
        if(rebasedRender.ReleasePrepared(rebasedRenderAllocator))space.release(rebasedRenderReservation);
        return false;
    }
    rebasedRenderScratchValid=true;return true;
}
bool BdwRebasedRenderReservation(BdwGgttReservation& out) {
    if(!rebasedRenderScratchValid || rebasedRenderScratchAmbiguous || rebasedRender.State()!=BdwOneShotState::Prepared ||
       !BdwGlobalGgttSpace().owns(rebasedRenderReservation))return false;
    out=rebasedRenderReservation;return true;
}
uint32_t BdwInspectRebasedRenderDma() {
    if(rebasedRender.State()!=BdwOneShotState::Prepared)return BdwDmaNotPrepared;
    if(!BdwGlobalGgttSpace().owns(rebasedRenderReservation))return BdwDmaReservationLost;
    uint32_t result=BdwDmaNotPrepared;
    if(!rebasedRender.InspectPrepared(rebasedRenderAllocator,[&](const BdwOwnedPage* pages,unsigned count,const BdwRenderPlan&){
        result=rebasedRenderAllocator.inspectDma(pages,count);return true;
    }))return BdwDmaNotPrepared;
    if(result)return result;
    return rebasedRenderScratchValid && !rebasedRenderScratchAmbiguous && rebasedRender.DistinctPage(rebasedRenderScratch) &&
        rebasedRenderAllocator.statusPagePrivate(rebasedRenderScratch)?BdwDmaSnapshotOk:BdwDmaPageOwner;
}
bool BdwPinRebasedRenderScratch(BdwOwnedPage& out,BdwGgttReservation& reservation) {
    if(!rebasedRenderScratchValid || rebasedRenderScratchAmbiguous || !BdwGlobalGgttSpace().owns(rebasedRenderReservation) ||
       !rebasedRender.DistinctPage(rebasedRenderScratch) || !rebasedRenderAllocator.statusPagePrivate(rebasedRenderScratch))return false;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&rebasedRenderScratchPinned,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    out=rebasedRenderScratch;reservation=rebasedRenderReservation;return true;
}
bool BdwRunRebasedRender(BdwRenderExecutionBackend& backend,uint64_t scratchDma,BdwRenderRunResult& result) {
    if(!__atomic_load_n(&rebasedRenderScratchPinned,__ATOMIC_ACQUIRE) || !rebasedRenderScratchValid ||
       rebasedRenderScratchAmbiguous || scratchDma!=rebasedRenderScratch.dma || !BdwGlobalGgttSpace().owns(rebasedRenderReservation))return false;
    result.attempted=true;if(!rebasedRender.ExecuteTakenOver(rebasedRenderAllocator,backend,scratchDma))return false;
    result.breadcrumb=true;
    while(backend.retirementContinue()){
        const auto state=rebasedRender.PollContextSwitch(backend);
        if(state==BdwCsbPoll::Fault)return false;
        if(state==BdwCsbPoll::Complete){result.contextComplete=true;return true;}
    }return false;
}
bool BdwCopyRebasedRenderContext(BdwRenderExecutionBackend& backend,uint32_t (&out)[20*1024]) {
    return __atomic_load_n(&rebasedRenderScratchPinned,__ATOMIC_ACQUIRE) &&
        BdwGlobalGgttSpace().owns(rebasedRenderReservation) && rebasedRender.CopyRetiredContext(backend,out);
}
bool BdwPreparePrivateImages() {
    PrivatePages pages;BdwBcsOneShot image;uint32_t staging[2048]{};
    if(!image.Prepare(pages,65536,1,0x42524f4f,staging))return false;
    const bool published=image.PublishPrepared(pages);
    return image.ReleasePrepared(pages) && published;
}
// Build and flush a private render context and PPGTT using real PMM pages.
// No page is mapped into the GPU, and all pages are freed before return.
bool BdwPreparePrivateRenderImages(uint32_t (&staging)[20*1024]) {
    PrivatePages allocator;BdwRenderOneShot image;
    if(!image.Prepare(allocator,65536,1,0x42524f4f,staging))return false;
    const bool published=image.PublishPrepared(allocator);
    return image.ReleasePrepared(allocator) && published;
}
}
