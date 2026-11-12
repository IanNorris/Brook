#include "intel_bdw_publish.h"
#include <cassert>
#include <cstdint>
#include <sys/mman.h>
using namespace brook;
struct Flush {
    unsigned fences=0,lines=0; uintptr_t first=0,last=0;const BdwOwnedPage* expected=nullptr;
    void fence(){++fences;}
    void line(void* p){assert(fences==1 && expected && p==expected[lines/64].cpu+(lines%64)*64);
        if(!lines)first=(uintptr_t)p;last=(uintptr_t)p;++lines;}
};
struct NativeFlush {
    void fence(){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
    void line(void* p){__asm__ volatile("clflush (%0)"::"r"(p):"memory");}
};
int main(){
    constexpr unsigned count=BdwPublishMaximumPages; BdwOwnedPage pages[count]{};
    auto* region=(uint8_t*)mmap(nullptr,4096*(count*2+1),PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(region!=MAP_FAILED);
    for(unsigned i=0;i<count;++i){pages[i].cpu=region+(2*i+1)*4096;assert(!mprotect(pages[i].cpu,4096,PROT_READ|PROT_WRITE));}
    Flush f;f.expected=pages;assert(BdwPublishCpuPages(f,pages,count,64));
    assert(f.fences==2 && f.lines==count*64 && f.first==(uintptr_t)pages[0].cpu && f.last==(uintptr_t)(pages[count-1].cpu+4032));
    uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    if(d&(1u<<19)) {
        for(unsigned i=0;i<count;++i)for(unsigned j=0;j<4096;++j)pages[i].cpu[j]=uint8_t(i+j);
        NativeFlush native;assert(BdwPublishCpuPages(native,pages,count,((b>>8)&255)*8));
        for(unsigned i=0;i<count;++i)for(unsigned j=0;j<4096;++j)assert(pages[i].cpu[j]==uint8_t(i+j));
    }
    Flush bad;assert(!BdwPublishCpuPages(bad,pages,count,48));assert(!bad.fences && !bad.lines);
    auto saved=pages[count-1];pages[count-1].cpu=nullptr;assert(!BdwPublishCpuPages(bad,pages,count,64));assert(!bad.fences && !bad.lines);
    pages[count-1]=saved;pages[count-1].cpu=pages[0].cpu;assert(!BdwPublishCpuPages(bad,pages,count,64));assert(!bad.fences && !bad.lines);
    assert(!BdwPublishCpuPages(bad,pages,count+1,64));assert(!bad.fences && !bad.lines);
    assert(!munmap(region,4096*(count*2+1)));
}
