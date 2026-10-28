#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <condition_variable>
#include "../../kernel/src/memory/user_access_ranges.h"
using namespace brook;
int main() {
    UserAccessRegistry registry;UserAccessLease outer,inner;
    registry.Add(outer);assert(registry.Count()==1);
    assert(outer.ranges.Add(0x10001,3));assert(registry.CoversPage(0x10000));
    assert(!registry.CoversPage(0x11000));assert(outer.ranges.Add(0x10004,4092));
    assert(outer.ranges.count==1);assert(!registry.CoversPage(0x11000));
    assert(outer.ranges.Add(0x11000,1));assert(outer.ranges.count==1);
    assert(registry.CoversPage(0x11000));
    registry.Add(inner);assert(inner.ranges.Add(0x90000,4096));
    assert(registry.Count()==2 && registry.CoversPage(0x10000) && registry.CoversPage(0x90000));
    assert(registry.Remove(inner));assert(!registry.Remove(inner));
    assert(registry.CoversPage(0x10000) && !registry.CoversPage(0x90000));
    assert(outer.ranges.Add(~uint64_t{0},0));
    assert(!outer.ranges.Add(~uint64_t{0},2));
    assert(!outer.ranges.Add(UserAccessRanges::Limit-1,2));
    assert(outer.ranges.Add(UserAccessRanges::Limit-1,1));
    assert(registry.Remove(outer));assert(!registry.Count());
    registry.Add(outer);
    for(unsigned i=0;i<32;++i)assert(outer.ranges.Add(0x10000+i*8192,1));
    assert(!outer.ranges.whole && outer.ranges.count==32);
    assert(outer.ranges.Add(0x1000000,1));assert(outer.ranges.whole);
    assert(registry.CoversPage(0x4000000));assert(!registry.CoversPage(UserAccessRanges::Limit));
    assert(registry.Remove(outer));
    // The same serialization gate protects range publication and the pager's
    // victim check. The lease remains live while its owner blocks outside it.
    std::mutex gate;std::condition_variable changed;bool published=false,release=false;
    std::thread reader([&] {
        std::unique_lock<std::mutex> lock(gate);registry.Add(outer);
        assert(outer.ranges.Add(0x10000,4096));published=true;changed.notify_all();
        changed.wait(lock,[&]{return release;});assert(registry.Remove(outer));
    });
    {
        std::unique_lock<std::mutex> lock(gate);changed.wait(lock,[&]{return published;});
        assert(registry.CoversPage(0x10000));assert(!registry.CoversPage(0x20000));
        release=true;changed.notify_all();
    }
    reader.join();assert(!registry.Count() && !registry.CoversPage(0x10000));
}
