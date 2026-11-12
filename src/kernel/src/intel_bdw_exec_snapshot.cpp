#include "intel_bdw_exec_snapshot.h"
#include "intel_bdw_submission_timing.h"
#include "intel_bdw_buffer.h"
#include "intel_bdw_file.h"
#include "intel_bdw_fence.h"
#include "intel_bdw_render_ring.h"
#include "intel_bdw_scanout.h"
#include "intel_vtd_observe.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "panic_test_reset.h"
#include "smp.h"
#include "memory/heap.h"
#if defined(BROOK_GPU_SUBMISSION_TIMING)
#include "native_tsc_observe.h"
#include "kprintf.h"
#endif
namespace brook {
struct BdwExecSnapshot {
    BdwGpuBuffer** buffers;
    BdwExecMemoryObject* objects;
    BdwGpuContext* context;
    BdwGpuBuffer* handoffBuffer;uint64_t handoffToken;
    BdwFence* completionFence;
    BdwFence* signalFences[BdwMaxSubmissionFences];uint32_t signalCount;
    BdwFence* waitFences[BdwMaxSubmissionFences];uint32_t waitCount;
    uint32_t count,pages,state,preparedFlags,batchIndex,batchOffset,probeShadowed;
    uint32_t submitterPid,contextId;
    uint64_t batchAddress,batchLength,submitterIncarnation;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    uint64_t diagnosticQueuedTicks;
    BdwTimingObjectHints diagnosticHints;
#endif
    BdwBatchSample batchSample;
    uint32_t captureIndices[4],captureCount,captureRequested;
    uint64_t captureAddresses[4];
};
static BdwBatchSample ReadBatch(const BdwExecSnapshot* s){
    BdwBatchSample sample{};
    if(!s || s->batchIndex>=s->count || !BdwGpuBufferAllowsUserMapping(s->buffers[s->batchIndex]))return sample;
    const uint32_t size=s->batchLength<32?uint32_t(s->batchLength):32;
    for(uint32_t i=0;i<size;++i){
        const uint64_t offset=uint64_t(s->batchOffset)+i;
        const auto page=BdwGpuBufferPageKernelAddress(s->buffers[s->batchIndex],uint32_t(offset/4096));
        if(!page)break;
        const auto byte=__atomic_load_n(reinterpret_cast<const uint8_t*>(page+(offset%4096)),__ATOMIC_RELAXED);
        sample.words[i/4]|=uint32_t(byte)<<(8*(i%4));++sample.bytes;
    }
    return sample;
}
static void SampleBatch(BdwExecSnapshot* s){s->batchSample=ReadBatch(s);}
// 0: prepared/unqueued, 1: armed, 2: releasing, 3: queued/reserved,
// 4: retired with the object references retained for bookkeeping.
static bool Preparing(BdwExecSnapshot* s){
    if(!s)return false;const uint32_t state=__atomic_load_n(&s->state,__ATOMIC_ACQUIRE);
    return state==0 || state==3;
}
static void Release(BdwExecSnapshot* s){
    if(s->buffers)for(uint32_t i=0;i<s->count;++i)BdwGpuBufferUnref(s->buffers[i]);
    BdwGpuContextUnref(s->context);
    BdwFenceUnref(s->completionFence);
    for(uint32_t i=0;i<s->signalCount;++i)BdwFenceUnref(s->signalFences[i]);
    for(uint32_t i=0;i<s->waitCount;++i)BdwFenceUnref(s->waitFences[i]);
    kfree(s->buffers);kfree(s->objects);kfree(s);
}
bool BdwExecSnapshotBindContext(BdwExecSnapshot* s,BdwGpuContext* context){
    if(!s || !context || s->context || __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=0)return false;
    BdwGpuContextRef(context);s->context=context;return true;
}
BdwGpuContext* BdwExecSnapshotContext(const BdwExecSnapshot* s){return s?s->context:nullptr;}
bool BdwExecSnapshotsIndependent(const BdwExecSnapshot* a,const BdwExecSnapshot* b){
    // Intel PRIME imports retain the same BdwGpuBuffer object; independent
    // allocations own distinct physical pages. If a future importer creates
    // multiple wrappers for the same backing, this test must change with it.
    if(!a || !b || !a->context || !b->context || a->context==b->context ||
       !a->buffers || !b->buffers || !a->count || !b->count ||
       uint64_t(a->count)*b->count>4096)return false;
    for(uint32_t i=0;i<a->count;++i)for(uint32_t j=0;j<b->count;++j)
        if(a->buffers[i]==b->buffers[j])return false;
    return true;
}
bool BdwExecSnapshotAddSignalFence(BdwExecSnapshot* s,BdwFence* f){
    if(!s || !f || __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=0 || s->signalCount==BdwMaxSubmissionFences || BdwFenceState(f))return false;
    for(uint32_t i=0;i<s->signalCount;++i)if(s->signalFences[i]==f)return false;
    BdwFenceRef(f);s->signalFences[s->signalCount++]=f;return true;
}
bool BdwExecSnapshotWritesWithSignal(const BdwExecSnapshot* s,const BdwGpuBuffer* buffer,const BdwFence* fence){
    if(!s || !buffer || !fence || BdwFenceState(fence))return false;
    const uint32_t state=__atomic_load_n(&s->state,__ATOMIC_ACQUIRE);
    if(state!=0 && state!=3)return false;
    bool signal=false,retained=false,writable=false;
    for(uint32_t i=0;i<s->signalCount;++i)if(s->signalFences[i]==fence){signal=true;break;}
    for(uint32_t i=0;i<s->count;++i)if(s->buffers[i]==buffer){retained=true;break;}
    if(!signal || !retained)return false;
    uint32_t pages=0;const auto* dma=BdwGpuBufferPages(buffer,pages);
    if(!dma || !pages)return false;
    for(uint32_t i=0;i<s->count;++i)if(s->objects[i].pageDma==dma && s->objects[i].writable){writable=true;break;}
    return writable;
}
BdwFence* BdwExecSnapshotAcquireWriteSignal(const BdwExecSnapshot* s,const BdwGpuBuffer* buffer){
    if(!s || !buffer || __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=0)return nullptr;
    for(uint32_t i=0;i<s->signalCount;++i)if(BdwExecSnapshotWritesWithSignal(s,buffer,s->signalFences[i])){
        BdwFenceRef(s->signalFences[i]);return s->signalFences[i];
    }
    return nullptr;
}
bool BdwExecSnapshotContainsBuffer(const BdwExecSnapshot* s,const BdwGpuBuffer* buffer){
    if(!s || !buffer || __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=0)return false;
    for(uint32_t i=0;i<s->count;++i)if(s->buffers[i]==buffer)return true;
    return false;
}
BdwFence* BdwExecSnapshotAcquireCompletionForBuffer(const BdwExecSnapshot* s,const BdwGpuBuffer* buffer){
    if(!s || !buffer || !s->completionFence)return nullptr;
    for(uint32_t i=0;i<s->count;++i)if(s->buffers[i]==buffer){BdwFenceRef(s->completionFence);return s->completionFence;}
    return nullptr;
}
uint32_t BdwExecSnapshotBufferUseCount(const BdwExecSnapshot* s,const BdwGpuBuffer* buffer){
    uint32_t count=0;if(s && buffer)for(uint32_t i=0;i<s->count;++i)if(s->buffers[i]==buffer)++count;return count;
}
bool BdwExecSnapshotBindHandoff(BdwExecSnapshot* s,BdwGpuBuffer* buffer,BdwFence* fence,uint64_t token){
    if(!s || __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=0 || s->handoffBuffer ||
       !BdwGpuBufferHandoffMatches(buffer,token) || !BdwExecSnapshotWritesWithSignal(s,buffer,fence))return false;
    uint32_t matches=0;for(uint32_t i=0;i<s->count;++i)if(s->buffers[i]==buffer)++matches;
    if(matches!=1)return false;
    s->handoffBuffer=buffer;s->handoffToken=token;return true;
}
bool BdwExecSnapshotHandoffUndelivered(const BdwExecSnapshot* s,const BdwGpuBuffer* buffer,uint64_t token){
    return s && buffer && token && __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)==0 &&
           s->handoffBuffer==buffer && s->handoffToken==token;
}
bool BdwExecSnapshotSignalFences(BdwExecSnapshot* s,bool success){
    if(!s || __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=4)return false;
    for(uint32_t i=0;i<s->signalCount;++i)if(!BdwFenceComplete(s->signalFences[i],success))return false;
    return BdwFenceComplete(s->completionFence,success);
}
bool BdwExecSnapshotAddWaitFence(BdwExecSnapshot* s,BdwFence* f){
    if(!s || !f || __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=0 || s->waitCount==BdwMaxSubmissionFences)return false;
    for(uint32_t i=0;i<s->waitCount;++i)if(s->waitFences[i]==f)return true;
    BdwFenceRef(f);s->waitFences[s->waitCount++]=f;return true;
}
uint32_t BdwExecSnapshotDependencies(const BdwExecSnapshot* s){
    if(!s)return 2;bool pending=false;
    for(uint32_t i=0;i<s->waitCount;++i){const auto state=BdwFenceState(s->waitFences[i]);
        if(state==2)return 2;if(!state)pending=true;}
    return pending?0:1;
}
bool BdwExecSnapshotShadowStoreProbe(BdwExecSnapshot* s){
    if(!s || __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=0 || s->probeShadowed || s->count!=2 ||
       s->batchIndex>=s->count || s->batchOffset || s->batchLength!=24)return false;
    auto* original=s->buffers[s->batchIndex];
    if(!BdwGpuBufferAllowsUserMapping(original) || BdwGpuBufferBytes(original)!=4096)return false;
    const auto address=BdwGpuBufferPageKernelAddress(original,0);if(!address)return false;
    auto* source=reinterpret_cast<const uint32_t*>(address);uint32_t words[6]{};
    // Read into kernel memory before checking: a racing writer can change
    // the input, but never the checked command image executed by the GPU.
    for(unsigned i=0;i<6;++i)words[i]=__atomic_load_n(&source[i],__ATOMIC_RELAXED);
    if(words[0]!=0x10000002 || words[2] || words[4]!=0x05000000 || words[5] || (words[1]&3))return false;
    const uint64_t target=words[1];bool allowed=false;uint32_t batchObject=s->count;
    uint32_t originalPages=0;const auto* originalDma=BdwGpuBufferPages(original,originalPages);
    if(!originalDma || originalPages!=1)return false;
    for(uint32_t i=0;i<s->count;++i){const auto& o=s->objects[i];
        if(o.pageDma==originalDma){if(o.writable || o.pageCount!=1)return false;batchObject=i;continue;}
        const uint64_t bytes=uint64_t(o.pageCount)*4096;
        if(o.writable && target>=o.address && target-o.address<=bytes-4)allowed=true;
    }
    if(!allowed || batchObject==s->count)return false;
    auto* shadow=BdwGpuBufferCreate(4096);if(!shadow)return false;
    const uint64_t shadowAddress=BdwGpuBufferPageKernelAddress(shadow,0);uint32_t shadowPages=0;
    const auto* shadowDma=BdwGpuBufferPages(shadow,shadowPages);
    if(!shadowAddress || !shadowDma || shadowPages!=1){BdwGpuBufferUnref(shadow);return false;}
    auto* destination=reinterpret_cast<uint32_t*>(shadowAddress);
    for(unsigned i=0;i<6;++i)destination[i]=words[i];
    s->buffers[s->batchIndex]=shadow;s->objects[batchObject].pageDma=shadowDma;s->probeShadowed=1;
    SampleBatch(s);
    BdwGpuBufferUnref(original);return true;
}
#ifdef BROOK_HOST_TEST
uint64_t BdwExecSnapshotTestBatchKernelAddress(const BdwExecSnapshot* s){
    return s && s->batchIndex<s->count?BdwGpuBufferPageKernelAddress(s->buffers[s->batchIndex],0):0;
}
#endif
static void SortObjects(BdwExecMemoryObject* a,uint32_t count){
    auto sift=[&](uint32_t root,uint32_t end){
        while(root<end/2){uint32_t child=root*2+1;
            if(child+1<end && a[child].address<a[child+1].address)++child;
            if(a[root].address>=a[child].address)break;
            const auto tmp=a[root];a[root]=a[child];a[child]=tmp;root=child;}
    };
    for(uint32_t i=count/2;i>0;--i)sift(i-1,count);
    for(uint32_t end=count;end>1;--end){const auto tmp=a[0];a[0]=a[end-1];a[end-1]=tmp;sift(0,end-1);}
}
BdwExecSnapshot* BdwExecSnapshotCreate(BdwGpuFile* file,const BdwExecObjectInput* inputs,uint32_t count,
                                       uint32_t batchIndex,uint32_t batchStart,uint32_t batchLength){
    // ASYNC is advisory: the single worker still serializes all requests.
    // CAPTURE is a best-effort debug hint; no capture support is advertised.
    constexpr uint64_t limit=uint64_t{1}<<48,allowed=4|8|16|64|128;
    if(!file || !inputs || !count || count>8192 || batchIndex>=count || (batchStart&7))return nullptr;
    auto* s=static_cast<BdwExecSnapshot*>(kmalloc(sizeof(BdwExecSnapshot)));if(!s)return nullptr;
    *s={};
    s->buffers=static_cast<BdwGpuBuffer**>(kmalloc(uint64_t(count)*sizeof(*s->buffers)));
    s->objects=static_cast<BdwExecMemoryObject*>(kmalloc(uint64_t(count)*sizeof(*s->objects)));
    auto* handles=static_cast<uint32_t*>(kmalloc(uint64_t(count)*sizeof(uint32_t)));
    if(!s->buffers || !s->objects || !handles){kfree(handles);Release(s);return nullptr;}
    for(uint32_t i=0;i<count;++i){s->buffers[i]=nullptr;handles[i]=inputs[i].handle;}
    if(!BdwGpuFileResolveBuffers(file,handles,count,s->buffers)){kfree(handles);Release(s);return nullptr;}
    s->count=count;s->batchIndex=batchIndex;s->batchOffset=batchStart;kfree(handles);
    for(uint32_t i=0;i<count;++i){const auto& in=inputs[i];
        const uint64_t upper=in.address>>48,low=in.address&(limit-1);
        if((in.flags&~allowed) || !(in.flags&16) || (upper && (upper!=0xffff || !(low&(uint64_t{1}<<47)))) ||
           (low&4095) || (in.alignment && (in.alignment<4096 || (in.alignment&(in.alignment-1)) || (low&(in.alignment-1))))) {
            Release(s);return nullptr;
        }
        uint32_t pages=0;const auto* dma=BdwGpuBufferPages(s->buffers[i],pages);
        const uint64_t size=BdwGpuBufferBytes(s->buffers[i]);
        if(!dma || !pages || pages>65536-s->pages || size>limit-low || (!(in.flags&8) && low+size>(uint64_t{1}<<32))) {
            Release(s);return nullptr;
        }
        // ASYNC opts out of implicit object synchronization; it does not
        // declare the object read-only. Explicitly synchronized clients
        // such as HasVK may write it without setting EXEC_OBJECT_WRITE.
        s->pages+=pages;s->objects[i]={low,dma,pages,(in.flags&(4|64))!=0};
#if defined(BROOK_GPU_SUBMISSION_TIMING)
        // These are input hints, not proven access directions: ASYNC objects
        // may be written without WRITE, and duplicate backing is counted again.
        BdwTimingCountObjectHints(s->diagnosticHints,in.flags,pages);
#endif
        if(in.flags&128){++s->captureRequested;if(s->captureCount<4 && BdwGpuBufferAllowsUserMapping(s->buffers[i])){
            s->captureIndices[s->captureCount]=i;s->captureAddresses[s->captureCount]=low;++s->captureCount;}}
        if(i==batchIndex){
            if(!BdwGpuBufferAllowsUserMapping(s->buffers[i]) || (in.flags&4) || batchStart>=size ||
               (batchLength && batchLength>size-batchStart)) {Release(s);return nullptr;}
            s->batchAddress=low+batchStart;s->batchLength=batchLength?batchLength:size-batchStart;
        }
    }
    SortObjects(s->objects,count);
    for(uint32_t i=1;i<count;++i){const auto& prev=s->objects[i-1];
        if(s->objects[i].address<prev.address+uint64_t(prev.pageCount)*4096){Release(s);return nullptr;}}
    s->completionFence=BdwFenceCreate();if(!s->completionFence){Release(s);return nullptr;}
    SampleBatch(s);return s;
}
uint64_t BdwExecSnapshotBatchAddress(const BdwExecSnapshot* s){return s?s->batchAddress:0;}
uint64_t BdwExecSnapshotBatchLength(const BdwExecSnapshot* s){return s?s->batchLength:0;}
uint32_t BdwExecSnapshotPageCount(const BdwExecSnapshot* s){return s?s->pages:0;}
uint32_t BdwExecSnapshotObjectCount(const BdwExecSnapshot* s){return s?s->count:0;}
BdwBatchSample BdwExecSnapshotBatchSample(const BdwExecSnapshot* s){return s?s->batchSample:BdwBatchSample{};}
BdwBatchSample BdwExecSnapshotCurrentBatchSample(const BdwExecSnapshot* s){return ReadBatch(s);}
void BdwExecSnapshotCapture(const BdwExecSnapshot* s,BdwGpuCapture& out){
    __builtin_memset(&out,0,sizeof(out));out.version=4;if(!s)return;out.requested=s->captureRequested;out.count=s->captureCount;
    for(uint32_t i=0;i<out.count;++i){auto& entry=out.entries[i];const auto* buffer=s->buffers[s->captureIndices[i]];
        entry.address=s->captureAddresses[i];entry.size=BdwGpuBufferBytes(buffer);
        const uint32_t count=entry.size<sizeof(entry.data)?uint32_t(entry.size):sizeof(entry.data);
        for(uint32_t j=0;j<count;){
            const auto address=BdwGpuBufferPageKernelAddress(buffer,j/4096);if(!address)break;
            const uint32_t n=count-j<4096?count-j:4096;
            for(uint32_t k=0;k<n;++k)entry.data[j+k]=__atomic_load_n(reinterpret_cast<const uint8_t*>(address+k),__ATOMIC_RELAXED);
            j+=n;entry.bytes=j;
        }
    }
}
void BdwExecSnapshotSetOrigin(BdwExecSnapshot* s,uint32_t pid,uint32_t contextId,uint64_t incarnation){
    if(s && __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)==0){s->submitterPid=pid;s->contextId=contextId;s->submitterIncarnation=incarnation;}
}
uint64_t BdwExecSnapshotOwner(const BdwExecSnapshot* s){return s?s->submitterIncarnation:0;}
#if defined(BROOK_GPU_SUBMISSION_TIMING)
static BdwTimingSnapshot gpuTiming{};static uint32_t gpuTimingLock=0;static uint64_t gpuTimingDropped=0,gpuTimingOwnerHint=0;
uint64_t BdwTimingNow(){uint32_t lo,hi;__asm__ volatile("lfence; rdtsc":"=a"(lo),"=d"(hi)::"memory");return (uint64_t(hi)<<32)|lo;}
bool BdwTimingConfigure(uint64_t owner){
    if(__atomic_exchange_n(&gpuTimingLock,1,__ATOMIC_ACQUIRE))return false;
    gpuTiming={};gpuTiming.owner=owner;__atomic_store_n(&gpuTimingDropped,0,__ATOMIC_RELAXED);
    __atomic_store_n(&gpuTimingOwnerHint,owner,__ATOMIC_RELEASE);
    __atomic_store_n(&gpuTimingLock,0,__ATOMIC_RELEASE);return true;
}
bool BdwTimingWatches(uint64_t owner){return owner && __atomic_load_n(&gpuTimingOwnerHint,__ATOMIC_ACQUIRE)==owner;}
bool BdwTimingRead(BdwTimingSnapshot& out){
    if(__atomic_exchange_n(&gpuTimingLock,1,__ATOMIC_ACQUIRE))return false;
    out=gpuTiming;out.dropped=__atomic_load_n(&gpuTimingDropped,__ATOMIC_RELAXED);
    __atomic_store_n(&gpuTimingLock,0,__ATOMIC_RELEASE);return true;
}
static void BdwTimingRecordOwner(uint64_t owner,BdwTimingStage stage,uint64_t us,uint64_t pages){
    if(!owner || stage>=BdwTimingStageCount)return;
    if(__atomic_exchange_n(&gpuTimingLock,1,__ATOMIC_ACQUIRE)){__atomic_fetch_add(&gpuTimingDropped,1,__ATOMIC_RELAXED);return;}
    BdwTimingAccumulate(gpuTiming,owner,stage,us,pages);
    __atomic_store_n(&gpuTimingLock,0,__ATOMIC_RELEASE);
}
void BdwTimingRecord(const BdwExecSnapshot* s,BdwTimingStage stage,uint64_t us,uint64_t pages){
    if(s)BdwTimingRecordOwner(s->submitterIncarnation,stage,us,pages);
}
void BdwTimingOwnerElapsed(uint64_t owner,BdwTimingStage stage,uint64_t start,uint64_t end){
    const auto& t=NativeTscObserveRead();
    if(start && end>=start && t.capable && t.measured && t.consistent && t.khz && end-start<=~uint64_t{0}/1000)
        BdwTimingRecordOwner(owner,stage,(end-start)*1000/t.khz,0);
}
void BdwTimingQueued(BdwExecSnapshot* s){if(s)s->diagnosticQueuedTicks=BdwTimingNow();}
void BdwTimingTaken(BdwExecSnapshot* s){
    if(!s || !s->diagnosticQueuedTicks)return;
    const auto end=BdwTimingNow();const auto& t=NativeTscObserveRead();
    if(t.capable && t.measured && t.consistent && t.khz && end>=s->diagnosticQueuedTicks)
        BdwTimingRecord(s,BdwTimingQueue,(end-s->diagnosticQueuedTicks)*1000/t.khz,s->pages);
}
#endif
void BdwExecSnapshotOrigin(const BdwExecSnapshot* s,uint32_t& pid,uint32_t& contextId){
    pid=s?s->submitterPid:0;contextId=s?s->contextId:0;
}
bool BdwExecSnapshotTableCount(const BdwExecSnapshot* s,uint32_t& out){
    return s && BdwExecAddressSpaceTableCount(s->objects,s->count,out);
}
bool BdwExecSnapshotBuildPpgtt(BdwExecSnapshot* s,const BdwPpgttPage (&scratch)[4],const BdwPpgttPage* tables,
                               uint32_t tableCount,BdwPpgttMapping* workspace,uint32_t workspaceCount){
    if(!Preparing(s))return false;
    s->preparedFlags&=~1u;
    if(!BdwBuildExecAddressSpace(scratch,tables,tableCount,s->objects,s->count,workspace,workspaceCount))return false;
    s->preparedFlags|=1u;return true;
}
bool BdwExecSnapshotBuildRenderImage(BdwExecSnapshot* s,const BdwPpgttPage (&scratch)[4],
                                    const BdwPpgttPage* tables,uint32_t tableCount,
                                    BdwPpgttMapping* workspace,uint32_t workspaceCount,
                                    uint64_t completion,uint32_t nonce,bool gt3,uint32_t (&ring)[56]){
    if(!Preparing(s))return false;
    s->preparedFlags&=~1u;
    if(!tables || !tableCount || tableCount>128 || !workspace || !workspaceCount || workspaceCount>65536)return false;
    const uintptr_t start=reinterpret_cast<uintptr_t>(ring);
    auto overlaps=[&](uintptr_t address,uint64_t size){
        return start>~uintptr_t{0}-sizeof(ring) || address>~uintptr_t{0}-size ||
            (start<address+size && address<start+sizeof(ring));
    };
    if(overlaps(reinterpret_cast<uintptr_t>(scratch),sizeof(scratch)) ||
       overlaps(reinterpret_cast<uintptr_t>(tables),uint64_t(tableCount)*sizeof(*tables)) ||
       overlaps(reinterpret_cast<uintptr_t>(workspace),uint64_t(workspaceCount)*sizeof(*workspace)))return false;
    for(unsigned i=0;i<4;++i)if(overlaps(reinterpret_cast<uintptr_t>(scratch[i].cpu),4096))return false;
    for(uint32_t i=0;i<tableCount;++i)if(overlaps(reinterpret_cast<uintptr_t>(tables[i].cpu),4096))return false;
    uint32_t next[56]{};
    if(!BdwBuildRenderRing(BdwExecSnapshotBatchAddress(s),completion,nonce,true,gt3,next) ||
       !BdwExecSnapshotBuildPpgtt(s,scratch,tables,tableCount,workspace,workspaceCount))return false;
    for(unsigned i=0;i<56;++i)ring[i]=next[i];
    return true;
}
static bool SyncSnapshotCpuCache(const BdwExecSnapshot& snapshot,uint64_t& flushedPages) {
    flushedPages=0;
    for(uint32_t i=0;i<snapshot.count;++i) {
        auto* buffer=snapshot.buffers[i];
        if(buffer==snapshot.handoffBuffer && !BdwGpuBufferAllowsUserMapping(buffer))continue;
        if(!BdwGpuBufferSyncCpuCache(buffer))return false;
        uint32_t pages=0;
        (void)BdwGpuBufferPages(buffer,pages);
        flushedPages+=pages;
    }
    return true;
}
bool BdwExecSnapshotSyncForDevice(BdwExecSnapshot* s){
    if(!Preparing(s))return false;
    s->preparedFlags&=~2u;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const bool countBuffers=BdwTimingWatches(s->submitterIncarnation);
    uint64_t uniquePages=0,repeatedPages=0;
    if(countBuffers)BdwTimingCountBufferPages(s->buffers,s->count,[](BdwGpuBuffer* b){
        uint32_t pages=0;(void)BdwGpuBufferPages(b,pages);return pages;
    },uniquePages,repeatedPages);
    auto tick=[](){uint32_t lo,hi;__asm__ volatile("lfence; rdtsc":"=a"(lo),"=d"(hi)::"memory");return (uint64_t(hi)<<32)|lo;};
    const uint64_t start=tick();
#endif
    // The exact protected handoff has no CPU writers after initial zeroing.
    uint64_t flushedPages=0;
    if(!SyncSnapshotCpuCache(*s,flushedPages))return false;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t end=tick();const auto& tsc=NativeTscObserveRead();
    if(tsc.capable && tsc.measured && tsc.consistent && tsc.khz && end>=start) {
        BdwTimingRecord(s,BdwTimingDeviceSync,(end-start)*1000/tsc.khz,flushedPages);
        BdwTimingRecord(s,BdwTimingExplicitWritePages,0,s->diagnosticHints.explicitWritePages);
        BdwTimingRecord(s,BdwTimingAsyncPages,0,s->diagnosticHints.asyncPages);
        if(countBuffers){
            BdwTimingRecord(s,BdwTimingUniqueBufferPages,0,uniquePages);
            BdwTimingRecord(s,BdwTimingRepeatedBufferPages,0,repeatedPages);
        }
    }
#endif
    s->preparedFlags|=2u;return true;
}
bool BdwExecSnapshotSyncForCpu(BdwExecSnapshot* s,bool safelyStopped){
    if(!s || !safelyStopped || __atomic_load_n(&s->state,__ATOMIC_ACQUIRE)!=1)return false;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    auto tick=[](){uint32_t lo,hi;__asm__ volatile("lfence; rdtsc":"=a"(lo),"=d"(hi)::"memory");return (uint64_t(hi)<<32)|lo;};
    const uint64_t start=tick();
#endif
    // The compositor fallback acquires CPU visibility only if it copies this
    // output. Retiring the producer still requires the device-side flush.
    uint64_t flushedPages=0;
    if(!SyncSnapshotCpuCache(*s,flushedPages))return false;
#if defined(BROOK_GPU_SUBMISSION_TIMING)
    const uint64_t end=tick();const auto& tsc=NativeTscObserveRead();
    if(tsc.capable && tsc.measured && tsc.consistent && tsc.khz && end>=start)
        BdwTimingRecord(s,BdwTimingCpuSync,(end-start)*1000/tsc.khz,flushedPages);
#endif
    return true;
}
bool BdwExecSnapshotMarkFailed(BdwExecSnapshot* s,bool pending){
    if(!s)return false;
    const auto state=__atomic_load_n(&s->state,__ATOMIC_ACQUIRE);
    if(state!=1 && state!=3)return false;
    BdwGpuContextMarkLost(s->context,pending);
    for(uint32_t i=0;i<s->count;++i)BdwGpuBufferMarkGpuFailed(s->buffers[i]);
    return true;
}
uint32_t BdwExecSnapshotInspectDma(BdwExecSnapshot* s){
#if defined(BROOK_SSD_AUTO_RECOVERY)
    if(!Preparing(s))return BdwDmaNotPrepared;
    s->preparedFlags&=~4u;
    if(!PanicRecoveryResetArmed())return BdwDmaResetUnarmed;
    if(SmpCurrentCpuIndex()!=0)return BdwDmaWrongCpu;
    const auto& prior=DellGpuVtdObserveRead();
    if(!prior.sampled || !prior.topology || !prior.inventory || !prior.bootMap)return BdwDmaMissingVtdEvidence;
    auto acceptable=[](const DellGpuVtdStatus& v){constexpr uint64_t protectedRegions=(1ull<<5)|(1ull<<6);
        return !(v.globalStatus&(1u<<31)) && (!(v.capability&protectedRegions) ||
            (v.protectedMemorySampled && !(v.protectedMemory&0x80000001u)));};
    DellGpuVtdStatus before{},after{};
    if(!VmmDellGpuVtdCriticalRead(before))return BdwDmaFirstVtdRead;
    if(!acceptable(before))return BdwDmaFirstVtdRestricted;
    for(uint32_t i=0;i<s->count;++i){uint32_t count=0;const auto* pages=BdwGpuBufferPages(s->buffers[i],count);
        if(!pages || !count)return BdwDmaPageAddress;
        for(uint32_t j=0;j<count;++j){const uint64_t dma=pages[j],kva=BdwGpuBufferPageKernelAddress(s->buffers[i],j);
            if(!dma || (dma&4095) || dma>=(uint64_t{16}<<30) ||
               (dma<0xdf800000ull && dma+4096>0xdd000000ull) || !kva || (kva&4095) ||
               VmmVirtToPhys(KernelPageTable,VirtualAddress(kva)).raw()!=dma)return BdwDmaPageAddress;
            const PhysicalAddress physical(dma);
            // User VMAs add page references and map counts; they do not
            // transfer the buffer's original kernel ownership.
            if(PmmGetTag(physical)!=MemTag::Device || PmmGetPid(physical)!=KernelPid)return BdwDmaPageOwner;
        }
    }
    if(!VmmDellGpuVtdCriticalRead(after))return BdwDmaSecondVtdRead;
    if(!acceptable(after))return BdwDmaSecondVtdRestricted;
    if(before.version!=after.version || before.capability!=after.capability || before.globalStatus!=after.globalStatus ||
       before.protectedMemorySampled!=after.protectedMemorySampled || before.protectedMemory!=after.protectedMemory)return BdwDmaVtdChanged;
    s->preparedFlags|=4u;return BdwDmaSnapshotOk;
#else
    (void)s;return BdwDmaUnavailable;
#endif
}
bool BdwExecSnapshotReserve(BdwExecSnapshot* s){
    uint32_t expected=0;
    if(!s || !__atomic_compare_exchange_n(&s->state,&expected,3,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    uint32_t acquired=0;
    for(;acquired<s->count;++acquired){
        auto* buffer=s->buffers[acquired];
        if(!(buffer==s->handoffBuffer?BdwGpuBufferBeginHandoffGpuUse(buffer,s->handoffToken):BdwGpuBufferBeginGpuUse(buffer)))break;
    }
    if(acquired==s->count)return true;
    while(acquired)if(!BdwGpuBufferEndGpuUse(s->buffers[--acquired]))__builtin_trap();
    __atomic_store_n(&s->state,0,__ATOMIC_RELEASE);return false;
}
bool BdwExecSnapshotArm(BdwExecSnapshot* s,bool tablesPublished){
    if(!s || !tablesPublished || s->preparedFlags!=7u)return false;
    uint32_t expected=3;
    if(__atomic_compare_exchange_n(&s->state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return true;
    if(expected!=0 || !BdwExecSnapshotReserve(s))return false;
    expected=3;
    if(!__atomic_compare_exchange_n(&s->state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))__builtin_trap();
    return true;
}
bool BdwExecSnapshotRetire(BdwExecSnapshot* s,bool safelyStopped){
    if(!s)return false;
    uint32_t expected=__atomic_load_n(&s->state,__ATOMIC_ACQUIRE);
    if(expected!=1 && expected!=3)return false;
    if(expected==1 && (!safelyStopped || !BdwExecSnapshotSyncForCpu(s,true)))return false;
    if(!__atomic_compare_exchange_n(&s->state,&expected,4,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    for(uint32_t i=0;i<s->count;++i)if(!BdwGpuBufferEndGpuUse(s->buffers[i]))__builtin_trap();
    return true;
}
bool BdwExecSnapshotRelease(BdwExecSnapshot* s,bool safelyStopped){
    if(!s)return false;
    uint32_t expected=__atomic_load_n(&s->state,__ATOMIC_ACQUIRE);
    for(;;){if((expected!=0 && expected!=1 && expected!=3 && expected!=4) || (expected==1 && !safelyStopped))return false;
        if(__atomic_compare_exchange_n(&s->state,&expected,2,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;}
    if(expected==1 || expected==3)for(uint32_t i=0;i<s->count;++i)if(!BdwGpuBufferEndGpuUse(s->buffers[i]))__builtin_trap();
    Release(s);return true;
}
}
