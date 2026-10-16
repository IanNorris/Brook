#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "memory/mapping_pin_registry.h"
#include "vfs.h"
#define CHECK(c) do { if(!(c)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);std::abort();} } while(0)
using namespace brook;
namespace {
std::atomic<int> allocations{0},calls{0},failAfter{-1};
MappingPinRegistry* allocationProbe=nullptr;
struct Gate {
    std::mutex mutex;std::condition_variable condition;bool reached=false,released=false;
    void park(){std::unique_lock<std::mutex> held(mutex);reached=true;condition.notify_all();condition.wait(held,[&]{return released;});}
    void wait(){std::unique_lock<std::mutex> held(mutex);condition.wait(held,[&]{return reached;});}
    void release(){std::lock_guard<std::mutex> held(mutex);released=true;condition.notify_all();}
};
}
namespace brook {
extern "C" void* kmalloc(uint64_t bytes) {
    ++calls;
    // Re-enter a registry query to detect allocation/free under its IRQ lock.
    if(allocationProbe)CHECK(!allocationProbe->Busy(999,999));
    int remaining=failAfter.load();
    if(remaining>=0 && failAfter.fetch_sub(1)==0)return nullptr;
    void* result=std::malloc(bytes);CHECK(result);++allocations;return result;
}
extern "C" void kfree(void* pointer) {
    if(!pointer)return;
    if(allocationProbe)CHECK(!allocationProbe->Busy(999,999));
    --allocations;std::free(pointer);
}
}
extern "C" [[noreturn]] void KernelPanic(const char*,...) { std::abort(); }
static void scaleAndIdentity() {
    MappingPinRegistry registry;allocationProbe=&registry;
    std::vector<Vnode> vnodes(4096);
    for(unsigned i=0;i<vnodes.size();++i)CHECK(registry.Pin(&vnodes[i],7,11)==0);
    CHECK(allocations==4097); // one identity, thousands of independent opens
    CHECK(registry.Busy(7,11) && registry.Busy(7,777,true));
    CHECK(!registry.Busy(8,11) && !registry.Busy(7,12));
    int before=calls;
    for(auto& vnode:vnodes)CHECK(registry.RetainIfPinned(&vnode));
    for(auto& vnode:vnodes)registry.Unpin(&vnode);
    CHECK(calls==before && allocations==4097 && registry.Busy(7,11));
    for(unsigned i=0;i<vnodes.size()-1;++i)registry.Unpin(&vnodes[i]);
    CHECK(allocations==2 && registry.Busy(7,11));
    registry.Unpin(&vnodes.back());CHECK(!registry.Busy(7,11) && allocations==0);
    // Distinct identities also exceed the old 1024-slot bound; different
    // devices with the same inode must remain independent.
    for(unsigned i=0;i<vnodes.size();++i)CHECK(registry.Pin(&vnodes[i],20+i%2,1+i/2)==0);
    CHECK(allocations==8192);
    for(unsigned i=0;i<vnodes.size();++i) {
        CHECK(registry.Busy(20+i%2,1+i/2));registry.Unpin(&vnodes[i]);
        CHECK(!registry.Busy(20+i%2,1+i/2));
    }
    CHECK(!registry.Busy(20,0,true) && !registry.Busy(21,0,true) && allocations==0);
    allocationProbe=nullptr;
}
static void allocationFailure() {
    MappingPinRegistry registry;allocationProbe=&registry;Vnode first{},second{};
    for(int failure=0;failure<2;++failure) {
        failAfter=failure;CHECK(registry.Pin(&first,2,3)==-12);failAfter=-1;
        CHECK(!registry.Busy(2,3) && !registry.RetainIfPinned(&first) && allocations==0);
    }
    CHECK(registry.Pin(&first,2,3)==0);
    failAfter=0;CHECK(registry.Pin(&second,2,3)==-12);failAfter=-1;
    CHECK(registry.Busy(2,3) && allocations==2 && !registry.RetainIfPinned(&second));
    // A failed speculative identity allocation does not reject a new binding
    // when the identity already exists.
    failAfter=1;CHECK(registry.Pin(&second,2,3)==0);failAfter=-1;CHECK(allocations==3);
    int before=calls;failAfter=0;
    CHECK(registry.Pin(&first,2,3)==0 && registry.RetainIfPinned(&second));
    registry.Unpin(&first);registry.Unpin(&second);CHECK(calls==before);failAfter=-1;
    registry.Unpin(&first);registry.Unpin(&second);CHECK(allocations==0);
    CHECK(registry.Pin(&first,0,3)==-95 && !registry.Busy(0,3));
    allocationProbe=nullptr;
}
static void concurrentPins() {
    MappingPinRegistry registry;allocationProbe=&registry;Vnode nodes[16]{};
    for(auto& node:nodes)CHECK(registry.Pin(&node,3,4)==0);
    std::vector<std::thread> threads;
    for(unsigned t=0;t<8;++t)threads.emplace_back([&,t]{
        for(unsigned i=0;i<2048;++i) {
            auto* node=&nodes[(i+t)%16];CHECK(registry.RetainIfPinned(node));
            CHECK(registry.Pin(node,3,4)==0);registry.Unpin(node);registry.Unpin(node);
        }
    });
    for(auto& thread:threads)thread.join();threads.clear();
    for(auto& node:nodes)registry.Unpin(&node);
    CHECK(!registry.Busy(3,4) && allocations==0);
    // Race first publication/last removal, including duplicate candidates.
    for(unsigned t=0;t<8;++t)threads.emplace_back([&,t]{
        for(unsigned i=0;i<1024;++i) {
            auto* node=&nodes[(i+t)%16];CHECK(registry.Pin(node,3,4)==0);
            CHECK(registry.RetainIfPinned(node));registry.Unpin(node);registry.Unpin(node);
        }
    });
    for(auto& thread:threads)thread.join();
    CHECK(!registry.Busy(3,4) && allocations==0);allocationProbe=nullptr;
}
static void mutationExclusion() {
    MappingPinRegistry registry;allocationProbe=&registry;std::mutex mutation;Vnode first{},second{};
    auto pin=[&](Vnode* vnode) {
        if(registry.RetainIfPinned(vnode))return 0;
        std::lock_guard<std::mutex> held(mutation);return registry.Pin(vnode,253,42);
    };
    CHECK(pin(&first)==0 && pin(&second)==0);
    {
        std::lock_guard<std::mutex> held(mutation);
        CHECK(registry.Busy(253,42)); // truncate and file rename/unlink must reject
        CHECK(registry.Busy(253,123,true)); // FAT directory mutation stays conservative
        CHECK(!registry.Busy(254,42) && !registry.Busy(254,0,true));
    }
    registry.Unpin(&first);CHECK(registry.Busy(253,42));registry.Unpin(&second);
    Gate backend,attempt;std::atomic<bool> published{false};
    std::thread truncator([&]{
        std::lock_guard<std::mutex> held(mutation);CHECK(!registry.Busy(253,42));
        backend.park(); // no registry slot/allocation needed throughout backend resize
        CHECK(!registry.Busy(253,42) && !published.load());
    });
    backend.wait();
    std::thread opener([&]{
        CHECK(!registry.RetainIfPinned(&first));attempt.park();
        CHECK(pin(&first)==0);published=true;
    });
    attempt.wait();attempt.release();backend.release();truncator.join();opener.join();
    CHECK(published && registry.Busy(253,42));registry.Unpin(&first);
    CHECK(allocations==0);allocationProbe=nullptr;
}
int main() {
    scaleAndIdentity();allocationFailure();concurrentPins();mutationExclusion();
    std::puts("MAPPING PINS PASS bindings=4096 identities=4096 aggregation/concurrency/OOM/mutation-exclusion/no-IRQ-allocation");
}
