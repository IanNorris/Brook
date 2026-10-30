/* Explicit fixed-size scratch-file write/readback gate for hardware updates.
 * Never creates, truncates or deletes a file. Provision the header on the host.
 * Successful write+fsync is not fresh-media verification: run verify after boot.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { FILE_BYTES=1048576, HEADER_BYTES=64 };
static unsigned char buffer[4096];
static uint32_t crc32(const unsigned char* p,unsigned n) {
    uint32_t crc=~0u;
    for(unsigned i=0;i<n;++i){crc^=p[i];for(unsigned b=0;b<8;++b)crc=(crc>>1)^(0xedb88320u&-(crc&1));}
    return ~crc;
}
static uint32_t le32(const unsigned char* p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned char pattern(uint64_t offset,const char* nonce){return (unsigned char)(offset*73+(offset>>8)*17+(offset>>16)*31+(unsigned char)nonce[offset%32]);}
static int error(const char* operation,int code){perror(operation);return code;}
int main(int argc,char** argv) {
    if(argc!=4 || (strcmp(argv[1],"write") && strcmp(argv[1],"verify"))) {
        fputs("usage: hardware_storage_probe write|verify PREPROVISIONED_FILE NONCE32\n",stderr);return 2;
    }
    if(strlen(argv[3])!=32)return 2;
    for(unsigned i=0;i<32;++i)if(!((argv[3][i]>='0' && argv[3][i]<='9') || (argv[3][i]>='a' && argv[3][i]<='f')))return 2;
    const int writing=!strcmp(argv[1],"write");
    int fd=open(argv[2],(writing?O_RDWR:O_RDONLY)|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return error("HWIO_OPEN",3);
    struct stat st;
    if(fstat(fd,&st)<0)return error("HWIO_STAT",4);
    if(!S_ISREG(st.st_mode) || st.st_size!=FILE_BYTES || st.st_nlink!=1) {
        fputs("HWIO_REFUSED expected one-link, preallocated 1MiB regular file\n",stderr);return 5;
    }
    unsigned char header[HEADER_BYTES];
    ssize_t headerCount;
    do{headerCount=pread(fd,header,sizeof(header),0);}while(headerCount<0 && errno==EINTR);
    if(headerCount!=(ssize_t)sizeof(header)){if(headerCount>=0)errno=EIO;return error("HWIO_HEADER_READ",6);}
    if(memcmp(header,"BROOK_IO_TEST_V1",16) || memcmp(header+16,argv[3],32) ||
       le32(header+48)!=FILE_BYTES || le32(header+52) || le32(header+56)!=crc32(header,56) || le32(header+60)) {
        fputs("HWIO_REFUSED scratch header/nonce mismatch; no writes performed\n",stderr);return 7;
    }
    for(uint64_t position=HEADER_BYTES;position<FILE_BYTES;) {
        unsigned n=FILE_BYTES-position>sizeof(buffer)?sizeof(buffer):(unsigned)(FILE_BYTES-position);
        if(writing)for(unsigned i=0;i<n;++i)buffer[i]=pattern(position+i-HEADER_BYTES,argv[3]);
        unsigned done=0;
        while(done<n) {
            ssize_t count=writing?pwrite(fd,buffer+done,n-done,position+done):pread(fd,buffer+done,n-done,position+done);
            if(count<0 && errno==EINTR)continue;
            if(count<=0){if(!count)errno=EIO;return error(writing?"HWIO_WRITE":"HWIO_READ",8);}
            done+=(unsigned)count;
        }
        if(!writing)for(unsigned i=0;i<n;++i)if(buffer[i]!=pattern(position+i-HEADER_BYTES,argv[3])) {
            fprintf(stderr,"HWIO_MISMATCH offset=%llu\n",(unsigned long long)(position+i));return 9;
        }
        position+=n;
    }
    if(writing && fsync(fd)<0)return error("HWIO_FSYNC",10);
    if(close(fd)<0)return error("HWIO_CLOSE",11);
    if(writing)puts("HWIO_WRITE_FSYNC_OK bytes=1048576 fresh_boot_verify_required=1");
    else puts("HWIO_VERIFY_OK bytes=1048576");
    return 0;
}
