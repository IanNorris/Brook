// x86-64 MAP_32BIT placement, cursor isolation, exhaustion and fixed semantics.
// Prefer a static non-PIE build so the test's own DSOs do not occupy low gaps.
#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CK(x) do {if(!(x)){fprintf(stderr,"MAP32_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}} while(0)
static const uint64_t limit=UINT64_C(0x80000000),base=UINT64_C(0x10000000);
static void* low(void* hint,size_t bytes) {
    void* p=mmap(hint,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CK(p!=MAP_FAILED && (uintptr_t)p<limit && bytes<=limit-(uintptr_t)p);
    ((volatile char*)p)[0]=17;((volatile char*)p)[bytes-1]=29;return p;
}
int main(int argc,char** argv) {
    int observe=argc==2 && !strcmp(argv[1],"--observe");CK(argc==1 || observe);
    const size_t large=UINT64_C(0x100000000);
    void* ordinary=mmap(NULL,large,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CK(ordinary!=MAP_FAILED && munmap(ordinary,large)==0);
    void* first=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CK(first!=MAP_FAILED);
    int in_range=(uintptr_t)first<limit && 65536<=limit-(uintptr_t)first;
    printf("MAP32_OBSERVE ordinary=%p low=%p within_first_2GiB=%d\n",ordinary,first,in_range);fflush(stdout);
    CK(munmap(first,65536)==0);
    if(!in_range && observe)return 42;
    CK(in_range);
    void* high_hint=low((void*)UINT64_C(0x100000000),4096);CK(munmap(high_hint,4096)==0);
    void* pages[128];
    for(unsigned i=0;i<128;++i) {
        pages[i]=low(NULL,65536);
        for(unsigned j=0;j<i;++j)CK((uintptr_t)pages[i]+65536<=(uintptr_t)pages[j] ||
            (uintptr_t)pages[j]+65536<=(uintptr_t)pages[i]);
    }
    for(unsigned i=0;i<128;++i)CK(munmap(pages[i],65536)==0);
    // Exact-address flags ignore MAP_32BIT, including NOREPLACE's fixed case.
    void* high=(void*)UINT64_C(0x200000000);
    CK(mmap(high,4096,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE|MAP_32BIT,-1,0)==high);
    CK(mmap(high,4096,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_32BIT,-1,0)==high);
    CK(munmap(high,4096)==0);
    CK(mmap(NULL,limit+4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0)==MAP_FAILED && errno==ENOMEM);
    // Fill Brook's entire admissible low band without allocating physical RAM.
    // The static test image is below it. No live mapping may be replaced.
    size_t band=limit-base;
    CK(mmap((void*)(uintptr_t)base,band,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)(uintptr_t)base);
    // Linux can also use lower addresses: reserve that remaining interval.
    void* lower=mmap((void*)UINT64_C(0x1000000),base-UINT64_C(0x1000000),PROT_NONE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    // Linux's MAP_32BIT allocator normally starts at 1 GiB; Brook's starts at
    // USER_MMAP_BASE. The upper band must never overflow into ordinary space.
    CK(mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0)==MAP_FAILED && errno==ENOMEM);
    if(lower!=MAP_FAILED)CK(munmap(lower,base-UINT64_C(0x1000000))==0);
    CK(munmap((void*)(uintptr_t)base,band)==0);
    void* reused=low(NULL,4096);CK(munmap(reused,4096)==0);
    puts("MAP32_PASS");return 0;
}
