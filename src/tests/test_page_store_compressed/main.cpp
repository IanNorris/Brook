#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include "../../kernel/src/memory/page_store.h"
#include "../../kernel/src/memory/compressed_page_pool.h"
#include "../../third_party/lz4/lz4.h"
using namespace brook;
struct Codec {
    alignas(8) char workspace[65536];bool fail=false,badDecode=false;int forced=0;
    static int Encode(void* p,const void* in,void* out,int size){auto& c=*static_cast<Codec*>(p);if(c.forced){assert(c.forced<=size);memset(out,0,c.forced);return c.forced;}return c.fail?0:LZ4_compress_fast_extState(c.workspace,static_cast<const char*>(in),static_cast<char*>(out),4096,size,1);}
    static int Decode(void* p,const void* in,int size,void* out,int capacity){auto& c=*static_cast<Codec*>(p);if(c.forced){memset(out,0,capacity);return capacity;}return c.badDecode?4095:LZ4_decompress_safe(static_cast<const char*>(in),static_cast<char*>(out),size,capacity);}
};
struct Disk {
    std::array<uint8_t,17*4096> data{};int error=0;bool shortIo=false;unsigned reads=0,writes=0;
    static int Read(void* p,uint64_t at,void* out,uint64_t n){auto& d=*static_cast<Disk*>(p);++d.reads;if(d.error)return d.error;assert(at+n<=d.data.size());if(d.shortIo)n/=2;memcpy(out,d.data.data()+at,n);return n;}
    static int Write(void* p,uint64_t at,const void* in,uint64_t n){auto& d=*static_cast<Disk*>(p);++d.writes;if(d.error)return d.error;assert(at+n<=d.data.size());if(d.shortIo)n/=2;memcpy(d.data.data()+at,in,n);return n;}
};
int main() {
    Codec codec;assert(LZ4_sizeofState()<=int(sizeof(codec.workspace)));
    Disk disk;CompressedPagePool pool;std::array<uint8_t,256> arena{};uint64_t bitmap=0;
    assert(pool.Initialize(arena.data(),arena.size(),&bitmap,1));
    PageStore store;std::array<PageStoreSlot,16> slots{};std::array<uint8_t,4096> encoded{},spill{},page{},out{};
    PageStoreHeader h{PAGE_STORE_MAGIC,1,4096,16,4096,{1},0};h.checksum=PageStoreChecksum(&h,48);
    assert(store.Initialize(h,disk.data.size(),slots.data(),slots.size(),{&disk,Disk::Read,Disk::Write}));
    PageStoreCompression c{&pool,&codec,Codec::Encode,Codec::Decode,encoded.data(),spill.data()};assert(store.AttachCompression(c));
    uint64_t tokens[6];for(unsigned i=0;i<6;++i){tokens[i]=store.Reserve();page.fill(i);assert(store.Store(tokens[i],page.data())==0);}
    assert(!store.AttachCompression(c));assert(store.Stats().ramSlots==4 && store.Stats().diskSlots==2 && pool.Allocated()==256);
    for(unsigned i=0;i<6;++i){page.fill(i);assert(store.Load(tokens[i],out.data())==0 && out==page);}
    auto a=tokens[0];auto& record=slots[(a&0xffffffffu)-1];assert(store.Retain(a));
    disk.error=-5;assert(store.Spill(a)==-5 && store.Stats().ramSlots==4 && pool.Allocated()==256);
    assert(store.Load(a,out.data())==0 && (out==std::array<uint8_t,4096>{}));
    disk.error=0;disk.shortIo=true;assert(store.Spill(a)==-5 && pool.Allocated()==256);disk.shortIo=false;
    codec.badDecode=true;assert(store.Load(a,out.data())==-5 && store.Spill(a)==-5);codec.badDecode=false;
    auto length=record.compressedBytes;record.compressedBytes=0;assert(store.Load(a,out.data())==-5);record.compressedBytes=length;
    auto original=arena;arena[record.ramOffset]^=0xff;assert(store.Load(a,out.data())==-5);arena=original;
    assert(store.Spill(a)==0 && pool.Allocated()==192 && record.state==PageSlotState::Stored);
    assert(store.Spill(a)==0 && store.Stats().spillSuccesses==1);assert(store.Release(a));
    assert(store.Load(a,out.data())==0 && (out==std::array<uint8_t,4096>{}));assert(store.Release(a));assert(!store.Release(a));
    for(unsigned i=1;i<6;++i)assert(store.Release(tokens[i]));
    assert(!pool.Allocated() && !pool.Payload() && !store.Stats().stored && !store.Stats().used);
    std::mt19937 rng(493);for(auto& byte:page)byte=rng();
    auto random=store.Reserve();assert(store.Store(random,page.data())==0 && store.Stats().diskSlots==1);assert(store.Load(random,out.data())==0 && page==out);assert(store.Release(random));
    codec.fail=true;auto failed=store.Reserve();disk.error=-28;
    assert(store.Store(failed,page.data())==-28 && !store.Stats().stored && !pool.Allocated());disk.error=0;
    assert(store.Store(failed,page.data())==0 && store.Load(failed,out.data())==0 && page==out);assert(store.Release(failed));codec.fail=false;
    assert(store.Load(a,out.data())==-22 && store.Spill(a)==-22);
    // Reuse mixed logical slots repeatedly; RAM objects and disk payloads have
    // identical retained-reference/last-release ownership.
    for(unsigned turn=0;turn<1000;++turn) {
        auto t=store.Reserve();assert(t && t!=a);page.fill(turn);assert(store.Store(t,page.data())==0);
        assert(store.Retain(t));if(turn%2)assert(store.Spill(t)==0);
        assert(store.Release(t));assert(store.Load(t,out.data())==0 && page==out);assert(store.Release(t));
    }
    assert(!pool.Allocated() && !pool.Payload() && !store.Stats().used && !store.Stats().ramSlots && !store.Stats().diskSlots);
    {
        PageStore boundary;CompressedPagePool space;std::array<uint8_t,4096> bytes{};uint64_t bits=0;
        std::array<PageStoreSlot,2> records{};auto header=h;header.slots=2;header.checksum=PageStoreChecksum(&header,48);
        assert(space.Initialize(bytes.data(),bytes.size(),&bits,1));
        assert(boundary.Initialize(header,disk.data.size(),records.data(),records.size(),{&disk,Disk::Read,Disk::Write}));
        auto config=c;config.pool=&space;assert(boundary.AttachCompression(config));page.fill(0);
        codec.forced=3072;auto t=boundary.Reserve();assert(boundary.Store(t,page.data())==0 && boundary.Stats().ramSlots==1 && space.Allocated()==3072);
        assert(boundary.Load(t,out.data())==0 && out==page);assert(boundary.Release(t));
        codec.forced=3073;t=boundary.Reserve();assert(boundary.Store(t,page.data())==0 && boundary.Stats().diskSlots==1 && !space.Allocated());assert(boundary.Release(t));
        codec.forced=0;
    }
    puts("compressed store: real LZ4 RAM/disk/fallback/corruption/partial-spill/retained-token/reuse pass");
}
