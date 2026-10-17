#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "memory/exec_page_cache.h"
#include "memory/physical_memory.h"
#define CHECK(c) do { if(!(c)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort();} } while(0)
using namespace brook;
namespace {
std::mutex frameLock;
std::unordered_map<uint64_t,unsigned> refs;
PhysicalAddress page(unsigned value) {
    auto* memory=static_cast<unsigned char*>(std::aligned_alloc(4096,4096));CHECK(memory);
    std::memset(memory,value,4096);
    std::lock_guard<std::mutex> guard(frameLock);
    auto p=PhysicalAddress(reinterpret_cast<uint64_t>(memory));CHECK(refs.emplace(p.raw(),1).second);return p;
}
uint64_t collision(PhysicalAddress) {return 1;}
void empty() {std::lock_guard<std::mutex> guard(frameLock);CHECK(refs.empty());}
}
namespace brook {
void PmmRefPage(PhysicalAddress p) {std::lock_guard<std::mutex> guard(frameLock);CHECK(refs.at(p.raw()));++refs.at(p.raw());}
void PmmUnrefPage(PhysicalAddress p) {
    std::lock_guard<std::mutex> guard(frameLock);auto found=refs.find(p.raw());CHECK(found!=refs.end()&&found->second);
    if(!--found->second){refs.erase(found);std::free(reinterpret_cast<void*>(p.raw()));}
}
bool PmmHasSingleReference(PhysicalAddress p) {std::lock_guard<std::mutex> guard(frameLock);return refs.at(p.raw())==1;}
uint32_t PmmGetRefCount(PhysicalAddress p) {std::lock_guard<std::mutex> guard(frameLock);return refs.at(p.raw());}
}
int main() {
    // Distinct source frames converge; active refs survive trim and clear.
    {
        ExecPageCacheEntry entries[16]{};ExecPageCache c(entries,16,8);
        auto a=c.Adopt(page(42));auto b=c.Adopt(page(42));CHECK(a==b&&PmmGetRefCount(a)==3);
        CHECK(c.Stats().hits==1&&c.Stats().entries==1&&!c.TrimIdle());
        PmmRefPage(a);auto same=c.Adopt(a);CHECK(same==a&&PmmGetRefCount(a)==4);
        PmmUnrefPage(same);CHECK(c.Clear()==1&&PmmGetRefCount(a)==2);
        PmmUnrefPage(a);PmmUnrefPage(b);empty();
    }
    // Exact collision, hole before canonical winner, and bounded admission.
    {
        ExecPageCacheEntry entries[8]{};ExecPageCache c(entries,8,2,4,collision);
        auto a=c.Adopt(page(1)),b=c.Adopt(page(2));CHECK(a!=b);
        auto uncached=c.Adopt(page(3));CHECK(PmmGetRefCount(uncached)==1&&c.Stats().full==1);
        PmmUnrefPage(uncached);PmmUnrefPage(a);CHECK(c.TrimIdle()==1);
        auto alias=c.Adopt(page(2));CHECK(alias==b&&c.Stats().entries==1);
        CHECK(c.Stats().collisions>=2);
        PmmUnrefPage(alias);PmmUnrefPage(b);CHECK(c.TrimIdle()==1);empty();
    }
    // Probe exhaustion is independent of capacity, and never drops caller ref.
    {
        ExecPageCacheEntry entries[8]{};ExecPageCache c(entries,8,8,1,collision);
        auto a=c.Adopt(page(1)),b=c.Adopt(page(2));CHECK(c.Stats().entries==1&&c.Stats().full==1);
        CHECK(PmmGetRefCount(b)==1);PmmUnrefPage(a);PmmUnrefPage(b);CHECK(c.TrimIdle()==1);empty();
    }
    // Failure cleanup can happen in either order: mapping then staging, or
    // staging then mapping. Only the final external release makes it idle.
    for(bool sourceFirst:{false,true}) {
        ExecPageCacheEntry entries[8]{};ExecPageCache c(entries,8,4);
        auto stage=page(7);PmmRefPage(stage);auto mapping=c.Adopt(stage);
        CHECK(PmmGetRefCount(stage)==3);
        PmmUnrefPage(sourceFirst?stage:mapping);CHECK(!c.TrimIdle());
        PmmUnrefPage(sourceFirst?mapping:stage);CHECK(c.TrimIdle()==1);empty();
    }
    // Concurrent misses and trim: keep a witness ref so every equal candidate
    // must converge even while other workers and a trimmer release refs.
    {
        ExecPageCacheEntry entries[128]{};ExecPageCache c(entries,128,64);
        auto witness=c.Adopt(page(93));std::atomic<bool> done{false};
        std::thread trimmer([&]{while(!done.load()){c.TrimIdle();std::this_thread::yield();}});
        std::vector<std::thread> workers;
        for(unsigned t=0;t<8;++t)workers.emplace_back([&]{
            for(unsigned i=0;i<1000;++i){auto p=c.Adopt(page(93));CHECK(p==witness);PmmUnrefPage(p);}
        });
        for(auto& worker:workers)worker.join();done=true;trimmer.join();
        CHECK(c.Stats().hits==8000&&c.Stats().entries==1&&PmmGetRefCount(witness)==2);
        PmmUnrefPage(witness);CHECK(c.TrimIdle()==1);empty();
    }
    std::puts("exec page cache: content, collisions, bounds, cleanup and concurrency PASS");
}
