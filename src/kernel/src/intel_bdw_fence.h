#pragma once
#include <stdint.h>
#include "memory/heap.h"
#include "sync/kmutex.h"
namespace brook {
// One immutable fence identity: state changes exactly once. A sync object may
// replace its current fence without changing references held by queued work.
struct BdwFence {uint32_t references,state,dependencyDepth,resolved,orderedPredecessor,isTimeline;BdwFence* predecessor;BdwFence* dependency;BdwFence* reapNext;uint64_t point;}; // pending=0, complete=1, failed=2
// Traversal and final release are iterative, including secondary dependencies.
inline BdwFence* BdwFenceCreate(bool completed=false,BdwFence* predecessor=nullptr,uint64_t point=0,BdwFence* dependency=nullptr){
    const uint32_t predDepth=predecessor?predecessor->dependencyDepth:0;
    if(dependency && dependency->dependencyDepth==~uint32_t{0})return nullptr;
    const uint32_t depDepth=dependency?dependency->dependencyDepth+1:0;
    const uint32_t depth=predDepth>depDepth?predDepth:depDepth;
    auto* f=static_cast<BdwFence*>(kmalloc(sizeof(BdwFence)));if(!f)return nullptr;
    f->references=1;f->state=completed?1:0;f->dependencyDepth=depth;f->resolved=0;
    f->orderedPredecessor=0;f->isTimeline=0;
    f->predecessor=predecessor;f->dependency=dependency;f->reapNext=nullptr;f->point=point;
    if(predecessor)__atomic_fetch_add(&predecessor->references,1,__ATOMIC_RELAXED);
    if(dependency)__atomic_fetch_add(&dependency->references,1,__ATOMIC_RELAXED);
    return f;
}
inline void BdwFenceRef(BdwFence* f){if(f)__atomic_fetch_add(&f->references,1,__ATOMIC_RELAXED);}
inline void BdwFenceUnref(BdwFence* f){
    BdwFence* pending=nullptr;
    auto drop=[&](BdwFence* node){if(node && __atomic_fetch_sub(&node->references,1,__ATOMIC_ACQ_REL)==1){node->reapNext=pending;pending=node;}};
    drop(f);
    while(pending){auto* node=pending;pending=node->reapNext;drop(node->predecessor);drop(node->dependency);kfree(node);}
}
inline uint32_t BdwFenceState(const BdwFence* f){
    if(!f)return 0;auto* root=const_cast<BdwFence*>(f);bool failed=false;
    const auto cached=__atomic_load_n(&root->resolved,__ATOMIC_ACQUIRE);if(cached)return cached;
    struct Frame {const BdwFence* resume;BdwFence* dependency;bool failed;};
    Frame local[64];Frame* stack=local;const uint64_t capacity=uint64_t(root->dependencyDepth)+1;
    if(capacity>64){stack=static_cast<Frame*>(kmalloc(capacity*sizeof(*stack)));if(!stack)return 0;}
    uint64_t used=0;uint32_t result=0;
    while(f || used){
        if(!f){const auto frame=stack[--used];
            __atomic_store_n(&frame.dependency->resolved,failed?2u:1u,__ATOMIC_RELEASE);
            failed=failed || frame.failed;f=frame.resume;continue;}
        const auto state=__atomic_load_n(&f->state,__ATOMIC_ACQUIRE);
        const auto resolved=__atomic_load_n(&f->resolved,__ATOMIC_ACQUIRE);
        if(resolved){if(resolved==2)failed=true;f=nullptr;continue;}
        if(!state)goto done;if(state==2)failed=true;
        if(f->dependency){stack[used++]={f->predecessor,f->dependency,failed};failed=false;f=f->dependency;}else f=f->predecessor;
    }
    result=failed?2:1;__atomic_store_n(&root->resolved,result,__ATOMIC_RELEASE);
done:
    if(stack!=local)kfree(stack);return result;
}
inline bool BdwFenceComplete(BdwFence* f,bool success){
    if(!f)return false;uint32_t expected=0;
    return __atomic_compare_exchange_n(&f->state,&expected,success?1u:2u,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
// Prepare a timeline node after an unpublished predecessor from the same
// admission transaction. The caller serializes all sync-object publication.
inline BdwFence* BdwFencePrepareTimelineAfter(BdwFence* predecessor,uint64_t point,BdwFence* dependency=nullptr){
    if(dependency)for(auto* ancestor=predecessor;ancestor;ancestor=ancestor->predecessor)
        if(ancestor==dependency){dependency=nullptr;break;}
    const uint64_t sequence=predecessor && predecessor->isTimeline && predecessor->point>point?predecessor->point:point;
    auto* result=BdwFenceCreate(false,predecessor,sequence,dependency);
    if(result){result->isTimeline=1;result->orderedPredecessor=predecessor && predecessor->isTimeline && point>predecessor->point;}
    return result;
}
// Owner must keep this object alive and initialize it before publication.
// Replacing/clearing is serialized with acquiring a reference to the current
// fence. A null fence means no submission, not a completed submission.
class BdwBinarySync {
    KMutex mutex_{};BdwFence* current_=nullptr;bool initialized_=false;
public:
    BdwBinarySync()=default;
    BdwBinarySync(const BdwBinarySync&)=delete;
    BdwBinarySync& operator=(const BdwBinarySync&)=delete;
    bool Init(){if(initialized_)return false;KMutexInit(&mutex_);initialized_=true;return true;}
    bool Replace(BdwFence* next){
        if(!initialized_)return false;BdwFenceRef(next);KMutexLock(&mutex_);
        auto* old=current_;current_=next;KMutexUnlock(&mutex_);BdwFenceUnref(old);return true;
    }
    BdwFence* Acquire(){
        if(!initialized_)return nullptr;KMutexLock(&mutex_);auto* f=current_;BdwFenceRef(f);KMutexUnlock(&mutex_);return f;
    }
    void Close(){if(initialized_){Replace(nullptr);initialized_=false;}}
};

// Internal timeline point registry. Preparation does not publish a fence;
// callers must serialize queue admission and handle a failed Publish before
// exposing work. Point zero retains its separate binary-sync meaning.
class BdwTimelineSync {
    KMutex mutex_{};BdwFence* head_=nullptr;bool initialized_=false;
public:
    BdwTimelineSync()=default;
    BdwTimelineSync(const BdwTimelineSync&)=delete;
    BdwTimelineSync& operator=(const BdwTimelineSync&)=delete;
    bool Init(){if(initialized_)return false;KMutexInit(&mutex_);initialized_=true;return true;}
    BdwFence* Prepare(uint64_t point,BdwFence* dependency=nullptr){
        if(!initialized_)return nullptr;
        KMutexLock(&mutex_);BdwFence* result=nullptr;BdwFence* retired=nullptr;
        // Completed successful history can be represented by one anchor;
        // outstanding readers still retain their original immutable chain.
        if(head_ && head_->predecessor && BdwFenceState(head_)==1){
            auto* anchor=BdwFenceCreate(true,nullptr,head_->point);
            if(anchor){anchor->isTimeline=head_->isTimeline;retired=head_;head_=anchor;}
        }
        // Like dma_fence_chain, an out-of-order insertion starts a new
        // ordering context and keeps the sequence number from decreasing.
        result=BdwFencePrepareTimelineAfter(head_,point,dependency);
        KMutexUnlock(&mutex_);BdwFenceUnref(retired);return result;
    }
    bool Publish(BdwFence* fence){
        if(!initialized_ || !fence || !fence->isTimeline)return false;
        KMutexLock(&mutex_);
        const bool accepted=fence->predecessor==head_;
        BdwFence* old=nullptr;if(accepted){BdwFenceRef(fence);old=head_;head_=fence;}
        KMutexUnlock(&mutex_);BdwFenceUnref(old);return accepted;
    }
    bool Replace(BdwFence* fence){
        if(!initialized_)return false;BdwFenceRef(fence);KMutexLock(&mutex_);
        auto* old=head_;head_=fence;KMutexUnlock(&mutex_);BdwFenceUnref(old);return true;
    }
    BdwFence* Find(uint64_t point){
        if(!initialized_)return nullptr;
        KMutexLock(&mutex_);auto* found=head_;
        if(!point){BdwFenceRef(found);}
        else if(found && found->isTimeline && found->point>=point){
            while(found->orderedPredecessor && found->predecessor->point>=point)found=found->predecessor;
            BdwFenceRef(found);
        }else found=nullptr;
        KMutexUnlock(&mutex_);return found;
    }
    uint64_t Query(bool lastSubmitted){
        if(!initialized_)return 0;KMutexLock(&mutex_);auto* current=head_;
        uint64_t point=lastSubmitted && current && current->isTimeline?current->point:0;
        if(!lastSubmitted)for(;current && current->isTimeline;current=current->orderedPredecessor?current->predecessor:nullptr)
            if(BdwFenceState(current)){point=current->point;break;}
        KMutexUnlock(&mutex_);return point;
    }
    void Close(){if(!initialized_)return;KMutexLock(&mutex_);auto* old=head_;head_=nullptr;
        KMutexUnlock(&mutex_);BdwFenceUnref(old);initialized_=false;}
};
}
