#include "gpu_sync.h"
#include "memory/heap.h"
#include "intel_bdw_wait_queue.h"
#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
#include <new>
#else
inline void* operator new(__SIZE_TYPE__,void* storage) noexcept{return storage;}
#endif

namespace brook {
// M5.0: the DRM sync object, shared by the i915 node (Bdw* names) and the nouveau node.
struct BdwGpuSync {uint32_t references;BdwTimelineSync timeline;};
// The sync object itself is GPU-neutral (gpu_sync.h); the i915 file keeps its Bdw* API.
GpuSync* GpuSyncCreate(bool signaled){
    void* memory=kmalloc(sizeof(BdwGpuSync));if(!memory)return nullptr;
    auto* s=new(memory) BdwGpuSync{};s->references=1;
    if(!s->timeline.Init()){s->~BdwGpuSync();kfree(s);return nullptr;}
    if(signaled){auto* fence=BdwFenceCreate(true);if(!fence){GpuSyncUnref(s);return nullptr;}
        s->timeline.Replace(fence);BdwFenceUnref(fence);}
    return s;
}
void GpuSyncRef(GpuSync* s){if(s)__atomic_fetch_add(&s->references,1,__ATOMIC_RELAXED);}
void GpuSyncUnref(GpuSync* s){
    if(s && __atomic_fetch_sub(&s->references,1,__ATOMIC_ACQ_REL)==1){s->timeline.Close();s->~BdwGpuSync();kfree(s);}
}
BdwFence* BdwGpuSyncAcquire(BdwGpuSync* s){return s?s->timeline.Find(0):nullptr;}
GpuFence* GpuSyncAcquire(GpuSync* s){return BdwGpuSyncAcquire(s);}
// Publication can satisfy WAIT_FOR_SUBMIT waits and transfer waits.
bool BdwGpuSyncReplace(BdwGpuSync* s,BdwFence* f){
    const bool ok=s && s->timeline.Replace(f);if(ok)BdwGpuEventSignal(BdwGpuEvent::Retire);return ok;}
BdwFence* BdwGpuSyncPrepareTimeline(BdwGpuSync* s,uint64_t point,BdwFence* dependency){return s?s->timeline.Prepare(point,dependency):nullptr;}
bool BdwGpuSyncPublishTimeline(BdwGpuSync* s,BdwFence* fence){
    const bool ok=s && s->timeline.Publish(fence);if(ok)BdwGpuEventSignal(BdwGpuEvent::Retire);return ok;}
BdwFence* BdwGpuSyncFindTimeline(BdwGpuSync* s,uint64_t point){return s?s->timeline.Find(point):nullptr;}
uint64_t BdwGpuSyncQueryTimeline(BdwGpuSync* s,bool lastSubmitted){return s?s->timeline.Query(lastSubmitted):0;}
bool GpuSyncReplace(GpuSync* s,GpuFence* f){return BdwGpuSyncReplace(s,f);}
GpuFence* GpuSyncPrepareTimeline(GpuSync* s,uint64_t point,GpuFence* dependency){return BdwGpuSyncPrepareTimeline(s,point,dependency);}
bool GpuSyncPublishTimeline(GpuSync* s,GpuFence* fence){return BdwGpuSyncPublishTimeline(s,fence);}
GpuFence* GpuSyncFindTimeline(GpuSync* s,uint64_t point){return BdwGpuSyncFindTimeline(s,point);}
uint64_t GpuSyncQueryTimeline(GpuSync* s,bool lastSubmitted){return BdwGpuSyncQueryTimeline(s,lastSubmitted);}
}  // namespace brook
