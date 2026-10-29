// Actual recovery orchestration with controlled scheduler, VMM and I/O edges.
// Production counterparts of those edges have separate actual-code tests.
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>
#include "../../kernel/src/memory/oom_recovery.cpp"
namespace {
struct Stop{};
uint64_t clockMs=0,freeFrames=0,stopAt=0,killAt=0;
unsigned kills=0,rejections=0,busyScorePasses=0;
bool active=true,completeCleanup=true,reclaimWorks=false,recoverDuringScore=false,scoringLocked=false;
brook::Process app{},kernelWorker{};
brook::AddressSpaceState appSpace{};
brook::MappingRangeNode mapping{};
brook::KernelThreadFn entry=nullptr;
std::vector<BrookSystemMessage> events;
}
namespace brook {
uint64_t ApicTickCount(){return clockMs;}
uint64_t PmmGetFreePageCount(){return freeFrames;}
uint64_t PmmGetRamPageCount(){return 256*1024;}
PhysicalAddress PmmAllocPage(MemTag,uint16_t) {if(!freeFrames)return {};--freeFrames;return PhysicalAddress((freeFrames+1)*4096);}
void PmmUnrefPage(PhysicalAddress){++freeFrames;}
uint32_t FwCfgReadFile(const char*,void* output,uint32_t) {std::memcpy(output,"on",2);return 2;}
Process* KernelThreadCreate(const char*,KernelThreadFn callback,void*,uint8_t priority) {
    assert(priority==1);entry=callback;return &kernelWorker;
}
void SchedulerAddProcess(Process* process){assert(process==&kernelWorker);}
extern "C" void SchedulerSleepMs(uint32_t ms) {
    assert(!scoringLocked);clockMs+=ms;
    if(kills && completeCleanup && active && clockMs>=killAt+30) {
        active=false;app.addressSpace=nullptr;appSpace.references=0;
        __atomic_store_n(&cleanupWatch.done,true,__ATOMIC_RELEASE);
        freeFrames=OomPolicy::HighPages*2+100;
    }
    if(ms==10 && clockMs>=stopAt)throw Stop{};
}
void SerialPuts(const char*){}
void SerialPrintf(const char*,...){}
bool KMutexTryLock(KMutex*){assert(!scoringLocked);if(busyScorePasses){--busyScorePasses;return false;}scoringLocked=true;return true;}
void KMutexUnlock(KMutex*){assert(scoringLocked);scoringLocked=false;}
FilePageCache::ReclaimResult FilePageCacheReclaim(uint32_t count) {
    assert(count==256);if(reclaimWorks)freeFrames=OomPolicy::HighPages;
    return {};
}
uint64_t ExecPageCacheReclaimIdleStep(uint32_t count){assert(count==256);return 0;}
PageableBackingStatus PageableMemoryBackingStatus() {
    PageableBackingStatus value{};value.configured=true;value.usageValid=true;
    value.logicalCapacityBytes=1024*4096;value.logicalUsedBytes=active?value.logicalCapacityBytes:0;
    return value;
}
bool VmmMeasureRangeStep(PageTable,uint64_t begin,uint64_t end,bool,VmmRangeStats* stats,uint64_t* next) {
    assert(scoringLocked && begin<end);*stats={};stats->exclusiveManagedPages=300;
    *next=end;if(recoverDuringScore)freeFrames=OomPolicy::HighPages;return true;
}
bool SchedulerNextOomCandidate(uint16_t after,Process** process,AddressSpaceState** space) {
    *process=nullptr;*space=nullptr;
    if(!active || app.pid<=after)return false;
    *process=&app;*space=&appSpace;ProcessRef(&app);AddressSpaceRef(&appSpace);return true;
}
Process* SchedulerFindProcessReference(uint16_t pid) {
    if(!active || pid!=app.pid)return nullptr;ProcessRef(&app);return &app;
}
AddressSpaceState* SchedulerReferenceProcessAddressSpace(uint16_t pid,uint64_t incarnation,bool* exists) {
    *exists=active && pid==app.pid && incarnation==app.incarnation;
    if(!*exists)return nullptr;AddressSpaceRef(&appSpace);return &appSpace;
}
void AddressSpaceRef(AddressSpaceState* space){assert(space->references);++space->references;}
void AddressSpaceUnref(AddressSpaceState* space){assert(space->references);--space->references;}
bool AddressSpaceWatchDestruction(AddressSpaceState* space,AddressSpaceDestructionWatch* watch) {
    assert(space==&appSpace);watch->identity=space->identity;watch->done=false;return true;
}
void AddressSpaceCancelDestructionWatch(AddressSpaceDestructionWatch* watch){assert(watch==&cleanupWatch);watch->identity=0;}
bool SchedulerOomCommitVictim(Process* process,AddressSpaceState* space,uint64_t incarnation) {
    assert(process==&app && space==&appSpace && incarnation==app.incarnation);
    if(rejections){--rejections;return false;}
    ++kills;killAt=clockMs;return true;
}
bool SchedulerOomEpisodePresent(uint64_t episode) {
    assert(episode==appSpace.identity);return active;
}
CredentialIds ProcessReadIds(Process*,bool){return {1000,1000,1000};}
uint64_t SystemMessageLastSequence(){return events.size();}
bool SystemMessagePublish(const BrookSystemMessage& message){events.push_back(message);return true;}
}
using namespace brook;
static void reset(bool candidate=true) {
    enabled=false;status={};lastEstimate=0;failedRequests=largestRequestPages=0;cleanupWatch={};pending={};victimGroup=0;victimIncarnation=0;victimEpisode=0;
    for(auto& frame:emergency)frame={};
    app={};appSpace={};mapping={};kernelWorker={};entry=nullptr;
    app.pid=app.tgid=10;app.incarnation=100;app.refCount=1;app.addressSpace=&appSpace;
    std::strcpy(app.name,"GIMP");std::strcpy(app.exePath,"/nix/store/gimp/bin/gimp");
    appSpace.references=1;appSpace.identity=50;appSpace.ranges.head=&mapping;
    mapping.range={};mapping.range.begin=4096;mapping.range.end=8192;
    mapping.range.kind=MappingKind::Anonymous;mapping.range.sharing=MappingSharing::Private;
    mapping.range.leafOwnership=MappingLeafOwnership::Managed;
    active=candidate;completeCleanup=true;reclaimWorks=recoverDuringScore=scoringLocked=false;
    clockMs=0;kills=rejections=busyScorePasses=0;killAt=0;events.clear();freeFrames=OomPolicy::HighPages*3;
    OomRecoveryStartWorker();assert(OomRecoveryEnabled() && entry);
    freeFrames=256;
}
static void run(uint64_t limit) {
    stopAt=limit;try {entry(nullptr);assert(false);}catch(const Stop&){}
    assert(!scoringLocked && app.refCount==1);
}
int main() {
    reset();run(1000);
    assert(kills==1 && events.size()==2 && events[0].code==BROOK_MESSAGE_TERMINATION_REQUESTED);
    assert(events[1].code==BROOK_MESSAGE_APP_CLOSED_MEMORY && (events[1].flags&BROOK_MESSAGE_AFTER_VALID));
    assert(events[0].flags&BROOK_MESSAGE_PAGING_FULL);
    assert(!(events[1].flags&BROOK_MESSAGE_PAGING_FULL) && !events[1].paging_used_bytes);
    assert(!(events[1].flags&BROOK_MESSAGE_RELEASED_VALID)); // global delta isn't a per-victim total
    assert(!appSpace.references && !victimIncarnation);
    auto snapshot=OomRecoverySnapshot();assert(snapshot.enabled && snapshot.lastVictimPid==10 && snapshot.lastEventSequence==2);
    assert(snapshot.exclusivePagesEstimate==300 && snapshot.lastFreeAfterBytes>snapshot.lastFreeBeforeBytes);
    reset();completeCleanup=false;run(25000);
    assert(kills==1 && events.size()==2 && events[1].code==BROOK_MESSAGE_RECOVERY_STALLED);
    assert(appSpace.references==1); // watch retains no ref beyond original protected operation
    reset(false);run(20000);
    assert(!kills && events.size()==1 && events[0].code==BROOK_MESSAGE_NO_MEMORY_VICTIM);
    reset();reclaimWorks=true;run(1000);assert(!kills && events.empty());
    reset();recoverDuringScore=true;run(1000);assert(!kills && events.empty() && !cleanupWatch.identity);
    reset();rejections=1;run(2000);assert(kills==1 && events.size()==2);
    reset();busyScorePasses=2;run(2000);assert(kills==1 && events.size()==2);
    reset();busyScorePasses=1000;run(20000);
    assert(!kills && events.size()==1 && events[0].code==BROOK_MESSAGE_NO_MEMORY_VICTIM);
    reset();freeFrames=OomPolicy::HighPages;OomRecoveryAllocationFailure(10000,freeFrames,true);
    assert(!failedRequests && !largestRequestPages);run(1000);assert(!kills && events.empty());
    std::puts("OOM worker: actual orchestration/reclaim/revalidation/one victim/stalled cleanup/backing telemetry PASS");
}
