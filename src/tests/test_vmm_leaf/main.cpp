// Real VMM walks, locks and PTE transitions; platform/PMM are instrumented.
// A delayed acknowledgement is modeled explicitly, not a hardware TLB test.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <memory>
#include <unordered_map>
#include <sys/mman.h>
#include "memory/mapping_transaction.h"
#include "memory/user_mapping.h"
#include "memory/mapping_access_timing.h"
#include "memory/file_object.h"
#include "memory/file_mapping_registry.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "acpi.h"
#include "serial.h"
#include "memory/address_space.h"
#include "process.h"
#include "robust_futex.h"
#include "vfs.h"
#include "panic.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); std::abort(); } } while (0)
using namespace brook;
namespace {
constexpr size_t pageSize = 4096, pageCount = 4096;
alignas(pageSize) unsigned char frames[pageCount][pageSize];
struct Frame { unsigned refs=0, maps=0; } records[pageCount];
std::mutex frameLock;
uint64_t aliasTestBase=0;bool aliasTestDeny=false;
std::atomic<bool> failAllocation{false};
std::atomic<int> failFrameAfter{-1};
std::atomic<int> heapAllocations{0};
std::atomic<int> failHeapAfter{-1};
std::mutex mutexRegistryGuard;
std::unordered_map<KMutex*,std::unique_ptr<std::mutex>> mutexRegistry;
std::mutex& hostMutex(KMutex* mutex) {
    std::lock_guard<std::mutex> guard(mutexRegistryGuard);
    CHECK(mutexRegistry.count(mutex));return *mutexRegistry.at(mutex);
}

size_t indexOf(PhysicalAddress p) {
    auto base=reinterpret_cast<uint64_t>(frames);
    CHECK(p.raw()>=base && p.raw()<base+sizeof(frames));
    CHECK((p.raw()-base)%pageSize==0);
    return (p.raw()-base)/pageSize;
}
unsigned maps(PhysicalAddress p) {
    std::lock_guard<std::mutex> lock(frameLock);return records[indexOf(p)].maps;
}
void dropLocked(size_t i) {
    CHECK(records[i].refs>0);
    if (!--records[i].refs) { CHECK(!records[i].maps);std::memset(frames[i],0xa5,pageSize); }
}
struct Gate {
    std::mutex mutex;std::condition_variable cv;unsigned arrived=0;bool released=false;
    void park() { std::unique_lock<std::mutex> lock(mutex);++arrived;cv.notify_all();cv.wait(lock,[&]{return released;}); }
    void wait(unsigned n) { std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return arrived==n;}); }
    void release() { std::lock_guard<std::mutex> lock(mutex);released=true;cv.notify_all(); }
};
// Test-only pause before the CoW slow path acquires a file gate. The reader
// thread opts in; mutation performed by the main thread is never intercepted.
KMutex* pageAccessPauseMutex=nullptr;
Gate* pageAccessPause=nullptr;
thread_local bool pageAccessPauseOnce=false;
thread_local unsigned pageAccessPauseSkip=0;
PageTable table() {
    auto p=PmmAllocPage(MemTag::PageTable);CHECK(p);std::memset(reinterpret_cast<void*>(p.raw()),0,pageSize);return PageTable(p);
}
constexpr uint64_t flags=VMM_USER|VMM_WRITABLE|VMM_NO_EXEC;
const VirtualAddress va(0x40000000);
uint64_t leaf(PageTable pt) { auto p=VmmGetPte(pt,va);return p?*p:0; }
void cow(PageTable pt) { auto p=VmmGetPte(pt,va);CHECK(p);*p=(*p&~VMM_WRITABLE)|PTE_COW_BIT; }
void empty() {
    std::lock_guard<std::mutex> lock(frameLock);
    for(const auto& record:records)CHECK(!record.refs&&!record.maps);
}
}
namespace {
Gate* pendingAcknowledgement=nullptr;
int closedVnodes=0, unpinnedVnodes=0, droppedMemfds=0;
struct BorrowedBacking { PhysicalAddress page; };
Process* expectedDestroy = nullptr;
ProcessCleanupCompletion* expectedCompletion = nullptr;
AddressSpaceState* lateRetirement = nullptr;
unsigned cleanupWakeups = 0;
}
extern "C" void KernelPanic(const char* format,...) {
    va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);std::abort();
}
namespace brook {
bool AcpiClassifyBootRange(uint64_t address,uint32_t bytes,BootResourceClassification& result) {
    result={};if(aliasTestDeny || !aliasTestBase || address<aliasTestBase || address-aliasTestBase>4*pageSize || bytes>4*pageSize-(address-aliasTestBase))return false;
    result.types=1u<<uint32_t(MemoryType::Free);return true;
}
extern "C" void* kmalloc(uint64_t size) {
    int countdown=failHeapAfter.load();
    if(countdown>=0) { if(!countdown){failHeapAfter=-1;return nullptr;}--failHeapAfter; }
    void* p=nullptr;if(posix_memalign(&p,64,size))return nullptr;++heapAllocations;return p;
}
extern "C" void kfree(void* p) { if(p){--heapAllocations;std::free(p);} }
void KMutexInit(KMutex* mutex) {
    std::memset(mutex,0,sizeof(*mutex));
    std::lock_guard<std::mutex> guard(mutexRegistryGuard);mutexRegistry[mutex]=std::make_unique<std::mutex>();
}
void KMutexLock(KMutex* mutex) {
    if(pageAccessPauseOnce && mutex==pageAccessPauseMutex) {
        if(pageAccessPauseSkip)--pageAccessPauseSkip;
        else {pageAccessPauseOnce=false;pageAccessPause->park();}
    }
    hostMutex(mutex).lock();
}
void KMutexUnlock(KMutex* mutex) { hostMutex(mutex).unlock(); }
bool KvmClockReadStableNs(uint64_t* result) {
    *result=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();return true;
}
void SchedulerKernelRegionEnter(Process* process) { ++process->kernelRegionDepth;ProcessRef(process); }
void SchedulerKernelRegionLeave(Process* process) { CHECK(process->kernelRegionDepth);--process->kernelRegionDepth;ProcessUnref(process); }
void SchedulerUnblock(Process* process) {
    CHECK(expectedCompletion && expectedCompletion->done);
    CHECK(process==expectedCompletion->waiter && process->refCount==1);
    CHECK(unpinnedVnodes==2 && closedVnodes==2);
    CHECK(ProcessGetCleanupStatus().activeFinalizers!=0);
    ++cleanupWakeups;
}
unsigned robustCleanups=0,robustDelays=0;
void RobustFutexCleanup(Process* process) {
    CHECK(process->state==ProcessState::Terminated && process->runningOnCpu==-1 && process->refCount==1);
    CHECK(ProcessGetCleanupStatus().activeFinalizers==1);
    process->robustListHead=0;process->robustExitState=3;++robustCleanups;
}
void SchedulerReleaseWaitReference(Process* process) {CHECK(process->robustExitState==3);ProcessUnref(process);}
extern "C" void SchedulerSleepMs(uint32_t) {++robustDelays;}
void ProcessDestroyNow(Process* process) {
    CHECK(process==expectedDestroy);
    CHECK(ProcessGetCleanupStatus().activeFinalizers!=0);
    AddressSpaceUnref(process->addressSpace);
    process->addressSpace=nullptr;
}
void CompositorReleaseAddressSpaceBuffers(AddressSpaceState* space) {
    CHECK(!PmmGetRefCount(space->pageTable.pml4));
    if(lateRetirement) { auto* late=lateRetirement;lateRetirement=nullptr;AddressSpaceUnref(late); }
}
void TlbShootdownFull(uint64_t,uint64_t) { if(pendingAcknowledgement)pendingAcknowledgement->park(); }
void MemFdHandleUnref(void* handle) { ++droppedMemfds;PmmFreePage(static_cast<BorrowedBacking*>(handle)->page); }
void VfsFileMapUnpin(Vnode*) { ++unpinnedVnodes; }
extern "C" void VfsClose(Vnode* vn) { CHECK(!vn->refCount);++closedVnodes; }
PhysicalAddress PmmAllocPage(MemTag,uint16_t) {
    if(failAllocation.exchange(false))return {};
    int countdown=failFrameAfter.load();
    if(countdown>=0) { if(!countdown){failFrameAfter=-1;return {};}--failFrameAfter; }
    std::lock_guard<std::mutex> lock(frameLock);
    for(size_t i=0;i<pageCount;++i)if(!records[i].refs) {
        CHECK(!records[i].maps);records[i].refs=1;std::memset(frames[i],0xcc,pageSize);
        return PhysicalAddress(reinterpret_cast<uint64_t>(frames[i]));
    }
    return {};
}
void PmmThrottleForDrain() {}
void PmmRefPage(PhysicalAddress p) {
    std::lock_guard<std::mutex> lock(frameLock);auto& r=records[indexOf(p)];CHECK(r.refs&&r.refs<~unsigned{0});++r.refs;
}
bool PmmRefPageIfAlive(PhysicalAddress p) {
    std::lock_guard<std::mutex> lock(frameLock);auto& r=records[indexOf(p)];if(!r.refs)return false;CHECK(r.refs<~unsigned{0});++r.refs;return true;
}
void PmmUnrefPage(PhysicalAddress p) { std::lock_guard<std::mutex> lock(frameLock);dropLocked(indexOf(p)); }
void PmmFreePage(PhysicalAddress p) { std::lock_guard<std::mutex> lock(frameLock);CHECK(records[indexOf(p)].refs==1);dropLocked(indexOf(p)); }
bool PmmHasSingleReference(PhysicalAddress p) { return PmmGetRefCount(p)==1; }
uint32_t PmmGetRefCount(PhysicalAddress p) { std::lock_guard<std::mutex> lock(frameLock);return records[indexOf(p)].refs; }
uint32_t PmmGetMapCount(PhysicalAddress p) { std::lock_guard<std::mutex> lock(frameLock);return records[indexOf(p)].maps; }
void PmmMapInc(PhysicalAddress p) { std::lock_guard<std::mutex> lock(frameLock);auto& r=records[indexOf(p)];CHECK(r.refs);++r.maps; }
void PmmMapDec(PhysicalAddress p) { std::lock_guard<std::mutex> lock(frameLock);auto& r=records[indexOf(p)];CHECK(r.maps);--r.maps; }
extern "C" int PmmDumpFreeLog(uint64_t) { CHECK(false);return 0; }
extern "C" void SerialPrintf(const char* format,...) {
    va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);
}
extern "C" void SerialPuts(const char* text) {std::fputs(text,stderr);}
}
static void competingInstallAndDetach() {
    for(unsigned round=0;round<64;++round) {
        auto pt=table();PhysicalAddress candidates[2]={PmmAllocPage(),PmmAllocPage()};
        VmmMapResult result[2];Gate start;
        std::thread a([&]{start.park();result[0]=VmmTryMapPage(pt,va,candidates[0],flags,MemTag::User,9);});
        std::thread b([&]{start.park();result[1]=VmmTryMapPage(pt,va,candidates[1],flags,MemTag::User,9);});
        start.wait(2);start.release();a.join();b.join();
        unsigned winner=result[0]==VmmMapResult::Mapped?0:1,loser=winner^1;
        CHECK(result[winner]==VmmMapResult::Mapped&&result[loser]==VmmMapResult::Occupied);
        CHECK(VmmLeafPhysical(leaf(pt))==candidates[winner]);
        CHECK(PmmGetRefCount(candidates[winner])==1&&maps(candidates[winner])==1);
        CHECK(PmmGetRefCount(candidates[loser])==1&&maps(candidates[loser])==0);
        CHECK(VmmGetPagePid(pt,va)==9&&VmmGetPageTag(pt,va)==MemTag::User);
        CHECK(leaf(pt)&VMM_NO_EXEC);PmmUnrefPage(candidates[loser]);
        uint64_t detached[2];Gate detach;
        std::thread c([&]{detach.park();detached[0]=VmmDetachPage(pt,va);});
        std::thread d([&]{detach.park();detached[1]=VmmDetachPage(pt,va);});
        detach.wait(2);detach.release();c.join();d.join();
        CHECK(bool(detached[0])!=bool(detached[1]));CHECK(!leaf(pt));
        auto old=VmmLeafPhysical(detached[0]|detached[1]);CHECK(old==candidates[winner]);
        CHECK(PmmGetRefCount(old)==1&&maps(old)==0);PmmUnrefPage(old);
        VmmDestroyUserPageTable(pt);empty();
    }
}
static void detachReinstall() {
    auto pt=table();auto old=PmmAllocPage();CHECK(VmmTryMapPage(pt,va,old,flags)==VmmMapResult::Mapped);
    auto token=VmmDetachPage(pt,va);CHECK(VmmLeafPhysical(token)==old);
    auto replacement=PmmAllocPage();CHECK(replacement!=old);
    CHECK(VmmTryMapPage(pt,va,replacement,VMM_USER|VMM_NO_EXEC)==VmmMapResult::Mapped);
    CHECK(PmmGetRefCount(old)==1&&maps(old)==0);
    PmmUnrefPage(VmmLeafPhysical(token)); // simulated remote acknowledgement
    CHECK(VmmVirtToPhys(pt,va)==replacement&&maps(replacement)==1);
    auto reused=PmmAllocPage();CHECK(reused==old);PmmFreePage(reused);
    VmmDestroyUserPageTable(pt);empty();
}
static void cowRetirement(bool concurrent) {
    auto pt=table();auto old=PmmAllocPage();std::memset(reinterpret_cast<void*>(old.raw()),0x6d,pageSize);
    CHECK(VmmTryMapPage(pt,va,old,flags)==VmmMapResult::Mapped);cow(pt);
    PmmRefPage(old); // separate owner's reference
    VmmCowResolution result[2]{};
    if(concurrent) {
        Gate start;
        std::thread a([&]{start.park();result[0]=VmmCowResolveWrite(pt,va,9);});
        std::thread b([&]{start.park();result[1]=VmmCowResolveWrite(pt,va,9);});
        start.wait(2);start.release();a.join();b.join();
        unsigned winner=result[0].result==VmmCowResult::Resolved?0:1;
        CHECK(result[winner].result==VmmCowResult::Resolved&&result[winner^1].result==VmmCowResult::NotCow);
        CHECK(result[winner].retired==old&&!result[winner^1].retired);
    } else result[0]=VmmCowResolveWrite(pt,va,9);
    CHECK(PmmGetRefCount(old)==2&&maps(old)==0);
    auto current=VmmVirtToPhys(pt,va);CHECK(current!=old&&maps(current)==1);
    CHECK((leaf(pt)&(PTE_COW_BIT|VMM_WRITABLE))==VMM_WRITABLE);
    for(size_t i=0;i<pageSize;++i)CHECK(reinterpret_cast<unsigned char*>(current.raw())[i]==0x6d);
    Gate acknowledgement;
    std::thread reclaimer([&] {
        PmmUnrefPage(old); // other process exits BEFORE delayed TLB acknowledgement
        acknowledgement.park();
        for(auto r:result)if(r.retired)PmmUnrefPage(r.retired);
    });
    acknowledgement.wait(1);CHECK(PmmGetRefCount(old)==1);
    auto spare=PmmAllocPage();CHECK(spare!=old);PmmFreePage(spare);
    for(size_t i=0;i<pageSize;++i)CHECK(reinterpret_cast<unsigned char*>(old.raw())[i]==0x6d);
    acknowledgement.release();reclaimer.join();CHECK(PmmGetRefCount(old)==0);
    VmmDestroyUserPageTable(pt);empty();
}
static void largeLeafAndBorrowedFrame() {
    for(unsigned level : {2u,3u}) {
        auto pt=table();auto pdpt=PmmAllocPage(MemTag::PageTable);
        std::memset(reinterpret_cast<void*>(pdpt.raw()),0,pageSize);
        reinterpret_cast<uint64_t*>(pt.pml4.raw())[0]=pdpt.raw()|VMM_PRESENT|VMM_WRITABLE;
        uint64_t rootEntry=reinterpret_cast<uint64_t*>(pt.pml4.raw())[0];
        PhysicalAddress pd;
        auto entries=reinterpret_cast<uint64_t*>(pdpt.raw());
        uint64_t* large;
        if(level==3)large=&entries[1]; // va=1GB
        else {
            pd=PmmAllocPage(MemTag::PageTable);std::memset(reinterpret_cast<void*>(pd.raw()),0,pageSize);
            entries[1]=pd.raw()|VMM_PRESENT|VMM_WRITABLE;
            large=&reinterpret_cast<uint64_t*>(pd.raw())[0];
        }
        uint64_t saved=0x40000000|VMM_PRESENT|VMM_USER|VMM_WRITABLE|(1u<<7);
        *large=saved;uint64_t directoryEntry=entries[1];auto candidate=PmmAllocPage();
        CHECK(VmmTryMapPage(pt,va,candidate,flags)==VmmMapResult::Occupied);
        VmmRangeStats census{};
        CHECK(!VmmMeasureRange(pt,va.raw(),va.raw()+4096,true,&census)&&!census.backedPages);
        CHECK(*large==saved&&!VmmDetachPage(pt,va));
        CHECK(!VmmClearAccessedIfMatches(pt,va,candidate)&&*large==saved);
        CHECK(reinterpret_cast<uint64_t*>(pt.pml4.raw())[0]==rootEntry&&entries[1]==directoryEntry);
        CHECK(PmmGetRefCount(candidate)==1&&maps(candidate)==0);
        // Preflight must encounter the later large leaf before detaching an
        // earlier ordinary leaf, including before invoking retirement callbacks.
        auto earlier=PmmAllocPage();const VirtualAddress early(0x200000);
        CHECK(VmmTryMapPage(pt,early,earlier,flags)==VmmMapResult::Mapped);
        unsigned callbacks=0;uint64_t count=0;
        auto retire=[](VirtualAddress,uint64_t,void* context){++*static_cast<unsigned*>(context);};
        CHECK(!VmmDetachRange(pt,early.raw(),va.raw()+pageSize,2,retire,&callbacks,&count));
        CHECK(!callbacks&&!count&&VmmVirtToPhys(pt,early)==earlier&&maps(earlier)==1);
        CHECK(*large==saved);
        PmmFreePage(candidate);*large=0;VmmDestroyUserPageTable(pt);empty();
    }
    auto pt=table();auto backing=PmmAllocPage(MemTag::Device);
    CHECK(VmmTryMapPage(pt,va,backing,flags,MemTag::Device,23)==VmmMapResult::Mapped);
    auto detached=VmmDetachPage(pt,va);
    CHECK(VmmLeafPhysical(detached)==backing&&(detached&VMM_NO_EXEC));
    CHECK(((detached&PTE_TAG_MASK)>>PTE_TAG_SHIFT)==static_cast<unsigned>(MemTag::Device));
    CHECK(((detached&PTE_PID_MASK)>>PTE_PID_SHIFT)==23);
    CHECK(PmmGetRefCount(backing)==1&&maps(backing)==0);
    // Borrowed mapping: simulated backing owner releases after acknowledgement.
    PmmFreePage(backing);VmmDestroyUserPageTable(pt);empty();
}
static void occupiedAncestors() {
    auto pt=table();auto original=PmmAllocPage();auto candidate=PmmAllocPage();
    CHECK(VmmTryMapPage(pt,va,original,flags)==VmmMapResult::Mapped);
    auto* root=reinterpret_cast<uint64_t*>(pt.pml4.raw());root[0]&=~VMM_USER;
    auto* directory=reinterpret_cast<uint64_t*>(VmmLeafPhysical(root[0]).raw());directory[1]&=~VMM_USER;
    auto* pages=reinterpret_cast<uint64_t*>(VmmLeafPhysical(directory[1]).raw());pages[0]&=~VMM_USER;
    const uint64_t ancestors[]={root[0],directory[1],pages[0]},saved=leaf(pt);
    CHECK(VmmTryMapPage(pt,va,candidate,flags)==VmmMapResult::Occupied);
    CHECK(root[0]==ancestors[0]&&directory[1]==ancestors[1]&&pages[0]==ancestors[2]&&leaf(pt)==saved);
    // An absent sibling needs another PT. Failure must not promote any ancestor.
    failAllocation=true;
    CHECK(VmmTryMapPage(pt,va+0x200000,candidate,flags)==VmmMapResult::OutOfMemory);
    CHECK(root[0]==ancestors[0]&&directory[1]==ancestors[1]&&pages[0]==ancestors[2]);
    CHECK(VmmTryMapPage(pt,va+0x200000,candidate,flags)==VmmMapResult::Mapped);
    CHECK((root[0]&VMM_USER)&&(directory[1]&VMM_USER)&&(pages[1]&VMM_USER));
    VmmDestroyUserPageTable(pt);empty();
}
static void accessedLeafAging() {
    auto pt=table();auto page=PmmAllocPage();
    CHECK(!VmmClearAccessedIfMatches(pt,va,page));
    CHECK(VmmTryMapPage(pt,va,page,flags|VMM_ACCESSED|VMM_DIRTY|VMM_LEAF_PAT,MemTag::User,37)==VmmMapResult::Mapped);
    auto* entry=VmmGetPte(pt,va);CHECK(entry);const uint64_t saved=*entry;
    CHECK(!VmmClearAccessedIfMatches(pt,va+1,page)&&*entry==saved);
    CHECK(!VmmClearAccessedIfMatches(pt,va,PhysicalAddress(page.raw()+4096))&&*entry==saved);
    CHECK(VmmClearAccessedIfMatches(pt,va,page)&&*entry==(saved&~VMM_ACCESSED));
    CHECK(!VmmClearAccessedIfMatches(pt,va,page)&&*entry==(saved&~VMM_ACCESSED));
    for(uint64_t rejected:{saved|PTE_COW_BIT,(saved&~VMM_PRESENT)|PTE_HIDDEN_BIT,saved&~VMM_USER}) {
        *entry=rejected;CHECK(!VmmClearAccessedIfMatches(pt,va,page)&&*entry==rejected);
    }
    *entry=saved;
    CHECK(VmmClearAccessedIfMatches(pt,va,page)&&PmmGetRefCount(page)==1&&maps(page)==1);
    VmmDestroyUserPageTable(pt);empty();
}
static void failureAndInPlace() {
    auto pt=table();auto candidate=PmmAllocPage();
    CHECK(VmmTryMapPage(pt,VirtualAddress(0),candidate,flags)==VmmMapResult::InvalidAddress);
    failAllocation=true;CHECK(VmmTryMapPage(pt,va,candidate,flags)==VmmMapResult::OutOfMemory);
    CHECK(!leaf(pt)&&PmmGetRefCount(candidate)==1&&maps(candidate)==0);
    CHECK(VmmTryMapPage(pt,va,candidate,flags)==VmmMapResult::Mapped);cow(pt);
    auto saved=leaf(pt);PmmRefPage(candidate);failAllocation=true;
    auto failed=VmmCowResolveWrite(pt,va,9);
    CHECK(failed.result==VmmCowResult::OutOfMemory&&!failed.retired);
    CHECK(leaf(pt)==saved&&PmmGetRefCount(candidate)==2&&maps(candidate)==1);
    PmmUnrefPage(candidate);
    auto inPlace=VmmCowResolveWrite(pt,va,9);
    CHECK(inPlace.result==VmmCowResult::Resolved&&!inPlace.retired);
    CHECK(VmmVirtToPhys(pt,va)==candidate&&PmmGetRefCount(candidate)==1);
    auto notCow=VmmCowResolveWrite(pt,va,9);CHECK(notCow.result==VmmCowResult::NotCow&&!notCow.retired);
    VmmDestroyUserPageTable(pt);empty();
}
static void releaseTestMappedFile(void* object) {
    auto* vnode=static_cast<Vnode*>(object);VfsFileMapUnpin(vnode);
    CHECK(vnode->refCount);if (!--vnode->refCount) VfsClose(vnode);
}
static void releaseTestNoop(void*) {}
// Fixtures construct unpublished AS metadata; the descriptor's initial caller
// reference transfers into the installed range and is dropped here.
static void attachTestRange(AddressSpaceState* space,uint64_t begin,MappingKind kind,
                            MappingBacking* backing,MappingLeafOwnership ownership=MappingLeafOwnership::Managed) {
    MappingRangePlan plan;CHECK(plan.Prepare(1));
    MappingRange range{begin,begin+pageSize,0,0,backing,3,7,kind,
        kind==MappingKind::File || kind==MappingKind::Anonymous?MappingSharing::Private:MappingSharing::Shared,ownership};
    CHECK(plan.Insert(space->ranges,range));MappingBackingUnref(backing);
}
static void addressSpaceLifetime() {
    const auto baseline=ProcessGetCleanupStatus();
    CHECK(!baseline.liveUserAddressSpaces&&!baseline.queuedProcesses&&!baseline.queuedAddressSpaces&&!baseline.activeFinalizers);
    auto kernelRoot=table();VmmHostSetKernelRoot(kernelRoot);AddressSpaceKernelInit();
    auto* immortal=AddressSpaceKernel();AddressSpaceRef(immortal);AddressSpaceUnref(immortal);CHECK(!ProcessCleanupStep());
    failAllocation=true;CHECK(!AddressSpaceCreate(17)&&heapAllocations==0);
    auto* old=AddressSpaceCreate(17);CHECK(old&&old->references==1&&old->identity&&old->generation);
    AddressSpaceRef(old);CHECK(old->references==2);
    auto file=PmmAllocPage(),memfd=PmmAllocPage(),framebuffer=PmmAllocPage(),window=PmmAllocPage();
    Vnode vnode{};vnode.refCount=1;BorrowedBacking backing{memfd};
    auto* fileBacking=MappingBackingCreate(&vnode,releaseTestMappedFile);CHECK(fileBacking);
    attachTestRange(old,va.raw(),MappingKind::File,fileBacking);
    auto* memfdBacking=MappingBackingCreate(&backing,MemFdHandleUnref);CHECK(memfdBacking);
    attachTestRange(old,va.raw()+4096,MappingKind::MemFd,memfdBacking,MappingLeafOwnership::Borrowed);
    auto* fbBacking=MappingBackingCreate(&vnode,releaseTestNoop);CHECK(fbBacking);
    attachTestRange(old,va.raw()+8192,MappingKind::Device,fbBacking);
    auto* windowBacking=MappingBackingCreate(&vnode,releaseTestNoop);CHECK(windowBacking);
    attachTestRange(old,va.raw()+12288,MappingKind::Device,windowBacking);
    PmmRefPage(framebuffer);PmmRefPage(window); // PTE refs separate from kernel owners
    CHECK(VmmTryMapPage(old->pageTable,va,file,flags)==VmmMapResult::Mapped);
    CHECK(VmmTryMapPage(old->pageTable,va+4096,memfd,flags)==VmmMapResult::Mapped);
    CHECK(VmmTryMapPage(old->pageTable,va+8192,framebuffer,flags,MemTag::Device)==VmmMapResult::Mapped);
    // Managed shared graphics aliases retain their own PTE references.
    CHECK(VmmTryMapPage(old->pageTable,va+12288,window,flags,MemTag::Device)==VmmMapResult::Mapped);
    AddressSpaceUnref(old);CHECK(old->references==1&&!ProcessCleanupStep());
    CHECK(ProcessGetCleanupStatus().liveUserAddressSpaces==1);
    CHECK(!ProcessGetCleanupStatus().queuedAddressSpaces); // Retained refs still matter.
    uint64_t identity=old->identity;
    const int ownedAllocations=heapAllocations.load();
    AddressSpaceUnref(old);CHECK(PmmGetRefCount(file)==1&&heapAllocations==ownedAllocations);
    CHECK(ProcessGetCleanupStatus().queuedAddressSpaces==1);
    Gate ack;pendingAcknowledgement=&ack;
    std::thread cleanup([]{CHECK(ProcessCleanupStep());});ack.wait(1);
    const auto blocked=ProcessGetCleanupStatus();
    CHECK(blocked.liveUserAddressSpaces==1&&!blocked.queuedAddressSpaces&&blocked.activeFinalizers==1);
    // Old AS is queued and still owns its pages through the acknowledgement.
    CHECK(PmmGetRefCount(file)==1&&PmmGetRefCount(memfd)==1&&maps(framebuffer)==1);
    auto* replacement=AddressSpaceCreate(17);CHECK(replacement&&replacement->identity!=identity);
    auto fresh=PmmAllocPage();CHECK(VmmTryMapPage(replacement->pageTable,va,fresh,flags)==VmmMapResult::Mapped);
    attachTestRange(replacement,va.raw(),MappingKind::Anonymous,nullptr);
    const int replacementAllocations=heapAllocations.load()-ownedAllocations;
    ack.release();cleanup.join();pendingAcknowledgement=nullptr;
    CHECK(!PmmGetRefCount(file)&&!PmmGetRefCount(memfd));
    CHECK(PmmGetRefCount(framebuffer)==1&&maps(framebuffer)==0);
    CHECK(closedVnodes==1&&unpinnedVnodes==1&&droppedMemfds==1);
    CHECK(VmmVirtToPhys(replacement->pageTable,va)==fresh&&PmmGetRefCount(fresh)==1);
    CHECK(PmmGetRefCount(window)==1&&maps(window)==0);
    CHECK(heapAllocations==replacementAllocations);PmmFreePage(framebuffer);PmmFreePage(window);
    AddressSpaceUnref(replacement);CHECK(ProcessCleanupStep()&&!ProcessCleanupStep());
    CHECK(heapAllocations==0);PmmFreePage(kernelRoot.pml4);VmmHostSetKernelRoot({});empty();
    std::puts("ADDRESS SPACE PASS refs/identity/queued-cleanup/borrowed-backing/reused-pid/OOM");
}
static void consumingWaitCleanup() {
    const auto baseline=ProcessGetCleanupStatus();
    auto root=table();VmmHostSetKernelRoot(root);
    auto* retiring=AddressSpaceCreate(23);lateRetirement=AddressSpaceCreate(24);
    CHECK(retiring&&lateRetirement);
    auto page=PmmAllocPage();Vnode vnode{};vnode.refCount=1;
    auto* backing=MappingBackingCreate(&vnode,releaseTestMappedFile);CHECK(backing);
    attachTestRange(retiring,va.raw(),MappingKind::File,backing);
    CHECK(VmmTryMapPage(retiring->pageTable,va,page,flags)==VmmMapResult::Mapped);
    Process waiter{},child{};ProcessCleanupCompletion completion{&waiter,false};
    child.addressSpace=retiring;child.destroyCompletion=&completion;
    expectedDestroy=&child;expectedCompletion=&completion;
    ProcessQueueDestroy(&child);
    CHECK(ProcessGetCleanupStatus().queuedProcesses==1);
    CHECK(!completion.done&&heapAllocations==4);
    CHECK(ProcessCleanupStep());
    CHECK(completion.done&&cleanupWakeups==1&&!waiter.refCount);
    const auto partial=ProcessGetCleanupStatus();
    CHECK(!partial.activeFinalizers&&!partial.queuedProcesses&&partial.queuedAddressSpaces==1&&partial.liveUserAddressSpaces==1);
    CHECK(partial.completedProcesses==baseline.completedProcesses+1);
    CHECK(partial.completedAddressSpaces==baseline.completedAddressSpaces+1);
    CHECK(!PmmGetRefCount(page)&&heapAllocations==1);
    // Work enqueued while draining the finite retirement batch stays queued;
    // it cannot postpone this child's consuming wait indefinitely.
    CHECK(ProcessCleanupStep()&&!ProcessCleanupStep()&&heapAllocations==0);
    CHECK(!ProcessGetCleanupStatus().liveUserAddressSpaces);
    CHECK(!ProcessGetCleanupStatus().queuedAddressSpaces);
    CHECK(ProcessGetCleanupStatus().completedAddressSpaces==baseline.completedAddressSpaces+2);
    expectedDestroy=nullptr;expectedCompletion=nullptr;
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();
    std::puts("ADDRESS SPACE WAIT PASS pin-release-before-wake/waiter-ref/finite-batch");
}
struct BackingRetirementWitness { AddressSpaceState* space; PhysicalAddress physical; bool released=false; Process* actor=nullptr; };
static void releaseTransactionBacking(void* opaque) {
    auto* witness=static_cast<BackingRetirementWitness*>(opaque);
    if(witness->actor) CHECK(witness->actor->kernelRegionDepth);
    CHECK(hostMutex(&witness->space->mutation).try_lock());hostMutex(&witness->space->mutation).unlock();
    CHECK(PmmGetRefCount(witness->physical)==1&&maps(witness->physical)==0);
    PmmFreePage(witness->physical);witness->released=true;
}
static void mappingTransactions() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto* space=AddressSpaceCreate(25);CHECK(space);Process actor{};actor.addressSpace=space;
    auto original=PmmAllocPage();CHECK(VmmTryMapPage(space->pageTable,va,original,flags)==VmmMapResult::Mapped);
    uint64_t generation=space->generation;
    Gate ack;pendingAcknowledgement=&ack;
    std::thread remover([&]{
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(1,0));
        operation.Lock();CHECK(operation.Detach(va));
        CHECK(actor.kernelRegionDepth&&space->references==2);
    });
    ack.wait(1);
    CHECK(!VmmVirtToPhys(space->pageTable,va)&&PmmGetRefCount(original)==1);
    CHECK(hostMutex(&space->mutation).try_lock());hostMutex(&space->mutation).unlock();
    CHECK(space->generation>generation);
    auto replacement=PmmAllocPage();CHECK(replacement!=original);
    CHECK(VmmTryMapPage(space->pageTable,va,replacement,flags)==VmmMapResult::Mapped);
    ack.release();remover.join();pendingAcknowledgement=nullptr;
    CHECK(!PmmGetRefCount(original)&&!actor.kernelRegionDepth&&!actor.refCount&&space->references==1);
    CHECK(VmmVirtToPhys(space->pageTable,va)==replacement);
    // Rollback retires only a matching published frame, leaving replacements.
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(2,2));
        CHECK(operation.PrepareZeroed(MemTag::User,25));operation.Lock();
        CHECK(operation.Publish(0,va+4096,flags,MemTag::User,25)==VmmMapResult::Mapped);
        auto zeroed=VmmVirtToPhys(space->pageTable,va+4096);
        for(unsigned i=0;i<pageSize;++i)CHECK(reinterpret_cast<unsigned char*>(zeroed.raw())[i]==0);
        CHECK(operation.Publish(1,va,flags,MemTag::User,25)==VmmMapResult::Occupied);
        operation.RollbackPublished();operation.RollbackPublished();
        CHECK(!VmmVirtToPhys(space->pageTable,va+4096));
        CHECK(VmmVirtToPhys(space->pageTable,va)==replacement);
    }
    // A replacement made after publication must not be detached by rollback.
    auto other=PmmAllocPage();PhysicalAddress previous;
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(1,1));CHECK(operation.PrepareZeroed(MemTag::User,25));operation.Lock();
        CHECK(operation.Publish(0,va+4096,flags,MemTag::User,25)==VmmMapResult::Mapped);
        auto retired=VmmDetachPage(space->pageTable,va+4096);previous=VmmLeafPhysical(retired);
        CHECK(VmmTryMapPage(space->pageTable,va+4096,other,flags)==VmmMapResult::Mapped);
        operation.RollbackPublished();CHECK(VmmVirtToPhys(space->pageTable,va+4096)==other);
    }
    PmmUnrefPage(previous); // external replacement owns its retirement token.
    for(int fail=0;fail<3;++fail) {
        MappingTransaction operation(&actor,space);failHeapAfter=fail;
        CHECK(!operation.Prepare(2,2,2));CHECK(VmmVirtToPhys(space->pageTable,va)==replacement);
    }
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(1,1));
        failAllocation=true;CHECK(!operation.PrepareZeroed(MemTag::User,25));
        CHECK(VmmVirtToPhys(space->pageTable,va)==replacement);
    }
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(3,3));
        failFrameAfter=1;CHECK(!operation.PrepareZeroed(MemTag::User,25));
    }
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(2,2));
        CHECK(operation.PrepareZeroed(MemTag::User,25));operation.Lock();
        CHECK(operation.Publish(0,va+8192,flags,MemTag::User,25)==VmmMapResult::Mapped);
        failAllocation=true;
        CHECK(operation.Publish(1,va+0x40000000,flags,MemTag::User,25)==VmmMapResult::OutOfMemory);
        operation.RollbackPublished();CHECK(!VmmVirtToPhys(space->pageTable,va+8192));
        CHECK(!VmmVirtToPhys(space->pageTable,va+0x40000000));
    }
    auto borrowed=PmmAllocPage(MemTag::Device);
    CHECK(VmmTryMapPage(space->pageTable,va+8192,borrowed,flags,MemTag::Device)==VmmMapResult::Mapped);
    auto* borrowedDescriptor=MappingBackingCreate(&actor,releaseTestNoop);CHECK(borrowedDescriptor);
    attachTestRange(space,va.raw()+8192,MappingKind::Device,borrowedDescriptor,MappingLeafOwnership::Borrowed);
    BackingRetirementWitness witness{space,borrowed};Gate backingAck;pendingAcknowledgement=&backingAck;
    std::thread backingRemover([&]{
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(1,0,1));CHECK(operation.PrepareRanges(0));operation.Lock();
        CHECK(operation.Detach(va+8192));CHECK(operation.ReplaceMapping(va.raw()+8192,va.raw()+12288));
        CHECK(operation.RetireBacking(&witness,releaseTransactionBacking));
    });
    backingAck.wait(1);CHECK(!witness.released&&PmmGetRefCount(borrowed)==1);
    backingAck.release();backingRemover.join();pendingAcknowledgement=nullptr;
    CHECK(witness.released&&!PmmGetRefCount(borrowed));
    CHECK(!actor.kernelRegionDepth&&!actor.refCount&&space->references==1&&heapAllocations==1);
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();
    std::puts("MAPPING TRANSACTION PASS delayed-ACK/unlocked-retirement/zero-publication/rollback/replacement/OOM");
}
static void rangeBackingReleased(void* object) { ++*static_cast<unsigned*>(object); }
static MappingRange rangeRecord(uint64_t begin, uint64_t end, MappingBacking* backing,
                                uint32_t prot=3, uint32_t max=7) {
    return {begin,end,0,0,backing,prot,max,backing?MappingKind::File:MappingKind::Anonymous,
            MappingSharing::Private,MappingLeafOwnership::Managed};
}
static void rangeTableEdits() {
    const auto initialHeap=heapAllocations.load();
    unsigned released=0;auto* backing=MappingBackingCreate(&released,rangeBackingReleased);CHECK(backing);
    MappingRangeTable table{};
    auto initial=rangeRecord(0x1000,0x9000,backing);
    { MappingRangePlan plan;CHECK(plan.Prepare(1));CHECK(plan.Insert(table,initial)); }
    CHECK(backing->references==2);
    auto originalId=table.head->range.identity;
    // Insufficient split storage is a true preflight failure, including refs.
    { MappingRangePlan plan;CHECK(plan.Prepare(1));CHECK(!plan.Replace(table,0x3000,0x5000)); }
    CHECK(table.head->range.identity==originalId&&table.head->range.end==0x9000&&backing->references==2);
    {
        MappingRangePlan plan;CHECK(plan.Prepare(2));CHECK(plan.Replace(table,0x3000,0x5000));
        CHECK(!MappingRangeFind(table,0x3000));CHECK(MappingRangeFind(table,0x5000)->offset==0x4000);
        CHECK(backing->references==4); // original held in retirement + two sides
    }
    CHECK(backing->references==3);
    auto reservation=rangeRecord(0x3000,0x5000,nullptr,0);
    { MappingRangePlan plan;CHECK(plan.Prepare(1));CHECK(plan.Insert(table,reservation)); }
    CHECK(MappingRangeFindGap(table,0x2000,0x2000,0x1000,0x10000)==0x9000);
    {
        MappingRangePlan plan;CHECK(plan.Prepare(2));
        CHECK(plan.Protect(table,0x2000,0x6000,1));
        CHECK(MappingRangeFind(table,0x1000)->protection==3);
        CHECK(MappingRangeFind(table,0x2000)->protection==1);
        CHECK(MappingRangeFind(table,0x3000)->protection==1);
        CHECK(MappingRangeFind(table,0x5000)->offset==0x4000);
        CHECK(MappingRangeFind(table,0x6000)->protection==3);
    }
    {
        MappingRangePlan plan;CHECK(plan.Prepare(2));
        CHECK(!plan.Protect(table,0x8000,0xa000,0)); // hole at end
        CHECK(MappingRangeFind(table,0x8000)->protection==3);
    }
    for(int fail=0;fail<3;++fail) {
        MappingRangePlan plan;failHeapAfter=fail;CHECK(!plan.Prepare(3));
        CHECK(MappingRangeFind(table,0x3000)->protection==1);
    }
    { MappingRangePlan plan;CHECK(plan.Prepare(0));CHECK(plan.Replace(table,0x1000,0x9000));CHECK(!released); }
    CHECK(!table.head&&backing->references==1&&!released);
    MappingBackingUnref(backing);CHECK(released==1&&heapAllocations==initialHeap);

    // Independent page-granularity oracle checks mixed interval edits, offsets,
    // coverage failures and gap selection, including sparse reservations.
    struct Page { bool present=false;uint32_t protection=0,maximum=0;uint64_t offset=0; } oracle[64];
    uint32_t random=0x3a2f9b41;
    auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned round=0;round<1000;++round) {
        unsigned begin=next()%64,end=begin+1+next()%(64-begin),op=next()%4;
        // Warm entries on both sides and inside the next edit. Keep positive
        // lookups live across replacement, hole creation and protection splits.
        for (unsigned i : {begin,end-1,next()%64})
            (void)MappingRangeFind(table,(i+1)*pageSize+13);
        uint32_t protection=next()%8;bool expected=true;
        {
            MappingRangePlan plan;CHECK(plan.Prepare(3));
            if(op==0||op==3) {
                if(op==3)for(unsigned i=begin;i<end;++i)expected&=!oracle[i].present;
                auto replacement=rangeRecord((begin+1)*pageSize,(end+1)*pageSize,nullptr,protection,protection|1);
                replacement.offset=round*pageSize;
                bool result=op==3?plan.Insert(table,replacement):plan.Replace(table,replacement.begin,replacement.end,&replacement);
                CHECK(result==expected);
                if(result)for(unsigned i=begin;i<end;++i)oracle[i]={true,protection,protection|1,(round+i-begin)*pageSize};
            } else if(op==1) {
                CHECK(plan.Replace(table,(begin+1)*pageSize,(end+1)*pageSize));
                for(unsigned i=begin;i<end;++i)oracle[i]={};
            } else {
                for(unsigned i=begin;i<end;++i)expected&=oracle[i].present&&!(protection&~oracle[i].maximum);
                CHECK(plan.Protect(table,(begin+1)*pageSize,(end+1)*pageSize,protection)==expected);
                if(expected)for(unsigned i=begin;i<end;++i)oracle[i].protection=protection;
            }
        }
        uint64_t previous=0;
        for(auto* node=table.head;node;node=node->next) { CHECK(node->range.begin>=previous&&node->range.begin<node->range.end&&node->range.identity);previous=node->range.end; }
        for(unsigned i=0;i<64;++i) {
            auto* actual=MappingRangeFind(table,(i+1)*pageSize+13);CHECK(bool(actual)==oracle[i].present);
            if(actual)CHECK(actual->protection==oracle[i].protection&&actual->maximumProtection==oracle[i].maximum&&
                            actual->offset+(i+1)*pageSize-actual->begin==oracle[i].offset);
        }
        unsigned hint=next()%64,size=1+next()%8;uint64_t expectedGap=0;
        for(unsigned start=hint;start+size<=64;++start) {
            bool free=true;for(unsigned i=start;i<start+size;++i)free&=!oracle[i].present;
            if(free){expectedGap=(start+1)*pageSize;break;}
        }
        CHECK(MappingRangeFindGap(table,(hint+1)*pageSize,size*pageSize,pageSize,65*pageSize)==expectedGap);
    }
    MappingRangeDestroy(table);CHECK(heapAllocations==initialHeap);
    std::puts("MAPPING RANGES PASS split/offset/reservation/gaps/permissions/OOM/oracle=1000");
}
static void rangeLookupCache() {
    const auto heap=heapAllocations.load();MappingRangeTable first{},second{};
    auto put=[](MappingRangeTable& table,uint64_t begin,uint64_t end,uint32_t protection) {
        MappingRangePlan plan;CHECK(plan.Prepare(3));auto r=rangeRecord(begin,end,nullptr,protection);
        CHECK(plan.Replace(table,begin,end,&r));
    };
    put(first,0x1000,0x9000,3);put(second,0x1000,0x9000,1);
    for (unsigned i=0;i<32;++i) {
        CHECK(MappingRangeFind(first,0x3123)->protection==3);
        CHECK(MappingRangeFind(second,0x3123)->protection==1);
    }
    { MappingRangePlan plan;CHECK(plan.Prepare(2));CHECK(plan.Protect(first,0x3000,0x5000,0)); }
    CHECK(MappingRangeFind(first,0x2fff)->protection==3);
    CHECK(MappingRangeFind(first,0x3000)->protection==0);
    CHECK(MappingRangeFind(first,0x4fff)->protection==0);
    CHECK(MappingRangeFind(first,0x5000)->protection==3);
    { MappingRangePlan plan;CHECK(plan.Prepare(0));CHECK(plan.Replace(first,0x3000,0x5000)); }
    CHECK(!MappingRangeFind(first,0x3000));CHECK(!MappingRangeFind(first,0x4fff));
    put(first,0x3000,0x5000,1);CHECK(MappingRangeFind(first,0x3123)->protection==1);
    const auto identity=MappingRangeFind(first,0x8123)->identity;
    { MappingRangePlan plan;CHECK(plan.Prepare(0));CHECK(plan.Extend(first,identity,0xb000)); }
    CHECK(MappingRangeFind(first,0xafff)->end==0xb000);CHECK(!MappingRangeFind(first,0xb000));
    MappingRangeDestroy(first);CHECK(!MappingRangeFind(first,0x3123));
    put(first,0x3000,0x4000,7);CHECK(MappingRangeFind(first,0x3123)->protection==7);
    // More distinct pages than slots guarantee collisions; neither gaps nor
    // table identity may inherit a previous page's metadata.
    MappingRangeDestroy(first);
    for (unsigned i=0;i<96;++i) put(first,(2*i+1)*pageSize,(2*i+2)*pageSize,i%8);
    for (unsigned round=0;round<4;++round) for (unsigned i=0;i<96;++i) {
        const auto begin=(2*i+1)*pageSize;
        CHECK(MappingRangeFind(first,begin+19)->protection==i%8);
        CHECK(!MappingRangeFind(first,begin+pageSize));
    }
    CHECK(MappingRangeFind(second,0x3123)->protection==1);
    MappingRangeDestroy(first);MappingRangeDestroy(second);CHECK(heapAllocations==heap);
    std::puts("MAPPING LOOKUP CACHE PASS replacement/unmap/remap/splits/extend/destroy/collisions/table-isolation");
}
static const MappingRange* linearRangeLookup(const MappingRangeTable& table,uint64_t address) {
    for (auto* node=table.head;node && node->range.begin<=address;node=node->next)
        if(address<node->range.end)return &node->range;
    return nullptr;
}
static void rangeLookupBenchmark() {
    MappingRangeTable table{};constexpr unsigned count=1658,iterations=200000;
    for(unsigned i=0;i<count;++i) {
        MappingRangePlan plan;CHECK(plan.Prepare(1));auto r=rangeRecord((2*i+1)*pageSize,(2*i+2)*pageSize,nullptr);
        CHECK(plan.Insert(table,r));
    }
    volatile uint64_t targets[8];for(unsigned i=0;i<8;++i)targets[i]=(2*(count-1-i)+1)*pageSize+13;
    for(unsigned i=0;i<8;++i)CHECK(MappingRangeFind(table,targets[i])==linearRangeLookup(table,targets[i]));
    for(unsigned workload=0;workload<3;++workload) {
        uint64_t sums[2]={};double ns[2]={};
        for(unsigned variant=0;variant<2;++variant) {
            auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<iterations;++i) {
                const uint64_t address=workload==1?(2*(i%count)+1)*pageSize+13:targets[i%8];
                if(workload==2 && i%256==0) {
                    const uint64_t begin=address&~uint64_t{4095};MappingRangePlan plan;CHECK(plan.Prepare(1));
                    auto r=rangeRecord(begin,begin+pageSize,nullptr,(i/256)%8);CHECK(plan.Replace(table,begin,begin+pageSize,&r));
                }
                auto* result=variant?MappingRangeFind(table,address):linearRangeLookup(table,address);CHECK(result);
                sums[variant]+=result->begin;
            }
            ns[variant]=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
        }
        CHECK(sums[0]==sums[1]);
        const char* name=workload==0?"hot8":workload==1?"cycle1658":"hot8_replace256";
        std::printf("MAPPING LOOKUP BENCH workload=%s ranges=%u iterations=%u linear_ns=%.2f cached_ns=%.2f speedup=%.2f checksum=%lu\n",name,count,iterations,ns[0],ns[1],ns[0]/ns[1],static_cast<unsigned long>(sums[1]));
    }
    MappingRangeDestroy(table);
}
static void rangeSnapshotRetirement() {
    auto root=table();VmmHostSetKernelRoot(root);auto* space=AddressSpaceCreate(29);CHECK(space);
    Process actor{},faultActor{};actor.addressSpace=faultActor.addressSpace=space;
    unsigned released=0;auto* backing=MappingBackingCreate(&released,rangeBackingReleased);CHECK(backing);
    {
        MappingTransaction operation(&actor,space);CHECK(operation.PrepareRanges(1));operation.Lock();
        CHECK(operation.InsertRange(rangeRecord(va.raw(),va.raw()+3*pageSize,backing)));
    }
    MappingBackingUnref(backing);CHECK(backing->references==1);
    auto original=PmmAllocPage();CHECK(VmmTryMapPage(space->pageTable,va,original,flags)==VmmMapResult::Mapped);
    Gate faultPrepared,resumeFault;
    std::thread fault([&]{
        MappingTransaction operation(&faultActor,space);CHECK(operation.Prepare(0,0,1));operation.Lock();
        MappingFaultSnapshot snapshot{};CHECK(operation.CaptureRange(va.raw(),1,snapshot));
        CHECK(operation.ValidateRange(snapshot,va.raw()+pageSize,1));
        CHECK(!operation.ValidateRange(snapshot,va.raw()+3*pageSize,1));
        operation.Unlock();faultPrepared.park();resumeFault.park();
        operation.Lock();CHECK(!operation.ValidateRange(snapshot,va.raw(),1));
        CHECK(!operation.ValidateRange(snapshot,va.raw()+pageSize,1)); // speculative install also stale
        CHECK(!released);operation.Unlock();
    });
    faultPrepared.wait(1);faultPrepared.release();resumeFault.wait(1);
    Gate ack;pendingAcknowledgement=&ack;
    std::thread replacer([&]{
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(3,0));CHECK(operation.PrepareRanges(1));operation.Lock();
        auto anon=rangeRecord(va.raw(),va.raw()+3*pageSize,nullptr,0);
        CHECK(operation.ReplaceMapping(anon.begin,anon.end,&anon));
    });
    ack.wait(1);CHECK(!released&&PmmGetRefCount(original)==1&&backing->references==2);
    CHECK(hostMutex(&space->mutation).try_lock());hostMutex(&space->mutation).unlock();
    ack.release();replacer.join();pendingAcknowledgement=nullptr;
    CHECK(!released&&backing->references==1&&!PmmGetRefCount(original));
    resumeFault.release();fault.join();CHECK(released==1);
    {
        MappingTransaction operation(&actor,space);CHECK(operation.PrepareRanges(2));operation.Lock();
        MappingFaultSnapshot inaccessible{};CHECK(!operation.CaptureRange(va.raw(),1,inaccessible));
        CHECK(operation.ProtectRange(va.raw()+pageSize,va.raw()+2*pageSize,3));
        MappingFaultSnapshot allowed{};CHECK(operation.CaptureRange(va.raw()+pageSize,2,allowed));
        CHECK(operation.ValidateRange(allowed,va.raw()+pageSize,2));
        CHECK(!operation.CaptureRange(va.raw(),1,inaccessible));
    }
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();
    CHECK(!actor.kernelRegionDepth&&!faultActor.kernelRegionDepth&&heapAllocations==0);
    std::puts("MAPPING SNAPSHOT PASS stale-primary/adjacent/backing-pin/post-ACK/reservation");
}
static void rangeLeafOwnership() {
    auto root=table();VmmHostSetKernelRoot(root);auto* space=AddressSpaceCreate(30);CHECK(space);
    Process actor{};actor.addressSpace=space;
    auto borrowed=PmmAllocPage();BackingRetirementWitness witness{space,borrowed,false,&actor};
    auto* backing=MappingBackingCreate(&witness,releaseTransactionBacking);CHECK(backing);
    auto range=rangeRecord(va.raw(),va.raw()+2*pageSize,backing);
    range.kind=MappingKind::MemFd;range.sharing=MappingSharing::Shared;range.leafOwnership=MappingLeafOwnership::Borrowed;
    {
        MappingTransaction operation(&actor,space);CHECK(operation.PrepareRanges(1));operation.Lock();CHECK(operation.InsertRange(range));
    }
    MappingBackingUnref(backing);
    // Deliberately User-tagged: descriptor ownership must override legacy tags.
    CHECK(VmmTryMapPage(space->pageTable,va,borrowed,flags,MemTag::User)==VmmMapResult::Mapped);
    auto originalId=space->ranges.head->range.identity;
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(2,0));CHECK(operation.PrepareRanges(0));operation.Lock();
        auto anon=rangeRecord(va.raw(),va.raw()+pageSize,nullptr);
        CHECK(!operation.ReplaceMapping(anon.begin,anon.end,&anon));
        CHECK(VmmVirtToPhys(space->pageTable,va)==borrowed&&space->ranges.head->range.identity==originalId);
        CHECK(backing->references==1&&!witness.released);
    }
    Gate ack;pendingAcknowledgement=&ack;
    std::thread remover([&]{
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(2,0));CHECK(operation.PrepareRanges(0));operation.Lock();
        CHECK(operation.ReplaceMapping(range.begin,range.end));
    });
    ack.wait(1);CHECK(!witness.released&&PmmGetRefCount(borrowed)==1&&maps(borrowed)==0);
    CHECK(!space->ranges.head);ack.release();remover.join();pendingAcknowledgement=nullptr;CHECK(witness.released);
    // Last-AS teardown must also understand borrowed User leaves with no entry
    // in any legacy memfd array, rather than destroying their owner's ref twice.
    auto finalBorrowed=PmmAllocPage();BackingRetirementWitness finalWitness{space,finalBorrowed};
    backing=MappingBackingCreate(&finalWitness,releaseTransactionBacking);CHECK(backing);range.backing=backing;
    range.begin=0x700040000000ULL;range.end=range.begin+2*pageSize;
    {
        MappingTransaction operation(&actor,space);CHECK(operation.PrepareRanges(1));operation.Lock();CHECK(operation.InsertRange(range));
    }
    MappingBackingUnref(backing);
    CHECK(VmmTryMapPage(space->pageTable,VirtualAddress(range.begin),finalBorrowed,flags)==VmmMapResult::Mapped);
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(0,2));CHECK(operation.PrepareZeroed(MemTag::User,30));
        CHECK(operation.PrepareRanges(1));operation.Lock();
        CHECK(operation.InsertRange(rangeRecord(va.raw()+4*pageSize,va.raw()+6*pageSize,nullptr)));
        MappingFaultSnapshot snapshot{};CHECK(operation.CaptureRange(va.raw()+4*pageSize,1,snapshot));
        CHECK(operation.Publish(0,va+4*pageSize,flags,MemTag::User,30)==VmmMapResult::Mapped);
        CHECK(operation.ValidateRange(snapshot,va.raw()+5*pageSize,1));
        CHECK(operation.Publish(1,va+5*pageSize,flags,MemTag::User,30)==VmmMapResult::Mapped);
        CHECK(operation.ValidateRange(snapshot,va.raw()+4*pageSize,1));
    }
    {
        MappingTransaction operation(&actor,space);CHECK(operation.PrepareRanges(1));operation.Lock();
        CHECK(operation.InsertRange(rangeRecord(0x10000000000ULL,0x20000000000ULL,nullptr,0)));
    }
    const uint64_t hugeBegin=0x10000000000ULL,hugeEnd=0x20000000000ULL;
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(0,2));CHECK(operation.PrepareZeroed(MemTag::User,30));
        CHECK(operation.PrepareRanges(0));operation.Lock();CHECK(operation.ProtectRange(hugeBegin,hugeEnd,3));
        CHECK(operation.Publish(0,VirtualAddress(hugeBegin),flags,MemTag::User,30)==VmmMapResult::Mapped);
        CHECK(operation.Publish(1,VirtualAddress(hugeEnd-pageSize),flags,MemTag::User,30)==VmmMapResult::Mapped);
    }
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(1,0));CHECK(operation.PrepareRanges(0));operation.Lock();
        uint64_t count=0;CHECK(operation.RangeLeafCount(hugeBegin,hugeEnd,count)&&count==2);
        auto id=MappingRangeFind(space->ranges,hugeBegin)->identity;
        CHECK(!operation.ReplaceMapping(hugeBegin,hugeEnd));
        CHECK(MappingRangeFind(space->ranges,hugeBegin)->identity==id);
        CHECK(operation.RangeLeafCount(hugeBegin,hugeEnd,count)&&count==2);
    }
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(2,0));CHECK(operation.PrepareRanges(0));operation.Lock();
        CHECK(operation.ReplaceMapping(hugeBegin,hugeEnd));
        uint64_t count=99;CHECK(operation.RangeLeafCount(hugeBegin,hugeEnd,count)&&count==0);
    }
    {
        MappingTransaction operation(&actor,space);CHECK(operation.PrepareRanges(1));operation.Lock();
        CHECK(operation.InsertRange(rangeRecord(hugeBegin,hugeEnd,nullptr,0)));
    }
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());CHECK(finalWitness.released);
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("MAPPING RANGE LEAVES PASS borrowed-user/preflight-atomic/final-teardown/sparse-1TiB/population-generation");
}
static int prepareMappingPages(MappingTransaction& operation,const UserMappingRequest&,void*) {
    return operation.PrepareZeroed(MemTag::User,31)?0:-12;
}
struct CloneBacking { PhysicalAddress page; unsigned released=0; };
static void releaseCloneBacking(void* object) {
    auto* backing=static_cast<CloneBacking*>(object);
    ++backing->released;PmmUnrefPage(backing->page);
}
static int prepareBorrowedClone(MappingTransaction& operation,const UserMappingRequest&,void* context) {
    return operation.StagePage(0,static_cast<CloneBacking*>(context)->page,false)?0:-12;
}
static void sharedMappingClone() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto* source=AddressSpaceCreate(34);auto* child=AddressSpaceCreate(35);CHECK(source&&child);
    Process actor{};actor.addressSpace=source;
    CloneBacking borrowed{PmmAllocPage()};CHECK(borrowed.page);
    auto* backing=MappingBackingCreate(&borrowed,releaseCloneBacking);CHECK(backing);
    UserMappingRequest request{};request.range=rangeRecord(0,pageSize,backing);
    request.range.kind=MappingKind::MemFd;request.range.sharing=MappingSharing::Shared;
    request.range.leafOwnership=MappingLeafOwnership::Borrowed;
    request.hint=va.raw();request.length=pageSize;request.preparedPages=1;
    request.fixed=true;request.prepare=prepareBorrowedClone;request.context=&borrowed;
    CHECK(UserMappingCreate(&actor,source,request)==static_cast<int64_t>(va.raw()));
    MappingBackingUnref(backing);
    request.range=rangeRecord(0,pageSize,nullptr);request.range.sharing=MappingSharing::Shared;
    request.hint=va.raw()+pageSize;request.prepare=prepareMappingPages;request.context=nullptr;
    CHECK(UserMappingCreate(&actor,source,request)==static_cast<int64_t>(request.hint));
    auto managed=VmmLeafPhysical(VmmReadLeaf(source->pageTable,va+pageSize));
    CHECK(UserMappingClone(&actor,source,child,35));
    CHECK(PmmGetRefCount(borrowed.page)==1&&maps(borrowed.page)==2);
    CHECK(PmmGetRefCount(managed)==2&&maps(managed)==2);
    for (auto* space:{source,child}) for (unsigned i=0;i<2;++i) {
        auto raw=VmmReadLeaf(space->pageTable,va+i*pageSize);
        CHECK((raw&VMM_WRITABLE)&&!(raw&PTE_COW_BIT));
    }
    AddressSpaceUnref(source);CHECK(ProcessCleanupStep());
    CHECK(!borrowed.released&&PmmGetRefCount(borrowed.page)==1&&maps(borrowed.page)==1);
    AddressSpaceUnref(child);CHECK(ProcessCleanupStep());CHECK(borrowed.released==1);
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("LIVE SHARED FORK PASS borrowed-descriptor/managed-shared/writable-no-CoW/last-owner-retirement");
}
static void borrowedDeviceCachePolicies() {
    for (const uint64_t cache : {uint64_t{0}, VMM_WRITE_THRU|VMM_CACHE_DISABLE, VMM_LEAF_PAT}) {
        auto root=table();VmmHostSetKernelRoot(root);
        auto* source=AddressSpaceCreate(34);auto* child=AddressSpaceCreate(35);CHECK(source&&child);
        Process actor{};actor.addressSpace=source;
        CloneBacking borrowed{PmmAllocPage()};CHECK(borrowed.page);
        auto* backing=MappingBackingCreate(&borrowed,releaseCloneBacking);CHECK(backing);
        UserMappingRequest request{};request.range=rangeRecord(0,pageSize,backing);
        request.range.kind=MappingKind::Device;request.range.sharing=MappingSharing::Shared;
        request.range.leafOwnership=MappingLeafOwnership::Borrowed;
        request.hint=va.raw();request.length=pageSize;request.preparedPages=1;
        request.fixed=true;request.prepare=prepareBorrowedClone;request.context=&borrowed;
        request.cacheFlags=VMM_USER;
        CHECK(UserMappingCreate(&actor,source,request)==-22);
        request.cacheFlags=cache;
        CHECK(UserMappingCreate(&actor,source,request)==static_cast<int64_t>(va.raw()));
        MappingBackingUnref(backing);
        CHECK((VmmReadLeaf(source->pageTable,va)&VMM_LEAF_CACHE_MASK)==cache);
        CHECK(UserMappingClone(&actor,source,child,35));
        CHECK(PmmGetRefCount(borrowed.page)==1&&maps(borrowed.page)==2);
        actor.addressSpace=child;
        CHECK(UserMappingProtect(&actor,child,va.raw(),pageSize,0)==0);
        auto raw=VmmReadLeaf(child->pageTable,va);
        CHECK(!(raw&VMM_PRESENT)&&(raw&PTE_HIDDEN_BIT)&&(raw&VMM_LEAF_CACHE_MASK)==cache);
        CHECK(UserMappingProtect(&actor,child,va.raw(),pageSize,3)==0);
        const uint64_t target=va.raw()+0x4000;
        CHECK(UserMappingResize(&actor,child,va.raw(),pageSize,pageSize,3,target,nullptr,nullptr)==static_cast<int64_t>(target));
        raw=VmmReadLeaf(child->pageTable,VirtualAddress(target));
        CHECK((raw&VMM_LEAF_CACHE_MASK)==cache&&VmmLeafPhysical(raw)==borrowed.page);
        CHECK((raw&VMM_WRITABLE)&&!(raw&PTE_COW_BIT)&&PmmGetRefCount(borrowed.page)==1);
        AddressSpaceUnref(source);CHECK(ProcessCleanupStep());CHECK(!borrowed.released);
        AddressSpaceUnref(child);CHECK(ProcessCleanupStep());CHECK(borrowed.released==1);
        PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    }
    std::puts("DEVICE CACHE PASS WB/UC/WC leaves, fork, hidden restoration, move and borrowed lifetime");
}
static void failedCloneAcknowledgement() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto* source=AddressSpaceCreate(36);auto* child=AddressSpaceCreate(37);CHECK(source&&child);
    Process actor{};actor.addressSpace=source;
    UserMappingRequest request{};request.range=rangeRecord(0,pageSize,nullptr);
    request.hint=va.raw();request.length=pageSize;request.preparedPages=1;
    request.fixed=true;request.prepare=prepareMappingPages;
    CHECK(UserMappingCreate(&actor,source,request)==static_cast<int64_t>(request.hint));
    // A different PT is required for the second leaf in the destination.
    request.hint+=0x200000;
    CHECK(UserMappingCreate(&actor,source,request)==static_cast<int64_t>(request.hint));
    auto first=VmmLeafPhysical(VmmReadLeaf(source->pageTable,va));
    Gate acknowledgement;pendingAcknowledgement=&acknowledgement;
    // Three child table pages allow the first share, then the next PT fails.
    failFrameAfter=3;std::atomic<bool> returned{false};bool cloned=true;
    std::thread worker([&]{cloned=UserMappingClone(&actor,source,child,37);returned=true;});
    acknowledgement.wait(1);CHECK(!returned);
    CHECK(VmmReadLeaf(source->pageTable,va)&PTE_COW_BIT);
    CHECK(PmmGetRefCount(first)==2&&maps(first)==2);
    acknowledgement.release();worker.join();pendingAcknowledgement=nullptr;
    CHECK(!cloned&&returned);
    AddressSpaceUnref(child);CHECK(ProcessCleanupStep());
    CHECK(PmmGetRefCount(first)==1&&maps(first)==1);
    MappingAccessTimingEnable(true);MappingAccessTimingReset();
    CHECK(UserMappingPageAccess(&actor,source,va.raw(),2,true)==UserPageAccess::Ready);
    MappingAccessTimingEnable(false);
    const auto timing=MappingAccessTimingRead();
    CHECK(timing.write.samples==1 && !timing.write.invalidClock && timing.read.samples==0);
    CHECK(VmmLeafPhysical(VmmReadLeaf(source->pageTable,va))==first);
    AddressSpaceUnref(source);CHECK(ProcessCleanupStep());
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("LIVE FORK FAILURE PASS partial-share/OOM/delayed-parent-ACK/ref-retirement/in-place-recovery");
}
static void sparseLiveRemap() {
    auto root=table();VmmHostSetKernelRoot(root);auto* space=AddressSpaceCreate(38);CHECK(space);
    Process actor{};actor.addressSpace=space;
    constexpr uint64_t begin=0x10000000000ULL,length=0x10000000000ULL,target=0x30000000000ULL;
    UserMappingRequest request{};request.range=rangeRecord(0,length,nullptr,0);
    request.hint=begin;request.length=length;request.fixed=true;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(begin));
    PhysicalAddress first,last;
    {
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(0,2));
        CHECK(operation.PrepareZeroed(MemTag::User,38));
        first=operation.PreparedPage(0);last=operation.PreparedPage(1);operation.Lock();
        CHECK(operation.Publish(0,VirtualAddress(begin),VMM_USER|VMM_NO_EXEC,MemTag::User,38,false)==VmmMapResult::Mapped);
        CHECK(operation.Publish(1,VirtualAddress(begin+length-pageSize),VMM_USER|VMM_NO_EXEC,MemTag::User,38,false)==VmmMapResult::Mapped);
    }
    request.range=rangeRecord(0,pageSize,nullptr);request.hint=target;request.length=pageSize;
    request.preparedPages=1;request.prepare=prepareMappingPages;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(target));
    auto displaced=VmmLeafPhysical(VmmReadLeaf(space->pageTable,VirtualAddress(target)));
    failFrameAfter=0;
    CHECK(UserMappingResize(&actor,space,begin,length,length,3,target,nullptr,nullptr)==-12);
    CHECK(VmmLeafPhysical(VmmReadLeaf(space->pageTable,VirtualAddress(target)))==displaced);
    CHECK(VmmLeafPhysical(VmmReadLeaf(space->pageTable,VirtualAddress(begin)))==first);
    CHECK(UserMappingResize(&actor,space,begin,length,length,3,target,nullptr,nullptr)==static_cast<int64_t>(target));
    CHECK(!MappingRangeFind(space->ranges,begin));
    CHECK(!VmmReadLeaf(space->pageTable,VirtualAddress(begin)));
    CHECK(!VmmReadLeaf(space->pageTable,VirtualAddress(begin+length-pageSize)));
    CHECK(VmmLeafPhysical(VmmReadLeaf(space->pageTable,VirtualAddress(target)))==first);
    CHECK(VmmLeafPhysical(VmmReadLeaf(space->pageTable,VirtualAddress(target+length-pageSize)))==last);
    CHECK(VmmReadLeaf(space->pageTable,VirtualAddress(target))&PTE_HIDDEN_BIT);
    CHECK(PmmGetRefCount(first)==1&&maps(first)==1&&PmmGetRefCount(last)==1&&maps(last)==1);
    CHECK(!PmmGetRefCount(displaced)&&!maps(displaced));
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("LIVE SPARSE REMAP PASS 1TiB/two-hidden-leaves/fixed-destination/OOM-atomic/refcounts");
}
static void pageableTableSkeleton() {
    auto root=table();VmmHostSetKernelRoot(root);auto* space=AddressSpaceCreate(38);CHECK(space);
    Process actor{};actor.addressSpace=space;
    constexpr uint64_t begin=0x50000000ULL,length=4*1024*1024;
    auto* backing=MappingBackingCreate(&actor,releaseTestNoop);CHECK(backing);
    UserMappingRequest request{};request.range=rangeRecord(0,length,backing);
    request.range.kind=MappingKind::Pageable;request.hint=begin;request.length=length;
    request.fixed=true;request.noReplace=true;
    // Metadata-only dedicated region still prepares every intermediate table.
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(begin));
    auto first=PmmAllocPage(),last=PmmAllocPage();failAllocation=true;
    CHECK(VmmTryMapPage(space->pageTable,VirtualAddress(begin),first,VMM_USER|VMM_WRITABLE|VMM_NO_EXEC,MemTag::User,38)==VmmMapResult::Mapped);
    CHECK(VmmTryMapPage(space->pageTable,VirtualAddress(begin+length-pageSize),last,VMM_USER|VMM_WRITABLE|VMM_NO_EXEC,MemTag::User,38)==VmmMapResult::Mapped);
    CHECK(failAllocation.exchange(false)); // neither install called the allocator
    request.hint=0x10000000000ULL;failAllocation=true;
    CHECK(UserMappingCreate(&actor,space,request)==-12);
    CHECK(!MappingRangeFind(space->ranges,request.hint));
    CHECK(!failAllocation.load());
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());MappingBackingUnref(backing);
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("PAGEABLE TABLES PASS lazy-preparation/allocator-denied-install/OOM-no-publication");
}
static void low32Mappings() {
    auto root=table();VmmHostSetKernelRoot(root);auto* space=AddressSpaceCreate(38);CHECK(space);
    Process actor{};actor.addressSpace=space;
    constexpr uint64_t cursor=0x600001000000ULL,limit=0x80000000ULL;
    space->mmapNext=cursor;
    UserMappingRequest request{};request.range=rangeRecord(0,pageSize,nullptr);
    request.length=pageSize;request.low32=true;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(USER_MMAP_BASE));
    CHECK(space->mmapNext==cursor);
    request.hint=USER_MMAP_BASE+4*pageSize;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(request.hint));
    request.hint=0x100000000ULL; // An above-band hint cannot defeat the bound.
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(USER_MMAP_BASE+pageSize));
    CHECK(space->mmapNext==cursor);
    request.length=limit+pageSize;
    CHECK(UserMappingCreate(&actor,space,request)==-12);
    CHECK(UserMappingRemove(&actor,space,USER_MMAP_BASE,5*pageSize)==0);
    // Fill the entire low range with one lazy reservation. Exhaustion must not
    // fall through to the high ordinary cursor or allocate physical pages.
    request.length=limit-USER_MMAP_BASE;request.hint=USER_MMAP_BASE;
    request.range=rangeRecord(0,request.length,nullptr,0);request.fixed=true;request.noReplace=true;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(USER_MMAP_BASE));
    request.fixed=false;request.noReplace=false;request.hint=0;request.length=pageSize;
    request.range=rangeRecord(0,pageSize,nullptr);
    CHECK(UserMappingCreate(&actor,space,request)==-12);
    CHECK(space->mmapNext==cursor);
    // Exact mappings ignore the low constraint, and NOREPLACE still protects
    // an existing mapping. Ordinary allocation continues at its prior cursor.
    request.fixed=true;request.noReplace=true;request.hint=0x200000000ULL;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(request.hint));
    CHECK(UserMappingCreate(&actor,space,request)==-17);
    request.fixed=false;request.noReplace=false;request.hint=0;request.low32=false;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(cursor));
    CHECK(space->mmapNext==cursor+pageSize);
    CHECK(UserMappingRemove(&actor,space,USER_MMAP_BASE,limit-USER_MMAP_BASE)==0);
    request.low32=true;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(USER_MMAP_BASE));
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("MAP32 PASS high-cursor/hints/exhaustion/fixed/NOREPLACE/reuse");
}
static void exactAddressNoReplace() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto* space=AddressSpaceCreate(39);CHECK(space);
    Process actor{};actor.addressSpace=space;
    constexpr uint64_t high=0x7f8800000000ULL;
    UserMappingRequest request{};request.range=rangeRecord(0,3*pageSize,nullptr);
    request.hint=high;request.length=3*pageSize;request.fixed=true;request.noReplace=true;
    request.preparedPages=3;request.prepare=prepareMappingPages;
    const auto oldNext=space->mmapNext;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(high));
    CHECK(space->mmapNext==oldNext);
    const auto first=VmmReadLeaf(space->pageTable,VirtualAddress(high));
    auto* bytes=reinterpret_cast<unsigned char*>(VmmLeafPhysical(first).raw());bytes[7]=0x63;
    const auto generation=space->generation;
    for (auto [begin,length]: {std::pair{high,3*pageSize},std::pair{high+pageSize,pageSize},
                              std::pair{high-pageSize,2*pageSize},std::pair{high+2*pageSize,2*pageSize},
                              std::pair{high-pageSize,5*pageSize}}) {
        request.hint=begin;request.length=length;request.range.end=length;request.preparedPages=length/pageSize;
        CHECK(UserMappingCreate(&actor,space,request)==-17);
        CHECK(space->generation==generation&&bytes[7]==0x63);
        CHECK(VmmReadLeaf(space->pageTable,VirtualAddress(high))==first);
    }
    request.hint=high+1;request.length=pageSize;request.range.end=pageSize;request.preparedPages=1;
    CHECK(UserMappingCreate(&actor,space,request)==-22);
    request.hint=high+pageSize;
    CHECK(UserMappingRemove(&actor,space,request.hint,pageSize)==0);
    request.range.protection=0;request.preparedPages=0;request.prepare=nullptr;
    CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(request.hint));
    CHECK(UserMappingCreate(&actor,space,request)==-17); // Metadata-only PROT_NONE is occupied.
    CHECK(UserMappingRemove(&actor,space,high,3*pageSize)==0);
    // A competing creator wins while preparation is unlocked. Generation
    // revalidation must retry and report EEXIST, preserving the winner.
    Gate preparing;
    request.range.protection=3;request.hint=high;
    request.prepare=[](MappingTransaction&,const UserMappingRequest&,void* context) {
        static_cast<Gate*>(context)->park();return 0;
    };
    request.context=&preparing;
    int64_t result=0;
    std::thread worker([&]{result=UserMappingCreate(&actor,space,request);});
    preparing.wait(1);
    auto winner=request;winner.prepare=nullptr;winner.context=nullptr;
    CHECK(UserMappingCreate(&actor,space,winner)==static_cast<int64_t>(high));
    const auto winnerIdentity=MappingRangeFind(space->ranges,high)->identity;
    preparing.release();worker.join();CHECK(result==-17);
    CHECK(MappingRangeFind(space->ranges,high)->identity==winnerIdentity);
    winner.hint=0x0000800000000000ULL-pageSize;
    CHECK(UserMappingCreate(&actor,space,winner)==static_cast<int64_t>(winner.hint));
    winner.length=2*pageSize;winner.range.end=winner.length;
    CHECK(UserMappingCreate(&actor,space,winner)==-22); // Cannot cross the user boundary.
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("EXACT ADDRESS NOREPLACE PASS high-address/overlap/preservation/PROT_NONE/commit-race/cleanup");
}
static void liveMappingClone() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto* source=AddressSpaceCreate(31);auto* child=AddressSpaceCreate(32);CHECK(source&&child);
    Process actor{};actor.addressSpace=source;
    UserMappingRequest request{};
    request.range=rangeRecord(0,pageSize,nullptr);
    request.hint=va.raw();request.length=3*pageSize;request.preparedPages=3;
    request.fixed=true;request.prepare=prepareMappingPages;
    CHECK(UserMappingCreate(&actor,source,request)==static_cast<int64_t>(va.raw()));
    auto original=VmmLeafPhysical(VmmReadLeaf(source->pageTable,va));
    std::memset(reinterpret_cast<void*>(original.raw()),0x73,pageSize);
    CHECK(UserMappingProtect(&actor,source,va.raw()+pageSize,pageSize,0)==0);
    auto hidden=VmmReadLeaf(source->pageTable,va+pageSize);
    CHECK((hidden&PTE_HIDDEN_BIT)&&!(hidden&VMM_PRESENT));
    auto hiddenPhysical=VmmLeafPhysical(hidden);
    source->argumentStart=va.raw()+pageSize-3;
    source->argumentEnd=source->argumentStart+7;
    std::memcpy(reinterpret_cast<unsigned char*>(original.raw())+pageSize-3,"A\0B",3);
    std::memcpy(reinterpret_cast<void*>(hiddenPhysical.raw()),"\0C\0\0",4);
    char argumentBytes[8]{};
    CHECK(UserMappingReadArguments(source,0,argumentBytes,sizeof(argumentBytes))==0);
    CHECK(UserMappingProtect(&actor,source,va.raw()+pageSize,pageSize,1)==0);
    CHECK(UserMappingReadArguments(source,0,argumentBytes,sizeof(argumentBytes))==7);
    CHECK(!std::memcmp(argumentBytes,"A\0B\0C\0\0",7));
    CHECK(UserMappingReadArguments(source,2,argumentBytes,3)==3);
    CHECK(!std::memcmp(argumentBytes,"B\0C",3));
    CHECK(UserMappingReadArguments(source,7,argumentBytes,sizeof(argumentBytes))==0);
    CHECK(UserMappingProtect(&actor,source,va.raw()+pageSize,pageSize,0)==0);
    CHECK(UserMappingProtect(&actor,source,va.raw()+2*pageSize,pageSize,1)==0);
    CHECK(UserMappingClone(&actor,source,child,32));
    CHECK(child->argumentStart==source->argumentStart && child->argumentEnd==source->argumentEnd);
    CHECK(UserMappingReadArguments(child,0,argumentBytes,sizeof(argumentBytes))==0);
    CHECK(PmmGetRefCount(original)==2&&maps(original)==2);
    auto parentRaw=VmmReadLeaf(source->pageTable,va),childRaw=VmmReadLeaf(child->pageTable,va);
    CHECK((parentRaw&PTE_COW_BIT)&&!(parentRaw&VMM_WRITABLE));
    CHECK(VmmLeafPhysical(childRaw)==original&&(childRaw&PTE_COW_BIT));
    CHECK(VmmLeafPhysical(VmmReadLeaf(child->pageTable,va+pageSize))==hiddenPhysical);
    CHECK(VmmReadLeaf(child->pageTable,va+pageSize)&PTE_HIDDEN_BIT);
    CHECK(UserMappingPageAccess(&actor,source,va.raw()+2*pageSize,2,true)==UserPageAccess::Denied);
    CHECK(UserMappingProtect(&actor,child,va.raw()+pageSize,pageSize,3)==0);
    CHECK(VmmReadLeaf(child->pageTable,va+pageSize)&PTE_COW_BIT);
    CHECK(UserMappingPageAccess(&actor,source,va.raw(),2,true)==UserPageAccess::Ready);
    auto privateCopy=VmmLeafPhysical(VmmReadLeaf(source->pageTable,va));
    CHECK(privateCopy!=original&&*reinterpret_cast<unsigned char*>(privateCopy.raw())==0x73);
    reinterpret_cast<unsigned char*>(privateCopy.raw())[pageSize-1]='D';
    CHECK(UserMappingProtect(&actor,source,va.raw()+pageSize,pageSize,1)==0);
    CHECK(UserMappingReadArguments(source,0,argumentBytes,3)==3);
    CHECK(!std::memcmp(argumentBytes,"A\0D",3));
    CHECK(UserMappingReadArguments(child,0,argumentBytes,3)==3);
    CHECK(!std::memcmp(argumentBytes,"A\0B",3));
    CHECK(PmmGetRefCount(original)==1&&maps(original)==1);
    // Moving a hidden range preserves its bytes, reference and accessibility.
    CHECK(UserMappingProtect(&actor,child,va.raw(),pageSize,0)==0);
    uint64_t target=va.raw()+16*pageSize;
    CHECK(UserMappingResize(&actor,child,va.raw(),pageSize,pageSize,3,target,nullptr,nullptr)==static_cast<int64_t>(target));
    CHECK(!VmmReadLeaf(child->pageTable,va));
    CHECK(VmmLeafPhysical(VmmReadLeaf(child->pageTable,VirtualAddress(target)))==original);
    CHECK(VmmReadLeaf(child->pageTable,VirtualAddress(target))&PTE_HIDDEN_BIT);
    CHECK(UserMappingProtect(&actor,child,target,pageSize,1)==0);
    CHECK(!(VmmReadLeaf(child->pageTable,VirtualAddress(target))&PTE_COW_BIT));
    CHECK(*reinterpret_cast<unsigned char*>(original.raw())==0x73);
    // Allocation failure before clone publication leaves parent access intact.
    auto* failed=AddressSpaceCreate(33);CHECK(failed);
    auto before=VmmReadLeaf(source->pageTable,va);failHeapAfter=0;
    CHECK(!UserMappingClone(&actor,source,failed,33));
    CHECK(VmmReadLeaf(source->pageTable,va)==before&&!failed->ranges.head);
    AddressSpaceUnref(failed);CHECK(ProcessCleanupStep());
    AddressSpaceUnref(child);CHECK(ProcessCleanupStep());
    AddressSpaceUnref(source);CHECK(ProcessCleanupStep());
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("LIVE MAPPING PASS create/protect/hidden-fork/permission-checked-CoW/hidden-remap/clone-OOM");
}
static void preparedThreadStack() {
    auto root=table();VmmHostSetKernelRoot(root);auto* space=AddressSpaceCreate(39);CHECK(space);
    Process actor{};actor.addressSpace=space;
    unsigned char arguments[9001];for(unsigned i=0;i<sizeof(arguments);++i) arguments[i]=static_cast<unsigned char>(i*37);
    uint64_t argumentAddress=123;
    CHECK(UserMappingStack(&actor,space,32768,nullptr,sizeof(arguments),&argumentAddress)==-22);
    CHECK(!space->ranges.head&&argumentAddress==123);
    failFrameAfter=3;
    CHECK(UserMappingStack(&actor,space,32768,arguments,sizeof(arguments),&argumentAddress)==-12);
    CHECK(!space->ranges.head&&argumentAddress==123);
    auto base=UserMappingStack(&actor,space,32768,arguments,sizeof(arguments),&argumentAddress);CHECK(base>0);
    CHECK(argumentAddress%16==0&&argumentAddress>=static_cast<uint64_t>(base));
    auto* range=MappingRangeFind(space->ranges,base);CHECK(range&&range->kind==MappingKind::Stack&&range->protection==3);
    CHECK(range->end-range->begin==32768&&range->maximumProtection==3);
    for (uint64_t offset=0;offset<32768;++offset) {
        uint64_t address=static_cast<uint64_t>(base)+offset;
        auto raw=VmmReadLeaf(space->pageTable,VirtualAddress(address&~uint64_t{4095}));
        CHECK((raw&VMM_NO_EXEC)&&(raw&VMM_WRITABLE));
        auto physical=VmmLeafPhysical(raw);
        unsigned char actual=*reinterpret_cast<unsigned char*>(physical.raw()+(address&4095));
        unsigned char expected=address>=argumentAddress&&address-argumentAddress<sizeof(arguments)?arguments[address-argumentAddress]:0;
        CHECK(actual==expected);
    }
    CHECK(UserMappingProtect(&actor,space,base,32768,7)==-13);
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("LIVE THREAD STACK PASS prepared-cross-page-arguments/zero-padding/NX/OOM-no-publication");
}
static void graphicsBufferMappings() {
    auto root=table();VmmHostSetKernelRoot(root);
    constexpr uint64_t kernelBase=0x200000;
    PhysicalAddress pages[3];
    for (unsigned i=0;i<3;++i) {
        pages[i]=PmmAllocPage();CHECK(pages[i]);
        CHECK(VmmTryMapPage(KernelPageTable,VirtualAddress(kernelBase+i*pageSize),pages[i],VMM_WRITABLE,MemTag::User)==VmmMapResult::Mapped);
    }
    failHeapAfter=1;CHECK(!UserMappingBufferSnapshot(kernelBase,3*pageSize));
    for (auto page:pages) CHECK(PmmGetRefCount(page)==1);
    auto* descriptor=UserMappingBufferSnapshot(kernelBase,3*pageSize);CHECK(descriptor);
    auto* space=AddressSpaceCreate(40);auto* child=AddressSpaceCreate(41);CHECK(space&&child);
    Process actor{};actor.addressSpace=space;KMutexInit(&actor.framebufferLock);
    UserFramebufferPublish(&actor,descriptor,reinterpret_cast<uint32_t*>(kernelBase),3*pageSize,64,48,64);
    CHECK(actor.fbBacking==descriptor&&actor.fbVirtualSize==3*pageSize&&actor.fbVfbStride==64);
    // Simulate mmap retaining its source before concurrent terminal clear.
    KMutexLock(&actor.framebufferLock);
    auto* captured=actor.fbBacking;MappingBackingRef(captured);
    KMutexUnlock(&actor.framebufferLock);
    UserFramebufferPublish(&actor,nullptr,nullptr,0,0,0,0);
    CHECK(!actor.fbBacking&&!actor.fbVirtual&&!actor.fbVirtualSize);
    failHeapAfter=0;CHECK(UserMappingBufferAlias(&actor,space,descriptor,3)==-12);
    CHECK(!space->ranges.head);
    auto address=UserMappingBufferAlias(&actor,space,descriptor,3);CHECK(address>0);
    CHECK(UserMappingClone(&actor,space,child,41));
    for (unsigned i=0;i<3;++i) {
        CHECK(PmmGetRefCount(pages[i])==4&&maps(pages[i])==2);
        auto raw=VmmReadLeaf(child->pageTable,VirtualAddress(address+i*pageSize));
        CHECK((raw&VMM_WRITABLE)&&!(raw&PTE_COW_BIT));
        auto detached=VmmDetachPage(KernelPageTable,VirtualAddress(kernelBase+i*pageSize));
        CHECK(VmmLeafPhysical(detached)==pages[i]);PmmUnrefPage(pages[i]);
    }
    // Preparation after source unpublication and kernel VA removal uses only
    // retained physical snapshot pages, including a nonzero buffer offset.
    UserMappingRequest slice{};slice.range=rangeRecord(0,pageSize,nullptr,1,3);
    slice.range.offset=pageSize;slice.length=pageSize;
    CHECK(UserMappingConfigureBuffer(slice,captured));
    auto sliceAddress=UserMappingCreate(&actor,space,slice);CHECK(sliceAddress>0);
    CHECK(VmmLeafPhysical(VmmReadLeaf(space->pageTable,VirtualAddress(sliceAddress)))==pages[1]);
    CHECK(!(VmmReadLeaf(space->pageTable,VirtualAddress(sliceAddress))&VMM_WRITABLE));
    MappingBackingUnref(captured);
    // Replace the middle page; protect and move the surviving owned pieces.
    UserMappingRequest replacement{};replacement.range=rangeRecord(0,pageSize,nullptr);
    replacement.hint=address+pageSize;replacement.length=pageSize;replacement.preparedPages=1;
    replacement.fixed=true;replacement.prepare=prepareMappingPages;
    CHECK(UserMappingCreate(&actor,space,replacement)==static_cast<int64_t>(replacement.hint));
    auto newPage=VmmLeafPhysical(VmmReadLeaf(space->pageTable,VirtualAddress(replacement.hint)));
    CHECK(UserMappingProtect(&actor,space,address+2*pageSize,pageSize,0)==0);
    auto moved=address+16*pageSize;
    CHECK(UserMappingResize(&actor,space,address,pageSize,pageSize,3,moved,nullptr,nullptr)==static_cast<int64_t>(moved));
    // Native backing removal must not depend on heap allocation at exit.
    failHeapAfter=0;
    Gate acknowledgement;pendingAcknowledgement=&acknowledgement;std::atomic<bool> returned{false};
    std::thread worker([&]{CHECK(UserMappingRemoveBacking(&actor,space,descriptor)==0);returned=true;});
    acknowledgement.wait(1);CHECK(!returned);
    // Detached managed refs are still held until the ACK, including hidden leaves.
    CHECK(PmmGetRefCount(pages[0])==3&&PmmGetRefCount(pages[2])==3);
    acknowledgement.release();worker.join();pendingAcknowledgement=nullptr;
    CHECK(failHeapAfter==0);failHeapAfter=-1;
    CHECK(!VmmReadLeaf(space->pageTable,VirtualAddress(moved)));
    CHECK(!VmmReadLeaf(space->pageTable,VirtualAddress(address+2*pageSize)));
    CHECK(VmmLeafPhysical(VmmReadLeaf(space->pageTable,VirtualAddress(replacement.hint)))==newPage);
    CHECK(UserMappingRemoveBacking(&actor,space,descriptor)==0); // idempotent
    MappingBackingUnref(descriptor);
    for (auto page:pages) CHECK(PmmGetRefCount(page)==2&&maps(page)==1);
    AddressSpaceUnref(child);CHECK(ProcessCleanupStep());
    for (auto page:pages) CHECK(!PmmGetRefCount(page)&&!maps(page));
    CHECK(PmmGetRefCount(newPage)==1&&maps(newPage)==1);
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("LIVE GRAPHICS MAPPINGS PASS source-retirement/fork-shared/fixed-replacement/protect-remap/descriptor-ACK/OOM");
}
static void fileMappingRegistrations() {
    auto root=table();VmmHostSetKernelRoot(root);AddressSpaceKernelInit();
    auto* source=AddressSpaceCreate(41);Process actor{};actor.addressSpace=source;
    FileObject file{};FileObjectInit(&file);unsigned released=0;
    auto* backing=MappingBackingCreate(&released,rangeBackingReleased);CHECK(backing);backing->file=&file;
    UserMappingRequest request{};request.range=rangeRecord(0,3*pageSize,backing,3,3);
    request.hint=va.raw();request.length=3*pageSize;request.fixed=true;
    request.preparedPages=3;request.prepare=prepareMappingPages;
    CHECK(UserMappingCreate(&actor,source,request)==static_cast<int64_t>(va.raw()));
    CHECK(FileMappingRegisteredRanges(&file)==1);
    CHECK(UserMappingProtect(&actor,source,va.raw()+pageSize,pageSize,0)==0);
    CHECK(FileMappingRegisteredRanges(&file)==3);
    auto* child=AddressSpaceCreate(42);CHECK(UserMappingClone(&actor,source,child,42));
    CHECK(FileMappingRegisteredRanges(&file)==6);
    KMutexLock(&file.gate);
    auto* first=FileMappingNextSpace(&file,0);CHECK(first==source);
    auto* second=FileMappingNextSpace(&file,first->identity);CHECK(second==child);
    CHECK(!FileMappingNextSpace(&file,second->identity));
    KMutexUnlock(&file.gate);AddressSpaceUnref(first);AddressSpaceUnref(second);
    // Failed fork leaves only an unpublished, internally registered AS until
    // caller cleanup, and zero-reference enumeration must not resurrect it.
    auto* failed=AddressSpaceCreate(43);failAllocation=true;
    CHECK(!UserMappingClone(&actor,source,failed,43));
    CHECK(FileMappingRegisteredRanges(&file)==9);
    AddressSpaceUnref(failed);CHECK(!AddressSpaceTryRef(failed));
    CHECK(FileMappingRetiringRanges(&file)==3);
    KMutexLock(&file.gate);CHECK(ProcessCleanupStep());KMutexUnlock(&file.gate);
    CHECK(FileMappingRegisteredRanges(&file)==6);
    Gate acknowledgement;pendingAcknowledgement=&acknowledgement;
    std::thread remover([&]{CHECK(UserMappingRemove(&actor,source,va.raw(),3*pageSize)==0);});
    acknowledgement.wait(1);
    CHECK(!source->ranges.head && FileMappingRegisteredRanges(&file)==6);
    CHECK(FileMappingRetiringRanges(&file)==3);
    // While metadata is gone, retired translations remain discoverable until
    // ACK. Completion can unlink even while a truncator owns the file gate.
    KMutexLock(&file.gate);first=FileMappingNextSpace(&file,0);CHECK(first==source);AddressSpaceUnref(first);
    acknowledgement.release();remover.join();KMutexUnlock(&file.gate);pendingAcknowledgement=nullptr;
    CHECK(FileMappingRegisteredRanges(&file)==3 && !FileMappingRetiringRanges(&file));
    AddressSpaceUnref(source);CHECK(ProcessCleanupStep());
    AddressSpaceUnref(child);CHECK(!AddressSpaceTryRef(child));
    KMutexLock(&file.gate);CHECK(!FileMappingNextSpace(&file,0));CHECK(FileMappingRetiringRanges(&file)==3);
    CHECK(ProcessCleanupStep());KMutexUnlock(&file.gate);
    CHECK(!FileMappingRegisteredRanges(&file));MappingBackingUnref(backing);CHECK(released==1);
    FileObjectDispose(&file);
    VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("FILE REGISTRY PASS split/fork/failed-fork/ref-zero/retired-ACK/unlink-under-file-gate");
}
static void fileMappingGateOom() {
    auto root=table();VmmHostSetKernelRoot(root);AddressSpaceKernelInit();
    auto* space=AddressSpaceCreate(44);Process actor{};actor.addressSpace=space;
    FileObject files[10]{};MappingBacking* backings[10]{};unsigned released=0;
    for(unsigned i=0;i<10;++i) {
        FileObjectInit(&files[i]);backings[i]=MappingBackingCreate(&released,rangeBackingReleased);CHECK(backings[i]);backings[i]->file=&files[i];
        UserMappingRequest request{};request.range=rangeRecord(0,pageSize,backings[i]);request.length=pageSize;
        request.hint=va.raw()+2*i*pageSize;request.fixed=true;
        CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(request.hint));
    }
    const auto generation=space->generation;const int allocated=heapAllocations.load();
    { MappingTransaction operation(&actor,space);failHeapAfter=0;CHECK(!operation.Lock());failHeapAfter=-1; }
    CHECK(space->generation==generation && heapAllocations==allocated);
    space->programBreak=0x400000;failHeapAfter=0;
    CHECK(UserMappingBreak(&actor,space,0)==0x400000);failHeapAfter=-1;
    for(auto& file:files)CHECK(FileMappingRegisteredRanges(&file)==1);
    // Multiple descriptors and many split-like aliases of the same files must
    // acquire each nonrecursive gate once and retain exactly one descriptor.
    MappingBacking* aliases[10]{};
    for(unsigned i=0;i<10;++i) {
        aliases[i]=MappingBackingCreate(&released,rangeBackingReleased);CHECK(aliases[i]);aliases[i]->file=&files[i];
    }
    for(unsigned round=0;round<8;++round)for(unsigned reverse=0;reverse<10;++reverse) {
        unsigned i=9-reverse;UserMappingRequest request{};
        request.range=rangeRecord(0,pageSize,aliases[i]);request.length=pageSize;
        request.hint=va.raw()+0x200000+(round*10+reverse)*2*pageSize;request.fixed=true;
        CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(request.hint));
    }
    uint32_t before[10];for(unsigned i=0;i<10;++i)before[i]=backings[i]->references+aliases[i]->references;
    FileMappingGateSet gates;
    for(unsigned repeat=0;repeat<3;++repeat) {
        CHECK(gates.Lock(space,aliases[0],nullptr));
        for(unsigned i=0;i<10;++i)CHECK(backings[i]->references+aliases[i]->references==before[i]+1);
        gates.Unlock();
        for(unsigned i=0;i<10;++i)CHECK(backings[i]->references+aliases[i]->references==before[i]);
        // Reuse dynamic storage for one target file without allocating, even
        // though its many aliases outnumber the original inline pin capacity.
        failHeapAfter=0;CHECK(gates.Lock(space,nullptr,aliases[0]));
        CHECK(failHeapAfter==0);failHeapAfter=-1;gates.Unlock();
    }
    // Reuse the dynamic/hash-backed full-space snapshot for a single target.
    // Its old hash slots must not suppress the new target descriptor pin.
    {
        failHeapAfter=0;CHECK(gates.LockAddress(space,va.raw()));
        CHECK(backings[0]->references+aliases[0]->references==before[0]+1);
        for(unsigned i=1;i<10;++i)CHECK(backings[i]->references+aliases[i]->references==before[i]);
        CHECK(failHeapAfter==0);failHeapAfter=-1;gates.Unlock();
    }
    gates.Finish();
    for(unsigned i=0;i<10;++i)CHECK(backings[i]->references+aliases[i]->references==before[i]);
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    for(unsigned i=0;i<10;++i){CHECK(!FileMappingRegisteredRanges(&files[i]));MappingBackingUnref(backings[i]);MappingBackingUnref(aliases[i]);FileObjectDispose(&files[i]);}
    CHECK(released==20);VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("FILE GATES PASS dynamic-capacity/OOM-unchanged/brk-contract/no-reference-leaks");
}
static void addressFileGateScope() {
    auto root=table();VmmHostSetKernelRoot(root);AddressSpaceKernelInit();
    auto* space=AddressSpaceCreate(52);Process actor{};actor.addressSpace=space;
    FileObject target{},unrelated{};FileObjectInit(&target);FileObjectInit(&unrelated);
    unsigned released=0;
    auto* backing=MappingBackingCreate(&released,rangeBackingReleased);
    auto* other=MappingBackingCreate(&released,rangeBackingReleased);
    CHECK(backing && other);backing->file=&target;other->file=&unrelated;
    {
        MappingTransaction operation(&actor,space,backing);CHECK(operation.PrepareRanges(2)&&operation.Lock());
        CHECK(operation.InsertRange(rangeRecord(va.raw(),va.raw()+pageSize,backing)));
        CHECK(operation.InsertRange(rangeRecord(va.raw()+2*pageSize,va.raw()+3*pageSize,nullptr)));
    }
    {
        MappingTransaction operation(&actor,space,other);CHECK(operation.PrepareRanges(1)&&operation.Lock());
        CHECK(operation.InsertRange(rangeRecord(va.raw()+4*pageSize,va.raw()+5*pageSize,other)));
    }
    const auto generation=space->generation;
    const auto targetRefs=backing->references,otherRefs=other->references;
    Gate acquired;
    std::thread holder([&] {
        FileMappingGateSet gates;CHECK(gates.LockAddress(space,va.raw()));
        acquired.park();gates.Unlock();
    });
    acquired.wait(1);
    const bool targetFree=hostMutex(&target.gate).try_lock();
    if(targetFree)hostMutex(&target.gate).unlock();
    const bool unrelatedFree=hostMutex(&unrelated.gate).try_lock();
    if(unrelatedFree)hostMutex(&unrelated.gate).unlock();
    acquired.release();holder.join();CHECK(!targetFree && unrelatedFree);
    std::mutex completionMutex;std::condition_variable completion;bool done=false;
    KMutexLock(&unrelated.gate);failHeapAfter=0;
    std::thread reader([&] {
        FileMappingGateSet gates;
        for(auto address:{va.raw(),va.raw()+2*pageSize,va.raw()+8*pageSize}) {
            CHECK(gates.LockAddress(space,address));
            CHECK(backing->references==targetRefs+(address==va.raw()?1:0));
            CHECK(other->references==otherRefs);gates.Unlock();
        }
        std::lock_guard<std::mutex> lock(completionMutex);done=true;completion.notify_all();
    });
    bool completedWhileHeld;
    {
        std::unique_lock<std::mutex> lock(completionMutex);
        completedWhileHeld=completion.wait_for(lock,std::chrono::seconds(2),[&]{return done;});
    }
    KMutexUnlock(&unrelated.gate);reader.join();
    const bool noHeapAttempt=failHeapAfter==0;failHeapAfter=-1;
    CHECK(completedWhileHeld && noHeapAttempt && space->generation==generation);
    CHECK(backing->references==targetRefs && other->references==otherRefs);
    // Pause after retaining the old target and dropping AS mutation, before
    // acquiring its file gate. A concurrent replacement must force retry and
    // acquire/pin the new range instead of returning with the old gate.
    Gate paused;pageAccessPauseMutex=&target.gate;pageAccessPause=&paused;
    std::atomic<bool> finished{false};
    std::thread racing([&] {
        pageAccessPauseOnce=true;FileMappingGateSet gates;
        CHECK(gates.LockAddress(space,va.raw()));
        CHECK(MappingRangeFind(space->ranges,va.raw())->backing==other);
        CHECK(released==1 && other->references==otherRefs+2);
        gates.Unlock();finished=true;
    });
    paused.wait(1);
    // Once the old VMA retires, only the acquisition pin owns this descriptor.
    MappingBackingUnref(backing);backing=nullptr;
    {
        MappingTransaction operation(&actor,space,other);CHECK(operation.PrepareRanges(1)&&operation.Lock());
        auto replacement=rangeRecord(va.raw(),va.raw()+pageSize,other);
        CHECK(operation.ReplaceMapping(va.raw(),va.raw()+pageSize,&replacement));
    }
    CHECK(!finished);paused.release();racing.join();pageAccessPauseMutex=nullptr;pageAccessPause=nullptr;
    CHECK(released==1 && other->references==otherRefs+1);
    {
        MappingTransaction operation(&actor,space);MappingFaultSnapshot snapshot{};
        CHECK(operation.LockFaultRange(va.raw()));
        CHECK(operation.CaptureRange(va.raw(),1,snapshot));
        MappingFaultSnapshot unrelatedSnapshot{};
        CHECK(!operation.CaptureRange(va.raw()+4*pageSize,1,unrelatedSnapshot));
        operation.Unlock();CHECK(operation.Prepare(0,1,1));
        CHECK(operation.PrepareZeroed(MemTag::User,52));
        CHECK(operation.LockFaultRange(va.raw()));CHECK(operation.ValidateRange(snapshot,va.raw(),1));
        CHECK(operation.PublishFault(snapshot,0,va,1,false)==MappingFaultCommit::Mapped);
        CHECK(operation.PublishFault(snapshot,0,va+4*pageSize,1,false)==MappingFaultCommit::Stale);
    }
    CHECK(VmmReadLeaf(space->pageTable,va)&VMM_PRESENT);
    // File-content generation changes and topology changes both invalidate
    // staged snapshots. A missing range cannot validate or publish a leaf.
    {
        MappingTransaction operation(&actor,space);MappingFaultSnapshot snapshot{};
        CHECK(operation.LockFaultRange(va.raw()));CHECK(operation.CaptureRange(va.raw(),1,snapshot));
        operation.Unlock();
        KMutexLock(&unrelated.gate);++unrelated.generation;KMutexUnlock(&unrelated.gate);
        CHECK(operation.LockFaultRange(va.raw()));CHECK(!operation.ValidateRange(snapshot,va.raw(),1));
    }
    {
        MappingTransaction operation(&actor,space);MappingFaultSnapshot snapshot{};
        CHECK(operation.LockFaultRange(va.raw()+8*pageSize));
        CHECK(!operation.CaptureMapping(va.raw()+8*pageSize,snapshot));
        CHECK(!operation.CaptureMapping(va.raw(),snapshot));
    }
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    MappingBackingUnref(backing);MappingBackingUnref(other);CHECK(released==2);
    FileObjectDispose(&target);FileObjectDispose(&unrelated);
    VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("ADDRESS FILE GATE PASS target/anonymous/hole/no-allocation/unrelated-held/replacement-retry/pins");
}
static void targetFileInvalidation() {
    auto root=table();VmmHostSetKernelRoot(root);AddressSpaceKernelInit();
    auto* space=AddressSpaceCreate(45);Process actor{};actor.addressSpace=space;
    FileObject target{},unrelated{};FileObjectInit(&target);FileObjectInit(&unrelated);
    unsigned released=0;
    auto* backing=MappingBackingCreate(&released,rangeBackingReleased);
    auto* other=MappingBackingCreate(&released,rangeBackingReleased);
    CHECK(backing && other);backing->file=&target;other->file=&unrelated;
    PhysicalAddress pages[6];for(auto& page:pages){page=PmmAllocPage(MemTag::User,45);CHECK(page);}
    {
        MappingTransaction operation(&actor,space,backing);
        CHECK(operation.PrepareRanges(3));CHECK(operation.Lock());
        auto range=rangeRecord(va.raw(),va.raw()+3*pageSize,backing);range.offset=2*pageSize;
        CHECK(operation.InsertRange(range));
        range=rangeRecord(va.raw()+4*pageSize,va.raw()+5*pageSize,backing);
        range.offset=3*pageSize;range.leafOwnership=MappingLeafOwnership::Borrowed;
        CHECK(operation.InsertRange(range));
        range=rangeRecord(va.raw()+10*pageSize,va.raw()+11*pageSize,backing);range.offset=100*pageSize;
        CHECK(operation.InsertRange(range));
        for(unsigned i=0;i<3;++i)CHECK(VmmTryMapPage(space->pageTable,va+i*pageSize,pages[i],flags,MemTag::User,45)==VmmMapResult::Mapped);
        CHECK(VmmTryMapPage(space->pageTable,va+4*pageSize,pages[3],flags,MemTag::User,45)==VmmMapResult::Mapped);
        CHECK(VmmTryMapPage(space->pageTable,va+10*pageSize,pages[5],flags,MemTag::User,45)==VmmMapResult::Mapped);
    }
    {
        MappingTransaction operation(&actor,space,other);CHECK(operation.PrepareRanges(1));CHECK(operation.Lock());
        CHECK(operation.InsertRange(rangeRecord(va.raw()+8*pageSize,va.raw()+9*pageSize,other)));
        CHECK(VmmTryMapPage(space->pageTable,va+8*pageSize,pages[4],flags,MemTag::User,45)==VmmMapResult::Mapped);
    }
    CHECK(UserMappingProtect(&actor,space,va.raw()+pageSize,pageSize,0)==0);
    CHECK(VmmReadLeaf(space->pageTable,va+pageSize)&PTE_HIDDEN_BIT);
    const auto generation=space->generation;
    auto* head=space->ranges.head;const auto identity=head->range.identity;
    const uint64_t registrations=FileMappingRegisteredRanges(&target);
    CHECK(registrations==5);
    KMutexLock(&target.gate);
    {
        FileMappingInvalidation job(&actor,space,backing);
        failHeapAfter=0;CHECK(!job.Prepare(4));failHeapAfter=-1;
        job.LockTargetFileAlreadyHeld();CHECK(!job.Detach(2*pageSize,5*pageSize));
    }
    {
        FileMappingInvalidation job(&actor,space,backing);CHECK(job.Prepare(3));
        job.LockTargetFileAlreadyHeld();uint64_t count=0;
        CHECK(job.Count(3*pageSize,4*pageSize,count) && count==2); // hidden + borrowed alias
        CHECK(job.Count(2*pageSize,5*pageSize,count) && count==4);
        CHECK(!job.Count(1,5*pageSize,count));
        CHECK(!job.Detach(2*pageSize,5*pageSize));
        CHECK(space->generation==generation && FileMappingRegisteredRanges(&target)==registrations);
        for(unsigned i=0;i<4;++i)CHECK(PmmGetRefCount(pages[i])==1 && maps(pages[i])==1);
    }
    KMutexUnlock(&target.gate);
    Gate ack;pendingAcknowledgement=&ack;
    // Hold an unrelated file gate throughout the operation. Acquiring the
    // ordinary all-file gate set here would deadlock instead of reaching ACK.
    KMutexLock(&unrelated.gate);
    std::thread invalidator([&]{
        KMutexLock(&target.gate);
        {
            FileMappingInvalidation job(&actor,space,backing);CHECK(job.Prepare(4));
            job.LockTargetFileAlreadyHeld();CHECK(job.Detach(2*pageSize,5*pageSize));
        }
        KMutexUnlock(&target.gate);
    });
    ack.wait(1);
    CHECK(space->ranges.head==head && head->range.identity==identity);
    CHECK(FileMappingRegisteredRanges(&target)==registrations && !FileMappingRetiringRanges(&target));
    CHECK(space->generation==generation+1);
    for(unsigned i=0;i<4;++i)CHECK(PmmGetRefCount(pages[i])==1 && !maps(pages[i]));
    for(unsigned i=0;i<3;++i)CHECK(!VmmReadLeaf(space->pageTable,va+i*pageSize));
    CHECK(!VmmReadLeaf(space->pageTable,va+4*pageSize));
    CHECK(VmmReadLeaf(space->pageTable,va+8*pageSize) && VmmReadLeaf(space->pageTable,va+10*pageSize));
    ack.release();invalidator.join();pendingAcknowledgement=nullptr;KMutexUnlock(&unrelated.gate);
    for(unsigned i=0;i<3;++i)CHECK(!PmmGetRefCount(pages[i]));
    CHECK(PmmGetRefCount(pages[3])==1); // borrowed owner was never consumed
    PmmUnrefPage(pages[3]);
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    CHECK(!FileMappingRegisteredRanges(&target) && !FileMappingRegisteredRanges(&unrelated));
    MappingBackingUnref(backing);MappingBackingUnref(other);CHECK(released==2);
    FileObjectDispose(&target);FileObjectDispose(&unrelated);
    VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("FILE INVALIDATION PASS target-only/split/hidden/borrowed/offset-filter/OOM/capacity/ACK-before-unref");
}

static int restoreFileStat(Vnode*,VnodeStat* stat) { stat->size=8*pageSize;return 0; }
static int restoreFileRead(Vnode*,void* buffer,uint64_t length,uint64_t* offset) {
    std::memset(buffer,0x39,length);*offset+=length;return static_cast<int>(length);
}
static void targetPrivateRestore() {
    auto root=table();VmmHostSetKernelRoot(root);AddressSpaceKernelInit();
    auto* space=AddressSpaceCreate(47);auto* child=AddressSpaceCreate(48);
    Process actor{};actor.addressSpace=space;
    FileObject file{};FileObjectInit(&file);unsigned released=0;
    auto* backing=MappingBackingCreate(&released,rangeBackingReleased);CHECK(backing);backing->file=&file;
    VnodeOps ops{};ops.stat=restoreFileStat;ops.read=restoreFileRead;
    Vnode vnode{&ops,VnodeType::File,nullptr,1,0};int error=0;
    auto canonical=FileObjectGetPage(&file,&vnode,0,true,&error);CHECK(canonical&&!error);
    auto full=PmmAllocPage(MemTag::User,47),boundary=PmmAllocPage(MemTag::User,47);
    auto beyond=PmmAllocPage(MemTag::User,47),shared=PmmAllocPage(MemTag::User,47);
    auto borrowed=PmmAllocPage(MemTag::User,47);CHECK(full&&boundary&&beyond&&shared&&borrowed);
    std::memset(reinterpret_cast<void*>(full.raw()),0x41,pageSize);
    std::memset(reinterpret_cast<void*>(boundary.raw()),0x52,pageSize);
    // Two AS aliases plus a hidden alias and an independent owner. Boundary
    // restoration must never modify this original physical frame in place.
    PmmRefPage(boundary);PmmRefPage(boundary);PmmRefPage(boundary);
    {
        MappingTransaction operation(&actor,space,backing);CHECK(operation.PrepareRanges(5)&&operation.Lock());
        CHECK(operation.InsertRange(rangeRecord(va.raw(),va.raw()+3*pageSize,backing)));
        CHECK(VmmTryMapPage(space->pageTable,va,full,flags,MemTag::User,47)==VmmMapResult::Mapped);
        CHECK(VmmTryMapPage(space->pageTable,va+pageSize,boundary,(flags&~VMM_WRITABLE)|PTE_COW_BIT,MemTag::User,777)==VmmMapResult::Mapped);
        CHECK(VmmTryMapPage(space->pageTable,va+2*pageSize,beyond,flags,MemTag::User,47)==VmmMapResult::Mapped);
        CHECK(operation.InsertRange(rangeRecord(va.raw()+4*pageSize,va.raw()+5*pageSize,backing)));
        CHECK(VmmTryMapPage(space->pageTable,va+4*pageSize,canonical,flags&~VMM_WRITABLE,MemTag::User,47)==VmmMapResult::Mapped);
        auto range=rangeRecord(va.raw()+6*pageSize,va.raw()+7*pageSize,backing);
        range.sharing=MappingSharing::Shared;CHECK(operation.InsertRange(range));
        CHECK(VmmTryMapPage(space->pageTable,va+6*pageSize,shared,flags,MemTag::User,47)==VmmMapResult::Mapped);
        range=rangeRecord(va.raw()+8*pageSize,va.raw()+9*pageSize,backing);
        range.leafOwnership=MappingLeafOwnership::Borrowed;CHECK(operation.InsertRange(range));
        CHECK(VmmTryMapPage(space->pageTable,va+8*pageSize,borrowed,flags,MemTag::User,47)==VmmMapResult::Mapped);
        range=rangeRecord(va.raw()+10*pageSize,va.raw()+11*pageSize,backing,0);range.offset=pageSize;
        CHECK(operation.InsertRange(range));
        CHECK(VmmTryMapPage(space->pageTable,va+10*pageSize,boundary,(flags&~VMM_WRITABLE)|PTE_COW_BIT,MemTag::User,777,false)==VmmMapResult::Mapped);
    }
    {
        MappingTransaction operation(&actor,child,backing);CHECK(operation.PrepareRanges(1)&&operation.Lock());
        auto range=rangeRecord(va.raw(),va.raw()+pageSize,backing);range.offset=pageSize;
        CHECK(operation.InsertRange(range));
        CHECK(VmmTryMapPage(child->pageTable,va,boundary,(flags&~VMM_WRITABLE)|PTE_COW_BIT,MemTag::User,48)==VmmMapResult::Mapped);
    }
    const uint64_t original=VmmReadLeaf(space->pageTable,va+pageSize);
    const uint64_t hidden=VmmReadLeaf(space->pageTable,va+10*pageSize);
    const uint64_t generation=space->generation;
    KMutexLock(&file.gate);
    // Heap/frame preparation failures leave mappings, bytes and refs intact.
    for (unsigned kind=0;kind<4;++kind) {
        FileMappingInvalidation job(&actor,space,backing);CHECK(job.Prepare(7));
        if(!kind)failHeapAfter=0;else failFrameAfter=kind-1;
        CHECK(!job.PreparePrivateRestore());
        failHeapAfter=-1;failFrameAfter=-1;
        job.LockTargetFileAlreadyHeld();CHECK(!job.Detach(0,8*pageSize));
        CHECK(space->generation==generation&&VmmReadLeaf(space->pageTable,va+pageSize)==original);
    }
    const unsigned slots[]={0,1,2,4,6,8,10};uint64_t before[7];
    for(unsigned i=0;i<7;++i)before[i]=VmmReadLeaf(space->pageTable,va+slots[i]*pageSize);
    // A failed post-ACK flush has not attempted resize: roll back exact PTEs,
    // including canonical/shared/borrowed/hidden aliases and private EOF padding.
    // This works with no copy preparation, and also frees unused prepared copies.
    for(unsigned prepare=0;prepare<2;++prepare) {
        FileMappingInvalidation job(&actor,space,backing);CHECK(job.Prepare(7));
        if(prepare)CHECK(job.PreparePrivateRestore());
        job.LockTargetFileAlreadyHeld();CHECK(!job.RestoreAll());CHECK(job.Detach(0,8*pageSize));
        job.Acknowledge();failAllocation=true;failHeapAfter=0;
        job.LockTargetFileAlreadyHeld();CHECK(job.RestoreAll());
        CHECK(!job.RestoreAll()&&!job.RestorePrivate(pageSize+123));job.Unlock();
        CHECK(failHeapAfter==0);failHeapAfter=-1;failAllocation=false;
        for(unsigned i=0;i<7;++i)CHECK(VmmReadLeaf(space->pageTable,va+slots[i]*pageSize)==before[i]);
        CHECK(PmmGetRefCount(canonical)==2&&maps(canonical)==1);
        CHECK(PmmGetRefCount(shared)==1&&maps(shared)==1);
        CHECK(PmmGetRefCount(borrowed)==1&&maps(borrowed)==1);
        CHECK(PmmGetRefCount(boundary)==4&&maps(boundary)==3);
        for(unsigned i=0;i<pageSize;++i)CHECK(reinterpret_cast<unsigned char*>(boundary.raw())[i]==0x52);
    }
    KMutexUnlock(&file.gate);CHECK(PmmGetRefCount(boundary)==4&&maps(boundary)==3);
    Gate ack,afterAck;pendingAcknowledgement=&ack;
    std::thread invalidator([&] {
        KMutexLock(&file.gate);
        {
            FileMappingInvalidation parentJob(&actor,space,backing),childJob(&actor,child,backing);
            CHECK(parentJob.Prepare(7)&&parentJob.PreparePrivateRestore());
            CHECK(childJob.Prepare(1)&&childJob.PreparePrivateRestore());
            parentJob.LockTargetFileAlreadyHeld();CHECK(parentJob.Detach(0,8*pageSize));
            CHECK(!parentJob.RestorePrivate(pageSize+123));parentJob.Unlock();
            childJob.LockTargetFileAlreadyHeld();CHECK(childJob.Detach(0,8*pageSize));childJob.Unlock();
            parentJob.Acknowledge();childJob.Acknowledge();
            parentJob.Acknowledge(); // idempotent; no extra shootdown
            afterAck.park(); // ACK must retain original refs until restore/destruction
            failAllocation=true;failHeapAfter=0;
            parentJob.LockTargetFileAlreadyHeld();CHECK(!parentJob.Detach(0,8*pageSize));
            CHECK(parentJob.RestorePrivate(pageSize+123));CHECK(!parentJob.RestorePrivate(pageSize+123));parentJob.Unlock();
            childJob.LockTargetFileAlreadyHeld();CHECK(childJob.RestorePrivate(pageSize+123));childJob.Unlock();
            CHECK(failHeapAfter==0);failHeapAfter=-1;failAllocation=false;
        }
        KMutexUnlock(&file.gate);
    });
    ack.wait(1);
    CHECK(PmmGetRefCount(boundary)==4&&!maps(boundary));
    CHECK(!VmmReadLeaf(space->pageTable,va)&&!VmmReadLeaf(child->pageTable,va));
    ack.release();afterAck.wait(1);
    CHECK(PmmGetRefCount(boundary)==4&&PmmGetRefCount(full)==1&&PmmGetRefCount(beyond)==1);
    CHECK(ack.arrived==2); // exactly parent + child ACK, despite explicit repeat
    afterAck.release();invalidator.join();pendingAcknowledgement=nullptr;
    CHECK(VmmLeafPhysical(VmmReadLeaf(space->pageTable,va))==full&&PmmGetRefCount(full)==1);
    CHECK(!VmmReadLeaf(space->pageTable,va+2*pageSize)&&!PmmGetRefCount(beyond));
    CHECK(!VmmReadLeaf(space->pageTable,va+4*pageSize)&&PmmGetRefCount(canonical)==1);
    CHECK(!VmmReadLeaf(space->pageTable,va+6*pageSize)&&!PmmGetRefCount(shared));
    CHECK(!VmmReadLeaf(space->pageTable,va+8*pageSize)&&PmmGetRefCount(borrowed)==1);
    const uint64_t restored=VmmReadLeaf(space->pageTable,va+pageSize);
    const uint64_t hiddenRestored=VmmReadLeaf(space->pageTable,va+10*pageSize);
    CHECK((restored&~0x000ffffffffff000ULL)==(original&~0x000ffffffffff000ULL));
    CHECK((hiddenRestored&~0x000ffffffffff000ULL)==(hidden&~0x000ffffffffff000ULL));
    auto copy=VmmLeafPhysical(restored),hiddenCopy=VmmLeafPhysical(hiddenRestored);
    auto childCopy=VmmLeafPhysical(VmmReadLeaf(child->pageTable,va));
    CHECK(copy!=boundary&&hiddenCopy!=boundary&&childCopy!=boundary&&copy!=hiddenCopy&&copy!=childCopy);
    for(auto frame:{copy,hiddenCopy,childCopy}) {
        CHECK(PmmGetRefCount(frame)==1&&maps(frame)==1);
        auto* bytes=reinterpret_cast<unsigned char*>(frame.raw());
        for(unsigned i=0;i<pageSize;++i)CHECK(bytes[i]==(i<123?0x52:0));
    }
    CHECK(PmmGetRefCount(boundary)==1&&!maps(boundary));
    for(unsigned i=0;i<pageSize;++i)CHECK(reinterpret_cast<unsigned char*>(boundary.raw())[i]==0x52);
    PmmUnrefPage(boundary);PmmUnrefPage(borrowed);
    AddressSpaceUnref(space);AddressSpaceUnref(child);CHECK(ProcessCleanupStep());CHECK(ProcessCleanupStep());
    CHECK(!FileMappingRegisteredRanges(&file));MappingBackingUnref(backing);CHECK(released==1);
    FileObjectDispose(&file);VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("PRIVATE RESTORE PASS prepare-OOM/exact-rollback/all-AS-ACK/retained-refs/canonical/shared/borrowed/hidden/CoW/boundary-copy/no-allocation");
}
static void privateRestoreObservedEof() {
    for(uint64_t limit:{uint64_t{0},uint64_t{37},uint64_t{pageSize},
                       uint64_t{pageSize+123},uint64_t{2*pageSize+17},~uint64_t{0}}) {
        auto root=table();VmmHostSetKernelRoot(root);AddressSpaceKernelInit();
        auto* space=AddressSpaceCreate(49);Process actor{};actor.addressSpace=space;
        FileObject file{};FileObjectInit(&file);unsigned released=0;
        auto* backing=MappingBackingCreate(&released,rangeBackingReleased);CHECK(backing);backing->file=&file;
        PhysicalAddress pages[3];
        for(auto& page:pages) {
            page=PmmAllocPage(MemTag::User,49);CHECK(page);PmmRefPage(page);
            std::memset(reinterpret_cast<void*>(page.raw()),0x63,pageSize);
        }
        {
            MappingTransaction operation(&actor,space,backing);CHECK(operation.PrepareRanges(1)&&operation.Lock());
            CHECK(operation.InsertRange(rangeRecord(va.raw(),va.raw()+3*pageSize,backing)));
            for(unsigned i=0;i<3;++i)
                CHECK(VmmTryMapPage(space->pageTable,va+i*pageSize,pages[i],flags,MemTag::User,49)==VmmMapResult::Mapped);
        }
        KMutexLock(&file.gate);
        {
            FileMappingInvalidation job(&actor,space,backing);CHECK(job.Prepare(3));
            CHECK(job.PreparePrivateRestore()); // no knowledge of eventual EOF
            job.LockTargetFileAlreadyHeld();CHECK(job.Detach(0,3*pageSize));job.Acknowledge();
            failAllocation=true;failHeapAfter=0;
            job.LockTargetFileAlreadyHeld();CHECK(job.RestorePrivate(limit));job.Unlock();
            CHECK(failHeapAfter==0);failHeapAfter=-1;failAllocation=false;
        }
        KMutexUnlock(&file.gate);
        for(unsigned i=0;i<3;++i) {
            const uint64_t offset=i*pageSize;
            const auto leaf=VmmReadLeaf(space->pageTable,va+offset);
            if(offset>=limit) { CHECK(!leaf&&PmmGetRefCount(pages[i])==1); }
            else {
                const auto frame=VmmLeafPhysical(leaf);
                const bool partial=limit-offset<pageSize;
                CHECK((frame!=pages[i])==partial);
                auto* bytes=reinterpret_cast<unsigned char*>(frame.raw());
                for(unsigned j=0;j<pageSize;++j)CHECK(bytes[j]==(partial&&j>=limit-offset?0:0x63));
                CHECK(PmmGetRefCount(pages[i])==(partial?1:2));
            }
            for(unsigned j=0;j<pageSize;++j)CHECK(reinterpret_cast<unsigned char*>(pages[i].raw())[j]==0x63);
            PmmUnrefPage(pages[i]);
        }
        AddressSpaceUnref(space);CHECK(ProcessCleanupStep());MappingBackingUnref(backing);CHECK(released==1);
        FileObjectDispose(&file);VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    }
    std::puts("PRIVATE RESTORE EOF PASS deferred-boundary/zero/aligned/full-retention/source-unchanged/no-restore-allocation");
}

static void targetQuarantineRecovery() {
    auto root=table();VmmHostSetKernelRoot(root);AddressSpaceKernelInit();
    auto* space=AddressSpaceCreate(50);auto* child=AddressSpaceCreate(51);
    Process actor{};actor.addressSpace=space;
    FileObject file{};FileObjectInit(&file);unsigned released=0;
    auto* backing=MappingBackingCreate(&released,rangeBackingReleased);CHECK(backing);backing->file=&file;
    VnodeOps ops{};ops.stat=restoreFileStat;ops.read=restoreFileRead;
    Vnode vnode{&ops,VnodeType::File,nullptr,1,0};int error=0;
    auto canonical=FileObjectGetPage(&file,&vnode,0,true,&error);CHECK(canonical&&!error);
    PhysicalAddress pages[3];
    for(unsigned i=0;i<3;++i) {
        pages[i]=PmmAllocPage(MemTag::User,50);CHECK(pages[i]);
        std::memset(reinterpret_cast<void*>(pages[i].raw()),0x61+i,pageSize);
    }
    PmmRefPage(pages[1]);PmmRefPage(pages[1]); // true hidden alias + independent pin
    auto shared=PmmAllocPage(MemTag::User,50),borrowed=PmmAllocPage(MemTag::User,50);CHECK(shared&&borrowed);
    {
        MappingTransaction operation(&actor,space,backing);CHECK(operation.PrepareRanges(5)&&operation.Lock());
        CHECK(operation.InsertRange(rangeRecord(va.raw(),va.raw()+3*pageSize,backing)));
        for(unsigned i=0;i<3;++i)
            CHECK(VmmTryMapPage(space->pageTable,va+i*pageSize,pages[i],flags,MemTag::User,50)==VmmMapResult::Mapped);
        auto range=rangeRecord(va.raw()+4*pageSize,va.raw()+5*pageSize,backing,0);range.offset=pageSize;
        CHECK(operation.InsertRange(range));
        CHECK(VmmTryMapPage(space->pageTable,va+4*pageSize,pages[1],flags,MemTag::User,50,false)==VmmMapResult::Mapped);
        CHECK(operation.InsertRange(rangeRecord(va.raw()+6*pageSize,va.raw()+7*pageSize,backing,1)));
        CHECK(VmmTryMapPage(space->pageTable,va+6*pageSize,canonical,flags&~VMM_WRITABLE,MemTag::User,50)==VmmMapResult::Mapped);
        range=rangeRecord(va.raw()+8*pageSize,va.raw()+9*pageSize,backing);range.sharing=MappingSharing::Shared;
        CHECK(operation.InsertRange(range));
        CHECK(VmmTryMapPage(space->pageTable,va+8*pageSize,shared,flags,MemTag::User,50)==VmmMapResult::Mapped);
        range=rangeRecord(va.raw()+10*pageSize,va.raw()+11*pageSize,backing);range.leafOwnership=MappingLeafOwnership::Borrowed;
        CHECK(operation.InsertRange(range));
        CHECK(VmmTryMapPage(space->pageTable,va+10*pageSize,borrowed,flags,MemTag::User,50)==VmmMapResult::Mapped);
    }
    {
        MappingTransaction stale(&actor,space,backing);CHECK(stale.Prepare(0,1)&&stale.Lock());
        MappingFaultSnapshot snapshot{};CHECK(stale.CaptureRange(va.raw(),1,snapshot));stale.Unlock();
        auto candidate=PmmAllocPage(MemTag::User,50);CHECK(candidate&&stale.StagePage(0,candidate,true));
        KMutexLock(&file.gate);
        {
            FileMappingInvalidation job(&actor,space,backing);CHECK(job.Prepare(7)); // no copy reservation needed
            job.LockTargetFileAlreadyHeld();CHECK(job.Detach(0,8*pageSize));job.Acknowledge();
            job.LockTargetFileAlreadyHeld();CHECK(!job.QuarantinePrivate());job.Unlock();
            FileObjectQuarantineLocked(&file,-5,pageSize+173);
            failAllocation=true;failHeapAfter=0;
            job.LockTargetFileAlreadyHeld();CHECK(!job.RestoreAll());CHECK(job.QuarantinePrivate());
            CHECK(!job.QuarantinePrivate());job.Unlock();
            CHECK(failHeapAfter==0);failHeapAfter=-1;failAllocation=false;
        }
        KMutexUnlock(&file.gate);
        CHECK(stale.Lock());
        CHECK(stale.PublishFault(snapshot,0,va,1,true)==MappingFaultCommit::Stale);
        CHECK(stale.CaptureRange(va.raw(),1,snapshot));
        CHECK(stale.PublishFault(snapshot,0,va,1,true)==MappingFaultCommit::Failed);
        CHECK(stale.Publish(0,va+pageSize,VMM_USER|VMM_WRITABLE,MemTag::User,50)==VmmMapResult::InvalidAddress);
        CHECK(!stale.ProtectMapping(va.raw(),va.raw()+3*pageSize,3));stale.Unlock();
    }
    CHECK(PmmGetRefCount(canonical)==1&&!PmmGetRefCount(shared)&&PmmGetRefCount(borrowed)==1);
    CHECK(!VmmReadLeaf(space->pageTable,va+6*pageSize)&&!VmmReadLeaf(space->pageTable,va+8*pageSize));
    CHECK(!VmmReadLeaf(space->pageTable,va+10*pageSize));
    for(unsigned i=0;i<3;++i) {
        CHECK(VmmReadLeaf(space->pageTable,va+i*pageSize)&PTE_HIDDEN_BIT);
        for(unsigned j=0;j<pageSize;++j)CHECK(reinterpret_cast<unsigned char*>(pages[i].raw())[j]==0x61+i);
    }
    const auto generation=space->generation;
    CHECK(UserMappingProtect(&actor,space,va.raw(),3*pageSize,1)==-5&&space->generation==generation);
    CHECK(UserMappingProtect(&actor,space,va.raw(),pageSize,0)==0); // remain PROT_NONE after recovery
    CHECK(UserMappingPageAccess(&actor,space,va.raw()+pageSize,2,true)==UserPageAccess::Missing);
    UserMappingRequest request{};request.range=rangeRecord(0,pageSize,backing);request.length=pageSize;
    CHECK(UserMappingCreate(&actor,space,request)==-5);
    CHECK(UserMappingClone(&actor,space,child,51));
    CHECK(VmmReadLeaf(child->pageTable,va+pageSize)&PTE_HIDDEN_BIT);
    CHECK(UserMappingProtect(&actor,child,va.raw()+pageSize,pageSize,3)==-5);
    const auto moved=va+16*pageSize;
    CHECK(UserMappingResize(&actor,space,va.raw()+pageSize,2*pageSize,2*pageSize,3,moved.raw(),nullptr,nullptr)==static_cast<int64_t>(moved.raw()));
    CHECK(!VmmReadLeaf(space->pageTable,va+pageSize));
    CHECK((VmmReadLeaf(space->pageTable,moved)&(PTE_HIDDEN_BIT|VMM_PRESENT))==PTE_HIDDEN_BIT);
    CHECK(PmmGetRefCount(pages[1])==5&&maps(pages[1])==4);
    // Later stat succeeds. Before any further size mutation, recovery captures
    // the new fork/remap aliases. A regrown EOF cannot widen the saved prefix.
    KMutexLock(&file.gate);
    auto* first=FileMappingNextSpace(&file,0);CHECK(first);
    auto* second=FileMappingNextSpace(&file,first->identity);CHECK(second);
    CHECK(!FileMappingNextSpace(&file,second->identity));AddressSpaceUnref(first);AddressSpaceUnref(second);
    {
        FileMappingInvalidation parentJob(&actor,space,backing),childJob(&actor,child,backing);
        CHECK(parentJob.Prepare(4)&&parentJob.PreparePrivateRestore());
        CHECK(childJob.Prepare(4)&&childJob.PreparePrivateRestore());
        parentJob.LockTargetFileAlreadyHeld();CHECK(parentJob.Detach(0,8*pageSize));parentJob.Unlock();
        childJob.LockTargetFileAlreadyHeld();CHECK(childJob.Detach(0,8*pageSize));childJob.Unlock();
        parentJob.Acknowledge();childJob.Acknowledge();
        failAllocation=true;failHeapAfter=0;
        parentJob.LockTargetFileAlreadyHeld();CHECK(parentJob.RestorePrivate(3*pageSize));parentJob.Unlock();
        childJob.LockTargetFileAlreadyHeld();CHECK(childJob.RestorePrivate(3*pageSize));childJob.Unlock();
        CHECK(file.mappingError==-5&&file.privateValidPrefix==pageSize+173);
        CHECK(failHeapAfter==0);failHeapAfter=-1;failAllocation=false;
        FileObjectCompleteMappingRecoveryLocked(&file);
    }
    KMutexUnlock(&file.gate);
    CHECK(!file.mappingError&&!PmmGetRefCount(canonical)&&!PmmGetRefCount(pages[2]));
    CHECK(PmmGetRefCount(pages[1])==1&&!maps(pages[1]));
    CHECK((VmmReadLeaf(space->pageTable,va)&(PTE_HIDDEN_BIT|VMM_PRESENT))==PTE_HIDDEN_BIT);
    CHECK((VmmReadLeaf(child->pageTable,va)&(PTE_HIDDEN_BIT|VMM_PRESENT))==PTE_HIDDEN_BIT);
    CHECK(VmmReadLeaf(space->pageTable,moved)&VMM_PRESENT);
    CHECK(VmmReadLeaf(child->pageTable,va+pageSize)&VMM_PRESENT);
    CHECK((VmmReadLeaf(space->pageTable,va+4*pageSize)&(PTE_HIDDEN_BIT|VMM_PRESENT))==PTE_HIDDEN_BIT);
    CHECK((VmmReadLeaf(child->pageTable,va+4*pageSize)&(PTE_HIDDEN_BIT|VMM_PRESENT))==PTE_HIDDEN_BIT);
    const uint64_t restored[]={VmmReadLeaf(space->pageTable,moved),VmmReadLeaf(child->pageTable,va+pageSize),
        VmmReadLeaf(space->pageTable,va+4*pageSize),VmmReadLeaf(child->pageTable,va+4*pageSize)};
    for(auto raw:restored) {
        CHECK(raw&PTE_COW_BIT);auto* bytes=reinterpret_cast<unsigned char*>(VmmLeafPhysical(raw).raw());
        for(unsigned j=0;j<pageSize;++j)CHECK(bytes[j]==(j<173?0x62:0));
    }
    CHECK(UserMappingPageAccess(&actor,space,moved.raw(),1,true)==UserPageAccess::Ready);
    CHECK(UserMappingProtect(&actor,space,va.raw(),pageSize,3)==0);
    CHECK(UserMappingPageAccess(&actor,space,va.raw(),1,true)==UserPageAccess::Ready);
    for(unsigned j=0;j<pageSize;++j)CHECK(reinterpret_cast<unsigned char*>(pages[1].raw())[j]==0x62);
    PmmUnrefPage(pages[1]);PmmUnrefPage(borrowed);
    AddressSpaceUnref(space);AddressSpaceUnref(child);CHECK(ProcessCleanupStep());CHECK(ProcessCleanupStep());
    CHECK(!FileMappingRegisteredRanges(&file));MappingBackingUnref(backing);CHECK(released==1);
    FileObjectDispose(&file);VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("QUARANTINE PASS retained-private/hidden/no-allocation/stale-fault/publication/protect/fork/remap/recovery-prefix/PROT_NONE/cache-eviction");
}

static void pageAccessWithoutUnrelatedGate() {
    auto root=table();VmmHostSetKernelRoot(root);AddressSpaceKernelInit();
    Process kernelActor{};kernelActor.addressSpace=AddressSpaceKernel();
    CHECK(UserMappingPageAccess(&kernelActor,kernelActor.addressSpace,0x7020,1,true)==UserPageAccess::Denied);
    auto* space=AddressSpaceCreate(46);CHECK(space);
    Process actor{};actor.addressSpace=space;
    FileObject unrelated{};FileObjectInit(&unrelated);unsigned released=0;
    auto* backing=MappingBackingCreate(&released,rangeBackingReleased);CHECK(backing);
    backing->file=&unrelated;
    auto physical=PmmAllocPage(MemTag::User,46);CHECK(physical);
    {
        MappingTransaction operation(&actor,space,backing);
        CHECK(operation.PrepareRanges(2)&&operation.Lock());
        CHECK(operation.InsertRange(rangeRecord(va.raw(),va.raw()+2*pageSize,nullptr)));
        CHECK(operation.InsertRange(rangeRecord(va.raw()+8*pageSize,va.raw()+9*pageSize,backing)));
        CHECK(VmmTryMapPage(space->pageTable,va,physical,flags,MemTag::User,46)==VmmMapResult::Mapped);
    }
    const auto original=VmmReadLeaf(space->pageTable,va);
    const auto generation=space->generation;
    const auto spaceRefs=space->references;
    std::mutex completionMutex;std::condition_variable completion;bool done=false;
    UserPageAccess statuses[5]{};
    KMutexLock(&unrelated.gate);
    failHeapAfter=0;
    std::thread reader([&] {
        statuses[0]=UserMappingPageAccess(&actor,space,va.raw(),1,true);
        statuses[1]=UserMappingPageAccess(&actor,space,va.raw(),2,true);
        statuses[2]=UserMappingPageAccess(&actor,space,va.raw()+pageSize,1,true);
        statuses[3]=UserMappingPageAccess(&actor,space,va.raw(),4,true);
        statuses[4]=UserMappingPageAccess(&actor,space,va.raw()+4*pageSize,1,true);
        std::lock_guard<std::mutex> lock(completionMutex);done=true;completion.notify_all();
    });
    bool completedWhileHeld;
    {
        std::unique_lock<std::mutex> lock(completionMutex);
        completedWhileHeld=completion.wait_for(lock,std::chrono::seconds(2),[&]{return done;});
    }
    // Baseline full-gate code must fail boundedly: release and join BEFORE
    // asserting, so a negative run cannot strand a worker behind this gate.
    KMutexUnlock(&unrelated.gate);reader.join();
    const bool noHeapAttempt=failHeapAfter==0;failHeapAfter=-1;
    CHECK(completedWhileHeld&&noHeapAttempt);
    CHECK(statuses[0]==UserPageAccess::Ready&&statuses[1]==UserPageAccess::Ready);
    CHECK(statuses[2]==UserPageAccess::Missing);
    CHECK(statuses[3]==UserPageAccess::Denied&&statuses[4]==UserPageAccess::Denied);
    CHECK(VmmReadLeaf(space->pageTable,va)==original&&space->generation==generation);
    CHECK(space->references==spaceRefs&&actor.kernelRegionDepth==0&&actor.refCount==0);

    // Force a genuine private CoW candidate, then change its permissions in
    // the gap before target-scope acquisition. The slow path must recheck and
    // deny the write, without allocating/copying a replacement physical page.
    KMutexLock(&space->mutation);cow(space->pageTable);KMutexUnlock(&space->mutation);
    PmmRefPage(physical); // external pin forces copy if stale CoW were attempted
    Gate beforeFullGate;pageAccessPauseMutex=&space->mutation;pageAccessPause=&beforeFullGate;
    UserPageAccess result=UserPageAccess::Ready;
    std::thread writer([&] {
        pageAccessPauseSkip=1;pageAccessPauseOnce=true;
        result=UserMappingPageAccess(&actor,space,va.raw(),2,true);
    });
    bool reachedSlowPath;
    {
        std::unique_lock<std::mutex> lock(beforeFullGate.mutex);
        reachedSlowPath=beforeFullGate.cv.wait_for(lock,std::chrono::seconds(2),
            [&]{return beforeFullGate.arrived==1;});
    }
    Process mutator{};mutator.addressSpace=space;
    CHECK(UserMappingProtect(&mutator,space,va.raw(),pageSize,1)==0);
    const auto protectedLeaf=VmmReadLeaf(space->pageTable,va);
    beforeFullGate.release();writer.join();pageAccessPause=nullptr;pageAccessPauseMutex=nullptr;
    CHECK(reachedSlowPath&&result==UserPageAccess::Denied);
    CHECK(VmmReadLeaf(space->pageTable,va)==protectedLeaf);
    CHECK(VmmLeafPhysical(protectedLeaf)==physical&&PmmGetRefCount(physical)==2&&maps(physical)==1);
    CHECK(space->references==spaceRefs&&actor.kernelRegionDepth==0&&actor.refCount==0);
    PmmUnrefPage(physical);
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    CHECK(!FileMappingRegisteredRanges(&unrelated));MappingBackingUnref(backing);CHECK(released==1);
    FileObjectDispose(&unrelated);
    VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("PAGE ACCESS PASS unrelated-gate/no-allocation/read-statuses/CoW-revalidation/lifetime");
}

static void anonymousIntervalIsolation() {
    auto root=table();VmmHostSetKernelRoot(root);AddressSpaceKernelInit();
    auto* space=AddressSpaceCreate(80);auto* child=AddressSpaceCreate(81);CHECK(space&&child);
    Process actor{};actor.addressSpace=space;
    FileObject unrelated{};FileObjectInit(&unrelated);unsigned released=0;
    auto* backing=MappingBackingCreate(&released,rangeBackingReleased);CHECK(backing);backing->file=&unrelated;
    UserMappingRequest request{};request.range=rangeRecord(0,pageSize,nullptr);
    request.length=pageSize;request.preparedPages=1;request.prepare=prepareMappingPages;request.fixed=true;
    request.hint=va.raw();CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(request.hint));
    request.hint+=pageSize;CHECK(UserMappingCreate(&actor,space,request)==static_cast<int64_t>(request.hint));
    {
        MappingTransaction op(&actor,space,backing);CHECK(op.PrepareRanges(1)&&op.Lock());
        CHECK(op.InsertRange(rangeRecord(va.raw()+8*pageSize,va.raw()+9*pageSize,backing)));
    }
    CHECK(UserMappingClone(&actor,space,child,81));
    auto old=VmmLeafPhysical(VmmReadLeaf(child->pageTable,va));
    *reinterpret_cast<unsigned char*>(old.raw())=0x51;
    KMutexLock(&unrelated.gate);
    std::mutex mutex;std::condition_variable completed;bool done=false;
    std::thread worker([&]{
        CHECK(UserMappingPageAccess(&actor,space,va.raw(),2,true)==UserPageAccess::Ready);
        CHECK(VmmLeafPhysical(VmmReadLeaf(space->pageTable,va))!=old);
        CHECK(UserMappingDiscardAnonymous(&actor,space,va.raw(),2*pageSize)==0);
        std::lock_guard<std::mutex> lock(mutex);done=true;completed.notify_all();
    });
    bool isolated;
    {std::unique_lock<std::mutex> lock(mutex);isolated=completed.wait_for(lock,std::chrono::seconds(2),[&]{return done;});}
    KMutexUnlock(&unrelated.gate);worker.join();CHECK(isolated);
    CHECK(*reinterpret_cast<unsigned char*>(old.raw())==0x51);
    for(unsigned i=0;i<2;++i) {
        auto page=VmmLeafPhysical(VmmReadLeaf(space->pageTable,va+i*pageSize));
        for(unsigned j=0;j<pageSize;++j)CHECK(reinterpret_cast<unsigned char*>(page.raw())[j]==0);
    }
    {
        MappingTransaction op(&actor,space);
        CHECK(op.LockAnonymousInterval(va.raw(),va.raw()+3*pageSize)==-12); // hole
        CHECK(op.LockAnonymousInterval(va.raw()+8*pageSize,va.raw()+9*pageSize)==-95); // file backing
        CHECK(op.LockAnonymousInterval(va.raw(),va.raw()+2*pageSize)==0);
        uint64_t count=123;CHECK(!op.RangeLeafCount(va.raw(),va.raw()+3*pageSize,count)&&!count);
        op.Unlock();
        CHECK(UserMappingProtect(&actor,space,va.raw(),pageSize,0)==0);
        CHECK(op.LockAnonymousInterval(va.raw(),va.raw()+2*pageSize)==0);op.Unlock();
        // A prepared interval is not authority after unlock: replacing even
        // one covered node with a file mapping must reject reacquisition.
        UserMappingRequest replacement{};replacement.range=rangeRecord(0,pageSize,backing);
        replacement.range.kind=MappingKind::File;replacement.hint=va.raw()+pageSize;
        replacement.length=pageSize;replacement.fixed=true;
        CHECK(UserMappingCreate(&actor,space,replacement)==static_cast<int64_t>(replacement.hint));
        CHECK(op.LockAnonymousInterval(va.raw(),va.raw()+2*pageSize)==-95);
    }
    AddressSpaceUnref(child);AddressSpaceUnref(space);CHECK(ProcessCleanupStep());CHECK(ProcessCleanupStep());
    CHECK(!FileMappingRegisteredRanges(&unrelated));MappingBackingUnref(backing);CHECK(released==1);FileObjectDispose(&unrelated);
    VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("ANONYMOUS INTERVAL PASS CoW/unrelated-file-held/multi-node-discard/child-integrity/hole/file/bounds/revalidation");
}
static void anonymousDiscard() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto* space=AddressSpaceCreate(40);auto* child=AddressSpaceCreate(41);CHECK(space&&child);
    Process actor{};actor.addressSpace=space;
    uint64_t argument=0;
    auto base=UserMappingStack(&actor,space,2*pageSize,nullptr,0,&argument);CHECK(base>0);
    const VirtualAddress first(base),second(base+pageSize);
    auto original=VmmLeafPhysical(VmmReadLeaf(space->pageTable,first));
    auto hidden=VmmLeafPhysical(VmmReadLeaf(space->pageTable,second));
    std::memset(reinterpret_cast<void*>(original.raw()),0x5a,pageSize);
    std::memset(reinterpret_cast<void*>(hidden.raw()),0x6b,pageSize);
    CHECK(UserMappingProtect(&actor,space,second.raw(),pageSize,0)==0);
    CHECK(UserMappingClone(&actor,space,child,41));
    auto beforeFirst=VmmReadLeaf(space->pageTable,first),beforeSecond=VmmReadLeaf(space->pageTable,second);
    uint64_t addresses[2]={123,456},count=99;
    CHECK(VmmCollectRangeAddresses(space->pageTable,second.raw()+pageSize,second.raw()+2*pageSize,nullptr,0,&count));
    CHECK(count==0);
    CHECK(!VmmCollectRangeAddresses(space->pageTable,first.raw(),second.raw()+pageSize,nullptr,0,&count));
    CHECK(count==0&&VmmReadLeaf(space->pageTable,first)==beforeFirst&&VmmReadLeaf(space->pageTable,second)==beforeSecond);
    CHECK(!VmmCollectRangeAddresses(space->pageTable,first.raw(),second.raw()+pageSize,addresses,1,&count));
    CHECK(count==0&&addresses[0]==123&&addresses[1]==456);
    CHECK(VmmCollectRangeAddresses(space->pageTable,first.raw(),second.raw()+pageSize,addresses,2,&count));
    CHECK(count==2&&addresses[0]==first.raw()&&addresses[1]==second.raw());
    failFrameAfter=1;
    CHECK(UserMappingDiscardAnonymous(&actor,space,first.raw(),2*pageSize)==-12);
    CHECK(VmmReadLeaf(space->pageTable,first)==beforeFirst&&VmmReadLeaf(space->pageTable,second)==beforeSecond);
    CHECK(PmmGetRefCount(original)==2&&PmmGetRefCount(hidden)==2);
    Gate ack;pendingAcknowledgement=&ack;std::atomic<bool> returned{false};
    std::thread discarder([&] {
        CHECK(UserMappingDiscardAnonymous(&actor,space,first.raw(),2*pageSize)==0);returned=true;
    });
    ack.wait(1);
    CHECK(!returned&&PmmGetRefCount(original)==2&&PmmGetRefCount(hidden)==2);
    CHECK(maps(original)==1&&maps(hidden)==1);
    CHECK(*reinterpret_cast<unsigned char*>(original.raw())==0x5a);
    CHECK(*reinterpret_cast<unsigned char*>(hidden.raw())==0x6b);
    auto newFirst=VmmReadLeaf(space->pageTable,first),newSecond=VmmReadLeaf(space->pageTable,second);
    CHECK(VmmLeafPhysical(newFirst)!=original&&(newFirst&VMM_WRITABLE)&&!(newFirst&PTE_COW_BIT));
    CHECK(VmmLeafPhysical(newSecond)!=hidden&&(newSecond&PTE_HIDDEN_BIT)&&!(newSecond&VMM_PRESENT));
    for (auto raw:{newFirst,newSecond}) for (size_t i=0;i<pageSize;++i)
        CHECK(reinterpret_cast<unsigned char*>(VmmLeafPhysical(raw).raw())[i]==0);
    CHECK(VmmLeafPhysical(VmmReadLeaf(child->pageTable,first))==original);
    ack.release();discarder.join();pendingAcknowledgement=nullptr;
    CHECK(returned&&PmmGetRefCount(original)==1&&PmmGetRefCount(hidden)==1);
    CHECK(!actor.kernelRegionDepth&&!actor.refCount);
    AddressSpaceUnref(child);CHECK(ProcessCleanupStep());
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("ANONYMOUS DISCARD PASS zero/hidden/CoW-isolation/OOM-atomicity/delayed-ACK/bounded-snapshot");
}

static void memoryCensus() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto* first=AddressSpaceCreate(50);auto* second=AddressSpaceCreate(51);
    auto* hidden=AddressSpaceCreate(52);CHECK(first&&second&&hidden);
    const uint64_t ceiling=AddressSpaceIdentityCeiling();
    CHECK(!AddressSpaceNextReference(0,ceiling)); // construction isn't published
    AddressSpacePublish(second);AddressSpacePublish(first);AddressSpacePublish(first);
    auto* late=AddressSpaceCreate(53);AddressSpacePublish(late);
    auto* one=AddressSpaceNextReference(0,ceiling);CHECK(one==first);
    auto* two=AddressSpaceNextReference(one->identity,ceiling);CHECK(two==second);
    CHECK(!AddressSpaceNextReference(two->identity,ceiling)); // creation ceiling
    // Snapshot pins remain valid after both owning tasks have dropped refs.
    AddressSpaceUnref(first);AddressSpaceUnref(second);CHECK(!ProcessCleanupStep());
    AddressSpaceUnref(one);CHECK(ProcessCleanupStep());
    AddressSpaceUnref(two);CHECK(ProcessCleanupStep());
    CHECK(!AddressSpaceNextReference(0,ceiling));
    AddressSpaceUnref(hidden);CHECK(ProcessCleanupStep());
    AddressSpaceUnref(late);CHECK(ProcessCleanupStep());
    CHECK(!AddressSpaceNextReference(0,AddressSpaceIdentityCeiling()));

    auto* watched=AddressSpaceCreate(54);CHECK(watched);
    AddressSpaceDestructionWatch completion{},other{};
    CHECK(AddressSpaceWatchDestruction(watched,&completion));
    CHECK(!AddressSpaceWatchDestruction(watched,&other));
    Gate destructionAck;pendingAcknowledgement=&destructionAck;
    AddressSpaceUnref(watched);
    std::thread destroyer([&]{CHECK(ProcessCleanupStep());});
    destructionAck.wait(1);CHECK(!__atomic_load_n(&completion.done,__ATOMIC_ACQUIRE));
    destructionAck.release();destroyer.join();pendingAcknowledgement=nullptr;
    CHECK(__atomic_load_n(&completion.done,__ATOMIC_ACQUIRE));
    watched=AddressSpaceCreate(55);CHECK(watched);
    CHECK(AddressSpaceWatchDestruction(watched,&completion));
    AddressSpaceCancelDestructionWatch(&completion);
    CHECK(AddressSpaceWatchDestruction(watched,&other));
    AddressSpaceUnref(watched);CHECK(ProcessCleanupStep());
    CHECK(other.done && !completion.done);

    auto pt=table();auto frame=PmmAllocPage();CHECK(frame);
    CHECK(VmmTryMapPage(pt,VirtualAddress(0x400000),frame,VMM_USER|PTE_COW_BIT)==VmmMapResult::Mapped);
    PmmRefPage(frame);
    CHECK(VmmTryMapPage(pt,VirtualAddress(0x800000),frame,VMM_USER|PTE_COW_BIT)==VmmMapResult::Mapped);
    auto privatePage=PmmAllocPage();CHECK(privatePage);
    CHECK(VmmTryMapPage(pt,VirtualAddress(0x1000000),privatePage,flags)==VmmMapResult::Mapped);
    auto* pte=VmmGetPte(pt,VirtualAddress(0x1000000));CHECK(pte);
    *pte=(*pte&~VMM_PRESENT)|PTE_HIDDEN_BIT;
    VmmRangeStats stats{};
    // Huge virtual holes must be skipped; no loop per reserved 4K/2MiB.
    CHECK(VmmMeasureRange(pt,4096,0x400000000000ULL,true,&stats));
    CHECK(stats.backedPages==3&&stats.hiddenPages==1&&stats.cowPages==2);
    CHECK(stats.multiplyMappedPages==2&&stats.unknownMapCountPages==0);
    CHECK(stats.exclusiveManagedPages==1);
    CHECK(stats.proportionalUnits==(8192ULL<<16));
    CHECK(stats.tableEntriesVisited<3000&&stats.lockBatches==1);
    PmmMapDec(privatePage); // deliberately missing accounting must be visible
    CHECK(VmmMeasureRange(pt,4096,0x400000000000ULL,true,&stats));
    CHECK(stats.unknownMapCountPages==1&&stats.proportionalUnits==(4096ULL<<16));
    PmmMapInc(privatePage);
    CHECK(VmmMeasureRange(pt,4096,0x400000000000ULL,false,&stats));
    CHECK(stats.backedPages==3&&!stats.proportionalUnits&&!stats.multiplyMappedPages);
    CHECK(!VmmMeasureRange(pt,1,8192,true,&stats)&&!stats.backedPages);
    VmmDestroyUserPageTable(pt);

    // Multiple traversal batches must neither omit nor double count leaves.
    pt=table();
    frame=PmmAllocPage();CHECK(frame);
    for(uint64_t i=0;i<5000;++i) {
        if(i)PmmRefPage(frame);
        CHECK(VmmTryMapPage(pt,VirtualAddress(0x400000+i*4096),frame,flags)==VmmMapResult::Mapped);
    }
    CHECK(VmmMeasureRange(pt,4096,0x400000000000ULL,true,&stats));
    CHECK(stats.backedPages==5000&&stats.writablePages==5000&&stats.lockBatches>1);
    CHECK(stats.proportionalUnits==((4096ULL<<16)/5000)*5000&&!stats.unknownMapCountPages);
    CHECK(!stats.exclusiveManagedPages);
    uint64_t cursor=4096,totalBacked=0,totalExclusive=0,batches=0;
    while(cursor<0x400000000000ULL) {
        uint64_t next=cursor;VmmRangeStats batch{};
        CHECK(VmmMeasureRangeStep(pt,cursor,0x400000000000ULL,true,&batch,&next));
        CHECK(next>cursor && batch.tableEntriesVisited<=4096 && batch.lockBatches==1);
        totalBacked+=batch.backedPages;totalExclusive+=batch.exclusiveManagedPages;
        cursor=next;++batches;
    }
    CHECK(batches>1 && totalBacked==5000 && !totalExclusive);
    VmmDestroyUserPageTable(pt);
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("MEMORY CENSUS PASS lifetime/publication/ceiling/sparse/hidden/aliases/bounded-batches");
}

namespace {
struct SharedFileBackend {
    FileObject file{};
    unsigned char bytes[8192]{};
    unsigned writes=0,syncs=0;
    bool failWrite=false,failSync=false,unmapOnSync=false;
    Process* actor=nullptr;
};
int sharedFileStat(Vnode*,VnodeStat* stat) {stat->size=8192;return 0;}
FileObject* sharedFileObject(Vnode* vnode) {return &static_cast<SharedFileBackend*>(vnode->priv)->file;}
int sharedFileRead(Vnode* vnode,void* output,uint64_t length,uint64_t* offset) {
    auto* b=static_cast<SharedFileBackend*>(vnode->priv);
    if(*offset>=sizeof(b->bytes))return 0;
    if(length>sizeof(b->bytes)-*offset)length=sizeof(b->bytes)-*offset;
    std::memcpy(output,b->bytes+*offset,length);*offset+=length;return static_cast<int>(length);
}
int sharedFileWrite(Vnode* vnode,const void* input,uint64_t length,uint64_t* offset) {
    auto* b=static_cast<SharedFileBackend*>(vnode->priv);++b->writes;
    if(b->failWrite)return -5;
    CHECK(*offset<=sizeof(b->bytes) && length<=sizeof(b->bytes)-*offset);
    std::memcpy(b->bytes+*offset,input,length);*offset+=length;return static_cast<int>(length);
}
int sharedFileSync(Vnode* vnode) {
    auto* b=static_cast<SharedFileBackend*>(vnode->priv);++b->syncs;
    // Backend I/O must run after both AS and file gates have been released.
    for(auto* mutex:{&b->file.gate,&b->actor->addressSpace->mutation}) {
        CHECK(hostMutex(mutex).try_lock());hostMutex(mutex).unlock();
    }
    if(b->unmapOnSync) {
        b->unmapOnSync=false;
        CHECK(UserMappingRemove(b->actor,b->actor->addressSpace,va.raw(),4*pageSize)==0);
        // The msync snapshot, not the removed VMA, still owns the descriptor.
        CHECK(vnode->refCount>=2);
    }
    return b->failSync?-5:0;
}
void sharedFilePublicationAndSync() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto* space=AddressSpaceCreate(61);CHECK(space);
    Process actor{};actor.addressSpace=space;
    SharedFileBackend backend{};backend.actor=&actor;FileObjectInit(&backend.file);
    VnodeOps ops{};ops.read=sharedFileRead;ops.write=sharedFileWrite;ops.stat=sharedFileStat;
    ops.fsync=sharedFileSync;ops.file_object=sharedFileObject;
    Vnode vnode{&ops,VnodeType::File,&backend,1,0};
    VfsRetain(&vnode);
    auto* backing=MappingBackingCreate(&vnode,releaseTestMappedFile);CHECK(backing);
    backing->file=&backend.file;backing->fileVnode=&vnode;
    auto install=[&](unsigned slot,uint64_t offset,uint32_t protection,bool canonical=true) {
        int error=0;
        auto physical=canonical?FileObjectGetPage(&backend.file,&vnode,offset/pageSize,true,&error):PmmAllocPage();
        CHECK(physical&&!error);
        MappingTransaction operation(&actor,space,backing);
        CHECK(operation.Prepare(0,1)&&operation.StagePage(0,physical,true)&&operation.PrepareRanges(1)&&operation.Lock());
        auto range=rangeRecord(va.raw()+slot*pageSize,va.raw()+(slot+1)*pageSize,backing,protection);
        range.offset=offset;range.sharing=MappingSharing::Shared;
        CHECK(operation.InsertRange(range));
        auto result=operation.Publish(0,va+slot*pageSize,VMM_USER|VMM_NO_EXEC|((protection&2)?VMM_WRITABLE:0),MemTag::User,61,protection!=0);
        CHECK(result==(canonical?VmmMapResult::Mapped:VmmMapResult::InvalidAddress));
    };
    install(0,0,3);
    CHECK(backend.file.writebackVnode==&vnode&&vnode.refCount==3);
    auto first=VmmLeafPhysical(VmmReadLeaf(space->pageTable,va));
    reinterpret_cast<unsigned char*>(first.raw())[5]=0x51;
    unsigned char value=0;uint64_t position=5;
    CHECK(FileObjectRead(&backend.file,&vnode,&value,1,&position)==1&&value==0x51);
    CHECK(!backend.bytes[5]);
    // A read-only mapping has no dirty exposure until mprotect grants write.
    install(2,pageSize,1);
    auto second=VmmLeafPhysical(VmmReadLeaf(space->pageTable,va+2*pageSize));
    reinterpret_cast<unsigned char*>(second.raw())[7]=0x62;
    CHECK(UserMappingSync(&actor,space,va.raw()+2*pageSize,pageSize,4)==0);
    CHECK(!backend.bytes[pageSize+7]);
    CHECK(UserMappingProtect(&actor,space,va.raw()+2*pageSize,pageSize,0)==0);
    CHECK(UserMappingProtect(&actor,space,va.raw()+2*pageSize,pageSize,3)==0);
    CHECK(VmmReadLeaf(space->pageTable,va+2*pageSize)&VMM_WRITABLE);
    CHECK(UserMappingSync(&actor,space,va.raw()+2*pageSize,1,4)==0);
    CHECK(backend.bytes[pageSize+7]==0x62&&!backend.bytes[5]);
    // Zero/invalid flags, alignment and holes do not silently become success.
    CHECK(UserMappingSync(&actor,space,va.raw(),0,4)==0);
    CHECK(UserMappingSync(&actor,space,va.raw()+1,pageSize,4)==-22);
    CHECK(UserMappingSync(&actor,space,va.raw(),pageSize,5)==-22);
    CHECK(UserMappingSync(&actor,space,va.raw(),pageSize,8)==-22);
    CHECK(UserMappingSync(&actor,space,va.raw(),~uint64_t{0},4)==-12);
    CHECK(UserMappingSync(&actor,space,va.raw(),3*pageSize,4)==-12);
    CHECK(backend.bytes[5]==0x51);
    backend.failWrite=true;
    CHECK(UserMappingSync(&actor,space,va.raw(),pageSize,4)==-5);
    backend.failWrite=false;backend.failSync=true;
    CHECK(UserMappingSync(&actor,space,va.raw(),pageSize,4)==-5);
    backend.failSync=false;
    CHECK(UserMappingSync(&actor,space,va.raw(),pageSize,0)==0);
    CHECK(UserMappingSync(&actor,space,va.raw(),pageSize,1)==0);
    CHECK(UserMappingSync(&actor,space,va.raw(),pageSize,6)==0);
    install(3,0,3,false); // Never expose an uncached/noncanonical shared frame.
    CHECK(!VmmReadLeaf(space->pageTable,va+3*pageSize));
    MappingBackingUnref(backing); // VMA is now the descriptor's only owner.
    backend.unmapOnSync=true;
    CHECK(UserMappingSync(&actor,space,va.raw(),pageSize,4)==0);
    CHECK(!space->ranges.head&&vnode.refCount==2);
    CHECK(FileObjectWritebackAll()==0&&vnode.refCount==1&&!backend.file.writebackVnode);
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    FileObjectDispose(&backend.file);
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("SHARED FILE PUBLICATION/SYNC PASS canonical/exposure/protection/range/errors/unmap-during-sync/cleanup");
}
}
// Real kernel allocation/rollback/free paths; the barrier is explicitly modeled
// here. This does not substitute for native SMP TLB-barrier acceptance.
static void freeEmptyKernelTables(PhysicalAddress physical, unsigned level) {
    auto* entries=reinterpret_cast<uint64_t*>(physical.raw());
    for (unsigned i=0;i<512;++i) if (entries[i]&VMM_PRESENT) {
        CHECK(level>1 && !(entries[i]&(1ULL<<7)));
        freeEmptyKernelTables(VmmLeafPhysical(entries[i]),level-1);
    }
    PmmFreePage(physical);
}
static void vmallocIntegration() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto warm=VmmAllocPages(2);CHECK(warm);
    VmmFreePages(warm,2);CHECK(VmmBeginReclaim());VmmCompleteReclaim();
    // Existing page tables remove intermediate allocation noise from this
    // injection: two data pages map, then the third PMM allocation fails.
    failFrameAfter=2;CHECK(!VmmAllocPages(8));CHECK(failFrameAfter==-1);
    for (unsigned i=0;i<8;++i) CHECK(!VmmReadLeaf(KernelPageTable,warm+i*pageSize));
    auto other=VmmAllocPages(2);CHECK(other && other.raw()>=warm.raw()+8*pageSize);
    CHECK(VmmBeginReclaim());
    VmmFreePages(other,2); // belongs to the NEXT barrier generation
    VmmCompleteReclaim();
    auto reused=VmmAllocPages(8);CHECK(reused==warm);
    CHECK(VmmReadLeaf(KernelPageTable,reused));
    VmmFreePages(reused,7); // wrong-size free must leave ownership intact
    CHECK(VmmReadLeaf(KernelPageTable,reused));
    VmmFreePages(reused,8);VmmFreePages(reused,8); // repeated free is harmless
    CHECK(VmmBeginReclaim());VmmCompleteReclaim();
    auto stack=VmmAllocKernelStack(2);CHECK(stack==warm+pageSize);
    CHECK(!VmmReadLeaf(KernelPageTable,stack-pageSize));
    CHECK(!VmmReadLeaf(KernelPageTable,stack+2*pageSize));
    CHECK(VmmReadLeaf(KernelPageTable,stack));
    VmmFreeKernelStack(stack,2);CHECK(VmmBeginReclaim());VmmCompleteReclaim();
    // The bounded diagnostic registry must not limit safe ordinary frees.
    VirtualAddress many[1100];
    for (auto& address:many) {address=VmmAllocPages(1);CHECK(address);}
    for (auto address:many) VmmFreePages(address,1);
    CHECK(VmmBeginReclaim());VmmCompleteReclaim();
    auto combined=VmmAllocPages(1100);CHECK(combined);
    VmmFreePages(combined,1100);CHECK(VmmBeginReclaim());VmmCompleteReclaim();
    CHECK(!VmmBeginReclaim());
    freeEmptyKernelTables(root.pml4,4);VmmHostSetKernelRoot({});empty();
    std::puts("VMALLOC INTEGRATION PASS partial failure/delayed reuse/guards/wrong-size/double-free/1100 allocations");
}
static void robustExitQueue() {
    Process process{};process.state=ProcessState::Terminated;process.runningOnCpu=2;process.robustListHead=0x4000;
    ProcessQueueRobustExit(&process);ProcessQueueRobustExit(&process);
    CHECK(process.refCount==1 && ProcessGetCleanupStatus().queuedProcesses==1);
    CHECK(ProcessCleanupStep() && robustCleanups==0 && robustDelays==1 && process.refCount==1);
    process.runningOnCpu=-1;
    CHECK(ProcessCleanupStep() && robustCleanups==1 && process.refCount==0);
    CHECK(!ProcessGetCleanupStatus().queuedProcesses && !ProcessGetCleanupStatus().activeFinalizers);
    ProcessQueueRobustExit(&process);CHECK(!ProcessCleanupStep() && robustCleanups==1);
    // Raw PI owners need the same forced-exit cleanup even without a registered
    // robust list. Queue once, wait for off-CPU, then release the lifetime pin.
    Process rawOwner{};rawOwner.state=ProcessState::Terminated;rawOwner.runningOnCpu=-1;
    CHECK(!rawOwner.robustListHead);
    ProcessQueueRobustExit(&rawOwner);ProcessQueueRobustExit(&rawOwner);
    CHECK(rawOwner.refCount==1 && ProcessGetCleanupStatus().queuedProcesses==1);
    CHECK(ProcessCleanupStep() && robustCleanups==2 && rawOwner.refCount==0);
    ProcessQueueRobustExit(&rawOwner);CHECK(!ProcessCleanupStep() && robustCleanups==2);
}
static void kernelAliasInspection() {
    void* region=mmap(nullptr,4*pageSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(region!=MAP_FAILED);aliasTestBase=reinterpret_cast<uint64_t>(region);CHECK(aliasTestBase<1ull<<34);
    auto* root=static_cast<uint64_t*>(region);auto* pdpt=root+512;auto* pd=pdpt+512;auto* pt=pd+512;
    constexpr uint64_t target=0x12345000;
    root[0]=(aliasTestBase+pageSize)|3;pdpt[0]=(aliasTestBase+2*pageSize)|3;
    pd[(target>>21)&511]=(aliasTestBase+3*pageSize)|3;pt[(target>>12)&511]=target|3;
    VmmHostSetKernelRoot(PageTable(PhysicalAddress(aliasTestBase)));KernelAliasMapping identity{},direct{};
    CHECK(VmmInspectKernelAliases(target,identity,direct) && identity.present && direct.present &&
          identity.physical==target && direct.physical==target && identity.pageShift==12 && identity.patIndex==0);
    aliasTestDeny=true;CHECK(!VmmInspectKernelAliases(target,identity,direct) && !identity.present && !direct.present);
    aliasTestDeny=false;VmmHostSetKernelRoot({});aliasTestBase=0;CHECK(!munmap(region,4*pageSize));
}
int main(int argc,char** argv) {
    if(argc==2 && !std::strcmp(argv[1],"--benchmark-mapping-lookup")) { rangeLookupBenchmark();return 0; }
    kernelAliasInspection();robustExitQueue();low32Mappings();pageableTableSkeleton();
    accessedLeafAging();competingInstallAndDetach();detachReinstall();largeLeafAndBorrowedFrame();occupiedAncestors();failureAndInPlace();
    cowRetirement(false);for(unsigned i=0;i<32;++i)cowRetirement(true);
    addressSpaceLifetime();consumingWaitCleanup();mappingTransactions();rangeTableEdits();rangeLookupCache();rangeSnapshotRetirement();rangeLeafOwnership();
    liveMappingClone();sharedMappingClone();borrowedDeviceCachePolicies();failedCloneAcknowledgement();sparseLiveRemap();preparedThreadStack();graphicsBufferMappings();
    fileMappingRegistrations();fileMappingGateOom();addressFileGateScope();targetFileInvalidation();targetPrivateRestore();privateRestoreObservedEof();
    targetQuarantineRecovery();pageAccessWithoutUnrelatedGate();
    anonymousIntervalIsolation();anonymousDiscard();memoryCensus();sharedFilePublicationAndSync();exactAddressNoReplace();
    const auto cleanup=ProcessGetCleanupStatus();
    CHECK(!cleanup.liveUserAddressSpaces&&!cleanup.queuedProcesses&&!cleanup.queuedAddressSpaces&&!cleanup.activeFinalizers);
    vmallocIntegration();
    std::puts("VMM LEAF PASS install/detach races=64 concurrent-cow=32 retirement/OOM/in-place/reuse");
}
