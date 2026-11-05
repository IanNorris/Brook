#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_file(const char *path, char *out, size_t capacity) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||(uint64_t)st.st_size>=capacity) {close(fd);return -1;}
    size_t done=0;
    while(done<(size_t)st.st_size) {
        ssize_t n=read(fd,out+done,(size_t)st.st_size-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){close(fd);return -1;}done+=(size_t)n;
    }
    if(close(fd))return -1;
    out[done]='\0';return 0;
}
static int split(char *buffer,char **lines,unsigned count) {
    char *p=buffer;
    for(unsigned i=0;i<count;++i) {
        lines[i]=p;char *end=strchr(p,'\n');
        if(!end || end==p)return -1;
        *end='\0';p=end+1;
    }
    return *p ? -1 : 0;
}
static int hex32(const char *s) {
    if(strlen(s)!=32)return 0;
    for(unsigned i=0;i<32;++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int range(const char *s) {
    int comma=0,digits=0;
    for(;*s;++s) {
        if(*s==',' && !comma && digits){comma=1;digits=0;continue;}
        if(*s<'0'||*s>'9')return 0;
        ++digits;
    }
    return comma&&digits;
}
static int load_identity(char out[97]) {
    char esp[160],data[192];char *e[4],*d[5];
    if(read_file("/etc/ahci-boot-volume",esp,sizeof(esp)) ||
       read_file("/etc/ahci-data-volume",data,sizeof(data)) || split(esp,e,4) || split(data,d,5) ||
       strcmp(e[0],"BRKESP1") || strcmp(d[0],"BRKDATA1") ||
       !hex32(e[1]) || !hex32(e[2]) || !hex32(d[1]) || !hex32(d[2]) || !hex32(d[4]) ||
       strcmp(e[1],d[1]) || !range(e[3]) || !range(d[3]))return -1;
    memcpy(out,d[1],32);memcpy(out+32,d[2],32);memcpy(out+64,d[4],32);out[96]='\0';return 0;
}
