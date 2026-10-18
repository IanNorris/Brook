#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include "../../kernel/src/drm_map_tokens.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"DRM_MAP_TOKENS line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
struct Gem { bool used; uint64_t mapToken; };
int main() {
    using namespace brook;
    DrmMapTokenAllocator allocator;
    Gem fileA[2] = {{true,allocator.Allocate()}, {true,0}};
    const uint64_t old = fileA[0].mapToken;
    CHECK(DrmFindMapToken(fileA,2,old)==&fileA[0]);
    CHECK(!DrmFindMapToken(fileA,2,0));
    CHECK(!DrmFindMapToken(fileA,2,old+1));
    CHECK(!DrmFindMapToken(fileA,2,old+4096));
    fileA[0].used=false;
    CHECK(!DrmFindMapToken(fileA,2,old));
    fileA[0]={true,allocator.Allocate()};
    CHECK(fileA[0].mapToken!=old && !DrmFindMapToken(fileA,2,old));
    Gem fileB[1]={{true,allocator.Allocate()}};
    CHECK(fileB[0].mapToken!=fileA[0].mapToken);
    CHECK(!DrmFindMapToken(fileB,1,old));
    CHECK(!DrmFindMapToken(fileB,1,fileA[0].mapToken));
    CHECK(DrmFindMapToken(fileB,1,fileB[0].mapToken)==&fileB[0]);
    CHECK(!DrmFindMapToken(fileB,1,DrmMapTokenAllocator::Last+4096));
    // Independent DRM-file locks do not serialize token allocation.
    constexpr unsigned workers=8, count=4096;
    std::vector<std::vector<uint64_t>> output(workers,std::vector<uint64_t>(count));
    std::vector<std::thread> threads;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    for (unsigned i=0;i<workers;++i) threads.emplace_back([&,i]{
        ready.fetch_add(1,std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (auto& token:output[i]) token=allocator.Allocate();
    });
    while (ready.load(std::memory_order_acquire)!=workers) std::this_thread::yield();
    start.store(true,std::memory_order_release);
    for (auto& thread:threads) thread.join();
    std::vector<uint64_t> all;
    for (const auto& row:output) all.insert(all.end(),row.begin(),row.end());
    std::sort(all.begin(),all.end());
    CHECK(all.front()>fileB[0].mapToken);
    for (unsigned i=0;i<all.size();++i) {
        CHECK(all[i] && !(all[i]&4095));
        if (i) CHECK(all[i]!=all[i-1]);
    }
    DrmMapTokenAllocator boundary{DrmMapTokenAllocator::Last};
    CHECK(boundary.Allocate()==DrmMapTokenAllocator::Last);
    for (unsigned i=0;i<1000;++i) CHECK(boundary.Allocate()==0);
    // Contended allocations must also stop at the last token. This exercises
    // CAS retries racing with the transition to the saturated sentinel.
    constexpr unsigned remaining=64, attempts=32;
    DrmMapTokenAllocator racingBoundary{
        DrmMapTokenAllocator::Last-(remaining-1)*4096};
    output.assign(workers,std::vector<uint64_t>(attempts));
    threads.clear();
    ready.store(0,std::memory_order_relaxed);
    start.store(false,std::memory_order_relaxed);
    for (unsigned i=0;i<workers;++i) threads.emplace_back([&,i]{
        ready.fetch_add(1,std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (auto& token:output[i]) token=racingBoundary.Allocate();
    });
    while (ready.load(std::memory_order_acquire)!=workers) std::this_thread::yield();
    start.store(true,std::memory_order_release);
    for (auto& thread:threads) thread.join();
    all.clear();
    for (const auto& row:output)
        for (auto token:row) if (token) all.push_back(token);
    std::sort(all.begin(),all.end());
    CHECK(all.size()==remaining);
    for (unsigned i=0;i<all.size();++i)
        CHECK(all[i]==DrmMapTokenAllocator::Last-(remaining-1-i)*4096);
    CHECK(racingBoundary.Allocate()==0);
    std::puts("DRM_MAP_TOKENS_PASS stale-slot/context/cross-file/concurrent/saturation");
}
