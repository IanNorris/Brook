#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <vector>
#include <algorithm>
#include <random>
#include <cstdio>
#include "../../kernel/src/virtio_blk_write_plan.h"
using namespace brook;
struct Backend {
    static constexpr unsigned Blocks=16,CacheSlots=4;
    struct Entry {uint64_t tag=0;bool valid=false;std::array<uint8_t,4096> data{};};
    std::vector<uint8_t> disk;
    std::array<Entry,CacheSlots> cache{};
    unsigned reads=0,writes=0,scatter=0,failWrite=0;
    bool scatterFails=false,partialScatter=false,failRead=false;
    Backend():disk(Blocks*4096) {for(unsigned i=0;i<disk.size();++i)disk[i]=(i*37+(i>>9))&255;}
    uint8_t* Cached(uint64_t block){auto& e=cache[block%CacheSlots];return e.valid&&e.tag==block?e.data.data():nullptr;}
    void Load(unsigned block){auto& e=cache[block%CacheSlots];e.valid=true;e.tag=block;std::copy_n(disk.data()+block*4096,4096,e.data.data());}
    void Invalidate(uint64_t first,uint64_t end){for(uint64_t b=first/8;b<=(end-1)/8;++b)if(Cached(b))cache[b%CacheSlots].valid=false;}
    bool Read(uint64_t sector,uint32_t count,uint8_t* data){++reads;if(failRead)return false;std::copy_n(disk.data()+sector*512,count*512,data);return true;}
    bool Write(uint64_t sector,uint32_t count,const uint8_t* data){
        ++writes;bool fail=failWrite==writes;
        std::copy_n(data,fail?std::min(512u,count*512):count*512,disk.data()+sector*512);
        return !fail;
    }
    int64_t ScatterWrite(uint64_t sector,uint32_t count,const uint8_t* data){
        ++scatter;if(scatterFails){if(partialScatter)std::copy_n(data,count*512,disk.data()+sector*512);return -1;}
        std::copy_n(data,count*512,disk.data()+sector*512);return count*512;
    }
    void Coherent(){for(auto& e:cache)if(e.valid)assert(std::equal(e.data.begin(),e.data.end(),disk.begin()+e.tag*4096));}
};
static void randomWrites(){
    std::mt19937 random(0x42726f6f);
    for(unsigned trial=0;trial<2000;++trial){
        Backend io;for(unsigned b=0;b<Backend::Blocks;++b)if(random()%2)io.Load(b);
        const unsigned offset=random()%(io.disk.size()-16384),length=1+random()%16384;
        std::vector<uint8_t> source(length);for(auto& v:source)v=random();
        auto expected=io.disk;std::copy(source.begin(),source.end(),expected.begin()+offset);
        std::array<uint8_t,3*512+64> buffer;buffer.fill(0xab);
        io.scatterFails=(random()%2)==0;
        auto r=VirtioPlanWrite(io,offset,source.data(),length,buffer.data()+32,3,8);
        assert(r.ok&&r.bytes==length&&io.disk==expected);io.Coherent();
        for(unsigned i=0;i<32;++i)assert(buffer[i]==0xab&&buffer[buffer.size()-1-i]==0xab);
    }
}
int main(){
    randomWrites();
    Backend hit;hit.Load(0);std::array<uint8_t,1024> dma{};std::array<uint8_t,128> source;source.fill(0x53);
    for(unsigned i=0;i<8;++i){auto r=VirtioPlanWrite(hit,128,source.data(),source.size(),dma.data(),2,8);assert(r.ok);hit.Coherent();}
    assert(!hit.reads&&hit.writes==8&&hit.Cached(0)); // eliminated repeated inode-size prereads
    Backend miss;miss.Load(0);std::array<uint8_t,2048> big;big.fill(0x6a);std::array<uint8_t,4096> wideDma{};
    auto before=miss.disk;auto r=VirtioPlanWrite(miss,3584,big.data(),big.size()-1,wideDma.data(),8,8);
    assert(r.ok&&miss.reads==1); // mixed hit/miss requires full preread
    std::copy_n(big.data(),big.size()-1,before.data()+3584);assert(before==miss.disk);miss.Coherent();
    Backend failed;failed.Load(0);failed.failWrite=1;
    r=VirtioPlanWrite(failed,128,source.data(),128,dma.data(),2,8);assert(!r.ok&&!failed.Cached(0));failed.Coherent();
    Backend sg;sg.Load(0);sg.Load(1);sg.Load(2);sg.scatterFails=sg.partialScatter=true;sg.failWrite=2;
    std::vector<uint8_t> payload(20*512,0x7c);
    r=VirtioPlanWrite(sg,0,payload.data(),payload.size(),wideDma.data(),4,16);
    assert(!r.ok&&r.bytes==4*512&&!sg.Cached(0)&&!sg.Cached(1)); // entire failed SG invalidated, including beyond bounce failure
    assert(sg.Cached(2));sg.Coherent();assert(std::equal(payload.begin(),payload.begin()+16*512,sg.disk.begin()));
    Backend collision;collision.Load(0);collision.Load(4);
    auto saved=collision.cache[0].data;
    r=VirtioPlanWrite(collision,128,source.data(),128,dma.data(),2,8);
    assert(r.ok&&collision.reads==1&&collision.cache[0].data==saved&&collision.Cached(4));collision.Coherent();
    Backend readFail;readFail.failRead=true;
    r=VirtioPlanWrite(readFail,128,source.data(),128,dma.data(),2,8);assert(!r.ok&&!readFail.writes);
    r=VirtioPlanWrite(readFail,~uint64_t{0}-10,source.data(),128,dma.data(),2,8);assert(!r.ok);
    puts("VIRTIO WRITE PLAN PASS random-byte-oracle=2000/cache-hit/mixed-miss/boundaries/collision/failed-read/partial-OUT/partial-SG-fallback");
}
