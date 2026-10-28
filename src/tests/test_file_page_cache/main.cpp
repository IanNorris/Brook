#include <atomic>
#include <algorithm>
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
std::atomic<unsigned> refs[1024],maps[1024];
unsigned index(PhysicalAddress physical) { auto i=physical.raw()/4096; CHECK(i>0 && i<1024); return i; }
PhysicalAddress fresh(unsigned i) { CHECK(i>0 && i<1024); CHECK(refs[i].fetch_add(1)==0); return PhysicalAddress(i*4096); }
void release_entries(FilePageCache& cache) {
    while(cache.Count()){auto result=cache.ReclaimStep(65536);CHECK(result.reclaimed);}
}
struct Gate {
    std::mutex lock; std::condition_variable cv; unsigned arrived=0;
    void rendezvous(unsigned total) { std::unique_lock<std::mutex> held(lock); ++arrived; cv.notify_all(); cv.wait(held,[&]{return arrived==total;}); }
};
}
namespace brook {
void PmmRefPage(PhysicalAddress physical) { CHECK(refs[index(physical)].fetch_add(1)>0); }
void PmmUnrefPage(PhysicalAddress physical) { CHECK(refs[index(physical)].fetch_sub(1)>0); }
uint32_t PmmGetRefCount(PhysicalAddress physical){return refs[index(physical)].load();}
uint32_t PmmGetMapCount(PhysicalAddress p){return maps[index(p)].load();}
}
int main() {
    std::vector<uint32_t> issued[8];std::thread allocators[8];
    for(unsigned i=0;i<8;++i)allocators[i]=std::thread([&,i]{for(unsigned n=0;n<1000;++n)issued[i].push_back(FilePageCacheAllocateNamespace());});
    for(auto& thread:allocators)thread.join();std::vector<uint32_t> all;
    for(auto& batch:issued)all.insert(all.end(),batch.begin(),batch.end());
    std::sort(all.begin(),all.end());CHECK(all.front()!=0 && std::adjacent_find(all.begin(),all.end())==all.end());
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
    CHECK(first.refCount+second.refCount==2);
    for(auto result:results) PmmUnrefPage(result);
    auto hit=cache.Lookup(&second,0); CHECK(hit==results[0]);
    CHECK(refs[index(hit)]==2); PmmUnrefPage(hit);
    // Adopting another reference to the winning frame is balanced too.
    auto same=cache.Lookup(&first,0); auto again=cache.Adopt(&second,0,same,false);
    CHECK(again==results[0] && refs[index(again)]==2); PmmUnrefPage(again);
    auto other=cache.Adopt(&first,1,fresh(20),false); CHECK(other.raw()==20*4096); PmmUnrefPage(other);
    Vnode distinct{}; distinct.cacheId=18; distinct.refCount=1;
    other=cache.Adopt(&distinct,0,fresh(21),false); CHECK(other.raw()==21*4096); PmmUnrefPage(other);
    release_entries(cache);
    CHECK(first.refCount==1 && second.refCount==1 && distinct.refCount==1);
    // Capacity failure is explicit for canonical users; the existing private
    // read-only cache fallback keeps its candidate but never pretends to cache it.
    FilePageCacheEntry full[1]{}; FilePageCache tiny(full,1);
    auto resident=tiny.Adopt(&first,0,fresh(30),false); PmmUnrefPage(resident);
    CHECK(!tiny.Adopt(&second,1,fresh(31),false)); CHECK(refs[31]==0);
    auto fallback=tiny.Adopt(&second,1,fresh(32),true); CHECK(fallback.raw()==32*4096 && refs[32]==1);
    CHECK(!tiny.Lookup(&second,1)); PmmUnrefPage(fallback);
    release_entries(tiny);
    Vnode disabled{}; disabled.refCount=1;
    CHECK(!tiny.Adopt(&disabled,0,fresh(33),false)); CHECK(refs[33]==0);
    fallback=tiny.Adopt(&disabled,0,fresh(34),true); CHECK(fallback.raw()==34*4096); PmmUnrefPage(fallback);
    CHECK(disabled.refCount==1 && !tiny.Lookup(&disabled,0));
    FilePageCacheEntry collisionEntries[8]{};FilePageCache collision(collisionEntries,8);
    for(unsigned i=0;i<3;++i){auto p=collision.Adopt(&first,i*8,fresh(40+i),false);PmmUnrefPage(p);}
    auto pinned=collision.Lookup(&first,8);PmmRefPage(PhysicalAddress(42*4096));maps[42]=1;
    auto reclaimed=collision.ReclaimStep(8);CHECK(reclaimed.scanned==8 && reclaimed.reclaimed==1 && collision.Count()==2 && refs[40]==0);
    auto beyond=collision.Lookup(&first,16);CHECK(beyond.raw()==42*4096);PmmUnrefPage(beyond);
    auto duplicate=collision.Adopt(&first,8,fresh(43),false);CHECK(duplicate==pinned && refs[43]==0 && collision.Count()==2);PmmUnrefPage(duplicate);
    auto replacement=collision.Adopt(&first,24,fresh(44),false);CHECK(replacement.raw()==44*4096 && collision.Count()==3);PmmUnrefPage(replacement);
    PmmUnrefPage(pinned);maps[42]=0;PmmUnrefPage(PhysicalAddress(42*4096));release_entries(collision);
    CHECK(!collision.Lookup(&first,8) && !collision.Count());
    replacement=collision.Adopt(&first,32,fresh(45),false);PmmUnrefPage(replacement);release_entries(collision);
    FilePageCacheEntry racingEntries[8]{};FilePageCache racing(racingEntries,8);Gate race;
    std::thread creator([&]{race.rendezvous(3);for(unsigned i=0;i<200;++i){auto p=racing.Adopt(&first,0,fresh(100+i),false);CHECK(p);PmmUnrefPage(p);std::this_thread::yield();}});
    std::thread lookup([&]{race.rendezvous(3);for(unsigned i=0;i<1000;++i){auto p=racing.Lookup(&first,0);if(p){CHECK(PmmGetRefCount(p)>=2);PmmUnrefPage(p);}std::this_thread::yield();}});
    std::thread reclaimer([&]{race.rendezvous(3);for(unsigned i=0;i<500;++i){auto result=racing.ReclaimStep(8);CHECK(result.scanned==8);std::this_thread::yield();}});
    creator.join();lookup.join();reclaimer.join();release_entries(racing);
    for(auto& ref:refs) CHECK(ref==0);
    std::puts("FILE_PAGE_CACHE PASS concurrent-winner/refcounts/identity/capacity");
}
