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
#include "memory/mapping_transaction.h"
#include "memory/user_mapping.h"
#include "memory/file_object.h"
#include "memory/file_mapping_registry.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "serial.h"
#include "memory/address_space.h"
#include "process.h"
#include "vfs.h"
#include "panic.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); std::abort(); } } while (0)
using namespace brook;
namespace {
constexpr size_t pageSize = 4096, pageCount = 1024;
alignas(pageSize) unsigned char frames[pageCount][pageSize];
struct Frame { unsigned refs=0, maps=0; } records[pageCount];
std::mutex frameLock;
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
        pageAccessPauseOnce=false;pageAccessPause->park();
    }
    hostMutex(mutex).lock();
}
void KMutexUnlock(KMutex* mutex) { hostMutex(mutex).unlock(); }
void SchedulerKernelRegionEnter(Process* process) { ++process->kernelRegionDepth;ProcessRef(process); }
void SchedulerKernelRegionLeave(Process* process) { CHECK(process->kernelRegionDepth);--process->kernelRegionDepth;ProcessUnref(process); }
void SchedulerUnblock(Process* process) {
    CHECK(expectedCompletion && expectedCompletion->done);
    CHECK(process==expectedCompletion->waiter && process->refCount==1);
    CHECK(unpinnedVnodes==2 && closedVnodes==2);
    ++cleanupWakeups;
}
void ProcessDestroyNow(Process* process) {
    CHECK(process==expectedDestroy);
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
    uint64_t identity=old->identity;
    const int ownedAllocations=heapAllocations.load();
    AddressSpaceUnref(old);CHECK(PmmGetRefCount(file)==1&&heapAllocations==ownedAllocations);
    Gate ack;pendingAcknowledgement=&ack;
    std::thread cleanup([]{CHECK(ProcessCleanupStep());});ack.wait(1);
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
    CHECK(!completion.done&&heapAllocations==4);
    CHECK(ProcessCleanupStep());
    CHECK(completion.done&&cleanupWakeups==1&&!waiter.refCount);
    CHECK(!PmmGetRefCount(page)&&heapAllocations==1);
    // Work enqueued while draining the finite retirement batch stays queued;
    // it cannot postpone this child's consuming wait indefinitely.
    CHECK(ProcessCleanupStep()&&!ProcessCleanupStep()&&heapAllocations==0);
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
        CHECK(!operation.Prepare(1,1,1));CHECK(VmmVirtToPhys(space->pageTable,va)==replacement);
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
    CHECK(UserMappingPageAccess(&actor,source,va.raw(),2,true)==UserPageAccess::Ready);
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
    CHECK(UserMappingProtect(&actor,source,va.raw()+2*pageSize,pageSize,1)==0);
    CHECK(UserMappingClone(&actor,source,child,32));
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
    gates.Finish();
    for(unsigned i=0;i<10;++i)CHECK(backings[i]->references+aliases[i]->references==before[i]);
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    for(unsigned i=0;i<10;++i){CHECK(!FileMappingRegisteredRanges(&files[i]));MappingBackingUnref(backings[i]);MappingBackingUnref(aliases[i]);FileObjectDispose(&files[i]);}
    CHECK(released==20);VmmDestroyUserPageTable(root);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("FILE GATES PASS dynamic-capacity/OOM-unchanged/brk-contract/no-reference-leaks");
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
    // the gap before full-gate acquisition. The slow path must recheck and
    // deny the write, without allocating/copying a replacement physical page.
    KMutexLock(&space->mutation);cow(space->pageTable);KMutexUnlock(&space->mutation);
    PmmRefPage(physical); // external pin forces copy if stale CoW were attempted
    Gate beforeFullGate;pageAccessPauseMutex=&unrelated.gate;pageAccessPause=&beforeFullGate;
    UserPageAccess result=UserPageAccess::Ready;
    std::thread writer([&] {
        pageAccessPauseOnce=true;
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
    VmmDestroyUserPageTable(pt);
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();CHECK(heapAllocations==0);
    std::puts("MEMORY CENSUS PASS lifetime/publication/ceiling/sparse/hidden/aliases/bounded-batches");
}

int main(int argc,char** argv) {
    if(argc==2 && !std::strcmp(argv[1],"--benchmark-mapping-lookup")) { rangeLookupBenchmark();return 0; }
    competingInstallAndDetach();detachReinstall();largeLeafAndBorrowedFrame();occupiedAncestors();failureAndInPlace();
    cowRetirement(false);for(unsigned i=0;i<32;++i)cowRetirement(true);
    addressSpaceLifetime();consumingWaitCleanup();mappingTransactions();rangeTableEdits();rangeLookupCache();rangeSnapshotRetirement();rangeLeafOwnership();
    liveMappingClone();sharedMappingClone();failedCloneAcknowledgement();sparseLiveRemap();preparedThreadStack();graphicsBufferMappings();
    fileMappingRegistrations();fileMappingGateOom();targetFileInvalidation();
    pageAccessWithoutUnrelatedGate();
    anonymousDiscard();memoryCensus();
    std::puts("VMM LEAF PASS install/detach races=64 concurrent-cow=32 retirement/OOM/in-place/reuse");
}
