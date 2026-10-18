#include <cstdio>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <set>
#include "process_ids.h"
namespace {
int failures=0;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"PROCESS_IDS FAIL %d: %s\n",__LINE__,#x);++failures;}}while(0)
}
int main() {
    brook::ProcessIdPool<1024> pool;
    std::set<uint16_t> ids;
    for(unsigned i=1;i<1024;++i){auto id=pool.Allocate(true);CHECK(id==i);CHECK(ids.insert(id).second);}
    for(unsigned i=0;i<100;++i)CHECK(pool.Allocate(true)==0);
    CHECK(pool.Pending()==1023);CHECK(pool.SetUserClosed(true)==1023);
    CHECK(!pool.Release(0));CHECK(!pool.Release(1024));CHECK(!pool.Publish(0));
    for(auto id:ids)CHECK(pool.Publish(id));
    CHECK(pool.Pending()==0);CHECK(pool.UserClosed());
    CHECK(pool.Release(17));CHECK(!pool.Release(17));
    CHECK(pool.Allocate(true)==0);CHECK(pool.Allocate(false)==17);
    CHECK(!pool.Publish(17));CHECK(pool.Pending()==0);
    CHECK(pool.Release(17));CHECK(pool.SetUserClosed(false)==0);
    CHECK(pool.Allocate(true)==17);CHECK(pool.Pending()==1);
    CHECK(pool.Release(17));CHECK(pool.Pending()==0);CHECK(pool.Allocate(true)==17);
    CHECK(pool.Publish(17));CHECK(!pool.Publish(17));CHECK(pool.Pending()==0);

    // Exercise the largest uint16_t pool and exhaustion without wrapping next.
    brook::ProcessIdPool<65536> large;
    for(unsigned i=1;i<65536;++i)CHECK(large.Allocate(false)==i);
    CHECK(large.Allocate(false)==0);CHECK(large.Release(65535));CHECK(large.Allocate(true)==65535);

    // Owner-lock interleavings: a fence sees each construction reservation,
    // while already admitted births can still publish or unwind after closure.
    brook::ProcessIdPool<1024> concurrent;std::mutex mutex;
    std::atomic<unsigned> reserved{0};std::atomic<bool> closed{false};
    std::atomic<unsigned> races{0};std::vector<std::thread> threads;
    for(unsigned i=0;i<8;++i)threads.emplace_back([&,i]{
        uint16_t id;
        {std::lock_guard<std::mutex> lock(mutex);id=concurrent.Allocate(true);}
        if(!id){++races;return;}++reserved;
        while(!closed)std::this_thread::yield();
        {std::lock_guard<std::mutex> lock(mutex);
         if(concurrent.Allocate(true))++races;
         if(i&1){if(!concurrent.Publish(id))++races;}
         else if(!concurrent.Release(id))++races;}
    });
    while(reserved!=8)std::this_thread::yield();
    {std::lock_guard<std::mutex> lock(mutex);CHECK(concurrent.SetUserClosed(true)==8);}
    closed=true;for(auto& t:threads)t.join();
    CHECK(races==0);CHECK(concurrent.Pending()==0);CHECK(concurrent.UserClosed());
    CHECK(concurrent.SetUserClosed(false)==0);
    std::set<uint16_t> live;
    for(unsigned n=0;n<100000;++n) {
        auto id=concurrent.Allocate(true);CHECK(id!=0);
        CHECK(live.insert(id).second);CHECK(concurrent.Pending()==1);
        CHECK(concurrent.Publish(id));CHECK(concurrent.Pending()==0);
        live.erase(id);CHECK(concurrent.Release(id));
    }
    std::printf("PROCESS_IDS %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
