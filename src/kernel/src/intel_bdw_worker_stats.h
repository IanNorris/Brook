#pragma once
#include <stdint.h>

namespace brook {
// Diagnostic occupancy accounting for the single Broadwell submission worker.
// The model is pure logic: all mutation happens on the worker thread; other
// threads read a seqlock-published copy and request resets via an atomic.
// Nothing here blocks, allocates or logs.
enum BdwWorkerClass : uint32_t {
    BdwWorkerApplication=0,BdwWorkerCompositor,BdwWorkerPresent,BdwWorkerOther,BdwWorkerIdle,BdwWorkerClassCount
};
enum BdwWorkerPresentStage : uint32_t {
    BdwPresentPrepare=0,BdwPresentDevice,BdwPresentBlit,BdwPresentRetire,BdwPresentTotal,BdwPresentStageCount
};
constexpr unsigned BdwWorkerYieldBuckets=6; // 0, 1, 2, 3-4, 5-8, >8
inline unsigned BdwWorkerYieldBucket(uint64_t yields){return yields<3?unsigned(yields):yields<=4?3u:yields<=8?4u:5u;}
// Broadwell RPSTAT1 CAGF (bits 13:7), in 50 MHz units, as i915 decodes it.
inline uint64_t BdwRpstatMhz(uint32_t status){return uint64_t((status>>7)&0x7f)*50;}
inline uint64_t BdwWorkerTicksToUs(uint64_t ticks,uint64_t khz){
    if(!khz)return 0;
    return ticks<=~uint64_t{0}/1000?ticks*1000/khz:ticks/khz*1000;
}
// Every member is a uint64_t so the published copy can be moved word-wise.
struct BdwWorkerSpan {uint64_t count,ticks,maxTicks;};
inline void BdwWorkerSpanAdd(BdwWorkerSpan& s,uint64_t ticks){
    ++s.count;s.ticks+=ticks;if(ticks>s.maxTicks)s.maxTicks=ticks;
}
struct BdwWorkerYields {uint64_t jobs,yields,breadcrumbYields,sleepTicks,buckets[BdwWorkerYieldBuckets];};
struct BdwWorkerFrequency {uint64_t samples,sumMhz,minMhz,maxMhz;};
inline void BdwWorkerFrequencyAdd(BdwWorkerFrequency& f,uint64_t mhz){
    if(!f.samples || mhz<f.minMhz)f.minMhz=mhz;
    if(mhz>f.maxMhz)f.maxMhz=mhz;
    ++f.samples;f.sumMhz+=mhz;
}
struct BdwWorkerJobFrequency {BdwWorkerFrequency issue,end;uint64_t rising,missing;};
struct BdwWorkerStats {
    uint64_t elapsedTicks,resets;
    BdwWorkerSpan classes[BdwWorkerClassCount];
    // [0]=full-screen present, [1]=partial present.
    BdwWorkerSpan present[2][BdwPresentStageCount];
    uint64_t presentPixels[2],presentFailed;
    // [0]=application RCS, [1]=compositor RCS, [2]=BCS present.
    BdwWorkerYields yields[3];
    // [0]=application RCS, [1]=compositor RCS.
    BdwWorkerJobFrequency frequency[2];
    // Page flip (BROOK_BDW_SCANOUT_FLIP). latch = DSPASURF write to the
    // first observation of DSPASURFLIVE (worker polls about every 1 ms);
    // latchFrames buckets 1,2,3,>3 vblanks; missed = vblanks beyond the first.
    uint64_t flipArmed,flipQueued,flipLatched,flipMissedVblanks,flipUncertain,flipGopReturns;
    BdwWorkerSpan flipLatch;
    uint64_t flipLatchFrames[4];
    // Persistent scanout map (BROOK_BDW_PERSISTENT_SCANOUT_MAP and flip):
    // hits used the cached GOP map after a cheap check; full = complete GOP
    // PTE revalidation; failed = fell back to the per-present PPGTT build.
    uint64_t mapHits,mapFull,mapFailed;
    // Presents per destination: [0]=GOP per-present map, [1]=GOP persistent, [2]=flip buffer.
    uint64_t presentTarget[3];
};
static_assert(sizeof(BdwWorkerStats)%8==0,"word-wise publication");

class BdwWorkerStatsModel {
    static constexpr unsigned Words=sizeof(BdwWorkerStats)/8;
    BdwWorkerStats local_{};          // worker thread only
    uint64_t mark_=0,resetMark_=0;    // worker thread only
    BdwWorkerClass current_=BdwWorkerOther;
    uint64_t published_[Words]{};
    uint64_t sequence_=0;
    uint32_t resetRequested_=0;
    uint64_t resets_=0;               // worker thread only
    void Charge(BdwWorkerClass c,uint64_t now){
        if(now>mark_)BdwWorkerSpanAdd(local_.classes[c],now-mark_);
        else BdwWorkerSpanAdd(local_.classes[c],0);
        if(now>mark_)mark_=now;
        local_.elapsedTicks=mark_-resetMark_;
    }
    void Publish(){
        const auto* words=reinterpret_cast<const uint64_t*>(&local_);
        const auto seq=__atomic_load_n(&sequence_,__ATOMIC_RELAXED);
        __atomic_store_n(&sequence_,seq+1,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        for(unsigned i=0;i<Words;++i)__atomic_store_n(&published_[i],words[i],__ATOMIC_RELAXED);
        __atomic_store_n(&sequence_,seq+2,__ATOMIC_RELEASE);
    }
public:
    // Worker thread: time since the previous boundary becomes "other"
    // service work; the following interval is attributed to next.
    void Begin(BdwWorkerClass next,uint64_t now){
        if(!mark_ || __atomic_exchange_n(&resetRequested_,0u,__ATOMIC_ACQ_REL)){
            if(mark_)++resets_;
            local_={};local_.resets=resets_;mark_=resetMark_=now;
        }else Charge(BdwWorkerOther,now);
        current_=next<BdwWorkerClassCount?next:BdwWorkerOther;
    }
    void End(uint64_t now){
        if(!mark_)return;
        Charge(current_,now);current_=BdwWorkerOther;Publish();
    }
    BdwWorkerClass Current()const{return current_;}
    // Worker thread, between Begin and End of an RCS job.
    void Render(uint64_t yields,uint64_t breadcrumbYields,uint64_t sleepTicks,
                bool frequencySampled,uint32_t issueStatus,uint32_t endStatus){
        if(current_!=BdwWorkerApplication && current_!=BdwWorkerCompositor)return;
        const unsigned c=current_==BdwWorkerCompositor?1:0;
        auto& y=local_.yields[c];
        ++y.jobs;y.yields+=yields;y.breadcrumbYields+=breadcrumbYields;y.sleepTicks+=sleepTicks;
        ++y.buckets[BdwWorkerYieldBucket(yields)];
        auto& f=local_.frequency[c];
        if(!frequencySampled){++f.missing;return;}
        const auto issue=BdwRpstatMhz(issueStatus),end=BdwRpstatMhz(endStatus);
        BdwWorkerFrequencyAdd(f.issue,issue);BdwWorkerFrequencyAdd(f.end,end);
        if(end>issue)++f.rising;
    }
    // Worker thread, between Begin and End of a BCS present.
    void Present(bool full,uint64_t pixels,const uint64_t (&ticks)[BdwPresentStageCount],uint64_t yields){
        if(current_!=BdwWorkerPresent)return;
        const unsigned k=full?0:1;
        for(unsigned i=0;i<BdwPresentStageCount;++i)BdwWorkerSpanAdd(local_.present[k][i],ticks[i]);
        local_.presentPixels[k]+=pixels;
        auto& y=local_.yields[2];++y.jobs;y.yields+=yields;++y.buckets[BdwWorkerYieldBucket(yields)];
    }
    void PresentFailed(){if(current_==BdwWorkerPresent)++local_.presentFailed;}
    // Worker thread, any interval (the flip poll also runs between jobs).
    void PresentTarget(unsigned target){if(target<3)++local_.presentTarget[target];}
    void FlipArmed(bool queued){if(queued)++local_.flipQueued;else ++local_.flipArmed;}
    void FlipLatched(uint32_t frames,uint64_t ticks){
        ++local_.flipLatched;BdwWorkerSpanAdd(local_.flipLatch,ticks);
        ++local_.flipLatchFrames[frames<=1?0:frames==2?1:frames==3?2:3];
        if(frames>1)local_.flipMissedVblanks+=frames-1;
    }
    void FlipUncertain(){++local_.flipUncertain;}
    void FlipGopReturn(){++local_.flipGopReturns;}
    void PersistentMap(bool hit,bool ok){if(!ok)++local_.mapFailed;else if(hit)++local_.mapHits;else ++local_.mapFull;}
    // Publish counters recorded outside a Begin/End interval.
    void Flush(){if(mark_)Publish();}
    // Any thread. Applied by the worker at its next boundary.
    void RequestReset(){__atomic_store_n(&resetRequested_,1u,__ATOMIC_RELEASE);}
    // Any thread. False only if the worker kept publishing throughout.
    bool Read(BdwWorkerStats& out)const{
        auto* words=reinterpret_cast<uint64_t*>(&out);
        for(unsigned attempt=0;attempt<64;++attempt){
            const auto before=__atomic_load_n(&sequence_,__ATOMIC_ACQUIRE);
            if(before&1)continue;
            for(unsigned i=0;i<Words;++i)words[i]=__atomic_load_n(&published_[i],__ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if(__atomic_load_n(&sequence_,__ATOMIC_RELAXED)==before)return true;
        }
        return false;
    }
};

#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
// Kernel glue (intel_bdw_prepare.cpp). Recorders are ignored unless called
// on the application submission worker thread inside the matching interval.
void BdwWorkerStatsRender(uint64_t yields,uint64_t breadcrumbYields,uint64_t sleepTicks,
                          bool frequencySampled,uint32_t issueStatus,uint32_t endStatus);
void BdwWorkerStatsPresent(bool full,uint64_t pixels,const uint64_t (&ticks)[BdwPresentStageCount],uint64_t yields);
bool BdwWorkerStatsRead(BdwWorkerStats& out);
void BdwWorkerStatsReset();
// Flip and persistent-map counters; ignored off the worker thread.
enum class BdwWorkerScanoutEvent : uint8_t {TargetLegacy,TargetPersistent,TargetFlip,FlipArmed,FlipQueued,
    FlipUncertain,FlipGopReturn,MapHit,MapFull,MapFailed};
void BdwWorkerStatsScanout(BdwWorkerScanoutEvent event);
void BdwWorkerStatsFlipLatched(uint32_t frames,uint64_t ticks);
#endif
}
