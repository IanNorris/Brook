#include "../../drivers/virtio_gpu/aperture_allocator.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"APERTURE line %d\n",__LINE__);std::abort(); } } while(0)
int main() {
    using A=brook::GpuApertureAllocator<64>;A a;A::Extent result{9,9,9};
    CHECK(!a.Allocate(0,4096,result));CHECK(!a.Allocate(~uint64_t{0},4096,result));
    CHECK(!a.Allocate(4097,4096,result));CHECK(!a.Allocate(1,4097,result));
    CHECK(result.lease==9&&a.Count()==0);
    CHECK(a.Allocate(1,4096,result)&&result.offset==0&&result.size==4096);
    auto first=result;CHECK(!a.Allocate(1,4096,result));CHECK(a.Release(first.lease));
    CHECK(a.Allocate(4096,4096,result)&&result.lease!=first.lease);
    CHECK(!a.Release(first.lease)&&a.Count()==1);CHECK(a.Release(result.lease));
    std::mt19937 rng(12345);std::vector<A::Extent> live;
    constexpr uint64_t capacity=256*4096;
    for(unsigned step=0;step<20000;++step) {
        if(live.size() && rng()%3==0) {
            const auto index=rng()%live.size();CHECK(a.Release(live[index].lease));live.erase(live.begin()+index);
        } else if(a.Allocate((1+rng()%16)*4096-1,capacity,result)) {
            CHECK(!(result.offset&4095)&&!(result.size&4095)&&result.size<=capacity-result.offset);
            for(auto span:live)CHECK(result.offset+result.size<=span.offset || span.offset+span.size<=result.offset);
            live.push_back(result);
        }
        uint64_t bytes=0;for(auto span:live)bytes+=span.size;
        CHECK(a.Count()==live.size()&&a.Bytes()==bytes);
    }
    for(auto span:live)CHECK(a.Release(span.lease));CHECK(a.Count()==0&&a.Bytes()==0);
    brook::GpuApertureAllocator<1> tiny;brook::GpuApertureAllocator<1>::Extent small{};
    CHECK(tiny.Allocate(1,capacity,small));CHECK(!tiny.Allocate(1,capacity,small));
    std::puts("GPU_APERTURE_PASS bounds, exhaustion, gaps, nonoverlap and stale lease rejection");
}
