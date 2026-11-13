#include "intel_bdw_file.h"
#include "intel_bdw_buffer.h"
#include "drm_map_tokens.h"
#include "memory/heap.h"
#include "sync/kmutex.h"
#include "intel_bdw_fence.h"
#if defined(BROOK_HOST_TEST) && !defined(BROOK_STANDALONE_TEST)
#include <new>
#else
inline void* operator new(__SIZE_TYPE__,void* storage) noexcept{return storage;}
#endif

namespace brook {
static constexpr uint32_t BdwMaxHandles=8192;
static constexpr uint32_t BdwMaxContexts=64;
static constexpr uint32_t BdwMaxSyncObjects=256;
static DrmMapTokenAllocator bdwMapTokens;
struct BdwGpuContext {
    uint32_t references,state,loss; // idle, active, poisoned; lost active/pending bits
    bool present;
    uint32_t saved[20*1024];
};
struct BdwGpuSync {uint32_t references;BdwTimelineSync timeline;};
struct BdwGpuFile {
    uint32_t references;
    KMutex lock;
    struct Slot {BdwGpuBuffer* buffer;uint64_t token;} slots[BdwMaxHandles];
    struct ContextSlot {BdwGpuContext* context;uint32_t id;} contexts[BdwMaxContexts];
    uint32_t nextContextId;
    BdwGpuContext* defaultContext;
    struct SyncSlot {BdwGpuSync* sync;uint32_t id;} syncs[BdwMaxSyncObjects];
    uint32_t nextSyncId;
};
struct BdwFileGuard {KMutex* mutex;explicit BdwFileGuard(KMutex* m):mutex(m){KMutexLock(m);}
    ~BdwFileGuard(){KMutexUnlock(mutex);}};
BdwGpuFile* BdwGpuFileCreate(){
    auto* f=static_cast<BdwGpuFile*>(kmalloc(sizeof(BdwGpuFile)));if(!f)return nullptr;
    f->references=1;KMutexInit(&f->lock);for(auto& s:f->slots)s={};
    for(auto& c:f->contexts)c={};f->nextContextId=1;f->defaultContext=nullptr;
    for(auto& s:f->syncs)s={};f->nextSyncId=1;return f;
}
void BdwGpuSyncRef(BdwGpuSync* s){if(s)__atomic_fetch_add(&s->references,1,__ATOMIC_RELAXED);}
void BdwGpuSyncUnref(BdwGpuSync* s){
    if(s && __atomic_fetch_sub(&s->references,1,__ATOMIC_ACQ_REL)==1){s->timeline.Close();s->~BdwGpuSync();kfree(s);}
}
uint32_t BdwGpuFileCreateSync(BdwGpuFile* f,bool signaled){
    if(!f)return 0;void* memory=kmalloc(sizeof(BdwGpuSync));if(!memory)return 0;
    auto* s=new(memory) BdwGpuSync{};s->references=1;
    if(!s->timeline.Init()){s->~BdwGpuSync();kfree(s);return 0;}
    if(signaled){auto* fence=BdwFenceCreate(true);if(!fence){BdwGpuSyncUnref(s);return 0;}
        s->timeline.Replace(fence);BdwFenceUnref(fence);}
    uint32_t id=0;{BdwFileGuard guard(&f->lock);if(f->nextSyncId)for(auto& slot:f->syncs)if(!slot.sync){
        id=f->nextSyncId++;slot={s,id};break;}}
    if(!id)BdwGpuSyncUnref(s);return id;
}
BdwGpuSync* BdwGpuFileGetSync(BdwGpuFile* f,uint32_t id){
    if(!f || !id)return nullptr;BdwFileGuard guard(&f->lock);
    for(auto& slot:f->syncs)if(slot.id==id && slot.sync){BdwGpuSyncRef(slot.sync);return slot.sync;}
    return nullptr;
}
uint32_t BdwGpuFileImportSync(BdwGpuFile* f,BdwGpuSync* sync){
    if(!f || !sync)return 0;BdwFileGuard guard(&f->lock);BdwGpuFile::SyncSlot* empty=nullptr;
    for(auto& slot:f->syncs)if(!slot.sync){empty=&slot;break;}
    if(!empty || !f->nextSyncId)return 0;
    const uint32_t id=f->nextSyncId++;BdwGpuSyncRef(sync);*empty={sync,id};return id;
}
bool BdwGpuFileDestroySync(BdwGpuFile* f,uint32_t id){
    if(!f || !id)return false;BdwGpuSync* s=nullptr;
    {BdwFileGuard guard(&f->lock);for(auto& slot:f->syncs)if(slot.id==id && slot.sync){s=slot.sync;slot={};break;}}
    if(!s)return false;BdwGpuSyncUnref(s);return true;
}
BdwFence* BdwGpuSyncAcquire(BdwGpuSync* s){return s?s->timeline.Find(0):nullptr;}
bool BdwGpuSyncReplace(BdwGpuSync* s,BdwFence* f){return s && s->timeline.Replace(f);}
BdwFence* BdwGpuSyncPrepareTimeline(BdwGpuSync* s,uint64_t point,BdwFence* dependency){return s?s->timeline.Prepare(point,dependency):nullptr;}
bool BdwGpuSyncPublishTimeline(BdwGpuSync* s,BdwFence* fence){return s && s->timeline.Publish(fence);}
BdwFence* BdwGpuSyncFindTimeline(BdwGpuSync* s,uint64_t point){return s?s->timeline.Find(point):nullptr;}
uint64_t BdwGpuSyncQueryTimeline(BdwGpuSync* s,bool lastSubmitted){return s?s->timeline.Query(lastSubmitted):0;}
void BdwGpuContextRef(BdwGpuContext* c){if(c)__atomic_fetch_add(&c->references,1,__ATOMIC_RELAXED);}
void BdwGpuContextUnref(BdwGpuContext* c){
    if(c && __atomic_fetch_sub(&c->references,1,__ATOMIC_ACQ_REL)==1)kfree(c);
}
uint32_t BdwGpuFileCreateContext(BdwGpuFile* f){
    if(!f)return 0;auto* c=static_cast<BdwGpuContext*>(kmalloc(sizeof(BdwGpuContext)));if(!c)return 0;
    c->references=1;c->state=0;c->loss=0;c->present=false;uint32_t id=0;
    {BdwFileGuard guard(&f->lock);
        if(f->nextContextId)for(auto& slot:f->contexts)if(!slot.context){
            id=f->nextContextId++;slot={c,id};break;}}
    if(!id)BdwGpuContextUnref(c);return id;
}
BdwGpuContext* BdwGpuFileGetContext(BdwGpuFile* f,uint32_t id){
    if(!f || !id)return nullptr;BdwFileGuard guard(&f->lock);
    for(auto& slot:f->contexts)if(slot.id==id && slot.context){
        BdwGpuContextRef(slot.context);return slot.context;}
    return nullptr;
}
BdwGpuContext* BdwGpuFileGetDefaultContext(BdwGpuFile* f){
    if(!f)return nullptr;
    {BdwFileGuard guard(&f->lock);if(f->defaultContext){BdwGpuContextRef(f->defaultContext);return f->defaultContext;}}
    auto* candidate=static_cast<BdwGpuContext*>(kmalloc(sizeof(BdwGpuContext)));if(!candidate)return nullptr;
    candidate->references=1;candidate->state=0;candidate->loss=0;candidate->present=false;BdwGpuContext* result=nullptr;
    {BdwFileGuard guard(&f->lock);if(!f->defaultContext){f->defaultContext=candidate;candidate=nullptr;}
        result=f->defaultContext;BdwGpuContextRef(result);}
    BdwGpuContextUnref(candidate);return result;
}
bool BdwGpuFileDestroyContext(BdwGpuFile* f,uint32_t id){
    if(!f || !id)return false;BdwGpuContext* context=nullptr;
    {BdwFileGuard guard(&f->lock);for(auto& slot:f->contexts)if(slot.id==id && slot.context){
        context=slot.context;slot={};break;}}
    if(!context)return false;BdwGpuContextUnref(context);return true;
}
bool BdwGpuContextBegin(BdwGpuContext* c,uint32_t (&saved)[20*1024],bool& present){
    present=false;if(!c)return false;uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&c->state,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return false;
    present=c->present;if(present)for(unsigned i=0;i<20*1024;++i)saved[i]=c->saved[i];
    return true;
}
bool BdwGpuContextFinish(BdwGpuContext* c,const uint32_t (*saved)[20*1024],bool safelyStopped){
    if(!c || __atomic_load_n(&c->state,__ATOMIC_ACQUIRE)!=1)return false;
    if(!safelyStopped){BdwGpuContextMarkLost(c,false);return false;}
    if(saved){for(unsigned i=0;i<20*1024;++i)c->saved[i]=(*saved)[i];c->present=true;}
    __atomic_store_n(&c->state,0,__ATOMIC_RELEASE);return true;
}
void BdwGpuContextMarkLost(BdwGpuContext* c,bool pending){
    if(!c)return;__atomic_fetch_or(&c->loss,pending?2u:1u,__ATOMIC_RELEASE);
    __atomic_store_n(&c->state,2,__ATOMIC_RELEASE);
}
uint32_t BdwGpuContextLoss(const BdwGpuContext* c){return c?__atomic_load_n(&c->loss,__ATOMIC_ACQUIRE):0;}
void BdwGpuFileRef(BdwGpuFile* f){if(f)__atomic_fetch_add(&f->references,1,__ATOMIC_RELAXED);}
void BdwGpuFileUnref(BdwGpuFile* f){
    if(!f || __atomic_fetch_sub(&f->references,1,__ATOMIC_ACQ_REL)!=1)return;
    // Last file reference implies no operation is active. Release outside any
    // file or mapping lock because a buffer's final reference frees memory.
    for(auto& s:f->slots)if(s.buffer)BdwGpuBufferUnref(s.buffer);
    for(auto& c:f->contexts)if(c.context)BdwGpuContextUnref(c.context);
    BdwGpuContextUnref(f->defaultContext);
    for(auto& s:f->syncs)if(s.sync)BdwGpuSyncUnref(s.sync);
    kfree(f);
}
uint32_t BdwGpuFileCreateBuffer(BdwGpuFile* f,uint64_t requested,uint64_t& rounded){
    rounded=0;if(!f)return 0;auto* buffer=BdwGpuBufferCreate(requested);if(!buffer)return 0;
    uint32_t handle=0;
    {BdwFileGuard guard(&f->lock);for(uint32_t i=0;i<BdwMaxHandles;++i)if(!f->slots[i].buffer){
        f->slots[i]={buffer,0};handle=i+1;rounded=BdwGpuBufferBytes(buffer);break;}}
    if(!handle)BdwGpuBufferUnref(buffer);
    return handle;
}
uint32_t BdwGpuFileImportBuffer(BdwGpuFile* f,BdwGpuBuffer* buffer){
    if(!f || !buffer)return 0;
    BdwFileGuard guard(&f->lock);uint32_t empty=BdwMaxHandles;
    for(uint32_t i=0;i<BdwMaxHandles;++i){
        if(f->slots[i].buffer==buffer)return i+1;
        if(!f->slots[i].buffer && empty==BdwMaxHandles)empty=i;
    }
    if(empty==BdwMaxHandles)return 0;
    BdwGpuBufferRef(buffer);f->slots[empty]={buffer,0};return empty+1;
}
bool BdwGpuFileCloseBuffer(BdwGpuFile* f,uint32_t handle){
    if(!f || !handle || handle>BdwMaxHandles)return false;
    BdwGpuBuffer* buffer=nullptr;
    {BdwFileGuard guard(&f->lock);auto& slot=f->slots[handle-1];buffer=slot.buffer;slot={};}
    if(!buffer)return false;
    BdwGpuBufferUnref(buffer);return true;
}
BdwGpuBuffer* BdwGpuFileGetBuffer(BdwGpuFile* f,uint32_t handle){
    if(!f || !handle || handle>BdwMaxHandles)return nullptr;
    BdwFileGuard guard(&f->lock);auto* b=f->slots[handle-1].buffer;BdwGpuBufferRef(b);return b;
}
bool BdwGpuFileResolveBuffers(BdwGpuFile* f,const uint32_t* handles,uint32_t count,BdwGpuBuffer** out){
    if(!f || !handles || !out || !count || count>BdwMaxHandles)return false;
    const uintptr_t a=reinterpret_cast<uintptr_t>(handles),b=reinterpret_cast<uintptr_t>(out);
    const uint64_t aBytes=uint64_t(count)*sizeof(*handles),bBytes=uint64_t(count)*sizeof(*out);
    if(a>~uintptr_t{0}-aBytes || b>~uintptr_t{0}-bBytes || (a<b+bBytes && b<a+aBytes))return false;
    uint64_t seen[BdwMaxHandles/64]{};
    BdwFileGuard guard(&f->lock);
    for(uint32_t i=0;i<count;++i){const uint32_t h=handles[i];
        if(!h || h>BdwMaxHandles || !f->slots[h-1].buffer)return false;
        const uint32_t index=h-1;const uint64_t bit=uint64_t{1}<<(index%64);
        if(seen[index/64]&bit)return false;
        seen[index/64]|=bit;
    }
    for(uint32_t i=0;i<count;++i){out[i]=f->slots[handles[i]-1].buffer;BdwGpuBufferRef(out[i]);}
    return true;
}
uint64_t BdwGpuFileMapToken(BdwGpuFile* f,uint32_t handle){
    if(!f || !handle || handle>BdwMaxHandles)return 0;
    BdwFileGuard guard(&f->lock);auto& slot=f->slots[handle-1];if(!slot.buffer || !BdwGpuBufferAllowsUserMapping(slot.buffer))return 0;
    if(!slot.token)slot.token=bdwMapTokens.Allocate(BdwGpuBufferBytes(slot.buffer));
    return slot.token;
}
BdwGpuBuffer* BdwGpuFileGetMapping(BdwGpuFile* f,uint64_t offset,uint64_t length,uint64_t& bufferOffset){
    bufferOffset=0;if(!f || !length || (offset&4095) || (length&4095))return nullptr;
    BdwFileGuard guard(&f->lock);
    for(auto& slot:f->slots){if(!slot.buffer || !BdwGpuBufferAllowsUserMapping(slot.buffer) || !slot.token || offset<slot.token)continue;
        const uint64_t relative=offset-slot.token,size=BdwGpuBufferBytes(slot.buffer);
        if(relative>=size || length>size-relative)continue;
        BdwGpuBufferRef(slot.buffer);bufferOffset=relative;return slot.buffer;
    }return nullptr;
}
}
