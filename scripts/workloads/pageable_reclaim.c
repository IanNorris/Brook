#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static unsigned char pattern(unsigned page,unsigned at){return (unsigned char)(page*31+at*73+(at>>8));}
static void verify(volatile unsigned char* p,unsigned i){for(unsigned j=0;j<4096;++j)assert(p[j]==pattern(i,j));}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--full-store")) {
        long base=ctl(0,1025*4096);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
        p[1024*4096]=93;
        for(unsigned i=0;i<1024;++i)assert(ctl(1,base+i*4096)==4096);
        assert(ctl(9,1)==0);assert(ctl(10,0)==-1 && errno==ENOSPC);
        assert(p[1024*4096]==93 && ctl(2,0)==1024);
        assert(munmap(p,1025*4096)==0 && ctl(2,0)==0);
        puts("PAGEABLE_RECLAIM_FULL_PASS victim_preserved_on_ENOSPC");return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--scan")) {
        enum { ScanPages=80, Reserve=8 };
        int fd=open("/tmp/pageable-reclaim-scan-data",O_CREAT|O_TRUNC|O_RDWR,0600);assert(fd>=0);
        assert(write(fd,"x",1)==1 && lseek(fd,0,SEEK_SET)==0);
        long base=ctl(0,ScanPages*4096);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
        for(unsigned i=0;i<ScanPages;++i)p[i*4096]=(unsigned char)i;
        for(unsigned i=0;i<ScanPages;++i)assert(ctl(1,base+i*4096)==4096);
        for(unsigned i=ScanPages-Reserve;i<ScanPages;++i) {
            assert(ctl(8,1)==0);assert(*(volatile unsigned char*)(p+i*4096)==i);
        }
        assert(ctl(7,0)==0 && ctl(9,1)==0);
        // More than a batch of cold pages precede the first eligible victim.
        assert(ctl(10,0)==4096 && ctl(7,0)==1);
        assert(ctl(8,1)==0);assert(*(volatile unsigned char*)p==0 && ctl(7,0)==0);
        // One full wrapped traversal, every resident victim covered by a lease.
        assert(ctl(8,1)==0);assert(read(fd,p,ScanPages*4096)==-1 && errno==EFAULT);
        assert(ctl(2,0)==ScanPages-Reserve && ctl(6,0)==0);
        assert(ctl(9,0)==0);
        for(unsigned i=0;i<ScanPages;++i) {
            errno=0;long result=ctl(1,base+i*4096);assert(result==4096 || (result==-1 && errno==EBUSY));
        }
        assert(ctl(7,0)==Reserve);
        assert(munmap(p,ScanPages*4096)==0 && ctl(2,0)==0);
        assert(close(fd)==0 && unlink("/tmp/pageable-reclaim-scan-data")==0);
        puts("PAGEABLE_RECLAIM_SCAN_PASS yield_boundary wrapped_all_leased clean_slots");return 0;
    }
    enum { Pages=12, Reserve=8 };
    int fd=open("/tmp/pageable-reclaim-io",O_CREAT|O_TRUNC|O_RDWR,0600);assert(fd>=0);
    assert(write(fd,"x",1)==1 && lseek(fd,0,SEEK_SET)==0);
    assert(ctl(2,0)==0 && ctl(7,0)==Reserve);
    long base=ctl(0,Pages*4096);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
    for(unsigned i=0;i<Pages;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    for(unsigned i=0;i<Pages;++i)assert(ctl(1,base+i*4096)==4096);
    for(unsigned i=0;i<Reserve;++i){assert(ctl(8,1)==0);verify(p+i*4096,i);}
    assert(ctl(7,0)==0 && ctl(2,0)==Pages-Reserve);
    assert(ctl(9,1)==0);
    // Eager preparation must NOT reclaim earlier pages behind its cursor.
    assert(ctl(8,1)==0);assert(syscall(SYS_getpid)==-1 && errno==ENOMEM);
    assert(ctl(7,0)==0 && ctl(2,0)==Pages-Reserve);
    // The read lease covers all victims before prefaulting the first cold page.
    assert(ctl(8,1)==0);assert(read(fd,p,Pages*4096)==-1 && errno==EFAULT);
    assert(ctl(6,0)==0 && ctl(7,0)==0 && ctl(2,0)==Pages-Reserve);
    // Victim failure never consumes stored target data or a good victim frame.
    const long failures[]={1,4,6};
    for(unsigned i=0;i<3;++i) {
        assert(ctl(4,failures[i])==0);
        assert(ctl(10,0)==-1 && errno==EIO);
        assert(ctl(2,0)==Pages-Reserve && ctl(7,0)==0);
        // Mode 6 leaves a Recovery-owned frame; next verification republishes it.
        for(unsigned j=0;j<Reserve;++j)verify(p+j*4096,j);
    }
    // Actual user instruction fault: no explicit pageout supplies this frame.
    assert(ctl(8,1)==0);verify(p+Reserve*4096,Reserve);
    assert(ctl(7,0)==0 && ctl(2,0)==Pages-Reserve);
    // Scoped usercopy can reclaim other pages while preserving its source lease.
    assert(ctl(8,1)==0);assert(write(fd,p+(Pages-1)*4096,4096)==4096);
    assert(ctl(8,0)==0 && ctl(6,0)==0 && ctl(7,0)==0);
    for(unsigned round=0;round<3;++round)for(unsigned i=0;i<Pages;++i) {
        assert(ctl(8,1)==0);verify(p+i*4096,i);assert(ctl(8,0)==0);
        assert(ctl(7,0)==0 && ctl(2,0)==Pages-Reserve);
    }
    assert(ctl(9,0)==0);
    // Return frames explicitly so subsequent fixtures retain the boot reserve.
    for(unsigned i=0;i<Pages;++i) {
        errno=0;long result=ctl(1,base+i*4096);assert(result==4096 || (result==-1 && errno==EBUSY));
    }
    assert(ctl(7,0)==Reserve);
    assert(munmap(p,Pages*4096)==0 && ctl(2,0)==0);
    assert(close(fd)==0);assert(unlink("/tmp/pageable-reclaim-io")==0);
    puts("PAGEABLE_RECLAIM_PASS user_fault scoped_copy eager_exclusion all_leased victim_errors cycles=3 clean_slots");
}
