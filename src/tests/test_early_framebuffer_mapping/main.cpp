#include "../../kernel/src/memory/early_framebuffer_mapping.h"
#include <array>
#include <map>
#include <cassert>
#include <cstring>
using namespace brook;
constexpr uint64_t mask=0x000ffffffffff000ull, huge=1ull<<21;
struct Memory {
    std::map<uint64_t,std::array<uint64_t,512>> tables;
    uint64_t next=0x10000;unsigned allocations=0,releases=0,failAllocation=0;
    bool transitioning=false,done=false;
    uint64_t* table(uint64_t p) { return tables.at(p).data(); }
    uint64_t allocate() {
        assert(!transitioning && !done);
        if(++allocations==failAllocation) return 0;
        const uint64_t p=next;next+=4096;tables[p]={};return p;
    }
    void release(uint64_t p) { assert(!transitioning);assert(tables.erase(p)==1);++releases; }
    void beginTransition() { assert(!transitioning && !done);transitioning=true; }
    void endTransition() { assert(transitioning);transitioning=false;done=true; }
    void setup(bool shared=false) {
        tables[0x1000]={};tables[0x2000]={};tables[0x3000]={};tables[0x4000]={};tables[0x5000]={};
        table(0x1000)[0]=0x2003;table(0x1000)[256]=shared?0x2003:0x3003;
        table(0x2000)[2]=0x4003;table(0x3000)[2]=0x5003;
        for(unsigned i=0;i<512;++i) {
            const uint64_t leaf=(2ull<<30)+uint64_t(i)*huge+0x183; // global, writable, present
            table(0x4000)[i]=leaf;table(0x5000)[i]=leaf;
        }
    }
    uint64_t leaf(unsigned alias,uint64_t physical) {
        auto* pdpt=table(table(0x1000)[alias]&mask);
        auto* pd=table(pdpt[(physical>>30)&511]&mask);
        const uint64_t pde=pd[(physical>>21)&511];
        if(pde&0x80) return pde;
        return table(pde&mask)[(physical>>12)&511];
    }
};
int main() {
    EarlyFramebufferRange bounds;
    assert(EarlyFramebufferBounds(0x80001004,800,600,3200,bounds));
    assert(bounds.begin==0x80001000 && bounds.end==0x801d6000);
    assert(!EarlyFramebufferBounds(0x80000000,800,600,3199,bounds));
    assert(!EarlyFramebufferBounds(~0ull-4095,800,600,3200,bounds));
    assert(!EarlyFramebufferBounds((16ull<<30)-4096,800,600,3200,bounds));
    assert(!EarlyFramebufferBounds(0x80000000,0,600,3200,bounds));
    assert(!EarlyFramebufferBounds(0x80000000,~0u,~0u,~0u,bounds));
    uint64_t flags=42;
    const EarlyFramebufferRange cache{0x80000000,0x80200000};
    assert(FramebufferAliasCacheFlags(cache,true,cache.begin,4096,flags) && flags==0x80);
    assert(FramebufferAliasCacheFlags(cache,true,cache.end,4096,flags) && flags==0);
    assert(!FramebufferAliasCacheFlags(cache,true,cache.begin-4096,8192,flags));
    assert(!FramebufferAliasCacheFlags(cache,true,cache.end-4096,8192,flags));
    assert(FramebufferAliasCacheFlags(cache,false,cache.begin-4096,8192,flags) && flags==0);
    assert(!FramebufferAliasCacheFlags(cache,true,~0ull-4095,8192,flags));
    const EarlyFramebufferRange range{0x801ff000,0x80203000}; // crosses huge-page boundary
    {
        Memory m;m.setup();const auto before=m.tables;
        EarlyFramebufferPlan plan;assert(MapEarlyFramebufferWc(m,m.table(0x1000),range,plan));
        assert(m.done && plan.count==4);
        for(unsigned alias: {0u,256u}) {
            for(uint64_t p=0x80000000;p<0x80400000;p+=4096) {
                const uint64_t leaf=m.leaf(alias,p);
                assert((leaf&mask)==p && (leaf&0x103)==0x103);
                assert(bool(leaf&0x80)==(p>=range.begin && p<range.end));
            }
        }
        assert(m.table(0x4000)[2]==before.at(0x4000)[2]); // neighbouring huge leaf unchanged
    }
    for(unsigned failure=1;failure<=4;++failure) {
        Memory m;m.setup();const auto before=m.tables;m.failAllocation=failure;
        EarlyFramebufferPlan plan;assert(!MapEarlyFramebufferWc(m,m.table(0x1000),range,plan));
        assert(m.tables==before && !m.done && plan.count==0);
        assert(m.releases==failure-1);
    }
    for(uint64_t badFlag: {0x1000ull,0x8ull,0x10ull,0x4ull,0x2000ull}) {
        Memory m;m.setup();m.table(0x5000)[1]|=badFlag;const auto before=m.tables;
        EarlyFramebufferPlan plan;assert(!MapEarlyFramebufferWc(m,m.table(0x1000),range,plan));
        assert(m.tables==before && !m.done);
    }
    {
        Memory m;m.setup();m.table(0x3000)[2]|=0x80;const auto before=m.tables;
        EarlyFramebufferPlan plan;assert(!MapEarlyFramebufferWc(m,m.table(0x1000),range,plan));assert(m.tables==before);
    }
    {
        Memory m;m.setup(true);EarlyFramebufferPlan plan;
        assert(MapEarlyFramebufferWc(m,m.table(0x1000),range,plan));assert(plan.count==2);
        assert(m.leaf(0,range.begin)==m.leaf(256,range.begin));
    }
}
