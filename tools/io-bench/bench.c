#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHUNK 65536
#define MAX_WORKERS 16
static char root[160];
static unsigned workers = 8, mib = 8, files = 64;
static pthread_barrier_t barrier;
static const char *phase;
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock"); exit(2); }
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static void die(const char *what) {
    fprintf(stderr,"IOBENCH FAIL phase=%s operation=%s errno=%d (%s)\n", phase,what,errno,strerror(errno));
    exit(2);
}
static void require(int ok, const char *what) { if (!ok) { errno=EIO; die(what); } }
static unsigned char byte(unsigned id, uint64_t offset) {
    uint64_t key=(((uint64_t)id<<32)^(offset>>12))*0x9e3779b97f4a7c15ULL+0xd1b54a32d192ed03ULL;
    return (key>>((offset&7)*8))^((offset&4095)*13);
}
static void fill(void *buffer, size_t length, unsigned id, uint64_t offset) {
    unsigned char *p=buffer;
    for(size_t i=0;i<length;i++) p[i]=byte(id,offset+i);
}
static void write_all(int fd,const void *buffer,size_t length) {
    const char *p=buffer;
    while(length) { ssize_t n=write(fd,p,length); if(n<0&&errno==EINTR)continue; if(n<=0)die("write"); p+=n;length-=n; }
}
static void verify(int fd,unsigned id,uint64_t size,void *buffer) {
    if(lseek(fd,0,SEEK_SET)<0)die("seek");
    uint64_t pos=0;
    while(pos<size) {
        size_t count=size-pos<CHUNK ? size-pos:CHUNK;
        ssize_t n=read(fd,buffer,count);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)die("readback");
        unsigned char *p=buffer;
        for(ssize_t i=0;i<n;i++) {
            unsigned char expected=byte(id,pos+i);
            if(p[i]!=expected) {
                fprintf(stderr,"IOBENCH_MISMATCH seed=%u offset=%llu actual=%u expected=%u\n",id,(unsigned long long)(pos+i),p[i],expected);
                require(0,"content mismatch");
            }
        }
        pos+=n;
    }
    char extra; require(read(fd,&extra,1)==0,"exact EOF");
    struct stat s;if(fstat(fd,&s))die("fstat");require((uint64_t)s.st_size==size,"exact size");
}
static int open_file(const char *path,int flags) { int fd=open(path,flags,0600);if(fd<0)die(path);return fd; }
static void close_file(int fd) { if(close(fd))die("close"); }
static void counters(const char *label) {
    printf("IOBENCH_COUNTERS %s\n",label);
    for(unsigned i=0;i<2;i++) {
        int fd=open(i?"/proc/diskstats":"/proc/blkprobe",O_RDONLY);
        if(fd<0)continue;
        char buffer[4096];ssize_t n;
        while((n=read(fd,buffer,sizeof(buffer)))>0)fwrite(buffer,1,n,stdout);
        close(fd);
    }
}
static void *worker(void *arg) {
    unsigned id=(uintptr_t)arg;
    char dir[200],path[256],other[256];
    snprintf(dir,sizeof(dir),"%s/w%u",root,id);
    snprintf(path,sizeof(path),"%s/bulk",dir);
    unsigned char *buffer=malloc(CHUNK);if(!buffer)die("malloc");
    pthread_barrier_wait(&barrier);
    if(!strcmp(phase,"sequential-write")) {
        int fd=open_file(path,O_RDWR|O_CREAT|O_EXCL);
        for(unsigned i=0;i<mib*16;i++){fill(buffer,CHUNK,id,(uint64_t)i*CHUNK);write_all(fd,buffer,CHUNK);}
        close_file(fd);
    } else if(!strcmp(phase,"sequential-read")) {
        int fd=open_file(path,O_RDONLY);
        verify(fd,id,(uint64_t)mib*1024*1024,buffer);
        // Force the large device-read path with byte-granular boundaries.
        // Adjacent sentinels detect DMA writes beyond the requested buffer.
        size_t count=CHUNK+1;unsigned char *guard=malloc(count+2);
        if(!guard)die("read guard");
        memset(guard,0x5a,count+2);
        require(pread(fd,guard+1,count,1)==(ssize_t)count,"large unaligned pread");
        require(guard[0]==0x5a&&guard[count+1]==0x5a,"large read guard bytes");
        for(size_t i=0;i<count;i++)require(guard[i+1]==byte(id,i+1),"large unaligned contents");
        free(guard);close_file(fd);
    } else if(!strcmp(phase,"boundaries-fragmentation")) {
        int fd=open_file(path,O_RDWR);
        unsigned char patch[97],check[97];memset(patch,0xa0+id,sizeof(patch));
        uint64_t edges[]={12*4096ULL,(12+1024)*4096ULL};
        for(unsigned i=0;i<2;i++) {
            require(pwrite(fd,patch,sizeof(patch),edges[i]-32)==sizeof(patch),"boundary overwrite");
            require(pread(fd,check,sizeof(check),edges[i]-32)==sizeof(check)&&!memcmp(check,patch,sizeof(patch)),"boundary readback");
        }
        uint64_t end=(uint64_t)mib*1024*1024;
        fill(buffer,37,id,end);require(pwrite(fd,buffer,37,end)==37,"unaligned append");
        require(pread(fd,check,37,end)==37&&!memcmp(check,buffer,37),"append readback");close_file(fd);
        // More than one coalescing run, an odd tail, and truncate/reuse.
        snprintf(path,sizeof(path),"%s/ragged",dir);
        unsigned char *odd=malloc(283688);if(!odd)die("odd buffer");fill(odd,283688,id,0);
        fd=open_file(path,O_RDWR|O_CREAT|O_EXCL);write_all(fd,odd,283688);verify(fd,id,283688,buffer);
        close_file(fd);
        fd=open_file(path,O_RDWR|O_TRUNC);
        struct stat emptied;
        if(fstat(fd,&emptied))die("truncate stat");
        require(emptied.st_size==0,"truncate size zero");
        char eof;require(read(fd,&eof,1)==0,"truncate EOF");
        fill(odd,283688,20000+id,0);
        size_t pos=0, chunks[]={4093,65521,1024,777,131077};
        for(unsigned i=0;i<5;i++) {write_all(fd,odd+pos,chunks[i]);pos+=chunks[i];}
        write_all(fd,odd+pos,283688-pos);verify(fd,20000+id,283688,buffer);close_file(fd);free(odd);
        // Alternating allocation creates noncontiguous runs and lookahead.
        snprintf(path,sizeof(path),"%s/fragment-a",dir);snprintf(other,sizeof(other),"%s/fragment-b",dir);
        int a=open_file(path,O_RDWR|O_CREAT|O_EXCL),b=open_file(other,O_RDWR|O_CREAT|O_EXCL);
        for(unsigned i=0;i<80;i++){fill(buffer,4096,id,(uint64_t)i*4096);write_all(a,buffer,4096);write_all(b,buffer,4096);}
        verify(a,id,80*4096,buffer);verify(b,id,80*4096,buffer);
        int fragmented[]={a,b};
        for(unsigned f=0;f<2;f++) {
            if(lseek(fragmented[f],0,SEEK_SET)<0)die("fragment overwrite seek");
            for(unsigned chunk=0;chunk<5;chunk++) {
                fill(buffer,CHUNK,30000+id*2+f,(uint64_t)chunk*CHUNK);
                write_all(fragmented[f],buffer,CHUNK);
            }
            verify(fragmented[f],30000+id*2+f,80*4096,buffer);
            // Append a large chunk to an existing fragmented block tree.
            fill(buffer,CHUNK,30000+id*2+f,80*4096);
            write_all(fragmented[f],buffer,CHUNK);
            verify(fragmented[f],30000+id*2+f,96*4096,buffer);
        }
        close_file(a);close_file(b);
        // The second batching chunk contains exactly one byte.
        snprintf(path,sizeof(path),"%s/tail",dir);
        unsigned char *tail=malloc(CHUNK+1);if(!tail)die("tail buffer");
        fill(tail,CHUNK+1,40000+id,0);
        fd=open_file(path,O_RDWR|O_CREAT|O_EXCL);write_all(fd,tail,CHUNK+1);
        verify(fd,40000+id,CHUNK+1,buffer);close_file(fd);free(tail);
        // Grow an open orphan through double-indirect blocks with batched writes.
        snprintf(path,sizeof(path),"%s/orphan",dir);
        fd=open_file(path,O_RDWR|O_CREAT|O_EXCL);if(unlink(path))die("orphan unlink");
        for(unsigned chunk=0;chunk<128;chunk++) {
            fill(buffer,CHUNK,50000+id,(uint64_t)chunk*CHUNK);write_all(fd,buffer,CHUNK);
        }
        verify(fd,50000+id,128ULL*CHUNK,buffer);
        struct stat orphan;if(fstat(fd,&orphan))die("orphan stat");
        require(orphan.st_nlink==0,"batched orphan links");close_file(fd);
    } else if(!strcmp(phase,"small-files")) {
        for(unsigned i=0;i<files;i++) {
            unsigned seed=1000+id*files+i;fill(buffer,4096,seed,0);
            snprintf(path,sizeof(path),"%s/s%04u",dir,i);snprintf(other,sizeof(other),"%s/d%04u",dir,i);
            int src=open_file(path,O_RDWR|O_CREAT|O_EXCL);write_all(src,buffer,4096);close_file(src);
            int victim=open_file(other,O_RDWR|O_CREAT|O_EXCL);unsigned char old[31];memset(old,0xd0+id,sizeof(old));write_all(victim,old,sizeof(old));
            if(rename(path,other))die("rename replacement");
            unsigned char check[31];require(pread(victim,check,sizeof(check),0)==sizeof(check)&&!memcmp(old,check,sizeof(old)),"retained rename victim");
            struct stat s;if(fstat(victim,&s))die("victim stat");require(s.st_nlink==0,"victim links");close_file(victim);
            int current=open_file(other,O_RDONLY);verify(current,seed,4096,buffer);close_file(current);
            if(i&1) {current=open_file(other,O_RDWR);if(unlink(other))die("open unlink");verify(current,seed,4096,buffer);close_file(current);}
            // Retain half for offline fsck while contents/inodes still exist.
        }
        snprintf(path,sizeof(path),"%s/empty",dir);if(mkdir(path,0700)||rmdir(path))die("empty directory");
    } else if(!strcmp(phase,"persistent-verify")) {
        int fd=open_file(path,O_RDONLY);uint64_t end=(uint64_t)mib*1024*1024+37,pos=0;
        while(pos<end) {
            size_t length=end-pos<CHUNK?end-pos:CHUNK;
            ssize_t n=read(fd,buffer,length);if(n<0&&errno==EINTR)continue;if(n<=0)die("persistent bulk read");
            for(ssize_t j=0;j<n;j++) {
                uint64_t off=pos+j;unsigned char expected=byte(id,off);
                if((off>=12*4096ULL-32&&off<12*4096ULL-32+97)||
                   (off>=(12+1024)*4096ULL-32&&off<(12+1024)*4096ULL-32+97))expected=0xa0+id;
                require(buffer[j]==expected,"persistent bulk contents");
            }
            pos+=n;
        }
        char extra;require(read(fd,&extra,1)==0,"persistent bulk EOF");close_file(fd);
        const char *names[]={"ragged","fragment-a","fragment-b"};
        for(unsigned i=0;i<3;i++){snprintf(path,sizeof(path),"%s/%s",dir,names[i]);fd=open_file(path,O_RDONLY);verify(fd,i?30000+id*2+i-1:20000+id,i?96*4096:283688,buffer);close_file(fd);}
        snprintf(path,sizeof(path),"%s/tail",dir);fd=open_file(path,O_RDONLY);
        verify(fd,40000+id,CHUNK+1,buffer);close_file(fd);
        for(unsigned i=0;i<files;i+=2){snprintf(path,sizeof(path),"%s/d%04u",dir,i);fd=open_file(path,O_RDONLY);verify(fd,1000+id*files+i,4096,buffer);close_file(fd);}
    } else if(!strcmp(phase,"cleanup")) {
        const char *fixed[]={"bulk","ragged","fragment-a","fragment-b","tail"};
        for(unsigned i=0;i<5;i++){snprintf(path,sizeof(path),"%s/%s",dir,fixed[i]);if(unlink(path))die("cleanup fixed");}
        for(unsigned i=0;i<files;i+=2){snprintf(path,sizeof(path),"%s/d%04u",dir,i);if(unlink(path))die("cleanup small");}
        if(rmdir(dir))die("cleanup worker directory");
    }
    free(buffer);return NULL;
}
static void run_phase(const char *name,uint64_t bytes,unsigned operations) {
    phase=name;counters("before");printf("IOBENCH_BEGIN %s tick_ns=%llu\n",phase,(unsigned long long)now_ns());
    pthread_t threads[MAX_WORKERS];pthread_attr_t attr;
    if(pthread_attr_init(&attr)||pthread_attr_setstacksize(&attr,1024*1024)||pthread_barrier_init(&barrier,NULL,workers+1))die("thread attributes");
    for(unsigned i=0;i<workers;i++)if(pthread_create(&threads[i],&attr,worker,(void*)(uintptr_t)i))die("pthread_create");
    uint64_t start=now_ns();pthread_barrier_wait(&barrier);
    for(unsigned i=0;i<workers;i++)if(pthread_join(threads[i],NULL))die("pthread_join");
    uint64_t work_end=now_ns();sync();uint64_t end=now_ns();
    pthread_barrier_destroy(&barrier);pthread_attr_destroy(&attr);
    printf("IOBENCH_RESULT {\"phase\":\"%s\",\"workers\":%u,\"bytes\":%llu,\"operations\":%u,\"work_start_ns\":%llu,\"work_ns\":%llu,\"sync_ns\":%llu,\"total_ns\":%llu}\n",name,workers,(unsigned long long)bytes,operations,(unsigned long long)start,(unsigned long long)(work_end-start),(unsigned long long)(end-work_end),(unsigned long long)(end-start));
    counters("after");
}
int main(int argc,char **argv) {
    setbuf(stdout,NULL);phase="setup";
    if(argc<3||argc>6){fprintf(stderr,"usage: io-bench root run|cleanup [workers=8] [MiB/worker=8] [files/worker=64]\n");return 2;}
    if(strlen(argv[1])>=sizeof(root)||argv[1][0]!='/')return 2;
    strcpy(root,argv[1]);
    if(argc>3)workers=strtoul(argv[3],NULL,10);
    if(argc>4)mib=strtoul(argv[4],NULL,10);
    if(argc>5)files=strtoul(argv[5],NULL,10);
    if(!workers||workers>MAX_WORKERS||mib<8||mib>128||!files||files>1024)return 2;
    if(!strcmp(argv[2],"cleanup")) {run_phase("persistent-verify",(uint64_t)workers*mib*1024*1024,0);run_phase("cleanup",0,workers*((files+1)/2+5));if(rmdir(root))die("cleanup root");sync();}
    else if(!strcmp(argv[2],"run")) {
        if(mkdir(root,0700))die("mkdir root");
        for(unsigned i=0;i<workers;i++){char dir[200];snprintf(dir,sizeof(dir),"%s/w%u",root,i);if(mkdir(dir,0700))die("mkdir worker");}
        run_phase("sequential-write",(uint64_t)workers*mib*1024*1024,workers*mib*16);
        run_phase("sequential-read",(uint64_t)workers*mib*1024*1024,0);
        run_phase("boundaries-fragmentation",0,0);
        run_phase("small-files",(uint64_t)workers*files*4096,workers*files);
    } else return 2;
    printf("IOBENCH_PASS mode=%s workers=%u\n",argv[2],workers);return 0;
}
