#include "../../kernel/src/gpu_context.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"GPU_CONTEXT line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
int main() {
    using namespace brook;
    const GpuAppCapabilities gl{6,true,false,false};
    GpuContextConfig out{2,7,3};
    const auto rejected=[&](const GpuContextParam* p,uint32_t n,const GpuAppCapabilities& c) {
        out={2,7,3};CHECK(!GpuContextParse(p,n,c,out));
        CHECK(out.capsetId==2 && out.numRings==7 && out.pollRingsMask==3);
    };
    CHECK(GpuContextParse(nullptr,0,gl,out));CHECK(out.capsetId==1 && out.numRings==0);
    const GpuContextParam valid[]={{3,0},{2,64},{1,2}};
    CHECK(GpuContextParse(valid,3,gl,out));CHECK(out.capsetId==2 && out.numRings==64);
    rejected(nullptr,1,gl);rejected(valid,5,gl);rejected(valid,3,{6,false,false,false});
    for(const uint64_t id:{0ull,4ull,63ull,64ull,~0ull}) {
        const GpuContextParam p{1,id};rejected(&p,1,gl);
    }
    for(const uint64_t value:{65ull,~0ull}) { const GpuContextParam p{2,value};rejected(&p,1,gl); }
    for(const uint64_t id:{0ull,4ull,~0ull}) { const GpuContextParam p{id,0};rejected(&p,1,gl); }
    const GpuContextParam dup[]={{1,1},{1,2}};rejected(dup,2,gl);
    const GpuContextParam poll{3,1};rejected(&poll,1,gl);
    rejected(nullptr,0,{4,true,false,false});
    // Future Venus transport must be explicitly present in the usable mask.
    const GpuContextParam venus[]={{1,4},{2,64},{3,0}};
    rejected(venus,3,gl);CHECK(GpuContextParse(venus,3,{22,true,true,true},out));
    CHECK(out.capsetId==4 && out.numRings==64);
    std::puts("GPU_CONTEXT_PASS capability identity, bounds, unsupported modes and atomic rejection");
}
