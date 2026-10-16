#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "memory/file_page_cache.h"
#include "memory/physical_memory.h"
#include "vfs.h"
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); std::abort(); } } while (0)
using namespace brook;
namespace {
std::atomic<unsigned> refs[256];
unsigned index(PhysicalAddress physical) { auto i=physical.raw()/4096; CHECK(i>0 && i<256); return i; }
PhysicalAddress fresh(unsigned i) { CHECK(i>0 && i<256); CHECK(refs[i].fetch_add(1)==0); return PhysicalAddress(i*4096); }
void release_entries(FilePageCacheEntry* entries, size_t count) {
    for (size_t i=0;i<count;++i) if(entries[i].cacheId) {
        PmmUnrefPage(entries[i].physical);
        CHECK(__atomic_fetch_sub(&entries[i].vnode->refCount,1,__ATOMIC_ACQ_REL)>1);
    }
}
struct Gate {
    std::mutex lock; std::condition_variable cv; unsigned arrived=0;
    void rendezvous(unsigned total) { std::unique_lock<std::mutex> held(lock); ++arrived; cv.notify_all(); cv.wait(held,[&]{return arrived==total;}); }
};
}
namespace brook {
void PmmRefPage(PhysicalAddress physical) { CHECK(refs[index(physical)].fetch_add(1)>0); }
void PmmUnrefPage(PhysicalAddress physical) { CHECK(refs[index(physical)].fetch_sub(1)>0); }
}
int main() {
    FilePageCacheEntry entries[8]{}; FilePageCache cache(entries,8);
    Vnode first{}; first.cacheId=17; first.refCount=1;
    Vnode second{}; second.cacheId=17; second.refCount=1;
    CHECK(!cache.Lookup(&first,0));
    // Independent opens racing an already completed read must publish one
    // physical winner. Every caller releases its returned reference; the one
    // cached frame and exactly one vnode reference remain.
    Gate gate; PhysicalAddress results[8]; std::vector<std::thread> workers;
    for(unsigned i=0;i<8;++i) workers.emplace_back([&,i] {
        auto candidate=fresh(i+1); gate.rendezvous(8);
        results[i]=cache.Adopt(i&1?&first:&second,0,candidate,false);
    });
    for(auto& worker:workers) worker.join();
    CHECK(results[0]); for(auto result:results) CHECK(result==results[0]);
    CHECK(refs[index(results[0])]==9);
    CHECK(first.refCount+second.refCount==3);
    for(auto result:results) PmmUnrefPage(result);
    auto hit=cache.Lookup(&second,0); CHECK(hit==results[0]);
    CHECK(refs[index(hit)]==2); PmmUnrefPage(hit);
    // Adopting another reference to the winning frame is balanced too.
    auto same=cache.Lookup(&first,0); auto again=cache.Adopt(&second,0,same,false);
    CHECK(again==results[0] && refs[index(again)]==2); PmmUnrefPage(again);
    auto other=cache.Adopt(&first,1,fresh(20),false); CHECK(other.raw()==20*4096); PmmUnrefPage(other);
    Vnode distinct{}; distinct.cacheId=18; distinct.refCount=1;
    other=cache.Adopt(&distinct,0,fresh(21),false); CHECK(other.raw()==21*4096); PmmUnrefPage(other);
    release_entries(entries,8);
    CHECK(first.refCount==1 && second.refCount==1 && distinct.refCount==1);
    // Capacity failure is explicit for canonical users; the existing private
    // read-only cache fallback keeps its candidate but never pretends to cache it.
    FilePageCacheEntry full[1]{}; FilePageCache tiny(full,1);
    auto resident=tiny.Adopt(&first,0,fresh(30),false); PmmUnrefPage(resident);
    CHECK(!tiny.Adopt(&second,1,fresh(31),false)); CHECK(refs[31]==0);
    auto fallback=tiny.Adopt(&second,1,fresh(32),true); CHECK(fallback.raw()==32*4096 && refs[32]==1);
    CHECK(!tiny.Lookup(&second,1)); PmmUnrefPage(fallback);
    release_entries(full,1);
    Vnode disabled{}; disabled.refCount=1;
    CHECK(!tiny.Adopt(&disabled,0,fresh(33),false)); CHECK(refs[33]==0);
    fallback=tiny.Adopt(&disabled,0,fresh(34),true); CHECK(fallback.raw()==34*4096); PmmUnrefPage(fallback);
    CHECK(disabled.refCount==1 && !tiny.Lookup(&disabled,0));
    for(auto& ref:refs) CHECK(ref==0);
    std::puts("FILE_PAGE_CACHE PASS concurrent-winner/refcounts/identity/capacity");
}
