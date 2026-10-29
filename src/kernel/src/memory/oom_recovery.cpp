#include "oom_recovery.h"
#include "oom_policy.h"
#include "physical_memory.h"
#include "address_space.h"
#include "virtual_memory.h"
#include "file_page_cache.h"
#include "exec_page_cache.h"
#include "pageable_memory.h"
#include "process.h"
#include "scheduler.h"
#include "system_message.h"
#include "serial.h"
#include "fw_cfg.h"
#include "apic.h"

namespace brook {
namespace {
bool enabled=false;
IrqSpinLock statusLock;
OomRecoveryStatus status;
uint64_t lastEstimate=0;
uint64_t failedRequests=0,largestRequestPages=0;
AddressSpaceDestructionWatch cleanupWatch;
BrookSystemMessage pending{};
uint16_t victimGroup=0;
uint64_t victimIncarnation=0,victimEpisode=0;
PhysicalAddress emergency[64]{};
extern "C" void SchedulerSleepMs(uint32_t);
void Copy(char* destination,unsigned capacity,const char* source) {
    unsigned i=0;
    for(;i+1<capacity && source[i];++i)destination[i]=source[i];
    destination[i]=0;
}
void BackingStatus(BrookSystemMessage& event) {
    event.flags&=~(BROOK_MESSAGE_PAGING_STATE_VALID|BROOK_MESSAGE_PAGING_CONFIGURED|
        BROOK_MESSAGE_PAGING_EXPERIMENTAL|BROOK_MESSAGE_PAGING_USAGE_VALID|BROOK_MESSAGE_PAGING_FULL);
    event.paging_capacity_bytes=event.paging_used_bytes=0;
    const auto backing=PageableMemoryBackingStatus();
    event.flags|=BROOK_MESSAGE_PAGING_STATE_VALID;
    if(!backing.configured)return;
    event.flags|=BROOK_MESSAGE_PAGING_CONFIGURED|BROOK_MESSAGE_PAGING_EXPERIMENTAL;
    if(!backing.usageValid)return;
    event.flags|=BROOK_MESSAGE_PAGING_USAGE_VALID;
    event.paging_capacity_bytes=backing.logicalCapacityBytes;
    event.paging_used_bytes=backing.logicalUsedBytes;
    if(event.paging_used_bytes==event.paging_capacity_bytes && event.paging_capacity_bytes)
        event.flags|=BROOK_MESSAGE_PAGING_FULL;
}
BrookSystemMessage Event(uint32_t code) {
    BrookSystemMessage event{};event.version=BROOK_SYSTEM_MESSAGE_VERSION;event.size=sizeof(event);
    event.code=code;event.monotonic_ms=ApicTickCount();
    event.flags=BROOK_MESSAGE_PHYSICAL_VALID;
    event.physical_total_bytes=PmmGetRamPageCount()*4096;
    event.physical_free_before_bytes=PmmGetFreePageCount()*4096;
    event.victim_uid=~uint32_t{0};
    const auto pages=__atomic_load_n(&largestRequestPages,__ATOMIC_RELAXED);
    event.requested_bytes=pages>(~uint64_t{0})/4096?~uint64_t{0}:pages*4096;
    BackingStatus(event);return event;
}
void ReleaseEmergency() {
    for(auto& frame:emergency)if(frame){PmmUnrefPage(frame);frame={};}
}
void RefillEmergency() {
    if(PmmGetFreePageCount()<OomPolicy::HighPages*2)return;
    for(auto& frame:emergency) {
        if(frame)continue;
        frame=PmmAllocPage(MemTag::KernelData,KernelPid);
        if(!frame)break;
    }
}
bool Score(AddressSpaceState* space,uint64_t deadline,uint64_t* pages) {
    *pages=0;uint64_t cursor=0;
    for(unsigned batch=0;batch<4096;++batch) {
        if(ApicTickCount()>=deadline || !KMutexTryLock(&space->mutation))return false;
        const MappingRange* range=nullptr;unsigned visited=0;
        for(auto* node=space->ranges.head;node && visited<4096;node=node->next,++visited) {
            const auto& candidate=node->range;
            if(candidate.end<=cursor)continue;
            if(candidate.sharing==MappingSharing::Private && candidate.leafOwnership==MappingLeafOwnership::Managed &&
               candidate.kind!=MappingKind::Device && candidate.kind!=MappingKind::File && candidate.kind!=MappingKind::MemFd) {
                range=&candidate;break;
            }
            cursor=candidate.end;
        }
        if(!range) {
            KMutexUnlock(&space->mutation);
            return visited<4096;
        }
        const uint64_t begin=range->begin>cursor?range->begin:cursor,end=range->end;
        VmmRangeStats stats{};uint64_t next=begin;
        const bool valid=VmmMeasureRangeStep(space->pageTable,begin,end,true,&stats,&next);
        KMutexUnlock(&space->mutation);
        if(!valid)return false;
        *pages+=stats.exclusiveManagedPages;cursor=next;
        SchedulerSleepMs(1);
    }
    return false;
}
struct Choice { OomCandidate score; bool deferred=false; };
Choice Select() {
    Choice best{};uint16_t after=0;const uint64_t deadline=ApicTickCount()+1500;
    while(ApicTickCount()<deadline) {
        Process* process=nullptr;AddressSpaceState* space=nullptr;
        if(!SchedulerNextOomCandidate(after,&process,&space))break;
        after=process->pid;
        OomCandidate candidate{};candidate.pid=process->pid;candidate.incarnation=process->incarnation;
        candidate.addressSpaceIdentity=space->identity;
        const bool scored=Score(space,deadline,&candidate.exclusiveResidentPages);
        if(scored && OomPreferCandidate(candidate,best.score))best.score=candidate;
        if(!scored)best.deferred=true;
        AddressSpaceUnref(space);ProcessUnref(process);
    }
    if(ApicTickCount()>=deadline)best.deferred=true;
    return best;
}
bool Commit(const Choice& choice,OomPolicy& policy) {
    if(!choice.score.Eligible())return false;
    auto* process=SchedulerFindProcessReference(choice.score.pid);
    if(!process)return false;
    bool exists=false;
    auto* space=SchedulerReferenceProcessAddressSpace(choice.score.pid,choice.score.incarnation,&exists);
    bool committed=false;
    if(space && space->identity==choice.score.addressSpaceIdentity &&
       AddressSpaceWatchDestruction(space,&cleanupWatch)) {
        pending=Event(BROOK_MESSAGE_TERMINATION_REQUESTED);
        pending.victim_pid=process->pid;pending.victim_uid=ProcessReadIds(process,false).real;
        pending.victim_incarnation=choice.score.incarnation;
        Copy(pending.app_name,sizeof(pending.app_name),process->name);
        Copy(pending.executable,sizeof(pending.executable),process->exePath);
        if(policy.CommitVictim(ApicTickCount(),PmmGetFreePageCount())) {
            committed=SchedulerOomCommitVictim(process,space,choice.score.incarnation);
            if(committed) {
                victimGroup=process->tgid;victimIncarnation=choice.score.incarnation;victimEpisode=space->identity;
                lastEstimate=choice.score.exclusiveResidentPages;
                SystemMessagePublish(pending);
                SerialPrintf("oom: requested pid=%u incarnation=%lu as=%lu exclusive_pages_estimate=%lu free_pages=%lu\n",
                    unsigned(process->pid),choice.score.incarnation,space->identity,
                    choice.score.exclusiveResidentPages,PmmGetFreePageCount());
            } else policy.SelectionRejected(ApicTickCount());
        }
        if(!committed)AddressSpaceCancelDestructionWatch(&cleanupWatch);
    }
    if(space)AddressSpaceUnref(space);
    ProcessUnref(process);return committed;
}
void Worker(void*) {
    OomPolicy policy;unsigned selectionRetries=0;
    for(;;) {
        const bool gone=victimIncarnation && __atomic_load_n(&cleanupWatch.done,__ATOMIC_ACQUIRE) &&
            !SchedulerOomEpisodePresent(victimEpisode);
        const auto action=policy.Tick(ApicTickCount(),PmmGetFreePageCount(),gone);
        if(policy.CurrentPhase()==OomPolicy::Phase::Idle || policy.CurrentPhase()==OomPolicy::Phase::Cooldown)selectionRetries=0;
        if(action==OomPolicy::Action::Reclaim) {
            ReleaseEmergency();
            // Existing reclaim owns cache/backing lifetime and TLB retirement.
            FilePageCacheReclaim(256);
            ExecPageCacheReclaimIdleStep(256);
        } else if(action==OomPolicy::Action::SelectVictim) {
            const auto choice=Select();
            if(!Commit(choice,policy)) {
                if(PmmGetFreePageCount()>OomPolicy::LowPages) {
                    (void)policy.Tick(ApicTickCount(),PmmGetFreePageCount());
                    continue;
                }
                if(policy.CurrentPhase()!=OomPolicy::Phase::Selecting)continue;
                // Contention or identity turnover is not proof that no safe
                // victim exists. Retry after reclaim, with a finite budget.
                if((choice.deferred || choice.score.Eligible()) && ++selectionRetries<5) {
                    policy.SelectionDeferred(ApicTickCount());SchedulerSleepMs(1);continue;
                }
                policy.NoEligibleVictim(ApicTickCount());
                SystemMessagePublish(Event(BROOK_MESSAGE_NO_MEMORY_VICTIM));
                SerialPrintf("oom: no eligible victim free_pages=%lu\n",PmmGetFreePageCount());
            }
        } else if(action==OomPolicy::Action::CleanupComplete) {
            auto complete=pending;complete.code=BROOK_MESSAGE_APP_CLOSED_MEMORY;
            complete.monotonic_ms=ApicTickCount();complete.flags|=BROOK_MESSAGE_AFTER_VALID;
            complete.physical_free_after_bytes=PmmGetFreePageCount()*4096;
            BackingStatus(complete);
            // This is observed global headroom. Do not claim a per-victim byte
            // total without independent attribution through PMM retirement.
            SystemMessagePublish(complete);pending=complete;
            SerialPrintf("oom: cleanup complete pid=%u free_pages=%lu failures=%lu\n",
                unsigned(pending.victim_pid),PmmGetFreePageCount(),__atomic_load_n(&failedRequests,__ATOMIC_RELAXED));
            victimIncarnation=0;victimGroup=0;victimEpisode=0;
            __atomic_store_n(&largestRequestPages,0,__ATOMIC_RELAXED);
        } else if(action==OomPolicy::Action::CleanupStalled) {
            auto stalled=pending;stalled.code=BROOK_MESSAGE_RECOVERY_STALLED;
            stalled.monotonic_ms=ApicTickCount();SystemMessagePublish(stalled);
            SerialPrintf("oom: protected cleanup stalled pid=%u; no further victim selected\n",unsigned(pending.victim_pid));
        }
        const auto flags=IrqSpinLockAcquire(&statusLock);
        status.phase=static_cast<uint32_t>(policy.CurrentPhase());
        status.lastVictimPid=pending.victim_pid;status.lastVictimIncarnation=pending.victim_incarnation;
        status.lastFreeBeforeBytes=pending.physical_free_before_bytes;
        status.lastFreeAfterBytes=pending.physical_free_after_bytes;
        status.exclusivePagesEstimate=lastEstimate;
        IrqSpinLockRelease(&statusLock,flags);
        RefillEmergency();SchedulerSleepMs(10);
    }
}
}
OomRecoveryStatus OomRecoverySnapshot() {
    const auto flags=IrqSpinLockAcquire(&statusLock);auto snapshot=status;
    IrqSpinLockRelease(&statusLock,flags);
    snapshot.enabled=OomRecoveryEnabled();
    snapshot.failures=__atomic_load_n(&failedRequests,__ATOMIC_RELAXED);
    snapshot.lastEventSequence=SystemMessageLastSequence();return snapshot;
}
bool OomRecoveryEnabled(){return __atomic_load_n(&enabled,__ATOMIC_ACQUIRE);}
void OomRecoveryAllocationFailure(uint64_t pages,uint64_t freePages,bool) {
    if(!OomRecoveryEnabled() || freePages>OomPolicy::LowPages)return;
    __atomic_fetch_add(&failedRequests,1,__ATOMIC_RELAXED);
    uint64_t largest=__atomic_load_n(&largestRequestPages,__ATOMIC_RELAXED);
    while(largest<pages && !__atomic_compare_exchange_n(&largestRequestPages,&largest,pages,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}
bool OomRecoveryAdmitApplication(const char* executable,uint32_t uid) {
    if(!OomRecoveryEnabled() || PmmGetFreePageCount()>OomPolicy::LowPages)return true;
    auto rejected=Event(BROOK_MESSAGE_LAUNCH_MEMORY);rejected.victim_uid=uid;rejected.requested_bytes=0;
    Copy(rejected.executable,sizeof(rejected.executable),executable);
    const char* name=executable;
    for(const char* p=executable;*p;++p)if(*p=='/')name=p+1;
    Copy(rejected.app_name,sizeof(rejected.app_name),name);
    SystemMessagePublish(rejected);return false;
}
void OomRecoveryStartWorker() {
    // Explicit opt-in until native recovery/notification acceptance passes.
    char option[8]{};const auto n=FwCfgReadFile("opt/oom-recovery",option,sizeof(option));
    if(n!=2 || option[0]!='o' || option[1]!='n')return;
    RefillEmergency();
    auto* worker=KernelThreadCreate("oom_recovery",Worker,nullptr,1);
    if(!worker){ReleaseEmergency();SerialPuts("oom: recovery worker unavailable\n");return;}
    __atomic_store_n(&enabled,true,__ATOMIC_RELEASE);SchedulerAddProcess(worker);
    SerialPuts("oom: bounded recovery worker enabled\n");
}
}
