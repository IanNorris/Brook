#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static uint64_t ns(void){struct timespec t;assert(syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static unsigned char pattern(unsigned i,unsigned j){return (unsigned char)(i*31+(i>>8)*17+j*73+(j>>8));}
static int order(const void* a,const void* b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);}
static uint64_t samples[1024];
int main(int argc,char** argv) {
    enum { Regions=16, RegionBytes=16*1024*1024, RegionPages=RegionBytes/4096, Target=4096, MaxHogs=8 };
    assert(argc==2 && (!strcmp(argv[1],"--eager") || !strcmp(argv[1],"--paged")));
    int eager=!strcmp(argv[1],"--eager");
    setvbuf(stdout,NULL,_IONBF,0);
    unsigned char* hogs[MaxHogs]={0};unsigned char* regions[Regions]={0};
    uint64_t before=ctl(3,0);assert(before>0 && ctl(2,0)==0);
    // Hold ordinary anonymous RAM to reach high physical use. Leave the soft
    // working set plus 32 MiB for kernel/backend/progress allocations. This is
    // bounded guest pressure, never an unbounded host/process stress loop.
    unsigned hogCount=0;
    while(hogCount<MaxHogs && (uint64_t)ctl(3,0)>Target+8192+RegionPages) {
        void* p=mmap(NULL,RegionBytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);
        hogs[hogCount++]=p;memset(p,0x5a,RegionBytes);
    }
    long afterHogs=ctl(3,0);long minFree=afterHogs;
    printf("PRESSURE_BEGIN mode=%s free_before=%llu hog_regions=%u free_after_hogs=%ld dataset_mib=256 target_mib=16\n",eager?"eager":"paged",(unsigned long long)before,hogCount,afterHogs);
    uint64_t start=ns();unsigned mapped=0;int failure=0;
    for(;mapped<Regions;++mapped) {
        long base=ctl(eager?0:11,RegionBytes);
        if(base<0){failure=errno;break;}
        regions[mapped]=(void*)(uintptr_t)base;
        long free=ctl(3,0);if(free<minFree)minFree=free;
    }
    if(eager) {
        for(unsigned r=0;r<mapped;++r)for(unsigned i=0;i<RegionBytes;i+=4096)regions[r][i]=(unsigned char)r;
        uint64_t elapsed=ns()-start;
        long held=ctl(13,0);
        for(unsigned r=0;r<mapped;++r)assert(munmap(regions[r],RegionBytes)==0);
        for(unsigned h=0;h<hogCount;++h)assert(munmap(hogs[h],RegionBytes)==0);
        assert(ctl(2,0)==0);
        printf("PRESSURE_EAGER_RESULT completed=%u mapped_mib=%u error=%d held_pages=%ld min_free_pages=%ld elapsed_ns=%llu\n",mapped==Regions,mapped*16,failure,held,minFree,(unsigned long long)elapsed);
        assert(mapped==Regions || failure==ENOMEM);return 0;
    }
    assert(mapped==Regions && ctl(13,0)==0 && ctl(13,1)==Regions*RegionPages);
    assert(ctl(9,1)==0 && ctl(12,Target)==0);
    start=ns();
    for(unsigned r=0;r<Regions;++r) {
        for(unsigned i=0;i<RegionPages;++i)for(unsigned j=0;j<4096;++j)regions[r][i*4096+j]=pattern(r*RegionPages+i,j);
        assert(ctl(13,0)==Target);
        long free=ctl(3,0);if(free<minFree)minFree=free;
    }
    uint64_t fill=ns()-start;start=ns();unsigned sample=0;
    for(unsigned r=0;r<Regions;++r)for(unsigned i=0;i<RegionPages;++i) {
        uint64_t at=(i%64)==0?ns():0;
        for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)regions[r])[i*4096+j]==pattern(r*RegionPages+i,j));
        if(at)samples[sample++]=ns()-at;
    }
    uint64_t scan=ns()-start;
    assert(sample==1024 && ctl(13,0)==Target && ctl(13,1)==0 && ctl(13,3)==0 && ctl(2,0)==Regions*RegionPages-Target);
    long held=ctl(13,0),stored=ctl(2,0),reserve=ctl(7,0);
    long capacity=ctl(14,0),allocated=ctl(14,1),payload=ctl(14,2),ramSlots=ctl(14,3),diskSlots=ctl(14,4),fallbacks=ctl(14,5);
    assert(ramSlots+diskSlots==stored);
    if(capacity>0)assert(ramSlots>0 && diskSlots>0 && fallbacks>0 && payload<=allocated && allocated<=capacity);
    assert(ctl(9,0)==0 && ctl(12,0)==0);
    for(unsigned r=0;r<Regions;++r)assert(munmap(regions[r],RegionBytes)==0);
    for(unsigned h=0;h<hogCount;++h) {
        for(unsigned i=0;i<RegionBytes;i+=4096)assert(((volatile unsigned char*)hogs[h])[i]==0x5a);
        assert(munmap(hogs[h],RegionBytes)==0);
    }
    assert(ctl(2,0)==0 && ctl(6,0)==0 && ctl(14,1)==0 && ctl(14,2)==0);
    qsort(samples,sample,sizeof(samples[0]),order);
    printf("PRESSURE_PAGED_PASS dataset_mib=256 held_pages=%ld stored_pages=%ld reserve=%ld min_free_pages=%ld fill_ns=%llu scan_ns=%llu page_verify_p50_ns=%llu p95_ns=%llu max_ns=%llu ram_slots=%ld disk_slots=%ld arena_capacity=%ld arena_allocated=%ld payload_bytes=%ld fallbacks=%ld cleanup_slots=0 cleanup_pool=0\n",held,stored,reserve,minFree,(unsigned long long)fill,(unsigned long long)scan,(unsigned long long)samples[sample/2],(unsigned long long)samples[sample*95/100],(unsigned long long)samples[sample-1],ramSlots,diskSlots,capacity,allocated,payload,fallbacks);
}
