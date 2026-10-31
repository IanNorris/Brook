// Disposable QEMU-only ext2 persistence workload. Run as root on the private
// fixture, never on a user filesystem. The create and verify phases are in
// different guest boots; host-side e2fsck is an additional independent gate.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { DATA_BYTES=150123 };
static unsigned char value(size_t i) { return (unsigned char)((i*37u+(i>>8)+19u)&255u); }
static int fail(const char* op) { perror(op);return 1; }
int main(int argc,char** argv) {
    if(argc!=2 || (strcmp(argv[1],"create") && strcmp(argv[1],"verify")))return 2;
    unsigned char* data=malloc(DATA_BYTES);if(!data)return fail("malloc");
    for(size_t i=0;i<DATA_BYTES;++i)data[i]=value(i);
    if(!strcmp(argv[1],"create")) {
        int fd=open("/mnt/ahcitest/.pending",O_WRONLY|O_CREAT|O_EXCL,0644);if(fd<0)return fail("open create");
        size_t done=0;while(done<DATA_BYTES){ssize_t n=write(fd,data+done,DATA_BYTES-done);if(n<=0)return fail("write");done+=(size_t)n;}
        if(fsync(fd))return fail("fsync file");
        if(close(fd))return fail("close");
        if(rename("/mnt/ahcitest/.pending","/mnt/ahcitest/persist.bin"))return fail("rename");
        fd=open("/mnt/ahcitest",O_RDONLY|O_DIRECTORY);if(fd<0)return fail("open directory");
        if(fsync(fd))return fail("fsync directory");
        if(close(fd))return fail("close directory");
        puts("AHCI_EXT2_CREATE_OK");
    } else {
        int fd=open("/mnt/ahcitest/persist.bin",O_RDONLY);if(fd<0)return fail("open verify");
        struct stat st;if(fstat(fd,&st) || st.st_size!=DATA_BYTES)return fail("stat size");
        unsigned char buffer[4096];size_t done=0;
        while(done<DATA_BYTES){ssize_t n=read(fd,buffer,sizeof(buffer));if(n<=0)return fail("read");
            if(memcmp(buffer,data+done,(size_t)n)){fprintf(stderr,"data mismatch at %zu\n",done);return 1;}done+=(size_t)n;}
        if(read(fd,buffer,1)!=0)return fail("expected EOF");
        if(close(fd))return fail("close verify");
        puts("AHCI_EXT2_VERIFY_OK");
    }
    free(data);return 0;
}
