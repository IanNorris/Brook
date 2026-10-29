#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
/* Trusted host-generated manifest: BRKBEN01, LE count, then LE path length,
 * byte size, FNV1a64 and path bytes per unique regular inode. Target is x86-64.
 * One pass per job lets the host bracket it with device-I/O counters. */
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static uint64_t ns(void){struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint64_t hash(uint64_t h,const unsigned char* p,size_t n){for(size_t i=0;i<n;++i)h=(h^p[i])*1099511628211ULL;return h;}
static int order(const void* a,const void* b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);}
int main(int argc,char** argv){
    assert(argc==4 && (!strcmp(argv[2],"read") || !strcmp(argv[2],"map")));
    int mapped=!strcmp(argv[2],"map");char* end;long mib=strtol(argv[3],&end,10);assert(!*end && mib>=0 && mib<=192);
    FILE* manifest=fopen(argv[1],"rb");assert(manifest);char magic[8];uint64_t count;
    assert(fread(magic,8,1,manifest)==1 && !memcmp(magic,"BRKBEN01",8));assert(fread(&count,8,1,manifest)==1 && count && count<=131072);
    uint64_t* samples=calloc(count,sizeof(*samples));assert(samples);
    size_t hogBytes=(size_t)mib*1024*1024;unsigned char* hog=NULL;
    if(hogBytes){hog=mmap(NULL,hogBytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(hog!=MAP_FAILED);memset(hog,0x5a,hogBytes);}
    unsigned char buffer[65536];char path[4096];uint64_t total=0,combined=0;
    long freeBefore=ctl(3,0),cacheBefore=ctl(16,0),reclaimedBefore=ctl(16,2);assert(freeBefore>=0 && cacheBefore>=0);
    uint64_t start=ns();
    for(uint64_t i=0;i<count;++i){
        uint32_t length;uint64_t size,expected;
        assert(fread(&length,4,1,manifest)==1 && length>0 && length<sizeof(path));assert(fread(&size,8,1,manifest)==1 && fread(&expected,8,1,manifest)==1);
        assert(fread(path,length,1,manifest)==1 && !memchr(path,0,length));path[length]=0;
        uint64_t fileStart=ns();int fd=open(path,O_RDONLY);assert(fd>=0);struct stat st;assert(fstat(fd,&st)==0 && (uint64_t)st.st_size==size);
        uint64_t digest=14695981039346656037ULL;
        if(mapped && size){
            void* bytes=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);assert(bytes!=MAP_FAILED);digest=hash(digest,bytes,size);assert(munmap(bytes,size)==0);
        }else{
            uint64_t remaining=size;while(remaining){size_t take=remaining>sizeof(buffer)?sizeof(buffer):(size_t)remaining;ssize_t got=read(fd,buffer,take);assert(got>0 && (uint64_t)got<=remaining);digest=hash(digest,buffer,got);remaining-=got;}
        }
        assert(close(fd)==0 && digest==expected);samples[i]=ns()-fileStart;total+=size;combined^=digest;
    }
    uint64_t elapsed=ns()-start;long freeAfter=ctl(3,0),cacheAfter=ctl(16,0),reclaimed=ctl(16,2)-reclaimedBefore;
    assert(fgetc(manifest)==EOF && fclose(manifest)==0);qsort(samples,count,sizeof(*samples),order);
    if(hogBytes){for(size_t i=0;i<hogBytes;i+=4096)assert(hog[i]==0x5a);assert(munmap(hog,hogBytes)==0);}
    printf("IMAGE_BENCH_PASS mode=%s hog_mib=%ld files=%llu bytes=%llu digest=%llx elapsed_ns=%llu start_ns=%llu end_ns=%llu file_p50_ns=%llu file_p95_ns=%llu file_max_ns=%llu free_before=%ld free_after=%ld cached_before=%ld cached_after=%ld reclaimed=%ld\n",argv[2],mib,(unsigned long long)count,(unsigned long long)total,(unsigned long long)combined,(unsigned long long)elapsed,(unsigned long long)start,(unsigned long long)(start+elapsed),(unsigned long long)samples[count/2],(unsigned long long)samples[(count-1)*95/100],(unsigned long long)samples[count-1],freeBefore,freeAfter,cacheBefore,cacheAfter,reclaimed);
    free(samples);return 0;
}
