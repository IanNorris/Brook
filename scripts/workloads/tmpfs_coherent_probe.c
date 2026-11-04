// Standalone native acceptance for tmpfs backing/mapping frame sharing.
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { perror(#x); return 1; } } while(0)
static uint64_t free_bytes(void) { struct statvfs s; if(statvfs("/dev/shm",&s))return 0;return (uint64_t)s.f_bfree*s.f_frsize; }
int main(void) {
    char path[]="/dev/shm/brook-coherent-XXXXXX";const size_t len=25*1024*1024+16;
    uint64_t before=free_bytes();CHECK(before>2*len);
    int fd=mkstemp(path);CHECK(fd>=0);CHECK(!unlink(path));CHECK(!ftruncate(fd,len));
    unsigned char* p=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(p!=MAP_FAILED);
    for(size_t i=0;i<len;i+=4096)p[i]=(unsigned char)(i/4096);
    uint64_t after=free_bytes();CHECK(after && before>after);
    uint64_t used=before-after;CHECK(used>=len && used<len+len/5+131072);
    CHECK(pwrite(fd,"abc",3,100)==3 && !memcmp(p+100,"abc",3));
    p[101]='Z';CHECK(!msync(p,len,MS_SYNC));char data[3];CHECK(pread(fd,data,3,100)==3 && !memcmp(data,"aZc",3));
    pid_t child=fork();CHECK(child>=0);
    if(!child){p[102]='!';_exit(0);}
    int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(pread(fd,data,3,100)==3 && !memcmp(data,"aZ!",3));
    unsigned char* priv=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);CHECK(priv!=MAP_FAILED);
    CHECK(!memcmp(priv+100,"aZ!",3));priv[100]='P';
    CHECK(pread(fd,data,3,100)==3 && !memcmp(data,"aZ!",3));CHECK(!munmap(priv,4096));
    CHECK(!munmap(p,len));CHECK(!close(fd));sync();
    printf("TMPFS_COHERENT_OK used=%llu bytes\n",(unsigned long long)used);return 0;
}
