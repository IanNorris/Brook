#pragma once
#include <stdint.h>
namespace brook {
struct BdwExecSnapshot;
enum BdwTimingStage : uint32_t {BdwTimingQueue=0,BdwTimingDeviceSync,BdwTimingPrepare,BdwTimingDevice,BdwTimingRetire,BdwTimingCpuSync,BdwTimingAdmissionWait,BdwTimingSnapshotBuild,BdwTimingExplicitWritePages,BdwTimingAsyncPages,BdwTimingGtInit,BdwTimingPatSetup,BdwTimingRenderRun,BdwTimingContextSave,BdwTimingResetAndUnbind,BdwTimingPatRestore,BdwTimingIssueToBreadcrumb,BdwTimingBreadcrumbToRetirement,BdwTimingUniqueBufferPages,BdwTimingRepeatedBufferPages,BdwTimingStageCount};
struct BdwTimingBucket {uint64_t count,totalUs,maxUs,totalPages;};
struct BdwTimingSnapshot {uint64_t owner,dropped;bool saturated;BdwTimingBucket stages[BdwTimingStageCount];};
struct BdwTimingObjectHints {uint32_t explicitWritePages=0,asyncPages=0;};
// Count exact duplicate buffer objects within one retained request. This is
// not a count of unique physical pages: distinct objects may alias backing.
template<class Buffer, class PageCount>
inline void BdwTimingCountBufferPages(Buffer* const* buffers,uint32_t count,PageCount pages,uint64_t& unique,uint64_t& repeated){
    unique=0;repeated=0;
    for(uint32_t i=0;i<count;++i){
        bool seen=false;for(uint32_t j=0;j<i;++j)if(buffers[i]==buffers[j]){seen=true;break;}
        if(seen)repeated+=pages(buffers[i]);else unique+=pages(buffers[i]);
    }
}
inline void BdwTimingCountObjectHints(BdwTimingObjectHints& hints,uint64_t flags,uint32_t pages){
    if(flags&4)hints.explicitWritePages+=pages;
    if(flags&64)hints.asyncPages+=pages;
}
inline void BdwTimingAccumulate(BdwTimingSnapshot& timing,uint64_t owner,BdwTimingStage stage,uint64_t us,uint64_t pages){
    if(!owner || timing.owner!=owner || stage>=BdwTimingStageCount)return;
    auto& b=timing.stages[stage];
    if(b.count==~uint64_t{0} || b.totalUs>~uint64_t{0}-us || b.totalPages>~uint64_t{0}-pages){timing.saturated=true;return;}
    ++b.count;b.totalUs+=us;b.totalPages+=pages;if(us>b.maxUs)b.maxUs=us;
}
#if defined(BROOK_GPU_SUBMISSION_TIMING)
bool BdwTimingConfigure(uint64_t owner);
bool BdwTimingRead(BdwTimingSnapshot& out);
bool BdwTimingWatches(uint64_t owner);
void BdwTimingRecord(const BdwExecSnapshot* snapshot,BdwTimingStage stage,uint64_t microseconds,uint64_t pages);
uint64_t BdwTimingNow();
void BdwTimingOwnerElapsed(uint64_t owner,BdwTimingStage stage,uint64_t start,uint64_t end);
void BdwTimingQueued(BdwExecSnapshot* snapshot);
void BdwTimingTaken(BdwExecSnapshot* snapshot);
#endif
}
