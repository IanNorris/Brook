#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include "../../kernel/src/memory/page_store.h"
using namespace brook;
struct Disk {
    FILE* file=tmpfile();int readError=0,writeError=0;bool shortIo=false;
    Disk(){assert(file);assert(ftruncate(fileno(file),4096*5)==0);}
    ~Disk(){fclose(file);}
    static int Read(void* p,uint64_t offset,void* out,uint64_t size) {
        auto& d=*static_cast<Disk*>(p);if(d.readError)return d.readError;
        return pread(fileno(d.file),out,d.shortIo?size/2:size,offset);
    }
    static int Write(void* p,uint64_t offset,const void* in,uint64_t size) {
        auto& d=*static_cast<Disk*>(p);if(d.writeError)return d.writeError;
        return pwrite(fileno(d.file),in,d.shortIo?size/2:size,offset);
    }
};
int main() {
    Disk disk;PageStore store;std::array<PageStoreSlot,4> slots{};
    PageStoreHeader header{PAGE_STORE_MAGIC,1,4096,4,4096,{1},0};
    header.checksum=PageStoreChecksum(&header,48);
    assert(PageStoreHeaderValid(header,4096*5));
    auto bad=header;bad.slots=~uint64_t{0};bad.checksum=PageStoreChecksum(&bad,48);
    assert(!PageStoreHeaderValid(bad,~uint64_t{0}));
    bad=header;bad.identity[0]=0;bad.checksum=PageStoreChecksum(&bad,48);
    assert(!PageStoreHeaderValid(bad,4096*5));
    assert(!store.Reserve());assert(!store.Release(0));
    assert(!store.Initialize(header,4096*5,slots.data(),3,{&disk,Disk::Read,Disk::Write}));
    assert(store.Initialize(header,4096*5,slots.data(),4,{&disk,Disk::Read,Disk::Write}));
    assert(!store.Initialize(header,4096*5,slots.data(),4,{&disk,Disk::Read,Disk::Write}));
    std::array<char,4096> page{},out{};for(unsigned i=0;i<page.size();++i)page[i]=i*73;
    auto a=store.Reserve();assert(a);assert(!store.Retain(a));assert(store.Load(a,out.data())==-11);
    disk.writeError=-28;assert(store.Store(a,page.data())==-28);
    assert(store.Stats().stored==0);disk.writeError=0;disk.shortIo=true;
    assert(store.Store(a,page.data())==-5);disk.shortIo=false;
    assert(store.Store(a,page.data())==0);assert(store.Store(a,page.data())==-16);
    assert(store.Retain(a));assert(store.Release(a));
    disk.readError=-5;assert(store.Load(a,out.data())==-5);disk.readError=0;
    disk.shortIo=true;assert(store.Load(a,out.data())==-5);disk.shortIo=false;
    assert(store.Load(a,out.data())==0);assert(out==page);
    const char corrupt=1;assert(pwrite(fileno(disk.file),&corrupt,1,4096)==1);
    assert(store.Load(a,out.data())==-5);assert(store.Stats().corruptReads==1);
    assert(store.Release(a));assert(!store.Release(a));assert(store.Load(a,out.data())==-22);
    uint64_t tokens[4];for(auto& t:tokens){t=store.Reserve();assert(t && t!=a);}
    assert(!store.Reserve());assert(!store.Retain(a));assert(store.Store(a,page.data())==-22);
    for(auto t:tokens)assert(store.Release(t));
    assert(store.Stats().used==0 && store.Stats().stored==0);
    // No generation wrap/ABA: exhaust rather than reuse a wrapped identity.
    for(auto& slot:slots)slot.generation=0xffffffffu;
    assert(!store.Reserve());
    // An immutable retained slot is independently usable until its last release.
    PageStore second;slots={};assert(second.Initialize(header,4096*5,slots.data(),4,{&disk,Disk::Read,Disk::Write}));
    a=second.Reserve();assert(second.Store(a,page.data())==0);assert(second.Retain(a));
    assert(second.Release(a));assert(second.Load(a,out.data())==0 && out==page);
    assert(second.Release(a));assert(second.Stats().used==0);
    puts("page store: file-backed roundtrip, bounded slots, stale tokens, corruption and I/O failures pass");
}
