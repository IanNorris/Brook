#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
#include "../../kernel/src/memory/compressed_page_pool.h"
using namespace brook;
int main() {
    std::array<uint8_t,65536> arena{};std::array<uint64_t,16> bitmap{};CompressedPagePool pool;
    assert(!pool.Initialize(arena.data(),65535,bitmap.data(),16));
    assert(!pool.Initialize(arena.data(),65536,bitmap.data(),15));
    assert(pool.Initialize(arena.data(),65536,bitmap.data(),16));
    assert(!pool.Initialize(arena.data(),65536,bitmap.data(),16));
    std::array<uint8_t,4096> source{};uint32_t offsets[32];
    for(auto& offset:offsets)assert(pool.Put(source.data(),2048,offset));
    for(unsigned i=0;i<32;i+=2)assert(pool.Release(offsets[i],2048));
    uint32_t impossible=0;assert(pool.Allocated()==32768 && !pool.Put(source.data(),4096,impossible));
    for(unsigned i=1;i<32;i+=2){assert(pool.Get(offsets[i],2048));assert(pool.Release(offsets[i],2048));}
    assert(!pool.Allocated() && !pool.Payload());
    struct Blob{uint32_t offset;std::vector<uint8_t> bytes;};std::vector<Blob> live;std::array<bool,1024> used{};
    std::mt19937 rng(0x42524b);
    for(unsigned turn=0;turn<30000;++turn) {
        if(!live.empty() && rng()%3==0) {
            auto i=rng()%live.size();auto b=live[i];assert(pool.Release(b.offset,b.bytes.size()));
            assert(!pool.Release(b.offset,b.bytes.size()));
            for(unsigned j=b.offset/64;j<(b.offset+((b.bytes.size()+63)/64)*64)/64;++j)used[j]=false;
            live[i]=live.back();live.pop_back();
        } else {
            unsigned length=1+rng()%4096,n=(length+63)/64;int expected=-1;
            for(unsigned i=0;i+n<=1024;++i) {bool free=true;for(unsigned j=0;j<n;++j)free&=!used[i+j];if(free){expected=i;break;}}
            Blob b{0,std::vector<uint8_t>(length,uint8_t(turn))};uint32_t offset=~0u;
            bool ok=pool.Put(b.bytes.data(),length,offset);assert(ok==(expected>=0));
            if(ok){assert(offset==unsigned(expected)*64);b.offset=offset;for(unsigned j=0;j<n;++j)used[expected+j]=true;live.push_back(b);}
        }
        uint64_t payload=0,allocated=0;
        for(auto& b:live){auto* p=static_cast<const uint8_t*>(pool.Get(b.offset,b.bytes.size()));assert(p);for(unsigned j=0;j<b.bytes.size();++j)assert(p[j]==b.bytes[j]);payload+=b.bytes.size();}
        for(auto bit:used)allocated+=bit?64:0;
        assert(pool.Allocated()==allocated && pool.Payload()==payload);
    }
    assert(!pool.Get(65536,1) && !pool.Get(1,1) && !pool.Get(0,0));
    for(auto& b:live)assert(pool.Release(b.offset,b.bytes.size()));
    assert(!pool.Allocated() && !pool.Payload());puts("compressed pool: 30000 oracle-checked fragmentation/reuse/bounds operations pass");
}
