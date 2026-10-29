#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
/* Bounded ordinary-memory pressure in a private memory-capped acceptance VM.
 * The controller owns a unique gate and releases it after its workload. An
 * optional fourth argument bounds the hold to 1..600 seconds (default90). */
static long free_pages(void){return syscall(0xB40,3L,0L,0L,0L,0L,0L);}
int main(int argc,char** argv){
    assert(argc>=3 && strlen(argv[2])<256);
    if(!strcmp(argv[1],"ready")){
        int fd=open(argv[2],O_RDONLY|O_NOFOLLOW);if(fd<0)return errno==ENOENT?3:1;
        char text[256];ssize_t n=read(fd,text,sizeof(text));assert(n>0 && write(STDOUT_FILENO,text,n)==n && close(fd)==0);return 0;
    }
    if(!strcmp(argv[1],"release")){assert(unlink(argv[2])==0 || errno==ENOENT);return 0;}
    assert((argc==4 || argc==5) && !strcmp(argv[1],"hold"));char* end;long mib=strtol(argv[3],&end,10);
    assert(!*end && mib>0 && mib<=160 && free_pages()>mib*256+64*256);
    long seconds=90;
    if(argc==5){seconds=strtol(argv[4],&end,10);assert(!*end && seconds>0 && seconds<=600);}
    size_t bytes=(size_t)mib*1024*1024;unsigned char* p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);memset(p,0x5a,bytes);
    int fd=open(argv[2],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);assert(fd>=0);
    char info[256];int length=snprintf(info,sizeof(info),"MEMORY_HOLD_READY mib=%ld free_pages=%ld\n",mib,free_pages());
    assert(write(fd,info,length)==length && close(fd)==0);int released=0;
    for(unsigned n=0;n<(unsigned)seconds*10;++n){if(access(argv[2],F_OK)<0){assert(errno==ENOENT);released=1;break;}usleep(100000);}
    for(size_t at=0;at<bytes;at+=4096)assert(p[at]==0x5a);
    assert(munmap(p,bytes)==0);if(!released)assert(unlink(argv[2])==0);
    printf("MEMORY_HOLD_%s mib=%ld integrity=pass\n",released?"PASS":"TIMEOUT",mib);return released?0:2;
}
