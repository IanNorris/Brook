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
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "serial.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); std::abort(); } } while (0)
using namespace brook;
namespace {
constexpr size_t pageSize = 4096, pageCount = 1024;
alignas(pageSize) unsigned char frames[pageCount][pageSize];
struct Frame { unsigned refs=0, maps=0; } records[pageCount];
std::mutex frameLock;
std::atomic<bool> failAllocation{false};
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
namespace brook {
PhysicalAddress PmmAllocPage(MemTag,uint16_t) {
    if(failAllocation.exchange(false))return {};
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
int main() {
    competingInstallAndDetach();detachReinstall();largeLeafAndBorrowedFrame();occupiedAncestors();failureAndInPlace();
    cowRetirement(false);for(unsigned i=0;i<32;++i)cowRetirement(true);
    std::puts("VMM LEAF PASS install/detach races=64 concurrent-cow=32 retirement/OOM/in-place/reuse");
}
