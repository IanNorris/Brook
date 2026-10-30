/* Host/guest diagnostic for the RAM-only reader. It never opens a raw device. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "update_initrd_reader.h"
int main(int argc,char** argv) {
    if(argc!=3){fputs("usage: INITRD-INSPECT REGULAR_IMAGE ALLOWED_METADATA_PATH\n",stderr);return 2;}
    int fd=open(argv[1],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0){perror("open");return 2;}
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<4096||st.st_size>64*1024*1024){close(fd);return 2;}
    size_t n=(size_t)st.st_size;uint8_t* bytes=malloc(n);if(!bytes){close(fd);return 2;}
    size_t pos=0;
    while(pos<n){ssize_t count=read(fd,bytes+pos,n-pos);if(count<0&&errno==EINTR)continue;if(count<=0){free(bytes);close(fd);return 2;}pos+=(size_t)count;}
    if(close(fd)){free(bytes);return 2;}
    uint8_t text[4096];size_t length=0;
    int result=BrookInitrdMetadata(bytes,n,argv[2],text,sizeof(text),&length);free(bytes);
    if(result==BROOK_INITRD_OK) {
        if(fwrite(text,1,length,stdout)!=length)return 2;
        return 0;
    }
    fprintf(stderr,"INITRD_METADATA_RESULT code=%d\n",result);return 10+result;
}
