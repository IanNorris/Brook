#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void die(const char *s) { fprintf(stderr,"MIXAPPEND FAIL %s errno=%d\n",s,errno);exit(2); }
static void put(int fd,size_t n,unsigned char value) {
    unsigned char *p=malloc(n);if(!p)die("malloc");memset(p,value,n);
    size_t pos=0;while(pos<n){ssize_t r=write(fd,p+pos,n-pos);if(r<=0)die("write");pos+=r;}free(p);
}
static void verify(int fd,size_t n,int chained) {
    unsigned char p[4096];if(lseek(fd,0,SEEK_SET)<0)die("seek");size_t pos=0;
    while(pos<n){size_t count=n-pos<sizeof(p)?n-pos:sizeof(p);ssize_t r=read(fd,p,count);if(r<=0)die("read");
        for(ssize_t i=0;i<r;i++){size_t off=pos+i;unsigned char expected=off<65536?0x31:off<106496?0x72:0x93;
            if(p[i]!=expected){fprintf(stderr,"MIXAPPEND offset=%zu actual=%u expected=%u chained=%d\n",off,p[i],expected,chained);die("contents");}}
        pos+=r;
    }
    if(read(fd,p,1)!=0)die("EOF");
}
static void one(const char *root,const char *mode) {
    char path[512];if(snprintf(path,sizeof(path),"%s/mixed-append-%s",root,mode)>=(int)sizeof(path))die("path");
    int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);if(fd<0)die("open");
    put(fd,65536,0x31);put(fd,40960,0x72);
    if(!strcmp(mode,"chain"))put(fd,65536,0x93);
    /* Verify before reclamation, without evicting the metadata cache by a
       large working-set scan. Fast->small->fast must preserve the middle. */
    verify(fd,!strcmp(mode,"chain")?172032:106496,!strcmp(mode,"chain"));
    if(close(fd))die("close");
    if(!strcmp(mode,"truncate")) {
        fd=open(path,O_RDWR|O_TRUNC);if(fd<0)die("truncate");
        unsigned char byte;if(read(fd,&byte,1)!=0)die("truncate EOF");if(close(fd))die("truncate close");
    }
    if(unlink(path))die("unlink");
    sync();printf("MIXAPPEND CASE PASS %s\n",mode);
}
int main(int argc,char **argv) {
    setbuf(stdout,NULL);if(argc!=3)return 2;
    if(!strcmp(argv[2],"all")){one(argv[1],"unlink");one(argv[1],"truncate");one(argv[1],"chain");}
    else if(!strcmp(argv[2],"unlink")||!strcmp(argv[2],"truncate")||!strcmp(argv[2],"chain"))one(argv[1],argv[2]);
    else return 2;
    printf("MIXAPPEND PASS mode=%s\n",argv[2]);return 0;
}
