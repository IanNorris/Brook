#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FILEIDENTITYCHECK FAIL line=%d %s\n",__LINE__,#x); ++failures; } } while(0)
static int same(const struct stat *a,const struct stat *b) {
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino;
}
static void file_check(const char *directory,size_t bytes) {
    char path[256];snprintf(path,sizeof(path),"%s/identity-%d-%zu",directory,getpid(),bytes);
    int rw=open(path,O_CREAT|O_EXCL|O_RDWR,0600); CHECK(rw>=0);if(rw<0)return;
    char block[4096];memset(block,0x5a,sizeof(block));
    for(size_t i=0;i<bytes;i+=sizeof(block)) CHECK(write(rw,block,sizeof(block))==(ssize_t)sizeof(block));
    CHECK(fsync(rw)==0);
    int ro=open(path,O_RDONLY);CHECK(ro>=0);if(ro<0){close(rw);unlink(path);return;}
    struct stat a,b,c,d;CHECK(fstat(rw,&a)==0);CHECK(fstat(ro,&b)==0);
    CHECK(a.st_ino!=0&&same(&a,&b));
    CHECK(fstat(rw,&c)==0&&same(&a,&c));
    CHECK(stat(path,&c)==0&&same(&a,&c));
    CHECK(lstat(path,&c)==0&&same(&a,&c));
    CHECK(fstatat(AT_FDCWD,path,&c,0)==0&&same(&a,&c));
    int copy=dup(ro);CHECK(copy>=0);if(copy>=0){CHECK(fstat(copy,&c)==0&&same(&a,&c));close(copy);}
    CHECK(ftruncate(rw,0)==0);CHECK(fstat(rw,&c)==0&&same(&a,&c));
    close(ro);close(rw);
    rw=open(path,O_RDWR);CHECK(rw>=0);if(rw>=0){CHECK(fstat(rw,&d)==0&&same(&a,&d));close(rw);}
    CHECK(unlink(path)==0);
}
static void memfd_check(void) {
    int a=memfd_create("identity-a",MFD_CLOEXEC),b=memfd_create("identity-b",MFD_CLOEXEC);
    CHECK(a>=0&&b>=0);if(a<0||b<0)return;
    struct stat sa,sb,sc;CHECK(fstat(a,&sa)==0);CHECK(fstat(b,&sb)==0);
    CHECK(sa.st_ino!=0&&sb.st_ino!=0&&!same(&sa,&sb));
    int copy=dup(a);CHECK(copy>=0);if(copy>=0){CHECK(fstat(copy,&sc)==0&&same(&sa,&sc));close(copy);}
    CHECK(ftruncate(a,8192)==0);CHECK(fstat(a,&sc)==0&&same(&sa,&sc)&&sc.st_size==8192);
    pid_t child=fork();CHECK(child>=0);
    if(child==0)_exit(fstat(a,&sc)==0&&same(&sa,&sc)?0:1);
    if(child>0){int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
    close(a);close(b);
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);if(argc!=2)return 2;
    file_check(argv[1],4096);file_check(argv[1],128*1024);memfd_check();
    if(failures)return 1;
    puts("FILEIDENTITYCHECK PASS small=4KiB cached=128KiB stat=fstat=lstat=fstatat dup fork memfd_distinct");return 0;
}
