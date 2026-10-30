#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
static unsigned char buffer[65536];
static unsigned long free_kib(void){
    char text[2048];int fd=open("/proc/meminfo",O_RDONLY);if(fd<0)return 0;
    int n=read(fd,text,sizeof(text)-1);close(fd);if(n<=0)return 0;text[n]=0;
    char* p=strstr(text,"MemFree:");return p?strtoul(p+8,0,10):0;
}
int main(int argc,char** argv){
    const int concurrent=argc==3 && !strcmp(argv[2],"--concurrent");
    if(argc!=2 && !concurrent)return 2;
    printf("USB_IO_PROBE_BEGIN\n");fflush(stdout);
    int fd=open(argv[1],O_RDONLY);if(fd<0){perror("USB_IO_OPEN");return 3;}
    struct stat st;if(fstat(fd,&st) || st.st_size<100000)return 4;
    unsigned long warmed=0,last=0;
    for(unsigned pass=0;pass<6;++pass){
        uint64_t offset=0;
        while(offset<(uint64_t)st.st_size){
            unsigned amount=sizeof(buffer)-17;if(amount>(uint64_t)st.st_size-offset)amount=st.st_size-offset;
            ssize_t n=pread(fd,buffer,amount,offset);if(n!=(ssize_t)amount){printf("USB_IO_SHORT offset=%llu got=%ld errno=%d\n",(unsigned long long)offset,(long)n,errno);return 5;}
            for(unsigned i=0;i<amount;++i)if(buffer[i]!=(unsigned char)(((offset+i)*17+5)%251)){printf("USB_IO_BAD offset=%llu\n",(unsigned long long)(offset+i));return 6;}
            offset+=amount;
        }
        last=free_kib();if(!pass)warmed=last;
        printf("USB_IO_PASS pass=%u bytes=%llu free_kib=%lu\n",pass,(unsigned long long)offset,last);fflush(stdout);
    }
    unsigned char tiny[17];
    if(pread(fd,tiny,17,511)!=17)return 7;
    for(unsigned i=0;i<17;++i)if(tiny[i]!=(unsigned char)(((511+i)*17+5)%251))return 8;
    if(pread(fd,tiny,1,st.st_size)!=0)return 9;
    close(fd);
    if(!concurrent && warmed && last && warmed>last+4096){printf("USB_IO_MEMORY_GROWTH kib=%lu\n",warmed-last);return 10;}
    fd=open(argv[1],O_WRONLY);if(fd>=0 || errno!=EROFS){if(fd>=0)close(fd);printf("USB_IO_NOT_READONLY errno=%d\n",errno);return 11;}
    if(concurrent)puts("USB_IO_MEMORY_NOTE other desktop processes are active; global free RAM is diagnostic only");
    printf("USB_IO_PROBE_OK bytes=%llu warmed_free=%lu final_free=%lu\n",(unsigned long long)st.st_size,warmed,last);fflush(stdout);
    return 0;
}
