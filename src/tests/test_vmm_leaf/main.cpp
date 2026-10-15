// Real VMM walks, locks and PTE transitions; platform/PMM are instrumented.
// A delayed acknowledgement is modeled explicitly, not a hardware TLB test.
#include <atomic>
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
void KMutexLock(KMutex* mutex) { hostMutex(mutex).lock(); }
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
void PmmRefPage(PhysicalAddress p) {
    std::lock_guard<std::mutex> lock(frameLock);auto& r=records[indexOf(p)];CHECK(r.refs&&r.refs<254);++r.refs;
}
bool PmmRefPageIfAlive(PhysicalAddress p) {
    std::lock_guard<std::mutex> lock(frameLock);auto& r=records[indexOf(p)];if(!r.refs)return false;CHECK(r.refs<254);++r.refs;return true;
}
void PmmUnrefPage(PhysicalAddress p) { std::lock_guard<std::mutex> lock(frameLock);dropLocked(indexOf(p)); }
void PmmFreePage(PhysicalAddress p) { std::lock_guard<std::mutex> lock(frameLock);CHECK(records[indexOf(p)].refs==1);dropLocked(indexOf(p)); }
uint8_t PmmGetRefCount(PhysicalAddress p) { std::lock_guard<std::mutex> lock(frameLock);return records[indexOf(p)].refs; }
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
        CHECK(*large==saved&&!VmmDetachPage(pt,va));
        CHECK(reinterpret_cast<uint64_t*>(pt.pml4.raw())[0]==rootEntry&&entries[1]==directoryEntry);
        CHECK(PmmGetRefCount(candidate)==1&&maps(candidate)==0);
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
static void addressSpaceLifetime() {
    auto kernelRoot=table();VmmHostSetKernelRoot(kernelRoot);AddressSpaceKernelInit();
    auto* immortal=AddressSpaceKernel();AddressSpaceRef(immortal);AddressSpaceUnref(immortal);CHECK(!ProcessCleanupStep());
    failAllocation=true;CHECK(!AddressSpaceCreate(17)&&heapAllocations==0);
    auto* old=AddressSpaceCreate(17);CHECK(old&&old->references==1&&old->identity&&old->generation);
    AddressSpaceRef(old);CHECK(old->references==2);
    auto file=PmmAllocPage(),memfd=PmmAllocPage(),framebuffer=PmmAllocPage(),window=PmmAllocPage();
    Vnode vnode{};vnode.refCount=1;BorrowedBacking backing{memfd};
    old->fileMaps[0]={va.raw(),4096,0,flags,&vnode};
    old->memfdMaps[0]={va.raw()+4096,4096,0,flags,&backing,3,3};
    old->fbMaps[0]={va.raw()+8192,4096};
    CHECK(VmmTryMapPage(old->pageTable,va,file,flags)==VmmMapResult::Mapped);
    CHECK(VmmTryMapPage(old->pageTable,va+4096,memfd,flags)==VmmMapResult::Mapped);
    CHECK(VmmTryMapPage(old->pageTable,va+8192,framebuffer,flags,MemTag::Device)==VmmMapResult::Mapped);
    // Native window/compositor aliases have no fbMaps entry but still borrow.
    CHECK(VmmTryMapPage(old->pageTable,va+12288,window,flags,MemTag::Device)==VmmMapResult::Mapped);
    AddressSpaceUnref(old);CHECK(old->references==1&&!ProcessCleanupStep());
    uint64_t identity=old->identity;
    AddressSpaceUnref(old);CHECK(PmmGetRefCount(file)==1&&heapAllocations==1);
    Gate ack;pendingAcknowledgement=&ack;
    std::thread cleanup([]{CHECK(ProcessCleanupStep());});ack.wait(1);
    // Old AS is queued and still owns its pages through the acknowledgement.
    CHECK(PmmGetRefCount(file)==1&&PmmGetRefCount(memfd)==1&&maps(framebuffer)==1);
    auto* replacement=AddressSpaceCreate(17);CHECK(replacement&&replacement->identity!=identity);
    auto fresh=PmmAllocPage();CHECK(VmmTryMapPage(replacement->pageTable,va,fresh,flags)==VmmMapResult::Mapped);
    ack.release();cleanup.join();pendingAcknowledgement=nullptr;
    CHECK(!PmmGetRefCount(file)&&!PmmGetRefCount(memfd));
    CHECK(PmmGetRefCount(framebuffer)==1&&maps(framebuffer)==0);
    CHECK(closedVnodes==1&&unpinnedVnodes==1&&droppedMemfds==1);
    CHECK(VmmVirtToPhys(replacement->pageTable,va)==fresh&&PmmGetRefCount(fresh)==1);
    CHECK(PmmGetRefCount(window)==1&&maps(window)==0);
    CHECK(heapAllocations==1);PmmFreePage(framebuffer);PmmFreePage(window);
    AddressSpaceUnref(replacement);CHECK(ProcessCleanupStep()&&!ProcessCleanupStep());
    CHECK(heapAllocations==0);PmmFreePage(kernelRoot.pml4);VmmHostSetKernelRoot({});empty();
    std::puts("ADDRESS SPACE PASS refs/identity/queued-cleanup/borrowed-backing/reused-pid/OOM");
}
static void consumingWaitCleanup() {
    auto root=table();VmmHostSetKernelRoot(root);
    auto* retiring=AddressSpaceCreate(23);lateRetirement=AddressSpaceCreate(24);
    CHECK(retiring&&lateRetirement);
    auto page=PmmAllocPage();Vnode vnode{};vnode.refCount=1;
    retiring->fileMaps[0]={va.raw(),4096,0,flags,&vnode};
    CHECK(VmmTryMapPage(retiring->pageTable,va,page,flags)==VmmMapResult::Mapped);
    Process waiter{},child{};ProcessCleanupCompletion completion{&waiter,false};
    child.addressSpace=retiring;child.destroyCompletion=&completion;
    expectedDestroy=&child;expectedCompletion=&completion;
    ProcessQueueDestroy(&child);
    CHECK(!completion.done&&heapAllocations==2);
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
struct BackingRetirementWitness { AddressSpaceState* space; PhysicalAddress physical; bool released=false; };
static void releaseTransactionBacking(void* opaque) {
    auto* witness=static_cast<BackingRetirementWitness*>(opaque);
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
    BackingRetirementWitness witness{space,borrowed};Gate backingAck;pendingAcknowledgement=&backingAck;
    std::thread backingRemover([&]{
        MappingTransaction operation(&actor,space);CHECK(operation.Prepare(1,0,1));operation.Lock();
        CHECK(operation.Detach(va+8192));CHECK(operation.RetireBacking(&witness,releaseTransactionBacking));
    });
    backingAck.wait(1);CHECK(!witness.released&&PmmGetRefCount(borrowed)==1);
    backingAck.release();backingRemover.join();pendingAcknowledgement=nullptr;
    CHECK(witness.released&&!PmmGetRefCount(borrowed));
    CHECK(!actor.kernelRegionDepth&&!actor.refCount&&space->references==1&&heapAllocations==1);
    AddressSpaceUnref(space);CHECK(ProcessCleanupStep());
    PmmFreePage(root.pml4);VmmHostSetKernelRoot({});empty();
    std::puts("MAPPING TRANSACTION PASS delayed-ACK/unlocked-retirement/zero-publication/rollback/replacement/OOM");
}
int main() {
    competingInstallAndDetach();detachReinstall();largeLeafAndBorrowedFrame();occupiedAncestors();failureAndInPlace();
    cowRetirement(false);for(unsigned i=0;i<32;++i)cowRetirement(true);
    addressSpaceLifetime();consumingWaitCleanup();mappingTransactions();
    std::puts("VMM LEAF PASS install/detach races=64 concurrent-cow=32 retirement/OOM/in-place/reuse");
}
