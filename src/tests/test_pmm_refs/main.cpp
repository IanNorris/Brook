#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include "memory/physical_memory.h"
#include "memory/oom_recovery.h"
#include "ksym_addrs.h"
#include "serial.h"
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); std::exit(1); } } while (0)
using namespace brook;
namespace brook {
void PmmHostTestInit(PageDescriptor*,uint32_t);
bool hostOomEnabled=false;
uint64_t failureRequests=0,lastRequested=0,lastFree=0;
bool lastContiguous=false;
bool OomRecoveryEnabled(){return hostOomEnabled;}
void OomRecoveryAllocationFailure(uint64_t requested,uint64_t free,bool contiguous) {
    if(!hostOomEnabled)return;
    ++failureRequests;lastRequested=requested;lastFree=free;lastContiguous=contiguous;
    // Prove the failure callback runs outside the PMM guard: this nested
    // kernel allocation would otherwise deadlock. It can use the reserved floor.
    auto emergency=PmmAllocPage(MemTag::KernelData,KernelPid);CHECK(emergency);
    PmmUnrefPage(emergency);
}
extern "C" void PmmDescribe(uint64_t,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*);
void SerialPrintf(const char*,...) {}
void SerialPuts(const char*) {}
bool KsymFindByAddr(uint64_t,const char**,uint64_t*) { return false; }
}
extern "C" [[noreturn]] void KernelPanic(const char*,...) { std::abort(); }
static void describe(PhysicalAddress page,unsigned expectedRefs,unsigned expectedMaps,unsigned expectedUsed) {
    uint32_t used=0,refs=0,maps=0;
    PmmDescribe(page.raw(),&used,&refs,&maps,nullptr,nullptr);
    if (refs!=expectedRefs || maps!=expectedMaps || used!=expectedUsed)
        std::fprintf(stderr,"PMM counts: refs=%u expected=%u maps=%u expected=%u used=%u expected=%u\n",refs,expectedRefs,maps,expectedMaps,used,expectedUsed);
    CHECK(refs==expectedRefs && maps==expectedMaps && used==expectedUsed);
    CHECK(PmmGetRefCount(page)==expectedRefs);
    CHECK(PmmGetMapCount(page)==expectedMaps);
}
static unsigned owned(uint16_t pid) {
    unsigned count=0;
    PmmEnumeratePid(pid,[](PhysicalAddress,MemTag,void* p){++*static_cast<unsigned*>(p);return true;},&count);
    return count;
}
int main() {
    PageDescriptor descriptors[2048]{}; PmmHostTestInit(descriptors,2048);
    auto page=PmmAllocPage(MemTag::User,7); CHECK(page); describe(page,1,0,1);
    constexpr unsigned aliases=70000;
    for(unsigned i=0;i<aliases;++i) {
        if (i&1) CHECK(PmmRefPageIfAlive(page)); else PmmRefPage(page);
        PmmMapInc(page);
        // Catch the old 255 saturation before exercising final release.
        if(i==254) describe(page,256,255,1);
    }
    describe(page,aliases+1,aliases,1);
    for(unsigned i=aliases;i>0;--i) { PmmMapDec(page); PmmUnrefPage(page); }
    describe(page,1,0,1);
    std::vector<std::thread> workers;
    for(unsigned t=0;t<8;++t) workers.emplace_back([&]{
        for(unsigned i=0;i<2048;++i) CHECK(PmmRefPageIfAlive(page));
    });
    for(auto& worker:workers) worker.join();
    describe(page,1+8*2048,0,1); workers.clear();
    for(unsigned t=0;t<8;++t) workers.emplace_back([&]{for(unsigned i=0;i<2048;++i) PmmUnrefPage(page);});
    for(auto& worker:workers) worker.join();
    describe(page,1,0,1);
    PmmUnrefPage(page); describe(page,0,0,0); CHECK(!PmmRefPageIfAlive(page));
    page=PmmAllocPage(MemTag::User,7);CHECK(page);
    CHECK(!PmmReclassifyPrivateFrame(PhysicalAddress(page.raw()+1),MemTag::User,MemTag::KernelData,KernelPid));
    CHECK(!PmmReclassifyPrivateFrame(page,MemTag::KernelData,MemTag::User,8));
    CHECK(!PmmReclassifyPrivateFrame(page,MemTag::User,MemTag::PageTable,8));
    CHECK(!PmmReclassifyPrivateFrame(page,MemTag::User,MemTag::KernelData,PMM_MAX_PIDS));
    PmmRefPage(page);CHECK(!PmmReclassifyPrivateFrame(page,MemTag::User,MemTag::KernelData,KernelPid));PmmUnrefPage(page);
    PmmMapInc(page);CHECK(!PmmReclassifyPrivateFrame(page,MemTag::User,MemTag::KernelData,KernelPid));PmmMapDec(page);
    CHECK(PmmGetTag(page)==MemTag::User && PmmGetPid(page)==7);
    auto kernelBefore=owned(KernelPid),userBefore=owned(8);
    CHECK(PmmReclassifyPrivateFrame(page,MemTag::User,MemTag::KernelData,KernelPid));
    CHECK(owned(KernelPid)==kernelBefore+1);
    CHECK(PmmGetTag(page)==MemTag::KernelData && PmmGetPid(page)==KernelPid);describe(page,1,0,1);
    CHECK(PmmReclassifyPrivateFrame(page,MemTag::KernelData,MemTag::User,8));
    CHECK(owned(KernelPid)==kernelBefore && owned(8)==userBefore+1);
    CHECK(PmmGetTag(page)==MemTag::User && PmmGetPid(page)==8);describe(page,1,0,1);
    PmmUnrefPage(page);CHECK(!PmmReclassifyPrivateFrame(page,MemTag::User,MemTag::KernelData,KernelPid));
    CHECK(owned(8)==userBefore);
    const uint64_t initialFree=PmmGetFreePageCount();hostOomEnabled=true;
    PhysicalAddress held[2048]{};unsigned heldCount=0;
    while(PmmGetFreePageCount()>OomUserReservePages) {
        held[heldCount]=PmmAllocPage(MemTag::User,9);CHECK(held[heldCount]);++heldCount;
    }
    CHECK(PmmGetFreePageCount()==OomUserReservePages);
    CHECK(!PmmAllocPage(MemTag::User,9));
    CHECK(failureRequests==1 && lastRequested==1 && lastFree==OomUserReservePages && !lastContiguous);
    auto kernelEmergency=PmmAllocPage(MemTag::KernelData,KernelPid);CHECK(kernelEmergency);
    CHECK(PmmGetFreePageCount()==OomUserReservePages-1);PmmUnrefPage(kernelEmergency);
    for(unsigned i=0;i<heldCount;++i)PmmUnrefPage(held[i]);
    CHECK(PmmGetFreePageCount()==initialFree);
    CHECK(!PmmAllocPages(initialFree-OomUserReservePages+1,MemTag::User,9));
    CHECK(failureRequests==2 && lastContiguous && lastFree==initialFree);
    const uint64_t bulkCount=initialFree-OomUserReservePages;
    auto bulk=PmmAllocPages(bulkCount,MemTag::User,9);CHECK(bulk);
    CHECK(PmmGetFreePageCount()==OomUserReservePages);
    for(uint64_t i=0;i<bulkCount;++i)PmmUnrefPage(PhysicalAddress(bulk.raw()+i*4096));
    CHECK(PmmGetFreePageCount()==initialFree);hostOomEnabled=false;
    std::puts("PMM_OOM_FLOOR PASS user denial/kernel reserve/bulk boundary/callback outside allocator guard");
    std::puts("PMM_REFS PASS aliases=70000 concurrent-pins=16384 final-release");
}
